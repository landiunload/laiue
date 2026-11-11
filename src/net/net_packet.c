#include "net/net_internal.h"

#include "platform/system.h"

#include <string.h>

/* Пакеты данных установленного соединения: номера и защита от повторов,
 * подтверждения, обнаружение потерь, управление перегрузкой и сборка
 * датаграмм из кадров.
 *
 * Надёжность устроена по образцу QUIC (RFC 9002). Номер пакета никогда не
 * повторяется: потерянные байты уезжают в новом пакете с новым номером,
 * поэтому подтверждение однозначно и время оборота измеряется без догадок.
 * Потерю выдаёт либо обгон на три пакета, либо возраст больше 9/8 времени
 * оборота. Когда подтверждений нет совсем, срабатывает пробный таймер: он
 * ничего не объявляет потерянным, а отправляет пробу, ответ на которую
 * запускает обычное обнаружение. Окно перегрузки растёт по NewReno, а
 * отправка размазана по времени оборота, а не выстреливает пачкой. */

uint32_t NetPeerMaxUnreliable(const NetPeer *peer)
{
    /* Заголовок пакета, тег и самый длинный заголовок кадра: вид, канал,
     * номер до десяти байт и длина в два. */
    return peer->datagramBytes - NET_SHORT_OVERHEAD - 14u;
}

uint64_t NetPeerProbeTimeout(const NetPeer *peer)
{
    const uint64_t smoothed = peer->haveRtt ? peer->smoothedRtt : NET_INITIAL_RTT;
    uint64_t spread = peer->haveRtt ? peer->rttVariance * 4u : NET_INITIAL_RTT * 2u;
    if (spread < NET_TIMER_GRANULARITY)
        spread = NET_TIMER_GRANULARITY;
    return smoothed + spread + NET_MAX_ACK_DELAY;
}

static uint64_t MinimumWindow(const NetPeer *peer)
{
    return 2u * (uint64_t)peer->datagramBytes;
}

bool NetPeerEstablish(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const NetParameters *local = &endpoint->local;
    const NetParameters *remote = &peer->remote;
    peer->datagramBytes =
        local->datagramBytes < remote->datagramBytes ? local->datagramBytes : remote->datagramBytes;
    peer->channelCount =
        local->channelCount < remote->channelCount ? local->channelCount : remote->channelCount;
    peer->idleTimeout =
        (uint64_t)(local->idleMilliseconds < remote->idleMilliseconds ? local->idleMilliseconds
                                                                      : remote->idleMilliseconds) *
        NET_MILLISECOND;
    peer->channels = PlatformAllocate((size_t)peer->channelCount * sizeof(NetChannel), true);
    peer->records = PlatformAllocate((size_t)NET_RECORDS_INITIAL * sizeof(NetSentPacket), true);
    if (peer->channels == NULL || peer->records == NULL)
    {
        NetPeerFreeTransport(peer);
        return false;
    }
    peer->recordCapacity = NET_RECORDS_INITIAL;
    peer->recordHead = 0u;
    peer->recordCount = 0u;
    peer->packetThreshold = NET_PACKET_THRESHOLD;
    peer->timeThreshold = NET_TIME_THRESHOLD;
    /* Начальное окно RFC 9002: десять датаграмм, но не больше 14720 байт и
     * не меньше двух датаграмм. */
    uint64_t window = 10u * (uint64_t)peer->datagramBytes;
    const uint64_t ceiling = MinimumWindow(peer) > 14720u ? MinimumWindow(peer) : 14720u;
    if (window > ceiling)
        window = ceiling;
    peer->congestionWindow = window;
    peer->slowStartThreshold = UINT64_MAX;
    peer->paceBudget = (int64_t)window;
    peer->paceTime = endpoint->now;
    peer->lastReceiveTime = endpoint->now;
    peer->lastAckElicitingSent = endpoint->now;
    return true;
}

void NetPeerFreeTransport(NetPeer *peer)
{
    NetChannelsFree(peer);
    PlatformFree(peer->records);
    peer->records = NULL;
    peer->recordCapacity = 0u;
    peer->recordHead = 0u;
    peer->recordCount = 0u;
    PlatformFree(peer->unreliable);
    peer->unreliable = NULL;
    peer->unreliableHead = 0u;
    peer->unreliableTail = 0u;
    peer->unreliableCapacity = 0u;
    peer->queuedReliable = 0u;
}

/* ---- Номера принятых пакетов ------------------------------------------------------
 *
 * Два кольцевых набора битов на последние NET_WINDOW_BITS номеров: «видели»
 * отсекает повторы, «подтвердить» кормит кадр ACK. Номер старше окна
 * отвергается: о нём уже нельзя сказать, повтор ли это. */

static bool WindowTest(const uint64_t *bits, uint64_t number)
{
    return ((bits[(number >> 6u) & (NET_WINDOW_WORDS - 1u)] >> (number & 63u)) & 1u) != 0u;
}

static void WindowSet(uint64_t *bits, uint64_t number)
{
    bits[(number >> 6u) & (NET_WINDOW_WORDS - 1u)] |= UINT64_C(1) << (number & 63u);
}

static void WindowClear(uint64_t *bits, uint64_t number)
{
    bits[(number >> 6u) & (NET_WINDOW_WORDS - 1u)] &= ~(UINT64_C(1) << (number & 63u));
}

/* Полный номер по младшим 32 битам: ближайший к ожидаемому. */
static uint64_t NumberDecode(const NetPeer *peer, uint32_t truncated)
{
    const uint64_t expected = peer->anySeen ? peer->largestSeen + 1u : 0u;
    const uint64_t span = UINT64_C(1) << 32u;
    const uint64_t half = span / 2u;
    uint64_t candidate = (expected & ~(span - 1u)) | truncated;
    if (expected >= half && candidate <= expected - half && candidate < UINT64_MAX - span)
        candidate += span;
    else if (candidate > expected + half && candidate >= span)
        candidate -= span;
    return candidate;
}

static bool NumberAcceptable(const NetPeer *peer, uint64_t number)
{
    if (!peer->anySeen || number > peer->largestSeen)
        return true;
    if (peer->largestSeen - number >= NET_WINDOW_BITS)
        return false;
    return !WindowTest(peer->seenBits, number);
}

static void NumberRecord(NetPeer *peer, uint64_t number)
{
    if (!peer->anySeen || number > peer->largestSeen)
    {
        /* Окно сдвигается: ячейки новых номеров ещё хранят биты старых. */
        const uint64_t first = peer->anySeen ? peer->largestSeen + 1u : number;
        if (number - first >= NET_WINDOW_BITS)
        {
            memset(peer->seenBits, 0, sizeof(peer->seenBits));
            memset(peer->ackBits, 0, sizeof(peer->ackBits));
        }
        else
        {
            for (uint64_t cleared = first; cleared <= number; ++cleared)
            {
                WindowClear(peer->seenBits, cleared);
                WindowClear(peer->ackBits, cleared);
            }
        }
        peer->anySeen = true;
        peer->largestSeen = number;
    }
    WindowSet(peer->seenBits, number);
}

/* ---- Кадр ACK -------------------------------------------------------------------
 *
 * Подтверждаются только пакеты, требовавшие подтверждения: у отправителя
 * запись есть именно для них, поэтому старший номер кадра всегда даёт ему
 * замер времени оборота, а задержка ответа отсчитана от приёма этого самого
 * пакета. */

static uint32_t WriteAck(const NetPeer *peer, uint8_t *output, uint32_t space, uint64_t now)
{
    /* Старший подтверждаемый пакет уже выехал из окна номеров: подтверждать
     * нечего, отправитель давно счёл его потерянным. */
    if (!peer->anyEliciting || peer->largestSeen - peer->largestEliciting >= NET_WINDOW_BITS)
        return 0u;
    const uint64_t lowest =
        peer->largestSeen >= NET_WINDOW_BITS - 1u ? peer->largestSeen - (NET_WINDOW_BITS - 1u) : 0u;
    const uint64_t largest = peer->largestEliciting;
    uint64_t low = largest;
    while (low > lowest && WindowTest(peer->ackBits, low - 1u))
        --low;
    const uint64_t first = largest - low;

    /* Дальше — отрезки ниже первого. Пропуск и длина кодируются как в QUIC:
     * обе величины на единицу меньше настоящих. */
    uint64_t gaps[NET_ACK_RANGES];
    uint64_t lengths[NET_ACK_RANGES];
    uint32_t count = 0u;
    uint64_t cursor = low;
    while (count < NET_ACK_RANGES && cursor > lowest + 1u)
    {
        uint64_t high = cursor - 2u;
        while (high > lowest && !WindowTest(peer->ackBits, high))
            --high;
        if (!WindowTest(peer->ackBits, high))
            break;
        uint64_t rangeLow = high;
        while (rangeLow > lowest && WindowTest(peer->ackBits, rangeLow - 1u))
            --rangeLow;
        gaps[count] = cursor - high - 2u;
        lengths[count] = high - rangeLow;
        ++count;
        cursor = rangeLow;
    }

    const uint64_t delay = now > peer->largestElicitingTime ? now - peer->largestElicitingTime : 0u;
    for (;;)
    {
        uint32_t size = 1u + NetVarintSize(largest) + NetVarintSize(delay) + NetVarintSize(count) +
                        NetVarintSize(first);
        for (uint32_t index = 0u; index < count; ++index)
            size += NetVarintSize(gaps[index]) + NetVarintSize(lengths[index]);
        if (size <= space)
            break;
        if (count == 0u)
            return 0u;
        --count;
    }
    output[0] = NET_FRAME_ACK;
    uint32_t used = 1u;
    used += NetVarintWrite(output + used, largest);
    used += NetVarintWrite(output + used, delay);
    used += NetVarintWrite(output + used, count);
    used += NetVarintWrite(output + used, first);
    for (uint32_t index = 0u; index < count; ++index)
    {
        used += NetVarintWrite(output + used, gaps[index]);
        used += NetVarintWrite(output + used, lengths[index]);
    }
    return used;
}

static void AckSent(NetPeer *peer)
{
    peer->ackPendingCount = 0u;
    peer->ackImmediate = false;
}

/* ---- Записи отправленных пакетов --------------------------------------------------
 *
 * Очередь в порядке отправки. Запись получает только пакет, требующий
 * подтверждения, поэтому память зависит от пакетов в пути, а не от того,
 * сколько номеров ушло на пакеты из одного ACK. Номера в очереди растут, и
 * запись по номеру находится двоичным поиском: подтверждение обходит только
 * те записи, что в нём названы, какие бы отрезки ни прислал собеседник.
 *
 * Голова снимается, когда пакет подтверждён либо потерян так давно, что
 * опоздавшее подтверждение уже не придёт: до тех пор потерянный пакет
 * помнится, чтобы распознать мнимую потерю. */

static NetSentPacket *RecordAt(const NetPeer *peer, uint32_t position)
{
    return &peer->records[(peer->recordHead + position) & (peer->recordCapacity - 1u)];
}

/* Позиция первой записи с номером не меньше number. */
static uint32_t RecordFind(const NetPeer *peer, uint64_t number)
{
    uint32_t low = 0u;
    uint32_t high = peer->recordCount;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        if (RecordAt(peer, middle)->number < number)
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static void RecordsPop(NetPeer *peer)
{
    peer->recordHead = (peer->recordHead + 1u) & (peer->recordCapacity - 1u);
    --peer->recordCount;
}

static void RecordsTrim(NetPeer *peer, uint64_t now)
{
    /* Подтверждение потерянного пакета, если оно вообще придёт, приходит в
     * пределах пробного интервала после его отправки. */
    const uint64_t memory = 2u * NetPeerProbeTimeout(peer);
    while (peer->recordCount != 0u)
    {
        const NetSentPacket *front = RecordAt(peer, 0u);
        if (front->state == NET_RECORD_SENT ||
            (front->state == NET_RECORD_LOST && front->sentTime + memory > now))
            break;
        RecordsPop(peer);
    }
}

static bool RecordsGrow(NetPeer *peer)
{
    const uint32_t capacity = peer->recordCapacity * 2u;
    NetSentPacket *grown = PlatformAllocate((size_t)capacity * sizeof(NetSentPacket), false);
    if (grown == NULL)
        return false;
    for (uint32_t position = 0u; position < peer->recordCount; ++position)
        grown[position] = *RecordAt(peer, position);
    PlatformFree(peer->records);
    peer->records = grown;
    peer->recordCapacity = capacity;
    peer->recordHead = 0u;
    return true;
}

/* ---- Перегрузка и оценка пути ------------------------------------------------------ */

static void CongestionOnAck(NetPeer *peer, const NetSentPacket *record)
{
    /* Пакеты, отправленные до начала восстановления, окно не расширяют; не
     * расширяет его и поток, которому окно не было тесно. */
    if ((peer->inRecovery && record->sentTime <= peer->recoveryStart) || !record->windowLimited)
        return;
    if (peer->congestionWindow < peer->slowStartThreshold)
        peer->congestionWindow += record->size;
    else
        peer->congestionWindow +=
            (uint64_t)peer->datagramBytes * record->size / peer->congestionWindow;
    if (peer->congestionWindow > NET_CONGESTION_WINDOW_LIMIT)
        peer->congestionWindow = NET_CONGESTION_WINDOW_LIMIT;
}

static void CongestionOnLoss(NetPeer *peer, uint64_t sentTime, uint64_t now)
{
    /* Одно событие перегрузки на полёт: потери пакетов, отправленных до
     * него, окно повторно не режут. */
    if (peer->inRecovery && sentTime <= peer->recoveryStart)
        return;
    /* Эпизод без мнимых потерь с прошлого раза: пороги понемногу
     * возвращаются к исходным, и однажды случившаяся перестановка не
     * замедляет обнаружение потерь навсегда. */
    if (!peer->spuriousSeen)
    {
        if (peer->packetThreshold > NET_PACKET_THRESHOLD)
            --peer->packetThreshold;
        if (peer->timeThreshold > NET_TIME_THRESHOLD)
            --peer->timeThreshold;
    }
    peer->spuriousSeen = false;
    ++peer->recoveryEpisode;
    peer->recoveryLosses = 0u;
    peer->recoverySpurious = 0u;
    peer->recoveryUndoable = true;
    peer->priorWindow = peer->congestionWindow;
    peer->priorThreshold = peer->slowStartThreshold;
    peer->inRecovery = true;
    peer->recoveryStart = now;
    peer->slowStartThreshold = peer->congestionWindow / 2u;
    if (peer->slowStartThreshold < MinimumWindow(peer))
        peer->slowStartThreshold = MinimumWindow(peer);
    peer->congestionWindow = peer->slowStartThreshold;
}

static void UpdateRtt(NetPeer *peer, uint64_t sample, uint64_t ackDelay, uint64_t now)
{
    peer->latestRtt = sample != 0u ? sample : 1u;
    if (!peer->haveRtt)
    {
        peer->haveRtt = true;
        peer->firstRttTime = now;
        peer->minRtt = peer->latestRtt;
        peer->smoothedRtt = peer->latestRtt;
        peer->rttVariance = peer->latestRtt / 2u;
        return;
    }
    if (peer->latestRtt < peer->minRtt)
        peer->minRtt = peer->latestRtt;
    if (ackDelay > NET_MAX_ACK_DELAY)
        ackDelay = NET_MAX_ACK_DELAY;
    /* Задержка подтверждения вычитается, только если остаток правдоподобен. */
    uint64_t adjusted = peer->latestRtt;
    if (adjusted >= peer->minRtt + ackDelay)
        adjusted -= ackDelay;
    const uint64_t deviation =
        peer->smoothedRtt > adjusted ? peer->smoothedRtt - adjusted : adjusted - peer->smoothedRtt;
    peer->rttVariance = (3u * peer->rttVariance + deviation) / 4u;
    peer->smoothedRtt = (7u * peer->smoothedRtt + adjusted) / 8u;
}

void NetPeerSeedRtt(NetPeer *peer, uint64_t sample, uint64_t now)
{
    if (!peer->haveRtt)
        UpdateRtt(peer, sample, 0u, now);
}

/* ---- Подтверждения и потери ---------------------------------------------------------- */

typedef struct AckContext
{
    uint64_t now;
    uint64_t largest;
    /* Позиция записи наибольшего подтверждённого номера. */
    uint32_t largestPosition;
    uint64_t largestSentTime;
    bool largestNewlyAcked;
    uint32_t newlyAcked;
} AckContext;

/* Время оборота, от которого отсчитывается порог потери по возрасту. */
static uint64_t LossReference(const NetPeer *peer)
{
    uint64_t reference = NET_INITIAL_RTT;
    if (peer->haveRtt)
        reference = peer->latestRtt > peer->smoothedRtt ? peer->latestRtt : peer->smoothedRtt;
    return reference > NET_TIMER_GRANULARITY ? reference : NET_TIMER_GRANULARITY;
}

static void PacketAcked(NetPeer *peer, NetSentPacket *record, AckContext *context)
{
    record->state = NET_RECORD_ACKED;
    --peer->ackElicitingOutstanding;
    peer->bytesInFlight -= record->size;
    CongestionOnAck(peer, record);
    for (uint32_t index = 0u; index < record->rangeCount; ++index)
        NetChannelOnAcked(peer, &record->ranges[index]);
    if (record->number == context->largest)
    {
        context->largestNewlyAcked = true;
        context->largestSentTime = record->sentTime;
    }
    ++context->newlyAcked;
}

static void PacketLost(NetPeer *peer, NetSentPacket *record, uint64_t now)
{
    record->state = NET_RECORD_LOST;
    --peer->ackElicitingOutstanding;
    peer->bytesInFlight -= record->size;
    ++peer->packetsLost;
    CongestionOnLoss(peer, record->sentTime, now);
    record->lossEpisode = (uint16_t)peer->recoveryEpisode;
    ++peer->recoveryLosses;
    for (uint32_t index = 0u; index < record->rangeCount; ++index)
        NetChannelOnLost(peer, &record->ranges[index]);
}

/* Пакет, уже объявленный потерянным, всё-таки подтверждён: потеря мнимая,
 * путь переставляет пакеты сильнее, чем допускали пороги. Пороги растут до
 * наблюдённого, ещё не отправленный повтор его данных отменяется, а
 * сокращение окна, вызванное одними мнимыми потерями, откатывается. */
static void SpuriousLoss(NetPeer *peer, NetSentPacket *record, uint32_t distance,
                         AckContext *context)
{
    record->state = NET_RECORD_ACKED;
    --peer->packetsLost;
    peer->spuriousSeen = true;
    for (uint32_t index = 0u; index < record->rangeCount; ++index)
        NetChannelOnAcked(peer, &record->ranges[index]);
    if (distance >= peer->packetThreshold)
        peer->packetThreshold =
            distance < NET_PACKET_THRESHOLD_LIMIT ? distance + 1u : NET_PACKET_THRESHOLD_LIMIT;
    const uint64_t eighths = (context->now - record->sentTime) * 8u / LossReference(peer) + 1u;
    if (eighths > peer->timeThreshold)
        peer->timeThreshold =
            eighths < NET_TIME_THRESHOLD_LIMIT ? (uint32_t)eighths : NET_TIME_THRESHOLD_LIMIT;
    if (peer->recoveryUndoable && record->lossEpisode == (uint16_t)peer->recoveryEpisode)
    {
        ++peer->recoverySpurious;
        if (peer->recoverySpurious == peer->recoveryLosses)
        {
            if (peer->congestionWindow < peer->priorWindow)
                peer->congestionWindow = peer->priorWindow;
            peer->slowStartThreshold = peer->priorThreshold;
            peer->inRecovery = false;
            peer->recoveryUndoable = false;
        }
    }
    /* Опоздавшее подтверждение тоже даёт замер пути: долгий, и потому
     * честно расширяющий разброс. */
    if (record->number == context->largest)
    {
        context->largestNewlyAcked = true;
        context->largestSentTime = record->sentTime;
    }
    ++context->newlyAcked;
}

static void AckRange(NetPeer *peer, uint64_t low, uint64_t high, AckContext *context)
{
    for (uint32_t position = RecordFind(peer, low); position < peer->recordCount; ++position)
    {
        NetSentPacket *record = RecordAt(peer, position);
        if (record->number > high)
            break;
        if (record->state == NET_RECORD_SENT)
            PacketAcked(peer, record, context);
        else if (record->state == NET_RECORD_LOST)
            SpuriousLoss(peer, record, context->largestPosition - position, context);
    }
}

static void DetectLost(NetPeer *peer, uint64_t now)
{
    peer->lossTime = 0u;
    if (!peer->anyAcked)
        return;
    const uint64_t lossDelay = LossReference(peer) * peer->timeThreshold / 8u;
    /* Затяжная перегрузка: подряд потеряно всё, что отправлено за три пробных
     * интервала. Путь изменился, и прежнее окно ничего о нём не говорит. */
    const uint64_t persistent = 3u * NetPeerProbeTimeout(peer);
    bool inRun = false;
    bool collapsed = false;
    uint64_t runStart = 0u;
    /* Обгон считается в пакетах, требовавших подтверждения: пакеты из одного
     * ACK его не ускоряют. */
    const uint32_t largest = RecordFind(peer, peer->largestAcked);
    for (uint32_t position = 0u; position < largest; ++position)
    {
        NetSentPacket *record = RecordAt(peer, position);
        if (record->state == NET_RECORD_SENT)
        {
            if (largest - position >= peer->packetThreshold || record->sentTime + lossDelay <= now)
            {
                PacketLost(peer, record, now);
            }
            else if (peer->lossTime == 0u || record->sentTime + lossDelay < peer->lossTime)
            {
                peer->lossTime = record->sentTime + lossDelay;
            }
        }
        if (record->state != NET_RECORD_LOST)
        {
            inRun = false;
        }
        else if (!inRun)
        {
            inRun = true;
            runStart = record->sentTime;
        }
        else if (peer->haveRtt && runStart > peer->firstRttTime &&
                 record->sentTime - runStart >= persistent)
        {
            collapsed = true;
        }
    }
    if (collapsed)
    {
        /* Окно, сжатое затяжной перегрузкой, мнимые потери не возвращают. */
        peer->congestionWindow = MinimumWindow(peer);
        peer->inRecovery = true;
        peer->recoveryStart = now;
        peer->recoveryUndoable = false;
    }
    RecordsTrim(peer, now);
}

static uint32_t ProcessAck(const LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t **cursor,
                           const uint8_t *end)
{
    uint64_t largest = 0u;
    uint64_t delay = 0u;
    uint64_t count = 0u;
    uint64_t first = 0u;
    if (!NetVarintRead(cursor, end, &largest) || !NetVarintRead(cursor, end, &delay) ||
        !NetVarintRead(cursor, end, &count) || !NetVarintRead(cursor, end, &first))
        return NET_FRAMES_VIOLATION;
    /* Подтверждать можно только отправленное. */
    if (largest >= peer->nextNumber || first > largest || count > 255u)
        return NET_FRAMES_VIOLATION;
    AckContext context = {
        .now = endpoint->now,
        .largest = largest,
        .largestPosition = RecordFind(peer, largest),
    };
    uint64_t low = largest - first;
    AckRange(peer, low, largest, &context);
    for (uint64_t index = 0u; index < count; ++index)
    {
        uint64_t gap = 0u;
        uint64_t length = 0u;
        if (!NetVarintRead(cursor, end, &gap) || !NetVarintRead(cursor, end, &length))
            return NET_FRAMES_VIOLATION;
        if (gap > low || low - gap < 2u)
            return NET_FRAMES_VIOLATION;
        const uint64_t high = low - gap - 2u;
        if (length > high)
            return NET_FRAMES_VIOLATION;
        low = high - length;
        AckRange(peer, low, high, &context);
    }
    if (context.newlyAcked == 0u)
        return NET_FRAMES_OK;
    if (!peer->anyAcked || largest > peer->largestAcked)
    {
        peer->anyAcked = true;
        peer->largestAcked = largest;
    }
    if (context.largestNewlyAcked)
        UpdateRtt(peer, endpoint->now - context.largestSentTime, delay, endpoint->now);
    DetectLost(peer, endpoint->now);
    peer->probeCount = 0u;
    return NET_FRAMES_OK;
}

/* ---- Разбор кадров ------------------------------------------------------------------- */

static uint32_t ProcessClose(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t **cursor,
                             const uint8_t *end)
{
    uint64_t code = 0u;
    if (*cursor == end)
        return NET_FRAMES_VIOLATION;
    const uint8_t kind = **cursor;
    ++*cursor;
    if (!NetVarintRead(cursor, end, &code) || code > UINT32_MAX)
        return NET_FRAMES_VIOLATION;
    uint32_t reason = LAIUE_NET_REASON_REMOTE;
    if (kind == NET_CLOSE_PROTOCOL)
        reason = LAIUE_NET_REASON_PROTOCOL;
    else if (kind == NET_CLOSE_RESOURCE)
        reason = LAIUE_NET_REASON_RESOURCE;
    NetPeerClose(endpoint, peer, reason, (uint32_t)code, NET_CLOSE_NONE);
    return NET_FRAMES_CLOSED;
}

static uint32_t ProcessStream(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t **cursor,
                              const uint8_t *end)
{
    uint64_t offset = 0u;
    uint64_t length = 0u;
    if (*cursor == end)
        return NET_FRAMES_VIOLATION;
    const uint32_t channel = **cursor;
    ++*cursor;
    if (channel >= peer->channelCount || !NetVarintRead(cursor, end, &offset) ||
        !NetVarintRead(cursor, end, &length) || length > (uint64_t)(end - *cursor))
        return NET_FRAMES_VIOLATION;
    const uint8_t *bytes = *cursor;
    *cursor += (size_t)length;
    return NetChannelReceiveStream(endpoint, peer, channel, offset, bytes, (uint32_t)length);
}

static uint32_t ProcessUnreliable(LaiueNetEndpoint *endpoint, NetPeer *peer, bool sequenced,
                                  const uint8_t **cursor, const uint8_t *end)
{
    uint64_t sequence = 0u;
    uint64_t length = 0u;
    if (*cursor == end)
        return NET_FRAMES_VIOLATION;
    const uint32_t channel = **cursor;
    ++*cursor;
    if (channel >= peer->channelCount || (sequenced && !NetVarintRead(cursor, end, &sequence)) ||
        !NetVarintRead(cursor, end, &length) || length > (uint64_t)(end - *cursor))
        return NET_FRAMES_VIOLATION;
    const uint8_t *bytes = *cursor;
    *cursor += (size_t)length;
    return NetChannelReceiveUnreliable(endpoint, peer, channel, sequenced, sequence, bytes,
                                       (uint32_t)length);
}

uint32_t NetPeerProcessFrames(LaiueNetEndpoint *endpoint, NetPeer *peer, const uint8_t *payload,
                              uint32_t size, bool *outAckEliciting)
{
    const uint8_t *cursor = payload;
    const uint8_t *end = payload + size;
    uint32_t result = NET_FRAMES_OK;
    bool acked = false;
    *outAckEliciting = false;
    while (cursor != end)
    {
        const uint8_t type = *cursor;
        ++cursor;
        uint32_t status = NET_FRAMES_OK;
        switch (type)
        {
        case NET_FRAME_PADDING:
            break;
        case NET_FRAME_PING:
            *outAckEliciting = true;
            break;
        case NET_FRAME_ACK:
            /* Честному отправителю хватает одного кадра на пакет, а разбор
             * каждого обходит записи отправленного: вторым кадром собеседник
             * лишь заставлял бы делать эту работу повторно. */
            status = acked ? NET_FRAMES_VIOLATION : ProcessAck(endpoint, peer, &cursor, end);
            acked = true;
            break;
        case NET_FRAME_CLOSE:
            return ProcessClose(endpoint, peer, &cursor, end);
        case NET_FRAME_STREAM:
            *outAckEliciting = true;
            status = ProcessStream(endpoint, peer, &cursor, end);
            break;
        case NET_FRAME_DATAGRAM:
        case NET_FRAME_SEQUENCED:
            *outAckEliciting = true;
            status = ProcessUnreliable(endpoint, peer, type == NET_FRAME_SEQUENCED, &cursor, end);
            break;
        default:
            status = NET_FRAMES_VIOLATION;
            break;
        }
        if (status == NET_FRAMES_DEFER)
            result = NET_FRAMES_DEFER;
        else if (status != NET_FRAMES_OK)
            return status;
    }
    return result;
}

/* ---- Отправка ------------------------------------------------------------------------- */

/* Шифрует кадры, уже лежащие в буфере пакета после заголовка, и отправляет. */
static void SealAndSend(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t payloadSize)
{
    uint8_t *packet = endpoint->packetBuffer;
    uint8_t *payload = packet + NET_SHORT_HEADER_SIZE;
    uint8_t nonce[NET_NONCE_SIZE] = {0u};
    const uint64_t number = peer->nextNumber;
    const uint32_t total = NET_SHORT_OVERHEAD + payloadSize;
    packet[0] = NET_PACKET_SHORT;
    NetStore64(packet + 1, peer->remoteCid);
    NetStore32(packet + 9, (uint32_t)number);
    NetStore64(nonce + 4, number);
    NetAeadSeal(peer->sendKey, nonce, packet, NET_SHORT_HEADER_SIZE, payload, payloadSize, payload,
                payload + payloadSize);
    NetTransmit(endpoint, &peer->address, packet, total);
    ++peer->nextNumber;
    ++peer->packetsSent;
    peer->bytesSent += total;
}

void NetPeerSendClose(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    uint8_t *payload = endpoint->packetBuffer + NET_SHORT_HEADER_SIZE;
    payload[0] = NET_FRAME_CLOSE;
    payload[1] = peer->closeKind;
    SealAndSend(endpoint, peer, 2u + NetVarintWrite(payload + 2, peer->closeCode));
}

static void SendAckOnly(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const uint32_t used = WriteAck(peer, endpoint->packetBuffer + NET_SHORT_HEADER_SIZE,
                                   peer->datagramBytes - NET_SHORT_OVERHEAD, endpoint->now);
    AckSent(peer);
    if (used != 0u)
        SealAndSend(endpoint, peer, used);
}

/* Есть ли место под запись нового пакета без вытеснения неразрешённых.
 * Очередь растёт до предела, а на пределе новые данные ждут подтверждений:
 * предел ограничивает и число пакетов в пути. Старейшая запись о потере
 * нужна лишь для распознавания мнимых потерь и место уступает. */
static bool RecordsRoom(NetPeer *peer, uint64_t now)
{
    RecordsTrim(peer, now);
    return peer->recordCount < peer->recordCapacity ||
           (peer->recordCapacity < NET_RECORDS_LIMIT && RecordsGrow(peer)) ||
           RecordAt(peer, 0u)->state != NET_RECORD_SENT;
}

/* Освобождает место под запись следующего пакета. Неразрешённую запись
 * вытесняет только проба: путь молчит, и старейший пакет объявляется
 * потерянным — его данные уйдут заново. */
static void RecordsMakeRoom(NetPeer *peer, uint64_t now)
{
    RecordsTrim(peer, now);
    if (peer->recordCount < peer->recordCapacity ||
        (peer->recordCapacity < NET_RECORDS_LIMIT && RecordsGrow(peer)))
        return;
    NetSentPacket *front = RecordAt(peer, 0u);
    if (front->state == NET_RECORD_SENT)
        PacketLost(peer, front, now);
    RecordsPop(peer);
}

/* Собирает и отправляет один пакет. false — отправлять нечего или нельзя. */
static bool SendPacket(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const uint64_t now = endpoint->now;
    uint8_t *payload = endpoint->packetBuffer + NET_SHORT_HEADER_SIZE;
    const uint32_t space = peer->datagramBytes - NET_SHORT_OVERHEAD;
    const bool probe = peer->probesPending != 0u;
    const bool hasData =
        peer->pingPending || NetPeerQueuedUnreliable(peer) != 0u || NetPeerHasStreamData(peer);
    /* Пробы идут мимо окна и размеренной отправки: они и выясняют, жив ли
     * путь. */
    const bool sendData = probe || (hasData && peer->bytesInFlight < peer->congestionWindow &&
                                    peer->paceBudget > 0 && RecordsRoom(peer, now));
    /* Одинокий пакет ждёт подтверждения до следующего обновления: за это
     * время ответ может уехать вместе с данными. */
    const bool ackDue =
        peer->ackPendingCount != 0u &&
        (peer->ackImmediate || peer->ackPendingCount >= 2u || peer->ackPendingSince < now);
    if (!sendData && !ackDue)
        return false;

    uint32_t used = 0u;
    if (peer->ackPendingCount != 0u)
    {
        used = WriteAck(peer, payload, space, now);
        AckSent(peer);
    }
    NetSentRange ranges[NET_RECORD_RANGES] = {0};
    uint32_t rangeCount = 0u;
    bool eliciting = false;
    if (sendData)
    {
        RecordsMakeRoom(peer, now);
        if (peer->pingPending)
        {
            payload[used] = NET_FRAME_PING;
            ++used;
            eliciting = true;
            peer->pingPending = false;
        }
        for (;;)
        {
            const uint32_t written = NetPeerWriteUnreliable(peer, payload + used, space - used);
            if (written == 0u)
                break;
            used += written;
            eliciting = true;
        }
        /* Каналы обслуживаются по кругу: большой поток не заслоняет малые. */
        for (uint32_t turn = 0u; turn < peer->channelCount; ++turn)
        {
            const uint32_t channel = (peer->roundRobin + turn) % peer->channelCount;
            while (rangeCount < NET_RECORD_RANGES)
            {
                const uint32_t written = NetChannelWriteStream(peer, channel, payload + used,
                                                               space - used, &ranges[rangeCount]);
                if (written == 0u)
                    break;
                used += written;
                ++rangeCount;
            }
        }
        if (rangeCount != 0u)
        {
            eliciting = true;
            peer->roundRobin = (peer->roundRobin + 1u) % peer->channelCount;
        }
        if (probe && !eliciting)
        {
            payload[used] = NET_FRAME_PING;
            ++used;
            eliciting = true;
        }
    }
    if (used == 0u)
        return false;

    if (eliciting)
    {
        const uint32_t total = NET_SHORT_OVERHEAD + used;
        NetSentPacket *record = RecordAt(peer, peer->recordCount);
        ++peer->recordCount;
        record->number = peer->nextNumber;
        record->sentTime = now;
        record->size = (uint16_t)total;
        record->lossEpisode = 0u;
        record->state = NET_RECORD_SENT;
        record->rangeCount = (uint8_t)rangeCount;
        record->windowLimited = (peer->bytesInFlight + total) * 2u >= peer->congestionWindow;
        memcpy(record->ranges, ranges, (size_t)rangeCount * sizeof(ranges[0]));
        peer->bytesInFlight += total;
        ++peer->ackElicitingOutstanding;
        peer->lastAckElicitingSent = now;
        peer->paceBudget -= (int64_t)total;
        if (probe)
            --peer->probesPending;
    }
    SealAndSend(endpoint, peer, used);
    return true;
}

/* ---- Приём ---------------------------------------------------------------------------- */

static void Migrate(NetPeer *peer, const LaiueNetAddress *from)
{
    /* Собеседник сменил адрес: сеть, NAT. Пакет подлинный, поэтому ответы
     * идут на новый адрес, но о новом пути ничего не известно — окно и
     * оценки начинаются заново. Так подложный переезд не превращает
     * соединение в усилитель: в чужой адрес уйдёт не больше двух пакетов и
     * редких проб. */
    peer->address = *from;
    peer->haveRtt = false;
    peer->inRecovery = false;
    peer->recoveryUndoable = false;
    peer->slowStartThreshold = UINT64_MAX;
    peer->packetThreshold = NET_PACKET_THRESHOLD;
    peer->timeThreshold = NET_TIME_THRESHOLD;
    if (peer->datagramBytes != 0u)
        peer->congestionWindow = MinimumWindow(peer);
}

void NetShortReceive(LaiueNetEndpoint *endpoint, const LaiueNetAddress *from, uint8_t *bytes,
                     uint32_t size)
{
    NetPeer *peer =
        size > NET_SHORT_OVERHEAD ? NetPeerFindByCid(endpoint, NetLoad64(bytes + 1)) : NULL;
    if (peer == NULL || peer->state == NET_STATE_CONNECTING)
    {
        ++endpoint->stats.datagramsDropped;
        return;
    }
    const uint64_t number = NumberDecode(peer, NetLoad32(bytes + 9));
    uint8_t *payload = bytes + NET_SHORT_HEADER_SIZE;
    const uint32_t payloadSize = size - NET_SHORT_OVERHEAD;
    uint8_t nonce[NET_NONCE_SIZE] = {0u};
    NetStore64(nonce + 4, number);
    if (!NumberAcceptable(peer, number))
    {
        ++endpoint->stats.datagramsDropped;
        return;
    }
    if (!NetAeadOpen(peer->receiveKey, nonce, bytes, NET_SHORT_HEADER_SIZE, payload, payloadSize,
                     payload + payloadSize, payload))
    {
        ++endpoint->stats.datagramsDropped;
        /* Каждая попытка подделки — шанс её успеха. После предела соединение
         * закрывается: подлинность его пакетов больше не гарантирована. */
        ++peer->forgeries;
        if (peer->forgeries >= NET_FORGERY_LIMIT && peer->state != NET_STATE_CLOSING)
            NetPeerClose(endpoint, peer, LAIUE_NET_REASON_AUTHENTICATION, 0u, NET_CLOSE_PROTOCOL);
        return;
    }

    const bool newest = !peer->anySeen || number > peer->largestSeen;
    const bool inOrder = peer->anySeen ? number == peer->largestSeen + 1u : number == 0u;
    NumberRecord(peer, number);
    ++peer->packetsReceived;
    peer->bytesReceived += size;
    peer->lastReceiveTime = endpoint->now;
    peer->confirmed = true;
    if (newest && !LaiueNetAddressEqual(&peer->address, from))
        Migrate(peer, from);

    if (peer->state == NET_STATE_CLOSING)
    {
        /* Собеседник тоже закрывается либо ещё не знает о закрытии: во втором
         * случае уведомление повторяется, но не чаще заданного. */
        if (payload[0] == NET_FRAME_CLOSE)
        {
            NetPeerRelease(endpoint, peer);
        }
        else if (endpoint->now >= peer->closeRepeatAt)
        {
            peer->closeRepeatAt = endpoint->now + NET_CLOSE_REPEAT_INTERVAL;
            NetPeerSendClose(endpoint, peer);
        }
        return;
    }
    if (peer->state == NET_STATE_HALF_OPEN)
    {
        /* Первый подлинный пакет: клиент владеет ключами, а значит, получил
         * ответ по своему адресу. Только теперь о нём узнаёт приложение. */
        if (!NetPeerEstablish(endpoint, peer) || !NetPeerAnnounce(endpoint, peer))
        {
            NetPeerClose(endpoint, peer, LAIUE_NET_REASON_RESOURCE, 0u, NET_CLOSE_RESOURCE);
            return;
        }
        NetPeerSetState(endpoint, peer, NET_STATE_CONNECTED);
        /* Ответ уходил один раз, значит, этот пакет — отклик именно на него.
         * Замер включает ожидание клиента и потому лишь завышает оценку. */
        if (!peer->helloResent)
            NetPeerSeedRtt(peer, endpoint->now - peer->helloSentAt, endpoint->now);
    }

    bool ackEliciting = false;
    const uint32_t status =
        NetPeerProcessFrames(endpoint, peer, payload, payloadSize, &ackEliciting);
    if (status == NET_FRAMES_VIOLATION)
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_PROTOCOL, 0u, NET_CLOSE_PROTOCOL);
    else if (status == NET_FRAMES_RESOURCE)
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_RESOURCE, 0u, NET_CLOSE_RESOURCE);
    if (status != NET_FRAMES_OK)
        return;

    if (!ackEliciting)
        return;
    WindowSet(peer->ackBits, number);
    if (!peer->anyEliciting || number > peer->largestEliciting)
    {
        peer->anyEliciting = true;
        peer->largestEliciting = number;
        peer->largestElicitingTime = endpoint->now;
    }
    if (peer->ackPendingCount == 0u)
        peer->ackPendingSince = endpoint->now;
    ++peer->ackPendingCount;
    if (!inOrder)
        peer->ackImmediate = true;
    /* Пачка длиннее окна номеров унесла бы подтверждения с собой. */
    if (peer->ackPendingCount >= NET_ACK_BURST)
        SendAckOnly(endpoint, peer);
}

/* ---- Таймеры -------------------------------------------------------------------------- */

static void ProbeTimeout(NetPeer *peer)
{
    ++peer->probeCount;
    /* Проба несёт самые старые неподтверждённые данные: если потерялись
     * именно они, собеседник получит их без лишнего круга. */
    for (uint32_t position = 0u; position < peer->recordCount; ++position)
    {
        const NetSentPacket *record = RecordAt(peer, position);
        if (record->state != NET_RECORD_SENT || record->rangeCount == 0u)
            continue;
        for (uint32_t index = 0u; index < record->rangeCount; ++index)
            NetChannelOnLost(peer, &record->ranges[index]);
        break;
    }
    /* Пока есть что везти, проб две: потеря одной не стоит целого удвоенного
     * ожидания. Пустому соединению хватает одного PING. */
    peer->probesPending =
        NetPeerHasStreamData(peer) || NetPeerQueuedUnreliable(peer) != 0u ? 2u : 1u;
}

static void PaceRefill(NetPeer *peer, uint64_t now)
{
    uint64_t elapsed = now - peer->paceTime;
    peer->paceTime = now;
    if (!peer->haveRtt)
    {
        /* Пока путь не измерен, отправку сдерживает одно окно перегрузки. */
        peer->paceBudget = (int64_t)peer->congestionWindow;
        return;
    }
    if (elapsed > 10u * NET_SECOND)
        elapsed = 10u * NET_SECOND;
    /* Темп — окно за время оборота с запасом в четверть. Накопить можно не
     * больше начальной пачки или того, что набежало за одно обновление. */
    const uint64_t rtt = peer->smoothedRtt > NET_MILLISECOND ? peer->smoothedRtt : NET_MILLISECOND;
    const int64_t earned = (int64_t)(peer->congestionWindow * 5u / 4u * elapsed / rtt);
    const int64_t burst = 10 * (int64_t)peer->datagramBytes;
    const int64_t ceiling = earned > burst ? earned : burst;
    peer->paceBudget += earned;
    if (peer->paceBudget > ceiling)
        peer->paceBudget = ceiling;
}

void NetPeerUpdate(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const uint64_t now = endpoint->now;
    if (peer->state == NET_STATE_CLOSING)
    {
        if (now >= peer->closeDeadline)
        {
            NetPeerRelease(endpoint, peer);
        }
        else if (peer->closeResends < NET_CLOSE_RESENDS && now >= peer->closeResendAt)
        {
            /* Собеседник, которому нечего сказать, не вызвал бы повтора своими
             * пакетами и узнал бы о закрытии только по тайм-ауту. */
            ++peer->closeResends;
            peer->closeResendAt = now + peer->closeInterval;
            NetPeerSendClose(endpoint, peer);
        }
        return;
    }
    /* Клиент, от которого сервер так и не получил ни пакета, ждёт не дольше
     * рукопожатия: полуоткрытое соединение сервер к тому времени забыл. */
    if (now - peer->lastReceiveTime >= peer->idleTimeout ||
        (!peer->confirmed && now >= peer->handshakeDeadline))
    {
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_TIMEOUT, 0u, NET_CLOSE_NONE);
        return;
    }
    NetPeerExpireUnreliable(peer, now);
    if (peer->lossTime != 0u && now >= peer->lossTime)
        DetectLost(peer, now);
    if (peer->ackElicitingOutstanding != 0u)
    {
        const uint64_t base = NetPeerProbeTimeout(peer);
        uint64_t timeout = base << (peer->probeCount < 16u ? peer->probeCount : 16u);
        /* Пока сервер молчит, пробы не редеют сильнее повторов запроса:
         * иначе несколько потерянных подряд первых пакетов отложили бы его
         * сторону установки на десятки секунд. */
        uint64_t limit = NET_PROBE_INTERVAL_LIMIT;
        if (!peer->confirmed)
            limit = base > NET_HELLO_INTERVAL_LIMIT ? base : NET_HELLO_INTERVAL_LIMIT;
        if (timeout > limit)
            timeout = limit;
        if (now - peer->lastAckElicitingSent >= timeout)
            ProbeTimeout(peer);
    }
    else if (now - peer->lastAckElicitingSent >= peer->idleTimeout / 3u)
    {
        /* Тишина в обе стороны: проверка связи не даёт соединению истечь. */
        peer->pingPending = true;
    }
    PaceRefill(peer, now);
    for (uint32_t sent = 0u; sent < NET_FLUSH_PACKETS; ++sent)
    {
        if (!SendPacket(endpoint, peer))
            break;
    }
    if (peer->state == NET_STATE_FLUSHING && peer->queuedReliable == 0u &&
        NetPeerQueuedUnreliable(peer) == 0u)
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_LOCAL, peer->closeCode,
                     NET_CLOSE_APPLICATION);
}
