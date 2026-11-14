#include "net/net_internal.h"

#include "platform/system.h"

#include <string.h>

/* Каналы соединения.
 *
 * Надёжный канал — поток байт, в котором сообщения идут подряд, каждое с
 * длиной впереди. Отправитель держит неподтверждённый хвост потока в кольце
 * и помнит, какие отрезки подтверждены вразбивку и какие потеряны.
 * Получатель разбирает байты на сообщения, как только они становятся
 * сплошными; пришедшее с опережением ждёт в кольце размером с окно приёма.
 *
 * Ненадёжные сообщения стоят в общей очереди соединения и уходят каждое
 * одним кадром; переполненная очередь теряет самые старые. */

/* ---- Наборы отрезков -----------------------------------------------------------
 *
 * Отсортированные непересекающиеся полуинтервалы [start, end); соседние
 * вплотную сливаются. Ёмкость постоянна, поэтому память канала ограничена
 * при любом поведении собеседника. */

static void RangesRemove(NetRange *ranges, uint32_t *count, uint32_t first, uint32_t removed)
{
    memmove(&ranges[first], &ranges[first + removed],
            (size_t)(*count - first - removed) * sizeof(*ranges));
    *count -= removed;
}

/* false — набор полон и отрезок не поместился; набор при этом не изменён. */
static bool RangesAdd(NetRange *ranges, uint32_t *count, uint32_t capacity, uint64_t start,
                      uint64_t end)
{
    if (start >= end)
        return true;
    uint32_t first = 0u;
    while (first < *count && ranges[first].end < start)
        ++first;
    uint32_t last = first;
    while (last < *count && ranges[last].start <= end)
    {
        if (ranges[last].start < start)
            start = ranges[last].start;
        if (ranges[last].end > end)
            end = ranges[last].end;
        ++last;
    }
    if (first == last)
    {
        if (*count == capacity)
            return false;
        memmove(&ranges[first + 1u], &ranges[first], (size_t)(*count - first) * sizeof(*ranges));
        ++*count;
    }
    else if (last - first > 1u)
    {
        RangesRemove(ranges, count, first + 1u, last - first - 1u);
    }
    ranges[first].start = start;
    ranges[first].end = end;
    return true;
}

/* Как RangesAdd, но полный набор огрубляется: отрезок сливается с ближайшим
 * соседом вместе с промежутком между ними. Набор покрывает лишнее, зато
 * ничего не теряет. */
static void RangesAddCoarse(NetRange *ranges, uint32_t *count, uint32_t capacity, uint64_t start,
                            uint64_t end)
{
    if (RangesAdd(ranges, count, capacity, start, end) || *count == 0u)
        return;
    uint32_t next = 0u;
    while (next < *count && ranges[next].end < start)
        ++next;
    /* Отказ значит, что набор полон, а отрезок никого не задел: слева от него
     * next - 1, справа next. */
    uint32_t chosen = next;
    if (next == *count || (next != 0u && start - ranges[next - 1u].end < ranges[next].start - end))
        chosen = next - 1u;
    if (ranges[chosen].start < start)
        start = ranges[chosen].start;
    if (ranges[chosen].end > end)
        end = ranges[chosen].end;
    (void)RangesAdd(ranges, count, capacity, start, end);
}

static void RangesTrimBelow(NetRange *ranges, uint32_t *count, uint64_t offset)
{
    uint32_t removed = 0u;
    while (removed < *count && ranges[removed].end <= offset)
        ++removed;
    if (removed != 0u)
        RangesRemove(ranges, count, 0u, removed);
    if (*count != 0u && ranges[0].start < offset)
        ranges[0].start = offset;
}

/* ---- Кольцо -------------------------------------------------------------------- */

/* Байт потока со смещением offset лежит в ячейке offset mod capacity;
 * capacity — степень двойки. */
static void RingWrite(uint8_t *ring, uint32_t capacity, uint64_t offset, const uint8_t *bytes,
                      uint32_t size)
{
    uint32_t done = 0u;
    while (done < size)
    {
        const uint32_t at = (uint32_t)((offset + done) & (capacity - 1u));
        uint32_t run = size - done;
        if (run > capacity - at)
            run = capacity - at;
        memcpy(ring + at, bytes + done, run);
        done += run;
    }
}

static void RingRead(const uint8_t *ring, uint32_t capacity, uint64_t offset, uint8_t *output,
                     uint32_t size)
{
    uint32_t done = 0u;
    while (done < size)
    {
        const uint32_t at = (uint32_t)((offset + done) & (capacity - 1u));
        uint32_t run = size - done;
        if (run > capacity - at)
            run = capacity - at;
        memcpy(output + done, ring + at, run);
        done += run;
    }
}

/* ---- Ненадёжные сообщения -------------------------------------------------------
 *
 * Запись очереди: u32 размер | u8 канал | u8 с номером | u16 ноль |
 * u64 номер | u64 срок | байты. Записи идут в порядке постановки, поэтому
 * сроки не убывают и устаревшие всегда стоят в голове. */

#define QUEUE_HEADER_SIZE 24u

uint32_t NetPeerQueuedUnreliable(const NetPeer *peer)
{
    return peer->unreliableTail - peer->unreliableHead;
}

static void UnreliablePop(NetPeer *peer)
{
    peer->unreliableHead += QUEUE_HEADER_SIZE + NetLoad32(peer->unreliable + peer->unreliableHead);
    if (peer->unreliableHead == peer->unreliableTail)
    {
        peer->unreliableHead = 0u;
        peer->unreliableTail = 0u;
    }
}

static void UnreliableDrop(NetPeer *peer)
{
    UnreliablePop(peer);
    ++peer->unreliableDropped;
}

void NetPeerExpireUnreliable(NetPeer *peer, uint64_t now)
{
    while (peer->unreliableHead != peer->unreliableTail &&
           NetLoad64(peer->unreliable + peer->unreliableHead + 16u) < now)
        UnreliableDrop(peer);
}

static bool UnreliableReserve(NetPeer *peer, uint32_t need)
{
    if (peer->unreliableTail + need <= peer->unreliableCapacity)
        return true;
    const uint32_t live = peer->unreliableTail - peer->unreliableHead;
    if (peer->unreliableHead != 0u)
    {
        memmove(peer->unreliable, peer->unreliable + peer->unreliableHead, live);
        peer->unreliableHead = 0u;
        peer->unreliableTail = live;
    }
    if (live + need <= peer->unreliableCapacity)
        return true;
    uint32_t capacity = peer->unreliableCapacity != 0u ? peer->unreliableCapacity : 2048u;
    while (capacity < live + need)
        capacity *= 2u;
    uint8_t *grown = PlatformReallocate(peer->unreliable, capacity, false);
    if (grown == NULL)
        return false;
    peer->unreliable = grown;
    peer->unreliableCapacity = capacity;
    return true;
}

LaiueNetStatus NetPeerQueueUnreliable(const LaiueNetEndpoint *endpoint, NetPeer *peer,
                                      uint32_t channelIndex, bool sequenced, const void *bytes,
                                      uint32_t size)
{
    if (size > NetPeerMaxUnreliable(peer))
        return LAIUE_NET_TOO_LARGE;
    const uint32_t need = QUEUE_HEADER_SIZE + size;
    /* Свежее состояние ценнее застрявшего: место уступают самые старые. */
    while (peer->unreliableHead != peer->unreliableTail &&
           NetPeerQueuedUnreliable(peer) + need > NET_UNRELIABLE_QUEUE_BYTES)
        UnreliableDrop(peer);
    if (!UnreliableReserve(peer, need))
        return LAIUE_NET_OUT_OF_MEMORY;
    uint8_t *entry = peer->unreliable + peer->unreliableTail;
    uint64_t sequence = 0u;
    if (sequenced)
    {
        NetChannel *channel = &peer->channels[channelIndex];
        ++channel->sequenceNext;
        sequence = channel->sequenceNext;
    }
    NetStore32(entry, size);
    entry[4] = (uint8_t)channelIndex;
    entry[5] = sequenced ? 1u : 0u;
    entry[6] = 0u;
    entry[7] = 0u;
    NetStore64(entry + 8, sequence);
    /* Отсчёт идёт от последнего обновления: точнее времени конечная точка
     * между обновлениями не знает. */
    NetStore64(entry + 16, endpoint->now + endpoint->unreliableLifetime);
    if (size != 0u)
        memcpy(entry + QUEUE_HEADER_SIZE, bytes, size);
    peer->unreliableTail += need;
    return LAIUE_NET_OK;
}

uint32_t NetPeerWriteUnreliable(NetPeer *peer, uint8_t *output, uint32_t space)
{
    if (peer->unreliableHead == peer->unreliableTail)
        return 0u;
    const uint8_t *entry = peer->unreliable + peer->unreliableHead;
    const uint32_t size = NetLoad32(entry);
    const bool sequenced = entry[5] != 0u;
    const uint64_t sequence = NetLoad64(entry + 8);
    const uint32_t frame =
        2u + (sequenced ? NetVarintSize(sequence) : 0u) + NetVarintSize(size) + size;
    if (frame > space)
        return 0u;
    output[0] = sequenced ? NET_FRAME_SEQUENCED : NET_FRAME_DATAGRAM;
    output[1] = entry[4];
    uint32_t used = 2u;
    if (sequenced)
        used += NetVarintWrite(output + used, sequence);
    used += NetVarintWrite(output + used, size);
    if (size != 0u)
        memcpy(output + used, entry + QUEUE_HEADER_SIZE, size);
    UnreliablePop(peer);
    return used + size;
}

uint32_t NetChannelReceiveUnreliable(LaiueNetEndpoint *endpoint, NetPeer *peer,
                                     uint32_t channelIndex, bool sequenced, uint64_t sequence,
                                     const uint8_t *bytes, uint32_t length)
{
    NetChannel *channel = &peer->channels[channelIndex];
    if (sequenced)
    {
        if (sequence <= channel->sequenceSeen)
            return NET_FRAMES_OK;
        channel->sequenceSeen = sequence;
    }
    /* Ненадёжному сообщению позволено пропасть: и при нехватке памяти тоже. */
    (void)NetEventPush(endpoint, peer, LAIUE_NET_EVENT_MESSAGE, channelIndex,
                       sequenced ? LAIUE_NET_SEQUENCED : LAIUE_NET_UNRELIABLE, bytes, length);
    return NET_FRAMES_OK;
}

/* ---- Отправка потока -------------------------------------------------------------- */

static bool SendRingReserve(NetChannel *channel, uint64_t additional)
{
    const uint64_t needed = channel->sendEnd - channel->sendAcked + additional;
    if (needed <= channel->sendCapacity)
        return true;
    uint32_t capacity = channel->sendCapacity != 0u ? channel->sendCapacity : 1024u;
    while (capacity < needed)
        capacity *= 2u;
    uint8_t *data = PlatformAllocate(capacity, false);
    if (data == NULL)
        return false;
    uint64_t offset = channel->sendAcked;
    while (offset < channel->sendEnd)
    {
        const uint32_t from = (uint32_t)(offset & (channel->sendCapacity - 1u));
        uint64_t run = channel->sendEnd - offset;
        if (run > channel->sendCapacity - from)
            run = channel->sendCapacity - from;
        RingWrite(data, capacity, offset, channel->sendData + from, (uint32_t)run);
        offset += run;
    }
    PlatformFree(channel->sendData);
    channel->sendData = data;
    channel->sendCapacity = capacity;
    return true;
}

LaiueNetStatus NetChannelQueueReliable(const LaiueNetEndpoint *endpoint, NetPeer *peer,
                                       uint32_t channelIndex, const void *bytes, uint32_t size)
{
    NetChannel *channel = &peer->channels[channelIndex];
    uint8_t header[10];
    const uint32_t headerSize = NetVarintWrite(header, size);
    const uint64_t total = (uint64_t)headerSize + size;
    if (size > peer->remote.maxMessageBytes || total > endpoint->sendBufferBytes)
        return LAIUE_NET_TOO_LARGE;
    if (peer->queuedReliable + total > endpoint->sendBufferBytes)
        return LAIUE_NET_WOULD_BLOCK;
    if (!SendRingReserve(channel, total))
        return LAIUE_NET_OUT_OF_MEMORY;
    RingWrite(channel->sendData, channel->sendCapacity, channel->sendEnd, header, headerSize);
    channel->sendEnd += headerSize;
    if (size != 0u)
        RingWrite(channel->sendData, channel->sendCapacity, channel->sendEnd, bytes, size);
    channel->sendEnd += size;
    peer->queuedReliable += total;
    return LAIUE_NET_OK;
}

/* Очередной потерянный отрезок, который всё ещё нужен собеседнику. */
static bool NextLostSpan(NetChannel *channel, uint64_t *outStart, uint64_t *outEnd)
{
    while (channel->lostCount != 0u)
    {
        NetRange *lost = &channel->lostRanges[0];
        uint64_t start = lost->start > channel->sendAcked ? lost->start : channel->sendAcked;
        uint64_t end = lost->end;
        /* Подтверждённое с тех пор, как отрезок сочли потерянным, пропускается. */
        for (uint32_t index = 0u; index < channel->ackedCount; ++index)
        {
            const NetRange *acked = &channel->ackedRanges[index];
            if (acked->end <= start)
                continue;
            if (acked->start > start)
            {
                if (acked->start < end)
                    end = acked->start;
                break;
            }
            start = acked->end;
        }
        if (start < end)
        {
            lost->start = start;
            *outStart = start;
            *outEnd = end;
            return true;
        }
        RangesRemove(channel->lostRanges, &channel->lostCount, 0u, 1u);
    }
    return false;
}

static bool NextFreshSpan(const NetPeer *peer, const NetChannel *channel, uint64_t *outStart,
                          uint64_t *outEnd)
{
    /* Окно собеседника отсчитывается от подтверждённого начала потока: дальше
     * него получатель не обязан хранить. */
    const uint64_t limit = channel->sendAcked + peer->remote.windowBytes;
    if (channel->sendNext >= channel->sendEnd || channel->sendNext >= limit)
        return false;
    *outStart = channel->sendNext;
    *outEnd = channel->sendEnd < limit ? channel->sendEnd : limit;
    return true;
}

bool NetPeerHasStreamData(const NetPeer *peer)
{
    for (uint32_t index = 0u; index < peer->channelCount; ++index)
    {
        const NetChannel *channel = &peer->channels[index];
        uint64_t start = 0u;
        uint64_t end = 0u;
        if (channel->lostCount != 0u || NextFreshSpan(peer, channel, &start, &end))
            return true;
    }
    return false;
}

bool NetPeerStreamPending(const NetPeer *peer)
{
    for (uint32_t index = 0u; index < peer->channelCount; ++index)
    {
        const NetChannel *channel = &peer->channels[index];
        if (channel->lostCount != 0u || channel->sendNext < channel->sendEnd)
            return true;
    }
    return false;
}

uint32_t NetChannelWriteStream(NetPeer *peer, uint32_t channelIndex, uint8_t *output,
                               uint32_t space, NetSentRange *outRange)
{
    NetChannel *channel = &peer->channels[channelIndex];
    uint64_t start = 0u;
    uint64_t end = 0u;
    const bool repeated = NextLostSpan(channel, &start, &end);
    if (!repeated && !NextFreshSpan(peer, channel, &start, &end))
        return 0u;
    /* Вид, канал, смещение, длина и хотя бы один байт данных. */
    const uint32_t fixed = 2u + NetVarintSize(start);
    if (space < fixed + 2u)
        return 0u;
    uint32_t room = space - fixed - 1u;
    if (room >= 0x80u)
        room = space - fixed - 2u;
    const uint32_t length = end - start < room ? (uint32_t)(end - start) : room;
    output[0] = NET_FRAME_STREAM;
    output[1] = (uint8_t)channelIndex;
    uint32_t used = 2u;
    used += NetVarintWrite(output + used, start);
    used += NetVarintWrite(output + used, length);
    RingRead(channel->sendData, channel->sendCapacity, start, output + used, length);
    if (repeated)
    {
        channel->lostRanges[0].start = start + length;
        if (channel->lostRanges[0].start >= channel->lostRanges[0].end)
            RangesRemove(channel->lostRanges, &channel->lostCount, 0u, 1u);
    }
    else
    {
        channel->sendNext = start + length;
    }
    outRange->offset = start;
    outRange->length = (uint16_t)length;
    outRange->channel = (uint8_t)channelIndex;
    return used + length;
}

void NetChannelOnAcked(NetPeer *peer, const NetSentRange *range)
{
    NetChannel *channel = &peer->channels[range->channel];
    const uint64_t end = range->offset + range->length;
    if (end <= channel->sendAcked)
        return;
    if (range->offset > channel->sendAcked)
    {
        /* Подтверждение, которому нет места в наборе, не забывается молча:
         * отрезок считается потерянным и уходит ещё раз, а получатель повтор
         * просто отбросит. Иначе этот отрезок не подтвердил бы уже никто, и
         * начало потока навсегда остановилось бы перед ним вместе с буфером
         * отправки. */
        if (!RangesAdd(channel->ackedRanges, &channel->ackedCount, NET_ACKED_RANGES, range->offset,
                       end))
            RangesAddCoarse(channel->lostRanges, &channel->lostCount, NET_LOST_RANGES,
                            range->offset, end);
        return;
    }
    uint64_t acked = end;
    while (channel->ackedCount != 0u && channel->ackedRanges[0].start <= acked)
    {
        if (channel->ackedRanges[0].end > acked)
            acked = channel->ackedRanges[0].end;
        RangesRemove(channel->ackedRanges, &channel->ackedCount, 0u, 1u);
    }
    peer->queuedReliable -= acked - channel->sendAcked;
    channel->sendAcked = acked;
    RangesTrimBelow(channel->lostRanges, &channel->lostCount, acked);
    if (channel->sendAcked == channel->sendEnd && channel->sendCapacity > NET_ASSEMBLY_RETAIN)
    {
        /* Большое кольцо после разовой передачи не держится до конца соединения. */
        PlatformFree(channel->sendData);
        channel->sendData = NULL;
        channel->sendCapacity = 0u;
    }
}

void NetChannelOnLost(NetPeer *peer, const NetSentRange *range)
{
    NetChannel *channel = &peer->channels[range->channel];
    const uint64_t end = range->offset + range->length;
    const uint64_t start = range->offset > channel->sendAcked ? range->offset : channel->sendAcked;
    if (start < end)
        RangesAddCoarse(channel->lostRanges, &channel->lostCount, NET_LOST_RANGES, start, end);
}

/* ---- Приём потока ------------------------------------------------------------------ */

static bool AssemblyReserve(NetChannel *channel, uint32_t needed)
{
    if (needed <= channel->assemblyCapacity)
        return true;
    uint32_t capacity = channel->assemblyCapacity != 0u ? channel->assemblyCapacity : 256u;
    while (capacity < needed)
        capacity *= 2u;
    if (capacity > channel->messageSize)
        capacity = channel->messageSize;
    uint8_t *grown = PlatformReallocate(channel->assembly, capacity, false);
    if (grown == NULL)
        return false;
    channel->assembly = grown;
    channel->assemblyCapacity = capacity;
    return true;
}

static bool EmitMessage(LaiueNetEndpoint *endpoint, const NetPeer *peer, uint32_t channelIndex,
                        const uint8_t *bytes, uint32_t size)
{
    return NetEventPush(endpoint, peer, LAIUE_NET_EVENT_MESSAGE, channelIndex, LAIUE_NET_RELIABLE,
                        bytes, size);
}

/* Длина сообщения — LEB128 не длиннее пяти байт. */
static uint32_t FeedHeader(const LaiueNetEndpoint *endpoint, NetChannel *channel, uint8_t byte,
                           bool *outComplete)
{
    channel->headerValue |= (uint64_t)(byte & 0x7Fu) << channel->headerShift;
    channel->headerShift += 7u;
    *outComplete = (byte & 0x80u) == 0u;
    if (!*outComplete)
        return channel->headerShift < 35u ? NET_FRAMES_OK : NET_FRAMES_VIOLATION;
    if (channel->headerValue > endpoint->local.maxMessageBytes)
        return NET_FRAMES_VIOLATION;
    channel->messageSize = (uint32_t)channel->headerValue;
    channel->messageFilled = 0u;
    channel->headerValue = 0u;
    channel->headerShift = 0u;
    return NET_FRAMES_OK;
}

/* Разбирает на сообщения очередные сплошные байты потока. */
static uint32_t FeedStream(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t channelIndex,
                           const uint8_t *bytes, uint32_t length)
{
    NetChannel *channel = &peer->channels[channelIndex];
    uint32_t used = 0u;
    while (used < length)
    {
        if (!channel->haveHeader)
        {
            bool complete = false;
            const uint32_t status = FeedHeader(endpoint, channel, bytes[used], &complete);
            ++used;
            if (status != NET_FRAMES_OK)
                return status;
            if (!complete)
                continue;
            if (channel->messageSize != 0u)
                channel->haveHeader = true;
            else if (!EmitMessage(endpoint, peer, channelIndex, bytes, 0u))
                return NET_FRAMES_RESOURCE;
            continue;
        }
        const uint32_t missing = channel->messageSize - channel->messageFilled;
        const uint32_t take = length - used < missing ? length - used : missing;
        if (channel->messageFilled == 0u && take == missing)
        {
            /* Сообщение целиком в этих байтах: сборка не нужна. */
            if (!EmitMessage(endpoint, peer, channelIndex, bytes + used, take))
                return NET_FRAMES_RESOURCE;
            channel->haveHeader = false;
        }
        else
        {
            if (!AssemblyReserve(channel, channel->messageFilled + take))
                return NET_FRAMES_RESOURCE;
            memcpy(channel->assembly + channel->messageFilled, bytes + used, take);
            channel->messageFilled += take;
            if (channel->messageFilled == channel->messageSize)
            {
                if (!EmitMessage(endpoint, peer, channelIndex, channel->assembly,
                                 channel->messageSize))
                    return NET_FRAMES_RESOURCE;
                channel->haveHeader = false;
                if (channel->assemblyCapacity > NET_ASSEMBLY_RETAIN)
                {
                    PlatformFree(channel->assembly);
                    channel->assembly = NULL;
                    channel->assemblyCapacity = 0u;
                }
            }
        }
        used += take;
    }
    channel->receiveOffset += length;
    return NET_FRAMES_OK;
}

uint32_t NetChannelReceiveStream(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t channelIndex,
                                 uint64_t offset, const uint8_t *bytes, uint32_t length)
{
    NetChannel *channel = &peer->channels[channelIndex];
    const uint32_t window = endpoint->local.windowBytes;
    if (offset > (UINT64_MAX >> 2u))
        return NET_FRAMES_VIOLATION;
    const uint64_t end = offset + length;
    if (end <= channel->receiveOffset)
        return NET_FRAMES_OK;
    /* Честный отправитель не выходит за окно: оно отсчитано от того, что он
     * уже знает подтверждённым, а это не больше принятого здесь. */
    if (end > channel->receiveOffset + window)
        return NET_FRAMES_VIOLATION;
    if (offset < channel->receiveOffset)
    {
        const uint32_t known = (uint32_t)(channel->receiveOffset - offset);
        bytes += known;
        length -= known;
        offset = channel->receiveOffset;
    }
    /* Начало потока разбирается сразу и места в наборе отрезков не занимает.
     * Иначе после потерь вразбивку, когда отрезков с опережением больше, чем
     * вмещает набор, повтор начала, короче дыры, откладывался бы раз за разом
     * и передача вставала. */
    if (offset == channel->receiveOffset)
    {
        const uint32_t status = FeedStream(endpoint, peer, channelIndex, bytes, length);
        if (status != NET_FRAMES_OK || channel->receivedCount == 0u)
            return status;
    }
    else
    {
        if (channel->receiveData == NULL)
        {
            channel->receiveData = PlatformAllocate(window, false);
            if (channel->receiveData == NULL)
                return NET_FRAMES_DEFER;
        }
        if (!RangesAdd(channel->receivedRanges, &channel->receivedCount, NET_RECEIVED_RANGES,
                       offset, end))
            return NET_FRAMES_DEFER;
        RingWrite(channel->receiveData, window, offset, bytes, length);
    }
    /* Отрезки, до которых дошло начало, разбираются из кольца. */
    while (channel->receivedCount != 0u &&
           channel->receivedRanges[0].start <= channel->receiveOffset)
    {
        const uint64_t rangeEnd = channel->receivedRanges[0].end;
        while (channel->receiveOffset < rangeEnd)
        {
            const uint32_t at = (uint32_t)(channel->receiveOffset & (window - 1u));
            uint64_t run = rangeEnd - channel->receiveOffset;
            if (run > window - at)
                run = window - at;
            const uint32_t status =
                FeedStream(endpoint, peer, channelIndex, channel->receiveData + at, (uint32_t)run);
            if (status != NET_FRAMES_OK)
                return status;
        }
        RangesRemove(channel->receivedRanges, &channel->receivedCount, 0u, 1u);
    }
    if (channel->receivedCount == 0u && window > NET_ASSEMBLY_RETAIN)
    {
        /* Поток снова сплошной: большое кольцо не держится до следующей потери,
         * и память соединений без потерь не зависит от окна. */
        PlatformFree(channel->receiveData);
        channel->receiveData = NULL;
    }
    return NET_FRAMES_OK;
}

void NetChannelsFree(NetPeer *peer)
{
    if (peer->channels == NULL)
        return;
    for (uint32_t index = 0u; index < peer->channelCount; ++index)
    {
        NetChannel *channel = &peer->channels[index];
        PlatformFree(channel->sendData);
        PlatformFree(channel->receiveData);
        PlatformFree(channel->assembly);
    }
    PlatformFree(peer->channels);
    peer->channels = NULL;
}
