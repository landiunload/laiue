/* Отказы памяти в транспорте. Через эти обёртки выделяют память только
 * скопированные исходники конечной точки; симулятор сети, тест и платформа
 * пользуются обычным аллокатором.
 *
 * Сценарий — соединение, обмен надёжными сообщениями по сети с потерями и
 * перестановками, ненадёжные сообщения, закрытие с доставкой очереди —
 * повторяется, и в каждом прогоне отказывает очередное выделение: одно
 * (отказ временный) или все начиная с него (память кончилась). Ни один
 * отказ не роняет процесс и не теряет память, а соединение либо доставляет
 * всё по порядку, либо закрывается с понятной причиной. Серия идёт и с
 * CUBIC, и с BBR: соединению BBR память нужна и под модель пути. */

#include "net/net.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PROTOCOL UINT64_C(0x6C61697565666C74)
#define TICK UINT64_C(1000)
#define CHANNELS 2u
#define SCENARIO_MICROSECONDS UINT64_C(20000000)
#define FAILURE_LIMIT 4000u

static uint32_t allocateCalls, failAllocateCall, liveAllocations;
static bool failPersistently;
static uint8_t scratch[80000];

static const uint32_t sizes[] = {0u, 3u, 700u, 3000u, 70000u, 1u, 1500u};
#define MESSAGE_COUNT ((uint32_t)(sizeof(sizes) / sizeof(sizes[0])))

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net failure check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Fails(void)
{
    ++allocateCalls;
    return failAllocateCall != 0u && (allocateCalls == failAllocateCall ||
                                      (failPersistently && allocateCalls > failAllocateCall));
}

void *NetTestAllocate(size_t size, bool zero)
{
    if (Fails())
        return NULL;
    void *memory = PlatformAllocate(size, zero);
    if (memory != NULL)
        ++liveAllocations;
    return memory;
}

void *NetTestReallocate(void *memory, size_t size, bool zeroNew)
{
    if (Fails())
        return NULL;
    void *grown = PlatformReallocate(memory, size, zeroNew);
    if (memory == NULL && grown != NULL)
        ++liveAllocations;
    return grown;
}

void NetTestFree(void *memory)
{
    if (memory != NULL)
        --liveAllocations;
    PlatformFree(memory);
}

typedef struct Side
{
    LaiueNetEndpoint *endpoint;
    LaiueNetDriverV1 driver;
    LaiueNetAddress address;
    uint64_t randomState;
    LaiueNetPeer peer;
    bool announced;
    uint32_t connected;
    uint32_t disconnected;
    uint32_t reason;
    uint32_t received[CHANNELS];
    uint32_t sent[CHANNELS];
} Side;

static uint8_t Pattern(uint32_t direction, uint32_t channel, uint32_t index, uint32_t position)
{
    return (uint8_t)(direction * 59u + channel * 31u + index * 7u + position * 11u +
                     (position >> 8u));
}

static bool TestRandom(void *context, void *output, uint32_t size)
{
    uint64_t *state = context;
    uint8_t *bytes = output;
    for (uint32_t index = 0u; index < size; ++index)
    {
        *state ^= *state << 13u;
        *state ^= *state >> 7u;
        *state ^= *state << 17u;
        bytes[index] = (uint8_t)(*state >> 24u);
    }
    return true;
}

static void Handle(Side *side, uint32_t direction, const LaiueNetEvent *event)
{
    if (event->type == LAIUE_NET_EVENT_CONNECTED)
    {
        Expect(side->connected == 0u, "a peer connects once");
        ++side->connected;
        side->announced = true;
        side->peer = event->peer;
        return;
    }
    if (event->type == LAIUE_NET_EVENT_DISCONNECTED)
    {
        Expect(side->announced && event->peer == side->peer && side->disconnected == 0u,
               "every announced peer is closed exactly once");
        ++side->disconnected;
        side->announced = false;
        side->reason = event->reason;
        return;
    }
    Expect(event->type == LAIUE_NET_EVENT_MESSAGE && event->peer == side->peer, "message event");
    if (event->delivery != LAIUE_NET_RELIABLE)
        return;
    /* Надёжные сообщения не теряются и не переставляются, даже когда память
     * отказывает: либо следующее по порядку, либо соединение закрыто. */
    const uint32_t channel = event->channel;
    Expect(channel < CHANNELS && side->received[channel] < MESSAGE_COUNT, "known channel");
    const uint32_t index = side->received[channel];
    Expect(event->size == sizes[index], "reliable messages arrive whole and in order");
    const uint8_t *bytes = event->data;
    for (uint32_t position = 0u; position < event->size; ++position)
    {
        if (bytes[position] != Pattern(direction, channel, index, position))
            Expect(false, "reliable message bytes are intact");
    }
    ++side->received[channel];
}

static void Poll(Side *side, uint32_t direction)
{
    LaiueNetEvent event = {.structSize = sizeof(event)};
    while (LaiueNetPollEvent(side->endpoint, &event))
        Handle(side, direction, &event);
}

/* Докладывает очередные сообщения; отказ памяти в очереди — тоже ответ. */
static void Feed(Side *side, uint32_t direction)
{
    if (side->connected == 0u || side->disconnected != 0u)
        return;
    for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
    {
        while (side->sent[channel] < MESSAGE_COUNT)
        {
            const uint32_t index = side->sent[channel];
            for (uint32_t position = 0u; position < sizes[index]; ++position)
                scratch[position] = Pattern(direction, channel, index, position);
            const LaiueNetStatus status = LaiueNetSend(side->endpoint, side->peer, channel,
                                                       LAIUE_NET_RELIABLE, scratch, sizes[index]);
            Expect(status == LAIUE_NET_OK || status == LAIUE_NET_OUT_OF_MEMORY ||
                       status == LAIUE_NET_WOULD_BLOCK || status == LAIUE_NET_NOT_CONNECTED,
                   "a send either queues or reports why not");
            if (status != LAIUE_NET_OK)
                break;
            ++side->sent[channel];
        }
    }
    memset(scratch, 0x5A, 64u);
    const LaiueNetStatus status =
        LaiueNetSend(side->endpoint, side->peer, 1u, LAIUE_NET_SEQUENCED, scratch, 64u);
    Expect(status == LAIUE_NET_OK || status == LAIUE_NET_OUT_OF_MEMORY ||
               status == LAIUE_NET_NOT_CONNECTED,
           "an unreliable send either queues or reports why not");
}

static bool Delivered(const Side *receiver)
{
    for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
    {
        if (receiver->received[channel] != MESSAGE_COUNT)
            return false;
    }
    return true;
}

/* Один прогон с управлением перегрузкой control. true — отказ так и не
 * наступил: сценарий выделяет меньше. */
static bool RunScenario(uint32_t control, uint32_t failAt, bool persistent)
{
    LaiueNetSimulator *simulator = NULL;
    const LaiueNetSimulatorDesc simulatorDesc = {.structSize = sizeof(simulatorDesc), .seed = 3u};
    const LaiueNetSimulatorConditions conditions = {
        .structSize = sizeof(conditions),
        .latencyMicroseconds = 5000u,
        .jitterMicroseconds = 5000u,
        .lossPerMillion = 50000u,
    };
    Side sides[2];
    Side *server = &sides[0];
    Side *client = &sides[1];
    memset(sides, 0, sizeof(sides));
    Expect(LaiueNetSimulatorCreate(&simulatorDesc, &simulator) == LAIUE_NET_OK &&
               LaiueNetSimulatorSetConditions(simulator, &conditions) == LAIUE_NET_OK,
           "simulator");
    allocateCalls = 0u;
    failAllocateCall = failAt;
    failPersistently = persistent;

    for (uint32_t index = 0u; index < 2u; ++index)
    {
        Side *side = &sides[index];
        LaiueNetEndpointDesc desc = {
            .structSize = sizeof(desc),
            .flags = index == 0u ? LAIUE_NET_ENDPOINT_ACCEPT : 0u,
            .driver = &side->driver,
            .protocolId = PROTOCOL,
            .channelCount = CHANNELS,
            .receiveWindowBytes = 16384u,
            .random = TestRandom,
            .randomContext = &side->randomState,
            .congestionControl = control,
        };
        side->randomState = UINT64_C(0x9E3779B97F4A7C15) * (index + 1u);
        Expect(LaiueNetAddressParse(index == 0u ? "10.0.0.1:4000" : "10.0.0.2:5000",
                                    &side->address) == LAIUE_NET_OK &&
                   LaiueNetSimulatorOpen(simulator, &side->address, &side->driver) == LAIUE_NET_OK,
               "simulator port");
        const LaiueNetStatus status = LaiueNetEndpointCreate(&desc, &side->endpoint);
        Expect(status == LAIUE_NET_OK ||
                   (status == LAIUE_NET_OUT_OF_MEMORY && side->endpoint == NULL),
               "an endpoint is created or the failure is reported");
    }
    if (server->endpoint != NULL && client->endpoint != NULL)
    {
        LaiueNetConnectDesc connect = {.structSize = sizeof(connect), .address = server->address};
        const LaiueNetStatus status = LaiueNetConnect(client->endpoint, &connect, &client->peer);
        Expect(status == LAIUE_NET_OK || status == LAIUE_NET_OUT_OF_MEMORY,
               "a connection starts or the failure is reported");
        client->announced = status == LAIUE_NET_OK;
        bool flushing = false;
        const uint64_t duration = persistent ? SCENARIO_MICROSECONDS / 8u : SCENARIO_MICROSECONDS;
        for (uint64_t now = TICK; now <= duration; now += TICK)
        {
            Expect(LaiueNetSimulatorAdvance(simulator, now) == LAIUE_NET_OK, "advance");
            for (uint32_t index = 0u; index < 2u; ++index)
            {
                Expect(LaiueNetUpdate(sides[index].endpoint, now) == LAIUE_NET_OK,
                       "an update survives any allocation failure");
                Poll(&sides[index], index);
            }
            Feed(client, 0u);
            Feed(server, 1u);
            const bool done = Delivered(server) && Delivered(client);
            if (done && !flushing && client->announced)
            {
                Expect(LaiueNetDisconnect(client->endpoint, client->peer, 3u,
                                          LAIUE_NET_DISCONNECT_FLUSH) == LAIUE_NET_OK,
                       "flushing disconnect");
                flushing = true;
            }
            if (!client->announced && !server->announced && (flushing || client->disconnected))
                break;
            /* Без соединения сценарию дальше делать нечего. */
            if (status != LAIUE_NET_OK)
                break;
        }
        if (!persistent && status == LAIUE_NET_OK)
        {
            /* Временный отказ кончает соединение только нехваткой памяти одной из
             * сторон; иначе оно доставляет всё и закрывается обычным порядком. */
            Expect(client->disconnected != 0u, "the connection ends in time");
            const bool normal = flushing && client->reason == LAIUE_NET_REASON_LOCAL &&
                                server->reason == LAIUE_NET_REASON_REMOTE;
            Expect(normal || client->reason == LAIUE_NET_REASON_RESOURCE ||
                       server->reason == LAIUE_NET_REASON_RESOURCE,
                   "a connection ends normally or for lack of memory");
            Expect(!normal || (Delivered(server) && Delivered(client)),
                   "a normal close follows full delivery");
            Expect(!server->announced, "the server learns about the end");
        }
    }
    const bool reached = failAt != 0u && allocateCalls >= failAt;
    failAllocateCall = 0u;
    LaiueNetEndpointDestroy(client->endpoint);
    LaiueNetEndpointDestroy(server->endpoint);
    LaiueNetSimulatorDestroy(simulator);
    Expect(liveAllocations == 0u, "endpoints free everything they allocated");
    return !reached;
}

LAIUE_TEST_ENTRY(NetFailureTestEntryPoint)
{
    for (uint32_t control = LAIUE_NET_CONGESTION_CUBIC; control <= LAIUE_NET_CONGESTION_BBR;
         ++control)
    {
        /* Без отказов сценарий доставляет всё и закрывается обычным порядком. */
        Expect(RunScenario(control, 0u, false), "the scenario runs without failures");
        uint32_t transient = 0u;
        bool completed = false;
        for (uint32_t failAt = 1u; failAt <= FAILURE_LIMIT && !completed; ++failAt)
        {
            completed = RunScenario(control, failAt, false);
            if (!completed)
                ++transient;
        }
        Expect(completed, "the scenario allocates a bounded number of times");
        Expect(transient > 20u, "the scenario allocates, so failures were injected");
        /* Память кончилась насовсем: конечные точки живут дальше и всё отдают. */
        for (uint32_t failAt = 1u; failAt <= transient; failAt += 1u + failAt / 8u)
            (void)RunScenario(control, failAt, true);
    }
    LaiueTestRuntimeWrite("net failure: passed\n");
    LAIUE_TEST_SUCCESS();
}
