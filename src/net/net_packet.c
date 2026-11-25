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
 * запускает обычное обнаружение. Окно и темп отправки ведёт один из двух
 * алгоритмов, по выбору конечной точки: CUBIC с HyStart++ (net_congestion.c)
 * либо BBR (net_bbr.c), которому каждый пакет везёт замер скорости доставки
 * (net_rate.c). Отправка размазана по времени, а не выстреливает пачкой.
 * Размер датаграмм ищут пробы (net_pmtu.c). */

uint32_t NetPeerMaxUnreliable(const NetPeer *peer)
{
    /* Заголовок пакета, тег и самый длинный заголовок кадра: вид, канал,
     * номер до десяти байт и длина в два. Предел считается от базового
     * размера: найденный поиском может упасть, а принятое к отправке
     * сообщение обязано поместиться в одну датаграмму. */
    return peer->pmtu.base - NET_SHORT_OVERHEAD - 14u;
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

/* Окно в начале соединения либо на новом пути. */
static void ControlReset(NetPeer *peer, uint64_t window, uint64_t now)
{
    if (peer->model == NULL)
    {
        NetCongestionReset(&peer->congestion, peer->datagramBytes, window, peer->nextNumber);
        return;
    }
    /* Идентификаторы соединения случайны и известны только сторонам: у
     * каждого соединения свои паузы между пробами полосы. */
    NetBbrReset(&peer->model->bbr, peer->datagramBytes, window,
                peer->localCid ^ (peer->remoteCid * UINT64_C(0x9E3779B97F4A7C15)), now);
    if (peer->haveRtt)
        NetBbrSeed(&peer->model->bbr, peer->smoothedRtt);
}

bool NetPeerEstablish(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const NetParameters *local = &endpoint->local;
    const NetParameters *remote = &peer->remote;
    /* Рукопожатие доказало базовый размер; больший, до меньшего из двух
     * пределов, найдёт поиск. */
    NetPmtuReset(&peer->pmtu, NET_DATAGRAM_BASE,
                 local->datagramBytes < remote->datagramBytes ? local->datagramBytes
                                                              : remote->datagramBytes,
                 endpoint->now);
    peer->datagramBytes = peer->pmtu.current;
    peer->channelCount =
        local->channelCount < remote->channelCount ? local->channelCount : remote->channelCount;
    peer->idleTimeout =
        (uint64_t)(local->idleMilliseconds < remote->idleMilliseconds ? local->idleMilliseconds
                                                                      : remote->idleMilliseconds) *
        NET_MILLISECOND;
    peer->channels = PlatformAllocate((size_t)peer->channelCount * sizeof(NetChannel), true);
    peer->records = PlatformAllocate((size_t)NET_RECORDS_INITIAL * sizeof(NetSentPacket), true);
    if (endpoint->modelBased)
    {
        peer->model = PlatformAllocate(sizeof(NetModel), true);
        if (peer->model != NULL)
            peer->model->stamps =
                PlatformAllocate((size_t)NET_RECORDS_INITIAL * sizeof(NetRateStamp), false);
    }
    if (peer->channels == NULL || peer->records == NULL ||
        (endpoint->modelBased && (peer->model == NULL || peer->model->stamps == NULL)))
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
    ControlReset(peer, window, endpoint->now);
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
    if (peer->model != NULL)
    {
        PlatformFree(peer->model->stamps);
        PlatformFree(peer->model);
        peer->model = NULL;
    }
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
 * пакета.
 *
 * Собеседнику, который просил о них при рукопожатии, — а просит BBR, —
 * кадр несёт и часы получателя: младшие биты времени, в которое принят
 * самый новый из названных пакетов. Обе стороны читают сеть раз в
 * обновление, и по своим часам отправитель знает приход подтверждения лишь с
 * точностью до шага; часы получателя говорят, за какое время принято
 * подтверждённое. Рядом их погрешность: за сколько до этих часов сеть была
 * прочитана до конца — раньше пакет прийти не мог. Младший бит погрешности
 * говорит, что с тех пор сеть до конца не прочитана и пакеты, которых в
 * кадре нет, могли прийти и раньше этих часов. */

static uint32_t WriteAck(const NetPeer *peer, uint8_t *output, uint32_t space, uint64_t now,
                         uint64_t drainedTime)
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
    const bool clocked = peer->remote.ackClock;
    uint64_t clock = 0u;
    uint64_t accuracy = 0u;
    if (clocked)
    {
        clock = peer->arrivalTime & NET_ACK_CLOCK_MASK;
        uint64_t age = peer->arrivalTime - peer->arrivalFloor;
        if (age > NET_ACK_CLOCK_MASK)
            age = NET_ACK_CLOCK_MASK;
        accuracy = age << 1u | (peer->arrivalTime > drainedTime ? 1u : 0u);
    }
    for (;;)
    {
        uint32_t size = 1u + NetVarintSize(largest) + NetVarintSize(delay) + NetVarintSize(count) +
                        NetVarintSize(first);
        if (clocked)
            size += NetVarintSize(clock) + NetVarintSize(accuracy);
        for (uint32_t index = 0u; index < count; ++index)
            size += NetVarintSize(gaps[index]) + NetVarintSize(lengths[index]);
        if (size <= space)
            break;
        if (count == 0u)
            return 0u;
        --count;
    }
    output[0] = clocked ? NET_FRAME_ACK_CLOCK : NET_FRAME_ACK;
    uint32_t used = 1u;
    used += NetVarintWrite(output + used, largest);
    used += NetVarintWrite(output + used, delay);
    if (clocked)
    {
        used += NetVarintWrite(output + used, clock);
        used += NetVarintWrite(output + used, accuracy);
    }
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

/* Снимок замера скорости для записи: то же место в своей очереди. */
static NetRateStamp *RecordStamp(const NetPeer *peer, const NetSentPacket *record)
{
    return &peer->model->stamps[record - peer->records];
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
    NetRateStamp *stamps = NULL;
    if (grown != NULL && peer->model != NULL)
        stamps = PlatformAllocate((size_t)capacity * sizeof(NetRateStamp), false);
    if (grown == NULL || (peer->model != NULL && stamps == NULL))
    {
        PlatformFree(grown);
        return false;
    }
    for (uint32_t position = 0u; position < peer->recordCount; ++position)
    {
        const uint32_t index = (peer->recordHead + position) & (peer->recordCapacity - 1u);
        grown[position] = peer->records[index];
        if (stamps != NULL)
            stamps[position] = peer->model->stamps[index];
    }
    PlatformFree(peer->records);
    peer->records = grown;
    if (stamps != NULL)
    {
        PlatformFree(peer->model->stamps);
        peer->model->stamps = stamps;
    }
    peer->recordCapacity = capacity;
    peer->recordHead = 0u;
    return true;
}

/* ---- Перегрузка и оценка пути ------------------------------------------------------ */

/* Поиск изменил размер датаграммы пути. */
static void DatagramResize(NetPeer *peer)
{
    peer->datagramBytes = peer->pmtu.current;
    if (peer->model != NULL)
        NetBbrSetDatagram(&peer->model->bbr, peer->datagramBytes);
    else
        NetCongestionSetDatagram(&peer->congestion, peer->datagramBytes);
}

static void CongestionOnLoss(NetPeer *peer, const NetSentPacket *record, bool contiguous,
                             uint64_t now)
{
    bool episode = false;
    if (peer->model != NULL)
    {
        const NetRateStamp *stamp = RecordStamp(peer, record);
        const NetBbrLoss loss = {
            .now = now,
            .sentTime = record->sentTime,
            .txInFlight = stamp->inFlight,
            .lost = peer->model->rate.lost - stamp->lost,
            .size = record->size,
            .appLimited = stamp->appLimited,
            .contiguous = contiguous,
        };
        episode = NetBbrOnLoss(&peer->model->bbr, &loss);
    }
    else
    {
        episode = NetCongestionOnLoss(&peer->congestion, record->sentTime, now);
    }
    /* Один эпизод на полёт: потери пакетов, отправленных до его начала,
     * нового не начинают. */
    if (!episode)
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
    if (peer->haveRtt)
        return;
    UpdateRtt(peer, sample, 0u, now);
    if (peer->model != NULL)
        NetBbrSeed(&peer->model->bbr, peer->smoothedRtt);
}

/* ---- Подтверждения и потери ---------------------------------------------------------- */

typedef struct AckContext
{
    uint64_t now;
    uint64_t largest;
    /* Позиция записи наибольшего подтверждённого номера. */
    uint32_t largestPosition;
    uint64_t largestSentTime;
    /* Байты, которые расширяют окно CUBIC: пакеты, которым оно было тесно,
     * вне восстановления. */
    uint64_t limitedBytes;
    uint64_t limitedSentTime;
    /* Замер скорости доставки этим подтверждением. */
    NetRateSample rate;
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
    if (peer->model != NULL)
        NetRateAcked(&peer->model->rate, &context->rate, RecordStamp(peer, record), record->number,
                     record->sentTime, record->size, context->now);
    if (record->sizeProbe)
    {
        if (NetPmtuProbeAcked(&peer->pmtu, record->number, record->size, record->sentTime,
                              context->now))
            DatagramResize(peer);
    }
    else
    {
        NetPmtuAcked(&peer->pmtu, record->number, record->size);
    }
    if (peer->model == NULL && record->windowLimited &&
        !NetCongestionInRecovery(&peer->congestion, record->sentTime))
    {
        context->limitedBytes += record->size;
        if (record->sentTime > context->limitedSentTime)
            context->limitedSentTime = record->sentTime;
    }
    for (uint32_t index = 0u; index < record->rangeCount; ++index)
        NetChannelOnAcked(peer, &record->ranges[index]);
    if (record->number == context->largest)
    {
        context->largestNewlyAcked = true;
        context->largestSentTime = record->sentTime;
    }
    ++context->newlyAcked;
}

/* contiguous — пакет, отправленный прямо перед этим, тоже потерян. */
static void PacketLost(NetPeer *peer, NetSentPacket *record, bool contiguous, uint64_t now)
{
    record->state = NET_RECORD_LOST;
    --peer->ackElicitingOutstanding;
    peer->bytesInFlight -= record->size;
    if (record->sizeProbe)
    {
        /* Проба теряется оттого, что велика для пути: перегрузки тут нет. */
        NetPmtuProbeLost(&peer->pmtu, record->size, record->sentTime, now);
        return;
    }
    ++peer->packetsLost;
    if (peer->model != NULL)
        peer->model->rate.lost += record->size;
    CongestionOnLoss(peer, record, contiguous, now);
    record->lossEpisode = (uint16_t)peer->recoveryEpisode;
    ++peer->recoveryLosses;
    for (uint32_t index = 0u; index < record->rangeCount; ++index)
        NetChannelOnLost(peer, &record->ranges[index]);
    if (NetPmtuLost(&peer->pmtu, record->number, record->size, now))
        DatagramResize(peer);
}

/* Пакет, уже объявленный потерянным, всё-таки подтверждён: потеря мнимая,
 * путь переставляет пакеты сильнее, чем допускали пороги. Пороги растут до
 * наблюдённого, ещё не отправленный повтор его данных отменяется, а
 * сокращение окна, вызванное одними мнимыми потерями, откатывается. */
static void SpuriousLoss(NetPeer *peer, NetSentPacket *record, uint32_t distance,
                         AckContext *context)
{
    record->state = NET_RECORD_ACKED;
    /* Пакет всё же доставлен: в счёт доставленного он идёт, как любой. */
    if (peer->model != NULL)
        NetRateAcked(&peer->model->rate, &context->rate, RecordStamp(peer, record), record->number,
                     record->sentTime, record->size, context->now);
    if (record->sizeProbe)
    {
        /* Запоздавшая проба всё же доказывает свой размер. Окна и порогов
         * её потеря не трогала, и откатывать нечего. */
        if (NetPmtuProbeAcked(&peer->pmtu, record->number, record->size, record->sentTime,
                              context->now))
            DatagramResize(peer);
        if (record->number == context->largest)
        {
            context->largestNewlyAcked = true;
            context->largestSentTime = record->sentTime;
        }
        ++context->newlyAcked;
        return;
    }
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
            if (peer->model != NULL)
                NetBbrUndo(&peer->model->bbr);
            else
                NetCongestionUndo(&peer->congestion);
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
                const bool contiguous =
                    position != 0u && RecordAt(peer, position - 1u)->state == NET_RECORD_LOST;
                PacketLost(peer, record, contiguous, now);
            }
            else if (peer->lossTime == 0u || record->sentTime + lossDelay < peer->lossTime)
            {
                peer->lossTime = record->sentTime + lossDelay;
            }
        }
        /* Потерянная проба размера о перегрузке не говорит: серию потерь она
         * не продолжает и не прерывает. Дошедшая прерывает, как любой пакет. */
        if (record->sizeProbe && record->state == NET_RECORD_LOST)
            continue;
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
        if (peer->model != NULL)
            NetBbrCollapse(&peer->model->bbr, now);
        else
            NetCongestionCollapse(&peer->congestion, now);
        peer->recoveryUndoable = false;
    }
    RecordsTrim(peer, now);
}

static void CongestionOnAck(NetPeer *peer, const AckContext *context)
{
    const uint64_t rttSample = context->largestNewlyAcked ? peer->latestRtt : 0u;
    if (peer->model == NULL)
    {
        const NetCongestionAck ack = {
            .now = context->now,
            .largest = context->largest,
            .nextNumber = peer->nextNumber,
            .rttSample = rttSample,
            .smoothedRtt = peer->haveRtt ? peer->smoothedRtt : NET_INITIAL_RTT,
            .limitedBytes = context->limitedBytes,
            .limitedSentTime = context->limitedSentTime,
        };
        NetCongestionOnAck(&peer->congestion, &ack);
        return;
    }
    const NetRateSample *sample = &context->rate;
    const NetBbrAck ack = {
        .now = context->now,
        .delivered = peer->model->rate.delivered,
        .inFlight = peer->bytesInFlight,
        .priorDelivered = sample->stamp.delivered,
        .sampleDelivered = sample->flight,
        .lost = sample->lost,
        .rate = sample->rate,
        .proven = sample->proven,
        .txInFlight = sample->stamp.inFlight,
        .acked = sample->acked,
        .sentTime = sample->sentTime,
        .rttSample = rttSample,
        .smoothedRtt = peer->haveRtt ? peer->smoothedRtt : 0u,
        .appLimited = sample->stamp.appLimited,
    };
    NetBbrOnAck(&peer->model->bbr, &ack);
    /* Пока BBR держит окно малым ради замера оборота, скорость доставки
     * говорит о нём самом, а не о пути. */
    if (NetBbrProbesRtt(&peer->model->bbr))
        NetRateLimited(&peer->model->rate, peer->bytesInFlight);
}

/* Расхождение хода часов сторон между кадрами ACK, которого не объяснить
 * разницей задержек: часы одной из сторон стояли, пока шли другие. Меньше
 * половины диапазона часов в кадре. */
#define ACK_CLOCK_SLACK (60u * NET_SECOND)

/* Часы получателя из кадра ACK, приведённые к часам этой стороны. Кадр несёт
 * младшие биты, а сколько часы получателя прошли с прошлого кадра,
 * восстанавливается по тому, сколько прошли свои: ходы расходятся лишь на
 * разницу задержек, и тишина любой длины ничего не путает. Кадр, обогнанный
 * более поздним, времени не двигает. Ходы, разошедшиеся больше
 * ACK_CLOCK_SLACK, говорят о стоявших часах, и тогда верен ход своих. */
static uint64_t AckClock(NetPeer *peer, uint64_t clock, uint64_t now)
{
    if (!peer->model->ackClockKnown)
    {
        /* Первое подтверждение привязывает часы получателя к своим. */
        peer->model->ackClockKnown = true;
        peer->model->ackClock = clock;
        peer->model->ackClockTime = now;
        peer->model->ackClockSeen = now;
        return now;
    }
    const uint64_t elapsed = now - peer->model->ackClockSeen;
    /* На сколько часы получателя ушли вперёд своих: ближайшее к нулю число с
     * теми же младшими битами. */
    const uint64_t ahead = (clock - peer->model->ackClock - elapsed) & NET_ACK_CLOCK_MASK;
    const uint64_t behind = NET_ACK_CLOCK_MASK + 1u - ahead;
    uint64_t advance = elapsed;
    if (ahead <= ACK_CLOCK_SLACK)
    {
        advance = elapsed + ahead;
    }
    else if (behind <= ACK_CLOCK_SLACK)
    {
        if (behind >= elapsed)
            return peer->model->ackClockTime;
        advance = elapsed - behind;
    }
    peer->model->ackClock = clock;
    peer->model->ackClockTime += advance;
    peer->model->ackClockSeen = now;
    return peer->model->ackClockTime;
}

/* clocked — кадр ACK_CLOCK, с часами получателя. */
static uint32_t ProcessAck(const LaiueNetEndpoint *endpoint, NetPeer *peer, bool clocked,
                           const uint8_t **cursor, const uint8_t *end)
{
    uint64_t largest = 0u;
    uint64_t delay = 0u;
    uint64_t clock = 0u;
    uint64_t accuracy = 0u;
    uint64_t count = 0u;
    uint64_t first = 0u;
    if (!NetVarintRead(cursor, end, &largest) || !NetVarintRead(cursor, end, &delay) ||
        (clocked &&
         (!NetVarintRead(cursor, end, &clock) || !NetVarintRead(cursor, end, &accuracy))) ||
        !NetVarintRead(cursor, end, &count) || !NetVarintRead(cursor, end, &first))
        return NET_FRAMES_VIOLATION;
    /* Подтверждать можно только отправленное. */
    if (largest >= peer->nextNumber || first > largest || count > 255u ||
        clock > NET_ACK_CLOCK_MASK || accuracy >> 1u > NET_ACK_CLOCK_MASK)
        return NET_FRAMES_VIOLATION;
    /* Замер скорости доставки ведётся только для BBR, и для CUBIC его поля не
     * заполняются вовсе. */
    AckContext context;
    context.now = endpoint->now;
    context.largest = largest;
    context.largestPosition = RecordFind(peer, largest);
    context.largestSentTime = 0u;
    context.limitedBytes = 0u;
    context.limitedSentTime = 0u;
    context.largestNewlyAcked = false;
    context.newlyAcked = 0u;
    if (peer->model != NULL)
    {
        memset(&context.rate, 0, sizeof(context.rate));
        /* Без часов получателя подтверждение датируется своими. */
        context.rate.ackTime = clocked ? AckClock(peer, clock, endpoint->now) : endpoint->now;
        context.rate.ackAge = accuracy >> 1u;
        context.rate.ackOpen = (accuracy & 1u) != 0u;
    }
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
    /* Замер считается после потерь: в него входит и потерянное за полёт
     * пакета. */
    if (peer->model != NULL)
        NetRateFinish(&peer->model->rate, &context.rate, peer->haveRtt ? peer->minRtt : 0u);
    CongestionOnAck(peer, &context);
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
        case NET_FRAME_ACK_CLOCK:
            /* Честному отправителю хватает одного кадра на пакет, а разбор
             * каждого обходит записи отправленного: вторым кадром собеседник
             * лишь заставлял бы делать эту работу повторно. */
            status = acked ? NET_FRAMES_VIOLATION
                           : ProcessAck(endpoint, peer, type == NET_FRAME_ACK_CLOCK, &cursor, end);
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
    const uint32_t used =
        WriteAck(peer, endpoint->packetBuffer + NET_SHORT_HEADER_SIZE,
                 peer->datagramBytes - NET_SHORT_OVERHEAD, endpoint->now, endpoint->drainedTime);
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
        PacketLost(peer, front, false, now);
    RecordsPop(peer);
}

/* Когда пакет ушёл бы при ровном темпе. Заработанное за время между
 * обновлениями уходит разом в его конце, а ровный темп растянул бы пачку на
 * всё это время. Отправленное сверх заработанного — из запаса, настоящей
 * пачкой. */
static uint64_t PacedTime(NetPeer *peer, uint32_t total, uint64_t now)
{
    uint64_t paced = now;
    peer->model->paceSpent += total;
    if (peer->model->paceSpent < peer->model->paceEarned)
        paced = now - peer->model->paceElapsed +
                NetScale(peer->model->paceSpent, peer->model->paceElapsed, peer->model->paceEarned);
    if (paced < peer->model->paceClock)
        paced = peer->model->paceClock;
    peer->model->paceClock = paced;
    return paced;
}

/* Запись пакета, требующего подтверждения, перед его отправкой. */
static NetSentPacket *RecordSent(NetPeer *peer, uint32_t total, uint64_t now)
{
    NetSentPacket *record = RecordAt(peer, peer->recordCount);
    ++peer->recordCount;
    record->number = peer->nextNumber;
    record->sentTime = now;
    record->size = (uint16_t)total;
    record->lossEpisode = 0u;
    record->state = NET_RECORD_SENT;
    record->rangeCount = 0u;
    /* Окно тесно, если пакет занимает его хотя бы наполовину. */
    record->windowLimited = (peer->bytesInFlight + total) * 2u >= NetPeerWindow(peer);
    record->sizeProbe = false;
    const uint64_t before = peer->bytesInFlight;
    peer->bytesInFlight += total;
    /* Скорость доставки нужна только модели пути BBR. */
    if (peer->model != NULL)
    {
        NetBbrOnSend(&peer->model->bbr, before, total, peer->model->rate.appLimited != 0u, now);
        NetRateSent(&peer->model->rate, RecordStamp(peer, record), peer->bytesInFlight, total,
                    PacedTime(peer, total, now), now);
    }
    ++peer->ackElicitingOutstanding;
    peer->lastAckElicitingSent = now;
    peer->paceBudget -= (int64_t)total;
    return record;
}

/* Проба размера датаграммы: PING и заполнитель до нужной длины. Уходит, когда
 * есть что везти и окно позволяет: больший размер нужен потоку данных, а
 * проба занимает окно наравне с ними. */
static void SendSizeProbe(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    const uint64_t now = endpoint->now;
    const uint32_t size = NetPmtuProbeSize(&peer->pmtu, now);
    if (size == 0u || !peer->confirmed ||
        (!NetPeerHasStreamData(peer) && NetPeerQueuedUnreliable(peer) == 0u) ||
        peer->bytesInFlight >= NetPeerWindow(peer) || peer->paceBudget <= 0 ||
        !RecordsRoom(peer, now))
        return;
    RecordsMakeRoom(peer, now);
    uint8_t *payload = endpoint->packetBuffer + NET_SHORT_HEADER_SIZE;
    const uint32_t payloadSize = size - NET_SHORT_OVERHEAD;
    payload[0] = NET_FRAME_PING;
    memset(payload + 1, NET_FRAME_PADDING, payloadSize - 1u);
    NetSentPacket *record = RecordSent(peer, size, now);
    /* Проба окно не расширяет: её подтверждение говорит о размере пути. */
    record->windowLimited = false;
    record->sizeProbe = true;
    NetPmtuProbeSent(&peer->pmtu, size);
    SealAndSend(endpoint, peer, payloadSize);
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
    const bool sendData = probe || (hasData && peer->bytesInFlight < NetPeerWindow(peer) &&
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
        used = WriteAck(peer, payload, space, now, endpoint->drainedTime);
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
        NetSentPacket *record = RecordSent(peer, NET_SHORT_OVERHEAD + used, now);
        record->rangeCount = (uint8_t)rangeCount;
        memcpy(record->ranges, ranges, (size_t)rangeCount * sizeof(ranges[0]));
        if (probe)
            --peer->probesPending;
    }
    SealAndSend(endpoint, peer, used);
    return true;
}

/* ---- Приём ---------------------------------------------------------------------------- */

static void Migrate(NetPeer *peer, const LaiueNetAddress *from, uint64_t now)
{
    /* Собеседник сменил адрес: сеть, NAT. Пакет подлинный, поэтому ответы
     * идут на новый адрес, но о новом пути ничего не известно — окно, размер
     * датаграммы и оценки начинаются заново. Так подложный переезд не
     * превращает соединение в усилитель: в чужой адрес уйдёт не больше двух
     * пакетов и редких проб. */
    peer->address = *from;
    peer->haveRtt = false;
    peer->recoveryUndoable = false;
    peer->packetThreshold = NET_PACKET_THRESHOLD;
    peer->timeThreshold = NET_TIME_THRESHOLD;
    /* До установки окна ещё нет: его заведёт первый пакет. */
    if (peer->datagramBytes != 0u)
    {
        NetPmtuReset(&peer->pmtu, peer->pmtu.base, peer->pmtu.limit, now);
        peer->datagramBytes = peer->pmtu.current;
        ControlReset(peer, MinimumWindow(peer), now);
    }
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
        Migrate(peer, from, endpoint->now);

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
    if (peer->remote.ackClock)
    {
        peer->arrivalTime = endpoint->now;
        peer->arrivalFloor = endpoint->drainedTime;
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

/* Самый большой пакет в пути, кроме проб размера. */
static uint32_t LargestInFlight(const NetPeer *peer)
{
    uint32_t largest = 0u;
    for (uint32_t position = 0u; position < peer->recordCount; ++position)
    {
        const NetSentPacket *record = RecordAt(peer, position);
        if (record->state == NET_RECORD_SENT && !record->sizeProbe && record->size > largest)
            largest = record->size;
    }
    return largest;
}

static void ProbeTimeout(NetPeer *peer, uint64_t now)
{
    ++peer->probeCount;
    /* Второй таймер подряд без единого подтверждения: путь мог перестать
     * пропускать большие датаграммы, и пробы, несущие данные, пропадали бы
     * вместе с ними. */
    if (NetPmtuTimeout(&peer->pmtu, peer->probeCount, LargestInFlight(peer), now))
        DatagramResize(peer);
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
    const uint64_t window = NetPeerWindow(peer);
    if (peer->model != NULL)
    {
        /* Ровный темп этого обновления ещё ничего не заработал. */
        peer->model->paceSpent = 0u;
        peer->model->paceEarned = 0u;
    }
    if (!peer->haveRtt || (peer->model != NULL && peer->model->bbr.pacingRate == 0u))
    {
        /* Пока путь не измерен, отправку сдерживает одно окно перегрузки. */
        peer->paceBudget = (int64_t)window;
        return;
    }
    if (elapsed > 10u * NET_SECOND)
        elapsed = 10u * NET_SECOND;
    /* Темп BBR задаёт его модель пути. У CUBIC это окно за время оборота с
     * запасом в четверть. */
    uint64_t earned = 0u;
    if (peer->model != NULL)
    {
        const uint64_t rate = peer->model->bbr.pacingRate < NET_PACING_RATE_LIMIT
                                  ? peer->model->bbr.pacingRate
                                  : NET_PACING_RATE_LIMIT;
        const uint64_t total = rate * elapsed + peer->model->paceRemainder;
        earned = total / NET_SECOND;
        peer->model->paceRemainder = total % NET_SECOND;
        if (earned > NET_CONGESTION_WINDOW_LIMIT)
            earned = NET_CONGESTION_WINDOW_LIMIT;
        peer->model->paceEarned = earned;
        peer->model->paceElapsed = elapsed;
    }
    else
    {
        const uint64_t rtt =
            peer->smoothedRtt > NET_MILLISECOND ? peer->smoothedRtt : NET_MILLISECOND;
        earned = window * 5u / 4u * elapsed / rtt;
    }
    /* Накопить можно не больше пачки или того, что набежало за одно
     * обновление. Пачка CUBIC — начальное окно; BBR после простоя
     * возвращается ровно в темпе пути, и его пачка — миллисекунда отправки. */
    const int64_t burst = peer->model != NULL ? (int64_t)NetBbrQuantum(&peer->model->bbr)
                                              : 10 * (int64_t)peer->datagramBytes;
    const int64_t ceiling = (int64_t)earned > burst ? (int64_t)earned : burst;
    peer->paceBudget += (int64_t)earned;
    if (peer->paceBudget > ceiling)
        peer->paceBudget = ceiling;
}

/* Отправка этого обновления кончилась: почему — важно замеру скорости. Если
 * слать больше нечего, хотя окно и темп ещё позволяли, пакеты ближайшего
 * оборота мерят приложение, а не путь. Данные, которым не даёт уйти окно
 * приёма собеседника, считаются неотправленными: узкое место тогда он. */
static void SendFinished(NetPeer *peer)
{
    const bool pending = NetPeerQueuedUnreliable(peer) != 0u || NetPeerStreamPending(peer);
    if (peer->bytesInFlight < NetPeerWindow(peer) && peer->paceBudget > 0)
    {
        if (!pending)
            NetRateLimited(&peer->model->rate, peer->bytesInFlight);
    }
    else if (pending && peer->model != NULL &&
             peer->bytesInFlight + peer->datagramBytes > NetPeerWindow(peer))
    {
        NetBbrWindowFilled(&peer->model->bbr);
    }
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
            ProbeTimeout(peer, now);
    }
    else if (now - peer->lastAckElicitingSent >= peer->idleTimeout / 3u)
    {
        /* Тишина в обе стороны: проверка связи не даёт соединению истечь. */
        peer->pingPending = true;
    }
    PaceRefill(peer, now);
    SendSizeProbe(endpoint, peer);
    for (uint32_t sent = 0u; sent < NET_FLUSH_PACKETS; ++sent)
    {
        if (!SendPacket(endpoint, peer))
            break;
    }
    if (peer->model != NULL)
        SendFinished(peer);
    if (peer->state == NET_STATE_FLUSHING && peer->queuedReliable == 0u &&
        NetPeerQueuedUnreliable(peer) == 0u)
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_LOCAL, peer->closeCode,
                     NET_CLOSE_APPLICATION);
}
