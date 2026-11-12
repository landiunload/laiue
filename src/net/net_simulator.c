#include "net/net.h"

#include "platform/system.h"

#include <string.h>

/* Симулятор сети: порты с адресами, между которыми датаграммы идут с
 * заданными задержкой, потерями, повторами, порчей и узким местом.
 *
 * Всё детерминировано: случайность берётся из зерна, время — только из
 * LaiueNetSimulatorAdvance, порядок доставки равных по времени датаграмм —
 * порядок отправки. Один и тот же тест видит одну и ту же сеть на любой
 * платформе. */

#define DEFAULT_QUEUED_DATAGRAMS 65536u
#define DEFAULT_QUEUE_MICROSECONDS 200000u
#define MICROSECONDS_PER_SECOND UINT64_C(1000000)
#define PER_MILLION 1000000u
/* Наибольшая полезная нагрузка датаграммы UDP. */
#define DATAGRAM_LIMIT 65507u

typedef struct SimDatagram
{
    uint64_t deliverAt;
    uint64_t order;
    LaiueNetAddress from;
    uint32_t size;
} SimDatagram;

typedef struct SimPort
{
    LaiueNetSimulator *simulator;
    LaiueNetAddress address;
    /* Двоичная куча по времени доставки. */
    SimDatagram **heap;
    uint32_t count;
    uint32_t capacity;
    /* Когда узкое место перед портом освободится. */
    uint64_t linkFreeAt;
} SimPort;

struct LaiueNetSimulator
{
    SimPort **ports;
    /* Порты по адресу: открытая адресация, в ячейке номер порта плюс один,
     * ноль — пусто. Ёмкость — степень двойки, вдвое больше числа портов:
     * поиск получателя каждой датаграммы не зависит от числа портов. */
    uint32_t *slots;
    uint32_t slotMask;
    uint32_t portCount;
    uint32_t portCapacity;
    LaiueNetSimulatorConditions conditions;
    LaiueNetSimulatorFilter filter;
    void *filterContext;
    uint64_t now;
    uint64_t order;
    uint64_t random;
    uint32_t queued;
    uint32_t maxQueued;
};

static uint8_t *DatagramBytes(SimDatagram *datagram)
{
    return (uint8_t *)(datagram + 1);
}

/* SplitMix64: короткий генератор с полным периодом. */
static uint64_t Random(LaiueNetSimulator *simulator)
{
    simulator->random += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t value = simulator->random;
    value = (value ^ (value >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27u)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31u);
}

static bool Chance(LaiueNetSimulator *simulator, uint32_t perMillion)
{
    return perMillion != 0u && Random(simulator) % PER_MILLION < perMillion;
}

static bool Earlier(const SimDatagram *first, const SimDatagram *second)
{
    return first->deliverAt != second->deliverAt ? first->deliverAt < second->deliverAt
                                                 : first->order < second->order;
}

static bool HeapPush(SimPort *port, SimDatagram *datagram)
{
    if (port->count == port->capacity)
    {
        const uint32_t capacity = port->capacity != 0u ? port->capacity * 2u : 64u;
        SimDatagram **heap = (SimDatagram **)PlatformReallocate(
            (void *)port->heap, (size_t)capacity * sizeof(SimDatagram *), false);
        if (heap == NULL)
            return false;
        port->heap = heap;
        port->capacity = capacity;
    }
    uint32_t index = port->count;
    ++port->count;
    while (index != 0u)
    {
        const uint32_t parent = (index - 1u) / 2u;
        if (!Earlier(datagram, port->heap[parent]))
            break;
        port->heap[index] = port->heap[parent];
        index = parent;
    }
    port->heap[index] = datagram;
    return true;
}

static SimDatagram *HeapPop(SimPort *port)
{
    SimDatagram *top = port->heap[0];
    --port->count;
    SimDatagram *last = port->heap[port->count];
    uint32_t index = 0u;
    for (;;)
    {
        uint32_t child = index * 2u + 1u;
        if (child >= port->count)
            break;
        if (child + 1u < port->count && Earlier(port->heap[child + 1u], port->heap[child]))
            ++child;
        if (!Earlier(port->heap[child], last))
            break;
        port->heap[index] = port->heap[child];
        index = child;
    }
    port->heap[index] = last;
    return top;
}

/* Хеш ровно тех полей, которые сравнивает LaiueNetAddressEqual: FNV-1a по
 * байтам адреса и перемешивание из MurmurHash3, чтобы младшие биты, по
 * которым выбирается ячейка, зависели и от порта. */
static uint32_t AddressHash(const LaiueNetAddress *address)
{
    const bool ipv4 = address->family == LAIUE_NET_FAMILY_IPV4;
    uint64_t hash = ((uint64_t)address->family << 48u) ^ ((uint64_t)address->port << 32u) ^
                    (ipv4 ? 0u : address->scope);
    const uint32_t length = ipv4 ? 4u : (uint32_t)sizeof(address->ip);
    for (uint32_t index = 0u; index < length; ++index)
        hash = (hash ^ address->ip[index]) * UINT64_C(0x100000001B3);
    hash = (hash ^ (hash >> 33u)) * UINT64_C(0xFF51AFD7ED558CCD);
    hash = (hash ^ (hash >> 33u)) * UINT64_C(0xC4CEB9FE1A85EC53);
    return (uint32_t)(hash ^ (hash >> 33u));
}

static SimPort *FindPort(const LaiueNetSimulator *simulator, const LaiueNetAddress *address)
{
    if (simulator->slots == NULL)
        return NULL;
    uint32_t slot = AddressHash(address) & simulator->slotMask;
    while (simulator->slots[slot] != 0u)
    {
        SimPort *port = simulator->ports[simulator->slots[slot] - 1u];
        if (LaiueNetAddressEqual(&port->address, address))
            return port;
        slot = (slot + 1u) & simulator->slotMask;
    }
    return NULL;
}

static void SlotInsert(LaiueNetSimulator *simulator, uint32_t portIndex)
{
    uint32_t slot = AddressHash(&simulator->ports[portIndex]->address) & simulator->slotMask;
    while (simulator->slots[slot] != 0u)
        slot = (slot + 1u) & simulator->slotMask;
    simulator->slots[slot] = portIndex + 1u;
}

/* Таблица для count портов заполнена не больше чем наполовину; при нехватке
 * строится вдвое большая. */
static bool SlotsReserve(LaiueNetSimulator *simulator, uint32_t count)
{
    const uint32_t capacity = simulator->slots != NULL ? simulator->slotMask + 1u : 0u;
    if ((uint64_t)count * 2u <= capacity)
        return true;
    if (count > (UINT32_C(1) << 30u))
        return false;
    uint32_t grown = capacity != 0u ? capacity * 2u : 16u;
    while (count * 2u > grown)
        grown *= 2u;
    uint32_t *slots = PlatformAllocate((size_t)grown * sizeof(uint32_t), true);
    if (slots == NULL)
        return false;
    PlatformFree(simulator->slots);
    simulator->slots = slots;
    simulator->slotMask = grown - 1u;
    for (uint32_t index = 0u; index < simulator->portCount; ++index)
        SlotInsert(simulator, index);
    return true;
}

/* Ставит датаграмму в очередь получателя; при переполнении она теряется. */
static void Deliver(LaiueNetSimulator *simulator, SimPort *target, SimDatagram *datagram)
{
    const LaiueNetSimulatorConditions *conditions = &simulator->conditions;
    uint64_t at = simulator->now;
    if (simulator->queued >= simulator->maxQueued)
    {
        PlatformFree(datagram);
        return;
    }
    if (conditions->bytesPerSecond != 0u)
    {
        const uint64_t queue = conditions->queueMicroseconds != 0u ? conditions->queueMicroseconds
                                                                   : DEFAULT_QUEUE_MICROSECONDS;
        const uint64_t start =
            target->linkFreeAt > simulator->now ? target->linkFreeAt : simulator->now;
        /* Очередь маршрутизатора полна: хвост отбрасывается. */
        if (start - simulator->now > queue)
        {
            PlatformFree(datagram);
            return;
        }
        at =
            start + (uint64_t)datagram->size * MICROSECONDS_PER_SECOND / conditions->bytesPerSecond;
        target->linkFreeAt = at;
    }
    at += conditions->latencyMicroseconds;
    if (conditions->jitterMicroseconds != 0u)
        at += Random(simulator) % ((uint64_t)conditions->jitterMicroseconds + 1u);
    datagram->deliverAt = at;
    datagram->order = simulator->order;
    ++simulator->order;
    if (HeapPush(target, datagram))
        ++simulator->queued;
    else
        PlatformFree(datagram);
}

static SimDatagram *CreateDatagram(const LaiueNetAddress *from, const uint8_t *bytes, uint32_t size)
{
    SimDatagram *datagram = PlatformAllocate(sizeof(*datagram) + size, false);
    if (datagram == NULL)
        return NULL;
    datagram->deliverAt = 0u;
    datagram->order = 0u;
    datagram->from = *from;
    datagram->size = size;
    memcpy(DatagramBytes(datagram), bytes, size);
    return datagram;
}

static int32_t SimulatorSend(void *context, const LaiueNetAddress *to, const void *bytes,
                             uint32_t size)
{
    SimPort *source = context;
    LaiueNetSimulator *simulator = source->simulator;
    const LaiueNetSimulatorConditions *conditions = &simulator->conditions;
    if (to == NULL || bytes == NULL || size == 0u || size > DATAGRAM_LIMIT)
        return 0;
    /* Дальше отправитель потери не видит: для него датаграмма ушла. */
    SimPort *target = FindPort(simulator, to);
    if (target == NULL)
        return 1;
    SimDatagram *datagram = CreateDatagram(&source->address, bytes, size);
    if (datagram == NULL)
        return 1;
    if ((simulator->filter != NULL && !simulator->filter(simulator->filterContext, &source->address,
                                                         to, DatagramBytes(datagram), size)) ||
        (conditions->maxDatagramBytes != 0u && size > conditions->maxDatagramBytes) ||
        Chance(simulator, conditions->lossPerMillion))
    {
        PlatformFree(datagram);
        return 1;
    }
    if (Chance(simulator, conditions->corruptPerMillion))
    {
        const uint64_t bit = Random(simulator) % ((uint64_t)size * 8u);
        DatagramBytes(datagram)[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
    if (Chance(simulator, conditions->duplicatePerMillion))
    {
        SimDatagram *copy = CreateDatagram(&source->address, DatagramBytes(datagram), size);
        if (copy != NULL)
            Deliver(simulator, target, copy);
    }
    Deliver(simulator, target, datagram);
    return 1;
}

static int32_t SimulatorReceive(void *context, LaiueNetAddress *outFrom, void *buffer,
                                uint32_t capacity)
{
    SimPort *port = context;
    LaiueNetSimulator *simulator = port->simulator;
    if (outFrom == NULL || buffer == NULL || capacity == 0u || port->count == 0u ||
        port->heap[0]->deliverAt > simulator->now)
        return 0;
    SimDatagram *datagram = HeapPop(port);
    --simulator->queued;
    const uint32_t copied = datagram->size < capacity ? datagram->size : capacity;
    memcpy(buffer, DatagramBytes(datagram), copied);
    *outFrom = datagram->from;
    PlatformFree(datagram);
    return (int32_t)copied;
}

LaiueNetStatus LaiueNetSimulatorCreate(const LaiueNetSimulatorDesc *desc,
                                       LaiueNetSimulator **outSimulator)
{
    if (outSimulator == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outSimulator = NULL;
    if (desc != NULL && desc->structSize < sizeof(*desc))
        return LAIUE_NET_INVALID_ARGUMENT;
    LaiueNetSimulator *simulator = PlatformAllocate(sizeof(*simulator), true);
    if (simulator == NULL)
        return LAIUE_NET_OUT_OF_MEMORY;
    simulator->maxQueued = desc != NULL && desc->maxQueuedDatagrams != 0u
                               ? desc->maxQueuedDatagrams
                               : DEFAULT_QUEUED_DATAGRAMS;
    simulator->random = desc != NULL ? desc->seed : 0u;
    simulator->conditions.structSize = sizeof(simulator->conditions);
    *outSimulator = simulator;
    return LAIUE_NET_OK;
}

void LaiueNetSimulatorDestroy(LaiueNetSimulator *simulator)
{
    if (simulator == NULL)
        return;
    for (uint32_t index = 0u; index < simulator->portCount; ++index)
    {
        SimPort *port = simulator->ports[index];
        for (uint32_t queued = 0u; queued < port->count; ++queued)
            PlatformFree(port->heap[queued]);
        PlatformFree((void *)port->heap);
        PlatformFree(port);
    }
    PlatformFree((void *)simulator->ports);
    PlatformFree(simulator->slots);
    PlatformFree(simulator);
}

LaiueNetStatus LaiueNetSimulatorOpen(LaiueNetSimulator *simulator, const LaiueNetAddress *address,
                                     LaiueNetDriverV1 *outDriver)
{
    if (simulator == NULL || address == NULL || outDriver == NULL ||
        FindPort(simulator, address) != NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (simulator->portCount == simulator->portCapacity)
    {
        const uint32_t capacity = simulator->portCapacity != 0u ? simulator->portCapacity * 2u : 8u;
        SimPort **ports = (SimPort **)PlatformReallocate(
            (void *)simulator->ports, (size_t)capacity * sizeof(SimPort *), false);
        if (ports == NULL)
            return LAIUE_NET_OUT_OF_MEMORY;
        simulator->ports = ports;
        simulator->portCapacity = capacity;
    }
    if (!SlotsReserve(simulator, simulator->portCount + 1u))
        return LAIUE_NET_OUT_OF_MEMORY;
    SimPort *port = PlatformAllocate(sizeof(*port), true);
    if (port == NULL)
        return LAIUE_NET_OUT_OF_MEMORY;
    port->simulator = simulator;
    port->address = *address;
    simulator->ports[simulator->portCount] = port;
    SlotInsert(simulator, simulator->portCount);
    ++simulator->portCount;
    outDriver->structSize = sizeof(*outDriver);
    outDriver->context = port;
    outDriver->send = SimulatorSend;
    outDriver->receive = SimulatorReceive;
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetSimulatorSetConditions(LaiueNetSimulator *simulator,
                                              const LaiueNetSimulatorConditions *conditions)
{
    if (simulator == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (conditions == NULL)
    {
        memset(&simulator->conditions, 0, sizeof(simulator->conditions));
        simulator->conditions.structSize = sizeof(simulator->conditions);
        return LAIUE_NET_OK;
    }
    if (conditions->structSize < sizeof(*conditions) || conditions->lossPerMillion > PER_MILLION ||
        conditions->duplicatePerMillion > PER_MILLION ||
        conditions->corruptPerMillion > PER_MILLION)
        return LAIUE_NET_INVALID_ARGUMENT;
    simulator->conditions = *conditions;
    simulator->conditions.structSize = sizeof(simulator->conditions);
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetSimulatorSetFilter(LaiueNetSimulator *simulator,
                                          LaiueNetSimulatorFilter filter, void *context)
{
    if (simulator == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    simulator->filter = filter;
    simulator->filterContext = context;
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetSimulatorAdvance(LaiueNetSimulator *simulator, uint64_t nowMicroseconds)
{
    if (simulator == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (nowMicroseconds > simulator->now)
        simulator->now = nowMicroseconds;
    return LAIUE_NET_OK;
}
