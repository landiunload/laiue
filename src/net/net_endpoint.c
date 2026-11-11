#include "net/net_internal.h"

#include "platform/system.h"

#include <string.h>

/* Конечная точка: таблица участников, очередь событий и цикл обновления. */

#define DEFAULT_PEERS 64u
#define PEERS_LIMIT (UINT32_C(1) << 24u)
#define DEFAULT_MESSAGE_BYTES (UINT32_C(1) << 20u)
#define DEFAULT_SEND_BUFFER_BYTES (UINT32_C(4) << 20u)
#define DEFAULT_UNRELIABLE_LIFETIME_MILLISECONDS 200u
#define SEND_BUFFER_LIMIT (UINT32_C(1) << 30u)
#define DEFAULT_WINDOW_BYTES (UINT32_C(256) << 10u)
#define DEFAULT_EVENT_BACKLOG_BYTES (UINT32_C(64) << 20u)
#define DEFAULT_HANDSHAKE_MILLISECONDS 10000u
#define DEFAULT_IDLE_MILLISECONDS 30000u
/* Буфер приёма на байт длиннее наибольшей датаграммы: так видно, что
 * пришедшая была длиннее и обрезана. */
#define RECEIVE_CAPACITY (NET_DATAGRAM_MAXIMUM + 1u)
#define BUFFER_STRIDE 1456u

bool NetRandom(LaiueNetEndpoint *endpoint, void *output, uint32_t size)
{
    return endpoint->random != NULL ? endpoint->random(endpoint->randomContext, output, size)
                                    : PlatformRandomBytes(output, size);
}

void NetTransmit(LaiueNetEndpoint *endpoint, const LaiueNetAddress *to, const uint8_t *bytes,
                 uint32_t size)
{
    const int32_t result = endpoint->driver.send(endpoint->driver.context, to, bytes, size);
    if (result < 0)
    {
        endpoint->driverBroken = true;
    }
    else if (result > 0)
    {
        ++endpoint->stats.datagramsSent;
        endpoint->stats.bytesSent += size;
    }
}

/* ---- Таблица CID -------------------------------------------------------------------
 *
 * Открытая адресация с линейным пробированием; при удалении хвост цепочки
 * сдвигается назад, поэтому надгробий нет и таблица не засоряется. */

static uint32_t CidHome(const LaiueNetEndpoint *endpoint, uint64_t cid)
{
    return (uint32_t)((cid * UINT64_C(0x9E3779B97F4A7C15)) >> 32u) & (endpoint->cidCapacity - 1u);
}

static NetCidEntry *CidFind(const LaiueNetEndpoint *endpoint, uint64_t cid)
{
    if (endpoint->cidCapacity == 0u)
        return NULL;
    uint32_t index = CidHome(endpoint, cid);
    while (endpoint->cids[index].used != 0u)
    {
        if (endpoint->cids[index].cid == cid)
            return &endpoint->cids[index];
        index = (index + 1u) & (endpoint->cidCapacity - 1u);
    }
    return NULL;
}

static void CidInsert(LaiueNetEndpoint *endpoint, uint64_t cid, uint32_t slot)
{
    uint32_t index = CidHome(endpoint, cid);
    while (endpoint->cids[index].used != 0u)
        index = (index + 1u) & (endpoint->cidCapacity - 1u);
    endpoint->cids[index].cid = cid;
    endpoint->cids[index].slot = slot;
    endpoint->cids[index].used = 1u;
    ++endpoint->cidCount;
}

/* Держит таблицу заполненной не больше чем наполовину. */
static bool CidReserve(LaiueNetEndpoint *endpoint)
{
    if ((endpoint->cidCount + 1u) * 2u <= endpoint->cidCapacity)
        return true;
    const uint32_t capacity = endpoint->cidCapacity != 0u ? endpoint->cidCapacity * 2u : 32u;
    NetCidEntry *entries = PlatformAllocate((size_t)capacity * sizeof(NetCidEntry), true);
    if (entries == NULL)
        return false;
    NetCidEntry *previous = endpoint->cids;
    const uint32_t previousCapacity = endpoint->cidCapacity;
    endpoint->cids = entries;
    endpoint->cidCapacity = capacity;
    endpoint->cidCount = 0u;
    for (uint32_t index = 0u; index < previousCapacity; ++index)
    {
        if (previous[index].used != 0u)
            CidInsert(endpoint, previous[index].cid, previous[index].slot);
    }
    PlatformFree(previous);
    return true;
}

static void CidRemove(LaiueNetEndpoint *endpoint, uint64_t cid)
{
    NetCidEntry *entry = CidFind(endpoint, cid);
    if (entry == NULL)
        return;
    const uint32_t mask = endpoint->cidCapacity - 1u;
    uint32_t hole = (uint32_t)(entry - endpoint->cids);
    uint32_t next = hole;
    for (;;)
    {
        next = (next + 1u) & mask;
        if (endpoint->cids[next].used == 0u)
            break;
        /* Запись остаётся, если её родная ячейка лежит по кругу между дырой
         * и ею самой: иначе поиск оборвался бы на дыре. */
        const uint32_t home = CidHome(endpoint, endpoint->cids[next].cid);
        const bool reachable =
            hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (reachable)
            continue;
        endpoint->cids[hole] = endpoint->cids[next];
        hole = next;
    }
    endpoint->cids[hole].used = 0u;
    --endpoint->cidCount;
}

NetPeer *NetPeerFindByCid(const LaiueNetEndpoint *endpoint, uint64_t cid)
{
    const NetCidEntry *entry = CidFind(endpoint, cid);
    return entry != NULL ? endpoint->slots[entry->slot].peer : NULL;
}

/* ---- Слоты участников ---------------------------------------------------------------- */

static LaiueNetPeer PeerHandle(const LaiueNetEndpoint *endpoint, const NetPeer *peer)
{
    return ((uint64_t)endpoint->slots[peer->slot].generation << 32u) | (peer->slot + 1u);
}

/* Участник по описателю приложения: только тот, о ком оно знает. */
static NetPeer *PeerFromHandle(const LaiueNetEndpoint *endpoint, LaiueNetPeer handle)
{
    const uint32_t slot = (uint32_t)handle - 1u;
    if ((uint32_t)handle == 0u || slot >= endpoint->slotCount ||
        endpoint->slots[slot].generation != (uint32_t)(handle >> 32u))
        return NULL;
    NetPeer *peer = endpoint->slots[slot].peer;
    return peer != NULL && peer->announced ? peer : NULL;
}

uint32_t NetPeerCount(const LaiueNetEndpoint *endpoint)
{
    return endpoint->activeCount - endpoint->lingerCount;
}

static bool SlotsReserve(LaiueNetEndpoint *endpoint)
{
    if (endpoint->freeSlot != 0u || endpoint->slotCount < endpoint->slotCapacity)
        return true;
    /* Сверх предела живут только закрытые участники, повторяющие
     * уведомление, а их не больше, чем самих участников. */
    const uint32_t limit = endpoint->maxPeers * 2u;
    uint32_t capacity = endpoint->slotCapacity != 0u ? endpoint->slotCapacity * 2u : 16u;
    if (capacity > limit)
        capacity = limit;
    if (capacity <= endpoint->slotCapacity)
        return false;
    NetSlot *slots = PlatformReallocate(endpoint->slots, (size_t)capacity * sizeof(NetSlot), false);
    if (slots == NULL)
        return false;
    endpoint->slots = slots;
    uint32_t *active =
        PlatformReallocate(endpoint->active, (size_t)capacity * sizeof(uint32_t), false);
    if (active == NULL)
        return false;
    endpoint->active = active;
    memset(&slots[endpoint->slotCapacity], 0,
           (size_t)(capacity - endpoint->slotCapacity) * sizeof(NetSlot));
    endpoint->slotCapacity = capacity;
    return true;
}

void NetPeerSetState(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t state)
{
    if (peer->state == NET_STATE_HALF_OPEN)
    {
        if (peer->halfOpenPrevious != 0u)
            endpoint->slots[peer->halfOpenPrevious - 1u].peer->halfOpenNext = peer->halfOpenNext;
        else
            endpoint->halfOpenHead = peer->halfOpenNext;
        if (peer->halfOpenNext != 0u)
            endpoint->slots[peer->halfOpenNext - 1u].peer->halfOpenPrevious =
                peer->halfOpenPrevious;
        else
            endpoint->halfOpenTail = peer->halfOpenPrevious;
        peer->halfOpenPrevious = 0u;
        peer->halfOpenNext = 0u;
    }
    else if (peer->state == NET_STATE_CLOSING)
    {
        --endpoint->lingerCount;
    }
    peer->state = state;
    if (state == NET_STATE_HALF_OPEN)
    {
        peer->halfOpenPrevious = endpoint->halfOpenTail;
        if (endpoint->halfOpenTail != 0u)
            endpoint->slots[endpoint->halfOpenTail - 1u].peer->halfOpenNext = peer->slot + 1u;
        else
            endpoint->halfOpenHead = peer->slot + 1u;
        endpoint->halfOpenTail = peer->slot + 1u;
    }
    else if (state == NET_STATE_CLOSING)
    {
        ++endpoint->lingerCount;
    }
}

NetPeer *NetPeerCreate(LaiueNetEndpoint *endpoint, const LaiueNetAddress *address,
                       const uint64_t *cid, LaiueNetStatus *outStatus)
{
    uint64_t localCid = 0u;
    *outStatus = LAIUE_NET_OUT_OF_MEMORY;
    if (!SlotsReserve(endpoint) || !CidReserve(endpoint))
        return NULL;
    if (cid != NULL)
    {
        localCid = *cid;
    }
    else
    {
        /* Совпадение случайных 64 бит невероятно, но проверка дешевле сбоя. */
        uint32_t attempts = 0u;
        do
        {
            if (attempts == 8u || !NetRandom(endpoint, &localCid, sizeof(localCid)))
            {
                *outStatus = LAIUE_NET_IO_ERROR;
                return NULL;
            }
            ++attempts;
        } while (CidFind(endpoint, localCid) != NULL);
    }
    NetPeer *peer = PlatformAllocate(sizeof(*peer), true);
    if (peer == NULL)
        return NULL;
    uint32_t slot = endpoint->slotCount;
    if (endpoint->freeSlot != 0u)
    {
        slot = endpoint->freeSlot - 1u;
        endpoint->freeSlot = endpoint->slots[slot].nextFree;
    }
    else
    {
        ++endpoint->slotCount;
    }
    endpoint->slots[slot].peer = peer;
    endpoint->slots[slot].nextFree = 0u;
    endpoint->active[endpoint->activeCount] = slot;
    peer->slot = slot;
    peer->activeIndex = endpoint->activeCount;
    peer->address = *address;
    peer->localCid = localCid;
    ++endpoint->activeCount;
    CidInsert(endpoint, localCid, slot);
    *outStatus = LAIUE_NET_OK;
    return peer;
}

void NetPeerRelease(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    NetPeerSetState(endpoint, peer, 0u);
    CidRemove(endpoint, peer->localCid);
    const uint32_t moved = endpoint->active[endpoint->activeCount - 1u];
    endpoint->active[peer->activeIndex] = moved;
    endpoint->slots[moved].peer->activeIndex = peer->activeIndex;
    --endpoint->activeCount;
    NetSlot *slot = &endpoint->slots[peer->slot];
    slot->peer = NULL;
    ++slot->generation;
    slot->nextFree = endpoint->freeSlot;
    endpoint->freeSlot = peer->slot + 1u;
    if (peer->announced)
        --endpoint->eventReserved;
    NetPeerFreeTransport(peer);
    /* В структуре лежат ключи соединения. */
    NetWipe(peer, sizeof(*peer));
    PlatformFree(peer);
}

bool NetPeerEvictHalfOpen(LaiueNetEndpoint *endpoint)
{
    if (endpoint->halfOpenHead == 0u)
        return false;
    NetPeerRelease(endpoint, endpoint->slots[endpoint->halfOpenHead - 1u].peer);
    return true;
}

/* ---- События ------------------------------------------------------------------------- */

static bool EventsEnsure(LaiueNetEndpoint *endpoint, uint32_t needed)
{
    if (needed <= endpoint->eventCapacity)
        return true;
    uint32_t capacity = endpoint->eventCapacity != 0u ? endpoint->eventCapacity : 64u;
    while (capacity < needed)
        capacity *= 2u;
    LaiueNetEvent *events =
        PlatformReallocate(endpoint->events, (size_t)capacity * sizeof(LaiueNetEvent), false);
    if (events == NULL)
        return false;
    endpoint->events = events;
    endpoint->eventCapacity = capacity;
    return true;
}

static uint8_t *ArenaAllocate(LaiueNetEndpoint *endpoint, uint32_t size)
{
    NetArenaChunk *chunk = endpoint->arenaCurrent;
    if (chunk == NULL || chunk->capacity - chunk->used < size)
    {
        const uint32_t capacity = size > NET_ARENA_CHUNK ? size : NET_ARENA_CHUNK;
        NetArenaChunk *created = PlatformAllocate(sizeof(NetArenaChunk) + capacity, false);
        if (created == NULL)
            return NULL;
        created->next = NULL;
        created->capacity = capacity;
        created->used = 0u;
        if (chunk != NULL)
            chunk->next = created;
        else
            endpoint->arenaFirst = created;
        endpoint->arenaCurrent = created;
        chunk = created;
    }
    uint8_t *bytes = (uint8_t *)(chunk + 1) + chunk->used;
    chunk->used += size;
    endpoint->arenaBytes += size;
    return bytes;
}

/* Память сообщений освобождается разом, когда приложение разобрало все
 * события. Первый кусок обычного размера остаётся на следующий раз. */
static void ArenaReset(LaiueNetEndpoint *endpoint, bool keepFirst)
{
    NetArenaChunk *chunk = endpoint->arenaFirst;
    endpoint->arenaFirst = NULL;
    endpoint->arenaCurrent = NULL;
    endpoint->arenaBytes = 0u;
    if (keepFirst && chunk != NULL && chunk->capacity == NET_ARENA_CHUNK)
    {
        NetArenaChunk *kept = chunk;
        chunk = chunk->next;
        kept->next = NULL;
        kept->used = 0u;
        endpoint->arenaFirst = kept;
        endpoint->arenaCurrent = kept;
    }
    while (chunk != NULL)
    {
        NetArenaChunk *next = chunk->next;
        PlatformFree(chunk);
        chunk = next;
    }
}

static LaiueNetEvent *EventAppend(LaiueNetEndpoint *endpoint, const NetPeer *peer,
                                  LaiueNetEventType type)
{
    LaiueNetEvent *event = &endpoint->events[endpoint->eventCount];
    ++endpoint->eventCount;
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->peer = PeerHandle(endpoint, peer);
    event->userData = peer->userData;
    return event;
}

bool NetEventReserveFinal(LaiueNetEndpoint *endpoint, uint32_t extra)
{
    if (!EventsEnsure(endpoint, endpoint->eventCount + endpoint->eventReserved + 1u + extra))
        return false;
    ++endpoint->eventReserved;
    return true;
}

bool NetEventPush(LaiueNetEndpoint *endpoint, const NetPeer *peer, LaiueNetEventType type,
                  uint32_t channel, LaiueNetDelivery delivery, const uint8_t *data, uint32_t size)
{
    uint8_t *copy = NULL;
    if (!EventsEnsure(endpoint, endpoint->eventCount + endpoint->eventReserved + 1u))
        return false;
    if (size != 0u)
    {
        copy = ArenaAllocate(endpoint, size);
        if (copy == NULL)
            return false;
        memcpy(copy, data, size);
    }
    LaiueNetEvent *event = EventAppend(endpoint, peer, type);
    event->channel = channel;
    event->delivery = delivery;
    event->data = copy;
    event->size = size;
    return true;
}

bool NetPeerAnnounce(LaiueNetEndpoint *endpoint, NetPeer *peer)
{
    /* Место нужно сразу двум событиям: этому и будущему DISCONNECTED. */
    if (!NetEventReserveFinal(endpoint, 1u))
        return false;
    peer->announced = true;
    (void)EventAppend(endpoint, peer, LAIUE_NET_EVENT_CONNECTED);
    return true;
}

void NetPeerClose(LaiueNetEndpoint *endpoint, NetPeer *peer, uint32_t reason, uint32_t code,
                  uint32_t closeKind)
{
    const bool keyed = peer->state == NET_STATE_HALF_OPEN || peer->state == NET_STATE_CONNECTED ||
                       peer->state == NET_STATE_FLUSHING;
    if (peer->announced)
    {
        /* Место этому событию обещано при объявлении участника. */
        LaiueNetEvent *event = EventAppend(endpoint, peer, LAIUE_NET_EVENT_DISCONNECTED);
        event->reason = reason;
        event->code = code;
        peer->announced = false;
        --endpoint->eventReserved;
    }
    if (!keyed || closeKind == NET_CLOSE_NONE)
    {
        NetPeerRelease(endpoint, peer);
        return;
    }
    peer->closeKind = (uint8_t)closeKind;
    peer->closeCode = code;
    NetPeerSendClose(endpoint, peer);
    if (endpoint->lingerCount >= endpoint->maxPeers)
    {
        NetPeerRelease(endpoint, peer);
        return;
    }
    /* Уведомление могло потеряться. Участник ещё немного живёт без буферов:
     * повторяет его сам через каждый пробный интервал и в ответ на пакеты
     * собеседника. */
    uint64_t interval = NetPeerProbeTimeout(peer);
    if (interval > NET_CLOSE_LINGER_LIMIT / (NET_CLOSE_RESENDS + 1u))
        interval = NET_CLOSE_LINGER_LIMIT / (NET_CLOSE_RESENDS + 1u);
    NetPeerFreeTransport(peer);
    peer->closeInterval = interval;
    peer->closeResends = 0u;
    peer->closeResendAt = endpoint->now + interval;
    peer->closeDeadline = endpoint->now + (NET_CLOSE_RESENDS + 1u) * interval;
    peer->closeRepeatAt = endpoint->now + NET_CLOSE_REPEAT_INTERVAL;
    NetPeerSetState(endpoint, peer, NET_STATE_CLOSING);
}

/* ---- Создание и уничтожение ---------------------------------------------------------- */

static uint32_t RoundUpToPowerOfTwo(uint32_t value)
{
    uint32_t power = 1u;
    while (power < value)
        power *= 2u;
    return power;
}

/* Подставляет значения по умолчанию и проверяет границы. */
static bool ResolveLimits(const LaiueNetEndpointDesc *desc, LaiueNetEndpoint *endpoint)
{
    const uint32_t window =
        desc->receiveWindowBytes != 0u ? desc->receiveWindowBytes : DEFAULT_WINDOW_BYTES;
    const uint32_t handshake = desc->handshakeTimeoutMilliseconds != 0u
                                   ? desc->handshakeTimeoutMilliseconds
                                   : DEFAULT_HANDSHAKE_MILLISECONDS;
    endpoint->maxPeers = desc->maxPeers != 0u ? desc->maxPeers : DEFAULT_PEERS;
    endpoint->local.channelCount = desc->channelCount != 0u ? desc->channelCount : 1u;
    endpoint->local.datagramBytes =
        desc->maxDatagramBytes != 0u ? desc->maxDatagramBytes : NET_DATAGRAM_DEFAULT;
    endpoint->local.maxMessageBytes =
        desc->maxMessageBytes != 0u ? desc->maxMessageBytes : DEFAULT_MESSAGE_BYTES;
    endpoint->local.idleMilliseconds = desc->idleTimeoutMilliseconds != 0u
                                           ? desc->idleTimeoutMilliseconds
                                           : DEFAULT_IDLE_MILLISECONDS;
    endpoint->sendBufferBytes =
        desc->sendBufferBytes != 0u ? desc->sendBufferBytes : DEFAULT_SEND_BUFFER_BYTES;
    endpoint->unreliableLifetime = (uint64_t)(desc->unreliableLifetimeMilliseconds != 0u
                                                  ? desc->unreliableLifetimeMilliseconds
                                                  : DEFAULT_UNRELIABLE_LIFETIME_MILLISECONDS) *
                                   NET_MILLISECOND;
    endpoint->eventBacklogBytes =
        desc->eventBacklogBytes != 0u ? desc->eventBacklogBytes : DEFAULT_EVENT_BACKLOG_BYTES;
    endpoint->handshakeTimeout = (uint64_t)handshake * NET_MILLISECOND;
    if (endpoint->maxPeers > PEERS_LIMIT || endpoint->local.channelCount > LAIUE_NET_MAX_CHANNELS ||
        endpoint->local.datagramBytes < NET_DATAGRAM_MINIMUM ||
        endpoint->local.datagramBytes > NET_DATAGRAM_MAXIMUM ||
        endpoint->local.maxMessageBytes > NET_MESSAGE_LIMIT ||
        endpoint->local.idleMilliseconds < 1000u || endpoint->sendBufferBytes > SEND_BUFFER_LIMIT ||
        window > NET_WINDOW_LIMIT)
        return false;
    endpoint->local.windowBytes =
        RoundUpToPowerOfTwo(window > NET_WINDOW_MINIMUM ? window : NET_WINDOW_MINIMUM);
    return true;
}

LaiueNetStatus LaiueNetEndpointCreate(const LaiueNetEndpointDesc *desc,
                                      LaiueNetEndpoint **outEndpoint)
{
    if (outEndpoint == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outEndpoint = NULL;
    if (desc == NULL || desc->structSize < sizeof(*desc) || desc->driver == NULL ||
        desc->driver->structSize < sizeof(*desc->driver) || desc->driver->send == NULL ||
        desc->driver->receive == NULL || (desc->flags & ~LAIUE_NET_ENDPOINT_ACCEPT) != 0u)
        return LAIUE_NET_INVALID_ARGUMENT;
    /* Буферы приёма и сборки пакета лежат сразу за структурой. */
    LaiueNetEndpoint *endpoint =
        PlatformAllocate(sizeof(*endpoint) + (size_t)2u * BUFFER_STRIDE, true);
    if (endpoint == NULL)
        return LAIUE_NET_OUT_OF_MEMORY;
    if (!ResolveLimits(desc, endpoint))
    {
        PlatformFree(endpoint);
        return LAIUE_NET_INVALID_ARGUMENT;
    }
    endpoint->receiveBuffer = (uint8_t *)(endpoint + 1);
    endpoint->packetBuffer = endpoint->receiveBuffer + BUFFER_STRIDE;
    endpoint->driver = *desc->driver;
    endpoint->driver.structSize = sizeof(endpoint->driver);
    endpoint->flags = desc->flags;
    endpoint->protocolId = desc->protocolId;
    endpoint->random = desc->random;
    endpoint->randomContext = desc->randomContext;
    if (desc->identityPrivateKey != NULL)
    {
        endpoint->hasIdentity = true;
        memcpy(endpoint->identityPrivate, desc->identityPrivateKey, NET_KEY_SIZE);
    }
    if (desc->presharedKey != NULL)
    {
        endpoint->hasPreshared = true;
        memcpy(endpoint->preshared, desc->presharedKey, NET_KEY_SIZE);
    }
    *outEndpoint = endpoint;
    return LAIUE_NET_OK;
}

void LaiueNetEndpointDestroy(LaiueNetEndpoint *endpoint)
{
    if (endpoint == NULL)
        return;
    while (endpoint->activeCount != 0u)
    {
        NetPeer *peer = endpoint->slots[endpoint->active[endpoint->activeCount - 1u]].peer;
        if (peer->state == NET_STATE_HALF_OPEN || peer->state == NET_STATE_CONNECTED ||
            peer->state == NET_STATE_FLUSHING)
        {
            /* Собеседник узнаёт о закрытии сразу, а не по тайм-ауту. Повторить
             * уведомление позже уже некому, поэтому оно уходит дважды: при
             * случайных потерях до тайм-аута доживает в десятки раз меньше
             * собеседников. */
            peer->closeKind = NET_CLOSE_APPLICATION;
            peer->closeCode = 0u;
            NetPeerSendClose(endpoint, peer);
            NetPeerSendClose(endpoint, peer);
        }
        NetPeerRelease(endpoint, peer);
    }
    ArenaReset(endpoint, false);
    PlatformFree(endpoint->events);
    PlatformFree(endpoint->slots);
    PlatformFree(endpoint->active);
    PlatformFree(endpoint->cids);
    PlatformFree(endpoint->deferred);
    NetWipe(endpoint, sizeof(*endpoint));
    PlatformFree(endpoint);
}

/* ---- Соединения ---------------------------------------------------------------------- */

LaiueNetStatus LaiueNetConnect(LaiueNetEndpoint *endpoint, const LaiueNetConnectDesc *desc,
                               LaiueNetPeer *outPeer)
{
    LaiueNetStatus status = LAIUE_NET_OK;
    if (outPeer == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outPeer = 0u;
    if (endpoint == NULL || desc == NULL || desc->structSize < sizeof(*desc))
        return LAIUE_NET_INVALID_ARGUMENT;
    if (NetPeerCount(endpoint) >= endpoint->maxPeers)
        return LAIUE_NET_LIMIT_REACHED;
    if (!NetEventReserveFinal(endpoint, 0u))
        return LAIUE_NET_OUT_OF_MEMORY;
    NetPeer *peer = NetPeerCreate(endpoint, &desc->address, NULL, &status);
    if (peer != NULL && !NetHandshakeBegin(endpoint, peer, desc->serverPublicKey))
    {
        NetPeerRelease(endpoint, peer);
        peer = NULL;
        status = LAIUE_NET_IO_ERROR;
    }
    if (peer == NULL)
    {
        --endpoint->eventReserved;
        return status;
    }
    peer->announced = true;
    peer->initiator = true;
    peer->userData = desc->userData;
    NetPeerSetState(endpoint, peer, NET_STATE_CONNECTING);
    *outPeer = PeerHandle(endpoint, peer);
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetDisconnect(LaiueNetEndpoint *endpoint, LaiueNetPeer handle, uint32_t code,
                                  uint32_t flags)
{
    if (endpoint == NULL || (flags & ~LAIUE_NET_DISCONNECT_FLUSH) != 0u)
        return LAIUE_NET_INVALID_ARGUMENT;
    NetPeer *peer = PeerFromHandle(endpoint, handle);
    if (peer == NULL)
        return LAIUE_NET_NO_PEER;
    const bool flush = (flags & LAIUE_NET_DISCONNECT_FLUSH) != 0u;
    if (peer->state == NET_STATE_CONNECTING)
    {
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_LOCAL, code, NET_CLOSE_NONE);
    }
    else if (flush && (peer->queuedReliable != 0u || NetPeerQueuedUnreliable(peer) != 0u))
    {
        /* Закрытие завершит обновление, когда очередь опустеет. */
        peer->closeCode = code;
        NetPeerSetState(endpoint, peer, NET_STATE_FLUSHING);
    }
    else
    {
        NetPeerClose(endpoint, peer, LAIUE_NET_REASON_LOCAL, code, NET_CLOSE_APPLICATION);
    }
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetSend(LaiueNetEndpoint *endpoint, LaiueNetPeer handle, uint32_t channel,
                            LaiueNetDelivery delivery, const void *bytes, uint32_t size)
{
    if (endpoint == NULL || (bytes == NULL && size != 0u) || delivery > LAIUE_NET_SEQUENCED)
        return LAIUE_NET_INVALID_ARGUMENT;
    NetPeer *peer = PeerFromHandle(endpoint, handle);
    if (peer == NULL)
        return LAIUE_NET_NO_PEER;
    if (peer->state != NET_STATE_CONNECTED)
        return LAIUE_NET_NOT_CONNECTED;
    if (channel >= peer->channelCount)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (delivery == LAIUE_NET_RELIABLE)
        return NetChannelQueueReliable(endpoint, peer, channel, bytes, size);
    return NetPeerQueueUnreliable(endpoint, peer, channel, delivery == LAIUE_NET_SEQUENCED, bytes,
                                  size);
}

/* ---- Обновление ---------------------------------------------------------------------- */

/* Память, занятая неразобранными событиями: и байты сообщений, и сами записи —
 * пустые сообщения тоже чего-то стоят. */
static uint64_t EventBacklog(const LaiueNetEndpoint *endpoint)
{
    return endpoint->arenaBytes +
           (uint64_t)(endpoint->eventCount - endpoint->eventHead) * sizeof(LaiueNetEvent);
}

static void ReceiveDatagrams(LaiueNetEndpoint *endpoint)
{
    for (uint32_t count = 0u; count < NET_RECEIVE_DATAGRAMS; ++count)
    {
        /* Приложение перестало разбирать события: новые датаграммы остаются
         * в сети, пока оно не освободит память. */
        if (EventBacklog(endpoint) > endpoint->eventBacklogBytes)
            return;
        LaiueNetAddress from;
        memset(&from, 0, sizeof(from));
        const int32_t received = endpoint->driver.receive(
            endpoint->driver.context, &from, endpoint->receiveBuffer, RECEIVE_CAPACITY);
        if (received < 0)
            endpoint->driverBroken = true;
        if (received <= 0)
            return;
        const uint32_t size = (uint32_t)received;
        ++endpoint->stats.datagramsReceived;
        endpoint->stats.bytesReceived += size;
        const uint8_t kind = endpoint->receiveBuffer[0];
        const bool fits = size <= NET_DATAGRAM_MAXIMUM;
        if (fits && kind == NET_PACKET_SHORT)
            NetShortReceive(endpoint, &from, endpoint->receiveBuffer, size);
        else if (fits && (kind & NET_PACKET_LONG) != 0u)
            NetHandshakeReceive(endpoint, &from, endpoint->receiveBuffer, size);
        else
            ++endpoint->stats.datagramsDropped;
    }
}

LaiueNetStatus LaiueNetUpdate(LaiueNetEndpoint *endpoint, uint64_t nowMicroseconds)
{
    if (endpoint == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (nowMicroseconds > endpoint->now)
        endpoint->now = nowMicroseconds;
    if (endpoint->eventHead == endpoint->eventCount)
    {
        endpoint->eventHead = 0u;
        endpoint->eventCount = 0u;
        ArenaReset(endpoint, true);
    }
    endpoint->driverBroken = false;
    endpoint->acceptBudget = NET_ACCEPTS_PER_UPDATE;
    /* Отложенные запросы старше новых и принимаются первыми. */
    NetHandshakeAdmitDeferred(endpoint);
    ReceiveDatagrams(endpoint);
    /* Обход с конца: закрытый участник уступает место последнему в списке, а
     * тот уже обработан. */
    for (uint32_t index = endpoint->activeCount; index != 0u; --index)
    {
        NetPeer *peer = endpoint->slots[endpoint->active[index - 1u]].peer;
        if (peer->state == NET_STATE_CONNECTING || peer->state == NET_STATE_HALF_OPEN)
            NetHandshakeUpdate(endpoint, peer);
        else
            NetPeerUpdate(endpoint, peer);
    }
    return endpoint->driverBroken ? LAIUE_NET_IO_ERROR : LAIUE_NET_OK;
}

bool LaiueNetPollEvent(LaiueNetEndpoint *endpoint, LaiueNetEvent *outEvent)
{
    if (endpoint == NULL || outEvent == NULL || outEvent->structSize < sizeof(*outEvent) ||
        endpoint->eventHead == endpoint->eventCount)
        return false;
    const uint32_t structSize = outEvent->structSize;
    *outEvent = endpoint->events[endpoint->eventHead];
    outEvent->structSize = structSize;
    ++endpoint->eventHead;
    return true;
}

/* ---- Сведения ------------------------------------------------------------------------ */

static uint32_t Saturate32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

LaiueNetStatus LaiueNetPeerGetInfo(const LaiueNetEndpoint *endpoint, LaiueNetPeer handle,
                                   LaiueNetPeerInfo *outInfo)
{
    if (endpoint == NULL || outInfo == NULL || outInfo->structSize < sizeof(*outInfo))
        return LAIUE_NET_INVALID_ARGUMENT;
    const NetPeer *peer = PeerFromHandle(endpoint, handle);
    if (peer == NULL)
        return LAIUE_NET_NO_PEER;
    const uint32_t structSize = outInfo->structSize;
    memset(outInfo, 0, sizeof(*outInfo));
    outInfo->structSize = structSize;
    outInfo->address = peer->address;
    outInfo->userData = peer->userData;
    outInfo->state = LAIUE_NET_PEER_CONNECTING;
    if (peer->state == NET_STATE_CONNECTING)
        return LAIUE_NET_OK;
    outInfo->state = peer->state == NET_STATE_CONNECTED ? LAIUE_NET_PEER_CONNECTED
                                                        : LAIUE_NET_PEER_DISCONNECTING;
    outInfo->channelCount = peer->channelCount;
    outInfo->maxMessageBytes = peer->remote.maxMessageBytes;
    outInfo->maxUnreliableBytes = NetPeerMaxUnreliable(peer);
    if (peer->haveRtt)
    {
        outInfo->roundTripMicroseconds = Saturate32(peer->smoothedRtt);
        outInfo->roundTripVarianceMicroseconds = Saturate32(peer->rttVariance);
    }
    outInfo->congestionWindowBytes = Saturate32(peer->congestionWindow);
    outInfo->bytesInFlight = Saturate32(peer->bytesInFlight);
    outInfo->queuedReliableBytes = Saturate32(peer->queuedReliable);
    outInfo->queuedUnreliableBytes = NetPeerQueuedUnreliable(peer);
    outInfo->packetsSent = peer->packetsSent;
    outInfo->packetsReceived = peer->packetsReceived;
    outInfo->packetsLost = peer->packetsLost;
    outInfo->bytesSent = peer->bytesSent;
    outInfo->bytesReceived = peer->bytesReceived;
    outInfo->unreliableDropped = peer->unreliableDropped;
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetPeerSetUserData(LaiueNetEndpoint *endpoint, LaiueNetPeer handle,
                                       void *userData)
{
    if (endpoint == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    NetPeer *peer = PeerFromHandle(endpoint, handle);
    if (peer == NULL)
        return LAIUE_NET_NO_PEER;
    peer->userData = userData;
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetEndpointGetStats(const LaiueNetEndpoint *endpoint,
                                        LaiueNetEndpointStats *outStats)
{
    if (endpoint == NULL || outStats == NULL || outStats->structSize < sizeof(*outStats))
        return LAIUE_NET_INVALID_ARGUMENT;
    const uint32_t structSize = outStats->structSize;
    *outStats = endpoint->stats;
    outStats->structSize = structSize;
    outStats->peerCount = NetPeerCount(endpoint);
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetGenerateKeyPair(uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                       uint8_t publicKey[LAIUE_NET_KEY_BYTES])
{
    if (privateKey == NULL || publicKey == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (!PlatformRandomBytes(privateKey, LAIUE_NET_KEY_BYTES))
        return LAIUE_NET_IO_ERROR;
    NetX25519Public(privateKey, publicKey);
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetPublicKey(const uint8_t privateKey[LAIUE_NET_KEY_BYTES],
                                 uint8_t publicKey[LAIUE_NET_KEY_BYTES])
{
    if (privateKey == NULL || publicKey == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    NetX25519Public(privateKey, publicKey);
    return LAIUE_NET_OK;
}

uint64_t LaiueNetClockMicroseconds(void)
{
    return PlatformMonotonicNanoseconds() / 1000u;
}
