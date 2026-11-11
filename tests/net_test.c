/* Транспорт laiue.net на симуляторе сети. Время двигает сам тест, а
 * случайность берётся из зёрен, поэтому каждый прогон видит одну и ту же
 * сеть: с задержками, потерями, перестановками, повторами и порчей.
 *
 * Содержимое сообщений вычисляется из их места в потоке, так что получатель
 * проверяет не только доставку, но и порядок, целостность и адресата. */

#include "net/net.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NODE_CAPACITY 72u
#define FLOW_CAPACITY 64u
#define CHANNELS 3u
#define TICK 1000u
#define SECOND UINT64_C(1000000)
#define PROTOCOL UINT64_C(0x6C61697565746573)
#define UNRELIABLE_CAPACITY 4096u
#define SCRATCH_BYTES (UINT32_C(1) << 20u)

/* Поток сообщений одного направления одного соединения глазами получателя. */
typedef struct Flow
{
    LaiueNetPeer peer;
    bool identified;
    uint32_t identity;
    uint32_t direction;
    const uint32_t *sizes;
    uint32_t sizeCount;
    uint32_t nextReliable[CHANNELS];
    uint64_t reliableBytes;
    uint32_t unreliableCount;
    uint32_t sequencedCount;
    uint32_t lastSequenced;
    uint64_t worstLatency;
    uint8_t unreliableSeen[UNRELIABLE_CAPACITY / 8u];
} Flow;

typedef struct Node
{
    LaiueNetEndpoint *endpoint;
    LaiueNetDriverV1 driver;
    LaiueNetAddress address;
    uint64_t randomState;
    uint32_t index;
    bool muted;
    uint32_t connected;
    uint32_t disconnected;
    uint32_t messages;
    uint32_t lastReason;
    uint32_t lastCode;
    LaiueNetPeer peer;
    Flow flows[FLOW_CAPACITY];
    uint32_t flowCount;
} Node;

typedef struct World
{
    LaiueNetSimulator *simulator;
    uint64_t now;
    Node nodes[NODE_CAPACITY];
    uint32_t nodeCount;
} World;

static World world;
/* Сдвиг всех зёрен: LAIUE_NET_TEST_SEED прогоняет те же сценарии на других сетях. */
static uint64_t seedOffset;
/* Когда отправлено ненадёжное сообщение с данным номером; 0 — не отмечено. */
static uint64_t unreliableSentAt[UNRELIABLE_CAPACITY];
static uint8_t scratch[SCRATCH_BYTES];

static const uint32_t smallSizes[] = {0u, 1u, 2u, 5u, 60u, 300u, 1100u, 1200u, 2500u};
static const uint32_t mixedSizes[] = {0u,    1u,    7u,     130u, 1150u,  1171u,
                                      1300u, 9000u, 40000u, 3u,   150000u};
static const uint32_t bulkSizes[] = {262144u};

#define COUNT_OF(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))

static void WriteNumber(uint64_t value)
{
    char text[21];
    uint32_t length = 0u;
    do
    {
        text[length] = (char)('0' + value % 10u);
        value /= 10u;
        ++length;
    } while (value != 0u);
    char ordered[21];
    for (uint32_t index = 0u; index < length; ++index)
        ordered[index] = text[length - 1u - index];
    ordered[length] = '\0';
    LaiueTestRuntimeWrite(ordered);
}

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(" (simulated microsecond ");
    WriteNumber(world.now);
    LaiueTestRuntimeWrite(")\n");
    LaiueTestRuntimeExit(1);
}

static void ExpectEqual(uint64_t actual, uint64_t expected, const char *message)
{
    if (actual == expected)
        return;
    LaiueTestRuntimeWrite("Net check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(": got ");
    WriteNumber(actual);
    LaiueTestRuntimeWrite(", expected ");
    WriteNumber(expected);
    LaiueTestRuntimeWrite(" (simulated microsecond ");
    WriteNumber(world.now);
    LaiueTestRuntimeWrite(")\n");
    LaiueTestRuntimeExit(1);
}

/* ---- Содержимое сообщений ---------------------------------------------------------- */

static uint8_t Pattern(uint32_t identity, uint32_t direction, uint32_t channel, uint32_t index,
                       uint32_t position)
{
    return (uint8_t)(identity * 131u + direction * 17u + channel * 29u + index * 7u +
                     position * 13u + (position >> 8u) * 3u);
}

static void Fill(uint8_t *bytes, uint32_t size, uint32_t identity, uint32_t direction,
                 uint32_t channel, uint32_t index)
{
    for (uint32_t position = 0u; position < size; ++position)
        bytes[position] = Pattern(identity, direction, channel, index, position);
}

static void Store32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static uint32_t Load32(const uint8_t *bytes)
{
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)bytes[index] << (index * 8u);
    return value;
}

/* Первое надёжное сообщение потока в нулевом канале называет отправителя;
 * остальные идут по расписанию размеров, сдвинутому на его номер. */
static uint32_t ScheduledSize(const Flow *flow, uint32_t channel, uint32_t index)
{
    return flow->sizes[(index + channel + flow->identity) % flow->sizeCount];
}

static void FlowReliable(Flow *flow, const LaiueNetEvent *event)
{
    const uint8_t *bytes = event->data;
    Expect(event->channel < CHANNELS, "reliable message names a negotiated channel");
    if (!flow->identified)
    {
        Expect(event->channel == 0u && event->size == 4u, "a flow starts with its identity");
        flow->identity = Load32(bytes);
        flow->identified = true;
        return;
    }
    const uint32_t index = flow->nextReliable[event->channel];
    ++flow->nextReliable[event->channel];
    ExpectEqual(event->size, ScheduledSize(flow, event->channel, index),
                "reliable message arrives whole and in order");
    for (uint32_t position = 0u; position < event->size; ++position)
    {
        if (bytes[position] !=
            Pattern(flow->identity, flow->direction, event->channel, index, position))
            Expect(false, "reliable message bytes are intact");
    }
    flow->reliableBytes += event->size;
}

static void FlowUnreliable(Flow *flow, const LaiueNetEvent *event)
{
    const uint8_t *bytes = event->data;
    Expect(event->size >= 4u, "unreliable message carries its number");
    const uint32_t index = Load32(bytes);
    Expect(index < UNRELIABLE_CAPACITY, "unreliable message number is in range");
    for (uint32_t position = 4u; position < event->size; ++position)
    {
        if (bytes[position] !=
            Pattern(flow->identity, flow->direction, event->channel, index, position))
            Expect(false, "unreliable message bytes are intact");
    }
    if (event->delivery == LAIUE_NET_SEQUENCED)
    {
        Expect(index > flow->lastSequenced, "sequenced message is newer than the last one");
        flow->lastSequenced = index;
        ++flow->sequencedCount;
        return;
    }
    if (unreliableSentAt[index] != 0u && world.now - unreliableSentAt[index] > flow->worstLatency)
        flow->worstLatency = world.now - unreliableSentAt[index];
    Expect((flow->unreliableSeen[index / 8u] & (1u << (index % 8u))) == 0u,
           "unreliable message is not delivered twice");
    flow->unreliableSeen[index / 8u] |= (uint8_t)(1u << (index % 8u));
    ++flow->unreliableCount;
}

static void NodeHandle(Node *node, const LaiueNetEvent *event)
{
    Flow *flow = event->userData;
    if (event->type == LAIUE_NET_EVENT_CONNECTED)
    {
        ++node->connected;
        node->peer = event->peer;
        if (flow == NULL)
        {
            /* Входящее соединение: поток заводится по событию. */
            Expect(node->flowCount < FLOW_CAPACITY, "flow table has room");
            flow = &node->flows[node->flowCount];
            ++node->flowCount;
            flow->peer = event->peer;
            flow->direction = 1u;
            flow->sizes = mixedSizes;
            flow->sizeCount = COUNT_OF(mixedSizes);
            Expect(LaiueNetPeerSetUserData(node->endpoint, event->peer, flow) == LAIUE_NET_OK,
                   "user data attaches to an incoming peer");
        }
    }
    else if (event->type == LAIUE_NET_EVENT_DISCONNECTED)
    {
        ++node->disconnected;
        node->lastReason = event->reason;
        node->lastCode = event->code;
        if (node->peer == event->peer)
            node->peer = 0u;
    }
    else
    {
        Expect(event->type == LAIUE_NET_EVENT_MESSAGE && flow != NULL,
               "message belongs to a known peer");
        ++node->messages;
        if (event->delivery == LAIUE_NET_RELIABLE)
            FlowReliable(flow, event);
        else
            FlowUnreliable(flow, event);
    }
}

/* ---- Мир ---------------------------------------------------------------------------- */

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

static LaiueNetAddress Address(const char *text)
{
    LaiueNetAddress address;
    Expect(LaiueNetAddressParse(text, &address) == LAIUE_NET_OK, "test address parses");
    return address;
}

static void WorldBeginLimited(uint64_t seed, uint32_t maxQueuedDatagrams)
{
    const LaiueNetSimulatorDesc desc = {
        .structSize = sizeof(desc),
        .maxQueuedDatagrams = maxQueuedDatagrams,
        .seed = seed + seedOffset * UINT64_C(0x100000001B3),
    };
    memset(&world, 0, sizeof(world));
    Expect(LaiueNetSimulatorCreate(&desc, &world.simulator) == LAIUE_NET_OK, "simulator creates");
}

static void WorldBegin(uint64_t seed)
{
    WorldBeginLimited(seed, 0u);
}

static LaiueNetEndpointDesc DefaultDesc(uint32_t flags)
{
    const LaiueNetEndpointDesc desc = {
        .structSize = sizeof(desc),
        .flags = flags,
        .protocolId = PROTOCOL,
        .channelCount = CHANNELS,
    };
    return desc;
}

/* Открывает порт симулятора и ставит на него конечную точку. */
static Node *WorldAdd(const char *address, const LaiueNetEndpointDesc *desc)
{
    Expect(world.nodeCount < NODE_CAPACITY, "node table has room");
    Node *node = &world.nodes[world.nodeCount];
    node->index = world.nodeCount;
    ++world.nodeCount;
    node->address = Address(address);
    node->randomState = UINT64_C(0x9E3779B97F4A7C15) * (node->index + 1u + seedOffset * 977u);
    Expect(LaiueNetSimulatorOpen(world.simulator, &node->address, &node->driver) == LAIUE_NET_OK,
           "simulator port opens");
    if (desc != NULL)
    {
        LaiueNetEndpointDesc resolved = *desc;
        resolved.driver = &node->driver;
        resolved.random = TestRandom;
        resolved.randomContext = &node->randomState;
        Expect(LaiueNetEndpointCreate(&resolved, &node->endpoint) == LAIUE_NET_OK,
               "endpoint creates");
    }
    return node;
}

static void NodePoll(Node *node)
{
    LaiueNetEvent event = {.structSize = sizeof(event)};
    while (LaiueNetPollEvent(node->endpoint, &event))
        NodeHandle(node, &event);
}

static void WorldStep(void)
{
    world.now += TICK;
    Expect(LaiueNetSimulatorAdvance(world.simulator, world.now) == LAIUE_NET_OK,
           "simulator advances");
    for (uint32_t index = 0u; index < world.nodeCount; ++index)
    {
        Node *node = &world.nodes[index];
        if (node->endpoint == NULL)
            continue;
        Expect(LaiueNetUpdate(node->endpoint, world.now) == LAIUE_NET_OK, "endpoint updates");
        if (!node->muted)
            NodePoll(node);
    }
}

static void WorldRun(uint64_t microseconds)
{
    const uint64_t until = world.now + microseconds;
    while (world.now < until)
        WorldStep();
}

static void WorldEnd(void)
{
    for (uint32_t index = 0u; index < world.nodeCount; ++index)
        LaiueNetEndpointDestroy(world.nodes[index].endpoint);
    LaiueNetSimulatorDestroy(world.simulator);
    world.simulator = NULL;
}

static void SetConditions(uint32_t latency, uint32_t jitter, uint32_t loss, uint32_t duplicate,
                          uint32_t corrupt)
{
    const LaiueNetSimulatorConditions conditions = {
        .structSize = sizeof(conditions),
        .latencyMicroseconds = latency,
        .jitterMicroseconds = jitter,
        .lossPerMillion = loss,
        .duplicatePerMillion = duplicate,
        .corruptPerMillion = corrupt,
    };
    Expect(LaiueNetSimulatorSetConditions(world.simulator, &conditions) == LAIUE_NET_OK,
           "conditions apply");
}

/* Начинает соединение клиента с сервером; поток клиента — его нулевой. */
static LaiueNetPeer BeginConnect(Node *client, const Node *server, const uint8_t *pinnedKey)
{
    Flow *flow = &client->flows[0];
    memset(flow, 0, sizeof(*flow));
    flow->direction = 2u;
    flow->sizes = mixedSizes;
    flow->sizeCount = COUNT_OF(mixedSizes);
    client->flowCount = 1u;
    const LaiueNetConnectDesc desc = {
        .structSize = sizeof(desc),
        .address = server->address,
        .serverPublicKey = pinnedKey,
        .userData = flow,
    };
    LaiueNetPeer peer = 0u;
    Expect(LaiueNetConnect(client->endpoint, &desc, &peer) == LAIUE_NET_OK && peer != 0u,
           "connect starts");
    flow->peer = peer;
    return peer;
}

static Flow *ServerFlow(Node *server, uint32_t identity)
{
    for (uint32_t index = 0u; index < server->flowCount; ++index)
    {
        if (server->flows[index].identified && server->flows[index].identity == identity)
            return &server->flows[index];
    }
    return NULL;
}

static void SendIdentity(Node *node, LaiueNetPeer peer, uint32_t identity)
{
    uint8_t bytes[4];
    Store32(bytes, identity);
    Expect(LaiueNetSend(node->endpoint, peer, 0u, LAIUE_NET_RELIABLE, bytes, 4u) == LAIUE_NET_OK,
           "identity is queued");
}

/* Соединяет клиента с сервером и обменивается именами: дальше обе стороны
 * проверяют содержимое по расписанию. */
static LaiueNetPeer Connect(Node *client, Node *server, uint64_t limit)
{
    const uint32_t serverConnected = server->connected;
    const LaiueNetPeer peer = BeginConnect(client, server, NULL);
    const uint64_t until = world.now + limit;
    while ((client->connected == 0u || server->connected == serverConnected) && world.now < until)
        WorldStep();
    Expect(client->connected == 1u && server->connected == serverConnected + 1u,
           "both sides report the connection");
    SendIdentity(client, peer, client->index);
    Flow *reply = NULL;
    while (reply == NULL && world.now < until)
    {
        WorldStep();
        reply = ServerFlow(server, client->index);
    }
    Expect(reply != NULL, "server learns the client's identity");
    SendIdentity(server, reply->peer, client->index);
    while (!client->flows[0].identified && world.now < until)
        WorldStep();
    Expect(client->flows[0].identified && client->flows[0].identity == client->index,
           "client is addressed by its own identity");
    return peer;
}

static LaiueNetStatus SendScheduled(Node *node, LaiueNetPeer peer, uint32_t direction,
                                    uint32_t identity, const uint32_t *sizes, uint32_t sizeCount,
                                    uint32_t channel, uint32_t index)
{
    const uint32_t size = sizes[(index + channel + identity) % sizeCount];
    Fill(scratch, size, identity, direction, channel, index);
    return LaiueNetSend(node->endpoint, peer, channel, LAIUE_NET_RELIABLE, scratch, size);
}

static LaiueNetStatus SendNumbered(Node *node, LaiueNetPeer peer, uint32_t direction,
                                   uint32_t identity, LaiueNetDelivery delivery, uint32_t channel,
                                   uint32_t index, uint32_t size)
{
    Fill(scratch, size, identity, direction, channel, index);
    Store32(scratch, index);
    return LaiueNetSend(node->endpoint, peer, channel, delivery, scratch, size);
}

/* ---- Адреса --------------------------------------------------------------------------- */

static uint32_t TextLength(const char *text)
{
    uint32_t length = 0u;
    while (text[length] != 0)
        ++length;
    return length;
}

static void ExpectRoundTrip(const char *text, uint8_t family, uint16_t port)
{
    char formatted[LAIUE_NET_ADDRESS_TEXT_CAPACITY];
    LaiueNetAddress address;
    Expect(LaiueNetAddressParse(text, &address) == LAIUE_NET_OK, text);
    Expect(address.family == family && address.port == port, "address family and port");
    const uint32_t length = LaiueNetAddressFormat(&address, formatted, sizeof(formatted));
    Expect(length == TextLength(text) && memcmp(formatted, text, length + 1u) == 0, text);
}

static void TestAddresses(void)
{
    static const char *const invalid[] = {
        "",
        "1.2.3",
        "1.2.3.4.5",
        "256.1.1.1",
        "01.2.3.4",
        "1.2.3.4:",
        "1.2.3.4:65536",
        "1.2.3.4:0x10",
        "1.2.3.4 ",
        "[::1",
        "[::1]x",
        "[::1]:",
        "1::2::3",
        ":::",
        "12345::",
        "[1.2.3.4]:5",
        "::1%",
        "g::1",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7",
        ":1",
        "1:",
        "::1.2.3",
        "1.2.3.4:-1",
    };
    LaiueNetAddress address;
    LaiueNetAddress other;
    char small[8];
    char small16[16];
    ExpectRoundTrip("203.0.113.7:4000", LAIUE_NET_FAMILY_IPV4, 4000u);
    ExpectRoundTrip("0.0.0.0", LAIUE_NET_FAMILY_IPV4, 0u);
    ExpectRoundTrip("[2001:db8::1]:4000", LAIUE_NET_FAMILY_IPV6, 4000u);
    ExpectRoundTrip("::1", LAIUE_NET_FAMILY_IPV6, 0u);
    ExpectRoundTrip("::", LAIUE_NET_FAMILY_IPV6, 0u);
    ExpectRoundTrip("1::", LAIUE_NET_FAMILY_IPV6, 0u);
    ExpectRoundTrip("[fe80::1%3]:9", LAIUE_NET_FAMILY_IPV6, 9u);
    ExpectRoundTrip("2001:db8:0:1:1:1:1:1", LAIUE_NET_FAMILY_IPV6, 0u);
    ExpectRoundTrip("[1:2:3:4:5:6:7:8]:65535", LAIUE_NET_FAMILY_IPV6, 65535u);
    Expect(LaiueNetAddressParse("::FFFF:1.2.3.4", &address) == LAIUE_NET_OK &&
               address.ip[10] == 0xFFu && address.ip[11] == 0xFFu && address.ip[12] == 1u &&
               address.ip[15] == 4u,
           "embedded IPv4 fills the last four bytes");
    Expect(LaiueNetAddressParse("1:0:0:2:0:0:0:3", &address) == LAIUE_NET_OK &&
               LaiueNetAddressFormat(&address, small16, sizeof(small16)) == 10u &&
               memcmp(small16, "1:0:0:2::3", 11u) == 0,
           "the longest zero run is the one compressed");
    for (uint32_t index = 0u; index < COUNT_OF(invalid); ++index)
        Expect(LaiueNetAddressParse(invalid[index], &address) == LAIUE_NET_INVALID_ARGUMENT,
               invalid[index]);
    Expect(LaiueNetAddressParse(NULL, &address) == LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetAddressParse("::1", NULL) == LAIUE_NET_INVALID_ARGUMENT,
           "null arguments are refused");
    address = Address("203.0.113.7:4000");
    Expect(LaiueNetAddressFormat(&address, small, sizeof(small)) == 0u && small[0] == '\0',
           "a short buffer gets an empty string");
    other = Address("203.0.113.7:4001");
    Expect(!LaiueNetAddressEqual(&address, &other), "ports distinguish addresses");
    other.port = 4000u;
    other.ip[9] = 77u;
    Expect(LaiueNetAddressEqual(&address, &other), "IPv4 compares four bytes");
    other = Address("[::ffff:203.0.113.7]:4000");
    Expect(!LaiueNetAddressEqual(&address, &other), "families distinguish addresses");
}

/* ---- Жизненный цикл ------------------------------------------------------------------- */

static void TestLifecycle(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    LaiueNetEvent event = {.structSize = 4u};
    WorldBegin(1u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    const LaiueNetPeer peer = BeginConnect(client, server, NULL);
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.state == LAIUE_NET_PEER_CONNECTING && info.userData == &client->flows[0] &&
               LaiueNetAddressEqual(&info.address, &server->address),
           "a starting connection is visible");
    Expect(LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, "x", 1u) ==
               LAIUE_NET_NOT_CONNECTED,
           "nothing is sent before the connection exists");
    client->muted = true;
    WorldRun(20u * TICK);
    Expect(!LaiueNetPollEvent(client->endpoint, &event), "a short event structure is refused");
    client->muted = false;
    NodePoll(client);
    Expect(client->connected == 1u && server->connected == 1u, "handshake completes");

    SendIdentity(client, peer, client->index);
    WorldRun(5u * TICK);
    Flow *serverFlow = ServerFlow(server, client->index);
    Expect(serverFlow != NULL, "the first message arrives");
    SendIdentity(server, serverFlow->peer, client->index);
    for (uint32_t index = 0u; index < 22u; ++index)
    {
        for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
        {
            Expect(SendScheduled(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes),
                                 channel, index) == LAIUE_NET_OK,
                   "client message is queued");
            Expect(SendScheduled(server, serverFlow->peer, 2u, client->index, mixedSizes,
                                 COUNT_OF(mixedSizes), channel, index) == LAIUE_NET_OK,
                   "server message is queued");
        }
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 1u, index,
                            40u + index) == LAIUE_NET_OK,
               "unreliable message is queued");
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_SEQUENCED, 2u, index + 1u,
                            4u) == LAIUE_NET_OK,
               "sequenced message is queued");
        WorldStep();
    }
    WorldRun(SECOND);
    for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
    {
        ExpectEqual(serverFlow->nextReliable[channel], 22u, "server received every message");
        ExpectEqual(client->flows[0].nextReliable[channel], 22u, "client received every message");
    }
    ExpectEqual(serverFlow->unreliableCount, 22u, "an ideal network loses nothing");
    ExpectEqual(serverFlow->lastSequenced, 22u, "the newest sequenced message arrives");

    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.state == LAIUE_NET_PEER_CONNECTED && info.channelCount == CHANNELS &&
               info.maxMessageBytes == (1u << 20u) && info.maxUnreliableBytes == 1200u - 43u &&
               info.packetsSent != 0u && info.packetsReceived != 0u && info.packetsLost == 0u &&
               info.queuedReliableBytes == 0u && info.bytesInFlight == 0u &&
               info.roundTripMicroseconds != 0u && info.congestionWindowBytes >= 12000u,
           "peer information describes the connection");
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.peerCount == 1u && stats.datagramsSent != 0u &&
               stats.datagramsReceived != 0u && stats.datagramsDropped == 0u &&
               stats.bytesSent > stats.datagramsSent,
           "endpoint statistics count the traffic");

    Expect(LaiueNetSend(client->endpoint, peer, CHANNELS, LAIUE_NET_RELIABLE, "x", 1u) ==
                   LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetSend(client->endpoint, peer, 0u, 3u, "x", 1u) ==
                   LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, NULL, 1u) ==
                   LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetSend(client->endpoint, peer + 1u, 0u, LAIUE_NET_RELIABLE, "x", 1u) ==
                   LAIUE_NET_NO_PEER &&
               LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_UNRELIABLE, scratch, 1158u) ==
                   LAIUE_NET_TOO_LARGE &&
               LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, scratch,
                            (1u << 20u) + 1u) == LAIUE_NET_TOO_LARGE &&
               LaiueNetDisconnect(client->endpoint, peer, 0u, 2u) == LAIUE_NET_INVALID_ARGUMENT,
           "bad sends are refused without side effects");

    Expect(LaiueNetDisconnect(client->endpoint, peer, 7u, 0u) == LAIUE_NET_OK, "disconnect");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_NO_PEER &&
               LaiueNetDisconnect(client->endpoint, peer, 7u, 0u) == LAIUE_NET_NO_PEER &&
               LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, "x", 1u) ==
                   LAIUE_NET_NO_PEER,
           "a closed handle is stale at once");
    WorldRun(10u * TICK);
    Expect(client->disconnected == 1u && client->lastReason == LAIUE_NET_REASON_LOCAL &&
               client->lastCode == 7u,
           "the closing side is told once");
    Expect(server->disconnected == 1u && server->lastReason == LAIUE_NET_REASON_REMOTE &&
               server->lastCode == 7u,
           "the other side learns the code");
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.peerCount == 0u,
           "a closed peer leaves the table");
    WorldEnd();
}

/* ---- Рукопожатие: отказы и проверка подлинности ------------------------------------- */

static void ExpectRefused(const LaiueNetEndpointDesc *serverDesc,
                          const LaiueNetEndpointDesc *clientDesc, const uint8_t *pinnedKey,
                          uint32_t reason, uint32_t code, const char *message)
{
    WorldBegin(2u);
    Node *server = WorldAdd("10.0.0.1:4000", serverDesc);
    Node *client = WorldAdd("10.0.0.2:5000", clientDesc);
    (void)BeginConnect(client, server, pinnedKey);
    WorldRun(SECOND);
    Expect(client->connected == 0u && client->disconnected == 1u, message);
    ExpectEqual(client->lastReason, reason, message);
    ExpectEqual(client->lastCode, code, message);
    Expect(server->connected == 0u && server->disconnected == 0u,
           "a refused client never reaches the application");
    WorldEnd();
}

static void TestHandshakePolicies(void)
{
    static const uint8_t secret[LAIUE_NET_KEY_BYTES] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    static const uint8_t otherSecret[LAIUE_NET_KEY_BYTES] = {9u, 9u, 9u};
    uint8_t identity[LAIUE_NET_KEY_BYTES];
    uint8_t identityPublic[LAIUE_NET_KEY_BYTES];
    uint8_t strangerPublic[LAIUE_NET_KEY_BYTES];
    LaiueNetEndpointDesc serverDesc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpointDesc clientDesc = DefaultDesc(0u);
    memset(identity, 0x42, sizeof(identity));
    Expect(LaiueNetPublicKey(identity, identityPublic) == LAIUE_NET_OK &&
               LaiueNetPublicKey(secret, strangerPublic) == LAIUE_NET_OK &&
               memcmp(identityPublic, strangerPublic, sizeof(identityPublic)) != 0,
           "public keys derive from private ones");
    serverDesc.handshakeTimeoutMilliseconds = 400u;
    clientDesc.handshakeTimeoutMilliseconds = 400u;

    clientDesc.protocolId = PROTOCOL + 1u;
    ExpectRefused(&serverDesc, &clientDesc, NULL, LAIUE_NET_REASON_REJECTED,
                  LAIUE_NET_REJECT_PROTOCOL, "another protocol is rejected");
    clientDesc.protocolId = PROTOCOL;
    ExpectRefused(&serverDesc, &clientDesc, identityPublic, LAIUE_NET_REASON_REJECTED,
                  LAIUE_NET_REJECT_IDENTITY, "a pinned key needs a server identity");
    clientDesc.presharedKey = secret;
    ExpectRefused(&serverDesc, &clientDesc, NULL, LAIUE_NET_REASON_REJECTED,
                  LAIUE_NET_REJECT_PRESHARED_KEY, "a secret on one side only is rejected");
    serverDesc.presharedKey = otherSecret;
    ExpectRefused(&serverDesc, &clientDesc, NULL, LAIUE_NET_REASON_AUTHENTICATION, 0u,
                  "different secrets do not connect");
    serverDesc.presharedKey = secret;
    serverDesc.identityPrivateKey = identity;
    ExpectRefused(&serverDesc, &clientDesc, strangerPublic, LAIUE_NET_REASON_AUTHENTICATION, 0u,
                  "a server without the pinned key is not trusted");
    /* Конечная точка без приёма молчит, как и адрес без порта. */
    ExpectRefused(&clientDesc, &clientDesc, NULL, LAIUE_NET_REASON_TIMEOUT, 0u,
                  "an endpoint that does not accept stays silent");

    /* Треть датаграмм теряется в обе стороны: рукопожатию нужно несколько
     * удачных оборотов подряд, и срок ему даётся с запасом. */
    LaiueNetEndpointDesc patientDesc = clientDesc;
    patientDesc.handshakeTimeoutMilliseconds = 20000u;
    serverDesc.handshakeTimeoutMilliseconds = 20000u;
    WorldBegin(3u);
    Node *server = WorldAdd("10.0.0.1:4000", &serverDesc);
    Node *client = WorldAdd("10.0.0.2:5000", &patientDesc);
    Node *other = WorldAdd("10.0.0.3:5000", &clientDesc);
    SetConditions(30000u, 20000u, 300000u, 100000u, 0u);
    const LaiueNetPeer peer = BeginConnect(client, server, identityPublic);
    const uint64_t until = world.now + 30u * SECOND;
    while ((client->connected == 0u || server->connected == 0u) && world.now < until)
        WorldStep();
    Expect(client->connected == 1u && server->connected == 1u && client->disconnected == 0u,
           "a pinned key and a shared secret connect through a lossy network");
    LaiueNetConnectDesc nowhere = {.structSize = sizeof(nowhere)};
    nowhere.address = Address("10.0.0.99:1");
    LaiueNetPeer lost = 0u;
    Expect(LaiueNetConnect(other->endpoint, &nowhere, &lost) == LAIUE_NET_OK, "connect starts");
    WorldRun(SECOND);
    Expect(other->disconnected == 1u && other->lastReason == LAIUE_NET_REASON_TIMEOUT,
           "nobody answers at an empty address");
    Expect(LaiueNetDisconnect(client->endpoint, peer, 0u, 0u) == LAIUE_NET_OK, "disconnect");
    WorldEnd();

    serverDesc.maxPeers = 1u;
    WorldBegin(4u);
    server = WorldAdd("10.0.0.1:4000", &serverDesc);
    client = WorldAdd("10.0.0.2:5000", &clientDesc);
    other = WorldAdd("10.0.0.3:5000", &clientDesc);
    (void)Connect(client, server, SECOND);
    (void)BeginConnect(other, server, NULL);
    WorldRun(SECOND);
    Expect(other->disconnected == 1u && other->lastReason == LAIUE_NET_REASON_REJECTED &&
               other->lastCode == LAIUE_NET_REJECT_FULL,
           "a full server says so");
    LaiueNetPeer extra = 0u;
    nowhere.address = server->address;
    Expect(LaiueNetConnect(client->endpoint, &nowhere, &extra) == LAIUE_NET_OK &&
               LaiueNetConnect(server->endpoint, &nowhere, &extra) == LAIUE_NET_LIMIT_REACHED,
           "the peer limit also covers outgoing connections");
    WorldEnd();
}

/* ---- Плохая сеть ---------------------------------------------------------------------- */

/* Докладывает очередные сообщения, пока очередь отправки принимает. */
static void Pump(Node *node, LaiueNetPeer peer, uint32_t direction, uint32_t identity,
                 const uint32_t *sizes, uint32_t sizeCount, uint32_t *next, uint32_t total)
{
    for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
    {
        while (next[channel] < total)
        {
            const LaiueNetStatus status = SendScheduled(node, peer, direction, identity, sizes,
                                                        sizeCount, channel, next[channel]);
            if (status == LAIUE_NET_WOULD_BLOCK)
                break;
            Expect(status == LAIUE_NET_OK, "pumped message is queued");
            ++next[channel];
        }
    }
}

static bool AllReceived(const Flow *flow, uint32_t total)
{
    for (uint32_t channel = 0u; channel < CHANNELS; ++channel)
    {
        if (flow->nextReliable[channel] != total)
            return false;
    }
    return true;
}

static void TestLossyNetwork(void)
{
    const uint32_t total = 33u;
    uint32_t clientNext[CHANNELS] = {0u};
    uint32_t serverNext[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    WorldBegin(5u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("[2001:db8::2]:5000", &desc);
    SetConditions(20000u, 20000u, 60000u, 30000u, 20000u);
    const LaiueNetPeer peer = Connect(client, server, 30u * SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    const uint64_t until = world.now + 90u * SECOND;
    while ((!AllReceived(serverFlow, total) || !AllReceived(&client->flows[0], total)) &&
           world.now < until)
    {
        Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), clientNext, total);
        Pump(server, serverFlow->peer, 2u, client->index, mixedSizes, COUNT_OF(mixedSizes),
             serverNext, total);
        WorldStep();
    }
    Expect(AllReceived(serverFlow, total) && AllReceived(&client->flows[0], total),
           "every message survives loss, reordering, duplication and corruption");
    Expect(client->disconnected == 0u && server->disconnected == 0u,
           "a bad network does not break the connection");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.packetsLost != 0u && info.roundTripMicroseconds > 40000u &&
               info.roundTripMicroseconds < 200000u,
           "losses are detected and the round trip is measured");
    WorldRun(2u * SECOND);
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.queuedReliableBytes == 0u && info.bytesInFlight == 0u,
           "everything sent is acknowledged in the end");
    WorldEnd();
}

/* Путь без потерь, но длинный и широкий: в пути больше пакетов, чем помнит
 * журнал отправленных. Отправитель ждёт подтверждений, а не объявляет
 * потерянными собственные пакеты. */
static void TestDeepPipe(void)
{
    const uint32_t total = 96u;
    uint32_t next[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    desc.receiveWindowBytes = 16u << 20u;
    desc.sendBufferBytes = 16u << 20u;
    WorldBegin(26u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    serverFlow->sizes = bulkSizes;
    serverFlow->sizeCount = COUNT_OF(bulkSizes);
    SetConditions(50000u, 0u, 0u, 0u, 0u);
    const uint64_t until = world.now + 60u * SECOND;
    while (serverFlow->nextReliable[0] != total && world.now < until)
    {
        while (next[0] < total && SendScheduled(client, peer, 1u, client->index, bulkSizes,
                                                COUNT_OF(bulkSizes), 0u, next[0]) == LAIUE_NET_OK)
            ++next[0];
        WorldStep();
    }
    ExpectEqual(serverFlow->nextReliable[0], total, "a long transfer completes");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK,
           "peer information is available");
    ExpectEqual(info.packetsLost, 0u, "a sender never loses its own packets on a clean path");
    WorldEnd();
}

/* Узкое место: окно перегрузки должно найти пропускную способность пути, а
 * не заваливать очередь маршрутизатора. */
static void TestBottleneck(void)
{
    const uint32_t total = 8u;
    uint32_t next[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    const LaiueNetSimulatorConditions conditions = {
        .structSize = sizeof(conditions),
        .latencyMicroseconds = 15000u,
        .bytesPerSecond = 250000u,
        .queueMicroseconds = 100000u,
    };
    WorldBegin(6u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    serverFlow->sizes = bulkSizes;
    serverFlow->sizeCount = COUNT_OF(bulkSizes);
    Expect(LaiueNetSimulatorSetConditions(world.simulator, &conditions) == LAIUE_NET_OK,
           "bottleneck applies");
    const uint64_t started = world.now;
    const uint64_t until = world.now + 120u * SECOND;
    while (serverFlow->nextReliable[0] != total && world.now < until)
    {
        /* Один канал: два мебибайта подряд. */
        while (next[0] < total && SendScheduled(client, peer, 1u, client->index, bulkSizes,
                                                COUNT_OF(bulkSizes), 0u, next[0]) == LAIUE_NET_OK)
            ++next[0];
        WorldStep();
    }
    ExpectEqual(serverFlow->nextReliable[0], total, "bulk transfer completes");
    const uint64_t elapsed = world.now - started;
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK,
           "peer information is available");
    /* Идеальная передача двух мебибайт на 250 кБ/с занимает 8,4 секунды. */
    Expect(elapsed < 11u * SECOND, "congestion control uses most of the bottleneck");
    Expect(info.packetsLost * 20u < info.packetsSent, "congestion control keeps losses rare");
    Expect(info.roundTripMicroseconds > 30000u && info.roundTripMicroseconds < 250000u,
           "round trip reflects the queue at the bottleneck");
    WorldEnd();
}

/* ---- Очереди и пределы ---------------------------------------------------------------- */

static void TestBackpressure(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    uint32_t accepted = 0u;
    desc.maxMessageBytes = 32768u;
    WorldBegin(7u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    desc.sendBufferBytes = 65536u;
    desc.maxMessageBytes = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    SetConditions(10000u, 0u, 0u, 0u, 0u);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    static const uint32_t sizes[] = {16000u};
    serverFlow->sizes = sizes;
    serverFlow->sizeCount = 1u;
    Expect(LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, scratch, 32769u) ==
               LAIUE_NET_TOO_LARGE,
           "the receiver's message limit is enforced by the sender");
    LaiueNetStatus status = LAIUE_NET_OK;
    while (status == LAIUE_NET_OK && accepted < 64u)
    {
        status = SendScheduled(client, peer, 1u, client->index, sizes, 1u, 0u, accepted);
        if (status == LAIUE_NET_OK)
            ++accepted;
    }
    Expect(status == LAIUE_NET_WOULD_BLOCK && accepted == 4u,
           "a full send queue pushes back instead of growing");
    WorldRun(SECOND);
    ExpectEqual(serverFlow->nextReliable[0], 4u, "queued messages are delivered");
    Expect(SendScheduled(client, peer, 1u, client->index, sizes, 1u, 0u, accepted) == LAIUE_NET_OK,
           "a drained queue accepts messages again");
    WorldRun(SECOND);
    ExpectEqual(serverFlow->nextReliable[0], 5u, "the late message is delivered too");
    WorldEnd();
}

static void TestUnreliable(void)
{
    const uint32_t total = 600u;
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    WorldBegin(8u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    /* Без потерь, но с перестановками и повторами: каждое ненадёжное
     * сообщение приходит ровно один раз, а упорядоченные — только вперёд. */
    SetConditions(10000u, 30000u, 0u, 200000u, 0u);
    for (uint32_t index = 0u; index < total; ++index)
    {
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 0u, index,
                            4u + index % 200u) == LAIUE_NET_OK,
               "unreliable message is queued");
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_SEQUENCED, 1u, index + 1u,
                            4u + index % 50u) == LAIUE_NET_OK,
               "sequenced message is queued");
        WorldStep();
    }
    WorldRun(SECOND);
    ExpectEqual(serverFlow->unreliableCount, total, "unreliable messages arrive exactly once");
    ExpectEqual(serverFlow->lastSequenced, total, "the newest sequenced message arrives");
    /* Джиттер в тридцать раз больше промежутка между отправками: большинство
     * упорядоченных сообщений обогнано более новыми. */
    Expect(serverFlow->sequencedCount < total && serverFlow->sequencedCount > total / 10u,
           "stale sequenced messages are dropped, fresh ones are not");

    /* Наибольшее ненадёжное сообщение занимает датаграмму целиком. */
    memset(serverFlow->unreliableSeen, 0, sizeof(serverFlow->unreliableSeen));
    serverFlow->unreliableCount = 0u;
    SetConditions(10000u, 0u, 0u, 0u, 0u);
    Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 0u, 0u, 1157u) ==
               LAIUE_NET_OK,
           "the largest unreliable message is queued");
    WorldRun(100u * TICK);
    ExpectEqual(serverFlow->unreliableCount, 1u, "the largest unreliable message arrives");

    /* Потери не возмещаются, но и не ломают ничего. */
    SetConditions(10000u, 0u, 200000u, 0u, 0u);
    for (uint32_t index = 1u; index <= total; ++index)
    {
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 0u, index,
                            100u) == LAIUE_NET_OK,
               "unreliable message is queued");
        WorldStep();
        WorldStep();
        WorldStep();
    }
    WorldRun(SECOND);
    Expect(serverFlow->unreliableCount > total / 2u && serverFlow->unreliableCount < total,
           "a fifth of the datagrams is lost and nothing resends them");

    /* Половина датаграмм теряется в обе стороны, а полноразмерные сообщения
     * идут чаще, чем такой путь их пропускает. Окно перегрузки сжимается, но
     * очередь не копит опоздавших: доставленное свежо, остальное выброшено. */
    memset(serverFlow->unreliableSeen, 0, sizeof(serverFlow->unreliableSeen));
    serverFlow->unreliableCount = 0u;
    SetConditions(10000u, 0u, 500000u, 0u, 0u);
    for (uint32_t index = 1u; index <= total; ++index)
    {
        unreliableSentAt[index] = world.now;
        Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 0u, index,
                            1157u) == LAIUE_NET_OK,
               "the largest unreliable message is queued");
        WorldStep();
        WorldStep();
        WorldStep();
    }
    WorldRun(SECOND);
    memset(unreliableSentAt, 0, sizeof(unreliableSentAt));
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.unreliableDropped != 0u && info.queuedUnreliableBytes == 0u,
           "messages the path cannot carry in time are dropped, not delayed");
    Expect(serverFlow->unreliableCount != 0u &&
               serverFlow->worstLatency <= 200000u + 10000u + 2u * TICK,
           "every delivered message is fresh");
    Expect(client->disconnected == 0u && server->disconnected == 0u,
           "losses do not break the connection");
    WorldEnd();
}

/* ---- Связь и её обрыв ----------------------------------------------------------------- */

static bool dropEverything;
static uint32_t corruptFromPort;
static uint32_t dropShortFromPort;
static uint8_t recorded[1500];
static uint32_t recordedSize;
static bool recordNext;

static bool WireFilter(void *context, const LaiueNetAddress *from, const LaiueNetAddress *to,
                       uint8_t *bytes, uint32_t size)
{
    (void)context;
    (void)to;
    if (dropEverything)
        return false;
    if (bytes[0] != 0x40u)
        return true;
    if (from->port == dropShortFromPort)
        return false;
    if (from->port == corruptFromPort)
        bytes[13u + (size * 7u) % (size - 13u)] ^= 0x10u;
    if (recordNext && from->port == 5000u)
    {
        memcpy(recorded, bytes, size);
        recordedSize = size;
        recordNext = false;
    }
    return true;
}

static void TestLiveness(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    desc.idleTimeoutMilliseconds = 3000u;
    WorldBegin(9u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    desc.idleTimeoutMilliseconds = 1500u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    WorldRun(10u * SECOND);
    Expect(client->disconnected == 0u && server->disconnected == 0u,
           "keepalive holds an idle connection open");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.packetsSent > 15u && info.packetsSent < 80u,
           "keepalive is a trickle, not a stream");
    /* Обмен перед обрывом: обе стороны только что слышали друг друга. */
    Flow *serverFlow = ServerFlow(server, client->index);
    Expect(SendScheduled(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), 0u,
                         0u) == LAIUE_NET_OK &&
               SendScheduled(server, serverFlow->peer, 2u, client->index, mixedSizes,
                             COUNT_OF(mixedSizes), 0u, 0u) == LAIUE_NET_OK,
           "messages are queued");
    WorldRun(5u * TICK);
    dropEverything = true;
    WorldRun(1400u * TICK);
    Expect(client->disconnected == 0u && server->disconnected == 0u,
           "silence shorter than the timeout is tolerated");
    WorldRun(200u * TICK);
    Expect(client->disconnected == 1u && client->lastReason == LAIUE_NET_REASON_TIMEOUT &&
               server->disconnected == 1u && server->lastReason == LAIUE_NET_REASON_TIMEOUT,
           "both sides give up after the smaller of the two idle timeouts");
    dropEverything = false;
    WorldEnd();
}

/* Клиент узнаёт об установке по ACCEPT, а сервер — только по первому пакету
 * клиента. Пока сервер не ответил, пробы клиента не редеют сильнее повторов
 * запроса, а ждёт клиент не дольше срока рукопожатия. */
static void TestConfirmation(void)
{
    LaiueNetEndpointDesc serverDesc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpointDesc clientDesc = DefaultDesc(0u);
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    serverDesc.handshakeTimeoutMilliseconds = 8000u;
    clientDesc.handshakeTimeoutMilliseconds = 8000u;
    WorldBegin(23u);
    Node *server = WorldAdd("10.0.0.1:4000", &serverDesc);
    Node *client = WorldAdd("10.0.0.2:5000", &clientDesc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    SetConditions(20000u, 0u, 0u, 0u, 0u);
    dropShortFromPort = 5000u;
    LaiueNetPeer peer = BeginConnect(client, server, NULL);
    WorldRun(6u * SECOND);
    Expect(client->connected == 1u && server->connected == 0u,
           "the client is connected before the server hears from it");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.packetsSent >= 6u && info.packetsSent <= 12u,
           "an unanswered client probes at a steady trickle");
    dropShortFromPort = 0u;
    const uint64_t lifted = world.now;
    while (server->connected == 0u && world.now < lifted + 3u * SECOND)
        WorldStep();
    Expect(server->connected == 1u && world.now - lifted <= SECOND + 20000u + 2u * TICK,
           "the next probe completes the connection");
    WorldRun(4u * SECOND);
    Expect(client->disconnected == 0u && server->disconnected == 0u,
           "a confirmed connection outlives the handshake deadline");
    WorldEnd();

    WorldBegin(24u);
    server = WorldAdd("10.0.0.1:4000", &serverDesc);
    client = WorldAdd("10.0.0.2:5000", &clientDesc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    SetConditions(20000u, 0u, 0u, 0u, 0u);
    dropShortFromPort = 5000u;
    peer = BeginConnect(client, server, NULL);
    WorldRun(7500u * TICK);
    Expect(client->connected == 1u && client->disconnected == 0u,
           "the client waits for the server through the handshake timeout");
    WorldRun(SECOND);
    Expect(client->disconnected == 1u && client->lastReason == LAIUE_NET_REASON_TIMEOUT,
           "a client the server never heard gives up after the handshake timeout");
    Expect(server->connected == 0u && server->disconnected == 0u &&
               LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.peerCount == 0u,
           "the server forgets the half-open connection without telling the application");
    dropShortFromPort = 0u;
    WorldEnd();
}

static void TestTamperAndReplay(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    uint32_t next[CHANNELS] = {0u};
    WorldBegin(10u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    serverFlow->sizes = smallSizes;
    serverFlow->sizeCount = COUNT_OF(smallSizes);

    /* Каждый пакет клиента испорчен в пути: ни один не принят, и ни один
     * испорченный байт не доходит до приложения. */
    corruptFromPort = 5000u;
    const uint32_t before = server->messages;
    Pump(client, peer, 1u, client->index, smallSizes, COUNT_OF(smallSizes), next, 20u);
    WorldRun(2u * SECOND);
    Expect(server->messages == before, "tampered packets deliver nothing");
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.datagramsDropped != 0u,
           "tampered packets are counted as dropped");
    corruptFromPort = 0u;
    WorldRun(5u * SECOND);
    Expect(AllReceived(serverFlow, 20u), "the same data arrives once the path is honest");

    /* Перехваченный пакет, отправленный повторно, не принимается второй раз. */
    recordNext = true;
    Expect(SendNumbered(client, peer, 1u, client->index, LAIUE_NET_UNRELIABLE, 0u, 77u, 100u) ==
               LAIUE_NET_OK,
           "unreliable message is queued");
    WorldRun(20u * TICK);
    Expect(recordedSize != 0u && serverFlow->unreliableCount == 1u,
           "the original packet is delivered and recorded");
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        Expect(client->driver.send(client->driver.context, &server->address, recorded,
                                   recordedSize) == 1,
               "replayed packet enters the network");
        WorldStep();
    }
    WorldRun(100u * TICK);
    Expect(serverFlow->unreliableCount == 1u && server->disconnected == 0u,
           "a replayed packet is ignored");
    recordedSize = 0u;
    WorldEnd();
}

/* ---- Много участников ----------------------------------------------------------------- */

static void TestManyPeers(void)
{
    const uint32_t clients = 48u;
    const uint32_t total = 12u;
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    char text[32] = "10.0.1.";
    WorldBegin(11u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    SetConditions(5000u, 5000u, 20000u, 0u, 0u);
    for (uint32_t index = 0u; index < clients; ++index)
    {
        const uint32_t host = index + 2u;
        uint32_t length = 7u;
        if (host >= 10u)
        {
            text[length] = (char)('0' + host / 10u);
            ++length;
        }
        text[length] = (char)('0' + host % 10u);
        memcpy(text + length + 1u, ":5000", 6u);
        Node *client = WorldAdd(text, &desc);
        (void)BeginConnect(client, server, NULL);
    }
    uint64_t until = world.now + 20u * SECOND;
    while (server->connected < clients && world.now < until)
        WorldStep();
    ExpectEqual(server->connected, clients, "every client connects");
    for (uint32_t index = 0u; index < clients; ++index)
    {
        Node *client = &world.nodes[1u + index];
        Expect(client->connected == 1u, "client sees its connection");
        SendIdentity(client, client->flows[0].peer, client->index);
    }
    until = world.now + 60u * SECOND;
    bool done = false;
    while (!done && world.now < until)
    {
        done = true;
        for (uint32_t index = 0u; index < clients; ++index)
        {
            Node *client = &world.nodes[1u + index];
            Flow *reply = ServerFlow(server, client->index);
            if (reply == NULL)
            {
                done = false;
                continue;
            }
            /* Сервер отвечает каждому его же расписанием: перепутанные
             * адресаты не сойдутся по содержимому. Ответы идут тем же
             * каналом, что и имя: порядок обещан только внутри канала. */
            if (reply->direction == 1u)
            {
                reply->direction = 3u;
                SendIdentity(server, reply->peer, client->index);
                for (uint32_t message = 0u; message < total; ++message)
                {
                    Expect(SendScheduled(client, client->flows[0].peer, 3u, client->index,
                                         smallSizes, COUNT_OF(smallSizes), 1u,
                                         message) == LAIUE_NET_OK &&
                               SendScheduled(server, reply->peer, 2u, client->index, smallSizes,
                                             COUNT_OF(smallSizes), 0u, message) == LAIUE_NET_OK,
                           "per-client messages are queued");
                }
                reply->sizes = smallSizes;
                reply->sizeCount = COUNT_OF(smallSizes);
                client->flows[0].sizes = smallSizes;
                client->flows[0].sizeCount = COUNT_OF(smallSizes);
            }
            if (reply->nextReliable[1] != total || client->flows[0].nextReliable[0] != total)
                done = false;
        }
        WorldStep();
    }
    Expect(done, "every client exchanges its own messages with the server");
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.peerCount == clients,
           "the server counts its peers");
    /* Уведомление при уничтожении не повторяется позже, поэтому проверка
     * точна лишь на сети без потерь. */
    SetConditions(5000u, 5000u, 0u, 0u, 0u);
    WorldRun(50u * TICK);
    LaiueNetEndpointDestroy(server->endpoint);
    server->endpoint = NULL;
    WorldRun(200u * TICK);
    for (uint32_t index = 0u; index < clients; ++index)
    {
        const Node *client = &world.nodes[1u + index];
        Expect(client->disconnected == 1u && client->lastReason == LAIUE_NET_REASON_REMOTE,
               "a destroyed server tells every client");
    }
    WorldEnd();
}

/* ---- Закрытие с доставкой -------------------------------------------------------------- */

static void TestFlushAndLinger(void)
{
    uint32_t next[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    WorldBegin(12u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    SetConditions(20000u, 10000u, 50000u, 0u, 0u);
    LaiueNetPeer peer = Connect(client, server, 10u * SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), next, 11u);
    Expect(LaiueNetDisconnect(client->endpoint, peer, 9u, LAIUE_NET_DISCONNECT_FLUSH) ==
               LAIUE_NET_OK,
           "flushing disconnect starts");
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.state == LAIUE_NET_PEER_DISCONNECTING &&
               LaiueNetSend(client->endpoint, peer, 0u, LAIUE_NET_RELIABLE, "x", 1u) ==
                   LAIUE_NET_NOT_CONNECTED,
           "a flushing peer takes no new messages");
    WorldStep();
    Expect(client->disconnected == 0u, "the event waits for the queue");
    const uint64_t until = world.now + 60u * SECOND;
    while ((client->disconnected == 0u || server->disconnected == 0u) && world.now < until)
        WorldStep();
    Expect(AllReceived(serverFlow, 11u), "queued messages arrive before the close");
    Expect(client->disconnected == 1u && client->lastReason == LAIUE_NET_REASON_LOCAL &&
               client->lastCode == 9u && server->disconnected == 1u &&
               server->lastReason == LAIUE_NET_REASON_REMOTE && server->lastCode == 9u,
           "both sides see the flushed close");
    WorldEnd();

    /* Уведомление о закрытии потеряно, а собеседнику нечего сказать: закрывшая
     * сторона повторяет уведомление сама, и тот узнаёт задолго до тайм-аута. */
    desc.flags = LAIUE_NET_ENDPOINT_ACCEPT;
    WorldBegin(13u);
    server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    client = WorldAdd("10.0.0.2:5000", &desc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    peer = Connect(client, server, SECOND);
    WorldRun(50u * TICK);
    dropShortFromPort = 5000u;
    Expect(LaiueNetDisconnect(client->endpoint, peer, 5u, 0u) == LAIUE_NET_OK, "disconnect");
    WorldRun(10u * TICK);
    Expect(server->disconnected == 0u, "the close notice is lost");
    dropShortFromPort = 0u;
    WorldRun(100u * TICK);
    Expect(server->disconnected == 1u && server->lastReason == LAIUE_NET_REASON_REMOTE &&
               server->lastCode == 5u,
           "a lost close notice is sent again");
    WorldEnd();

    /* Потеряны и оба повтора, но собеседник ещё говорит: уведомление уходит в
     * ответ на его пакет. */
    desc.flags = LAIUE_NET_ENDPOINT_ACCEPT;
    WorldBegin(14u);
    server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    client = WorldAdd("10.0.0.2:5000", &desc);
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    peer = Connect(client, server, SECOND);
    serverFlow = ServerFlow(server, client->index);
    WorldRun(50u * TICK);
    dropShortFromPort = 5000u;
    Expect(LaiueNetDisconnect(client->endpoint, peer, 6u, 0u) == LAIUE_NET_OK, "disconnect");
    Expect(LaiueNetEndpointGetStats(client->endpoint, &stats) == LAIUE_NET_OK, "statistics");
    const uint64_t sentAtClose = stats.datagramsSent;
    const uint64_t repeatsUntil = world.now + SECOND;
    while (stats.datagramsSent < sentAtClose + 2u && world.now < repeatsUntil)
    {
        WorldStep();
        Expect(LaiueNetEndpointGetStats(client->endpoint, &stats) == LAIUE_NET_OK, "statistics");
    }
    Expect(stats.datagramsSent == sentAtClose + 2u && server->disconnected == 0u,
           "both repeats of the close notice are lost too");
    dropShortFromPort = 0u;
    Expect(SendScheduled(server, serverFlow->peer, 2u, client->index, mixedSizes,
                         COUNT_OF(mixedSizes), 0u, 0u) == LAIUE_NET_OK,
           "server message is queued");
    WorldRun(10u * TICK);
    Expect(server->disconnected == 1u && server->lastReason == LAIUE_NET_REASON_REMOTE &&
               server->lastCode == 6u,
           "the close notice answers a peer that keeps talking");
    Expect(LaiueNetEndpointGetStats(client->endpoint, &stats) == LAIUE_NET_OK &&
               stats.peerCount == 0u,
           "a closing peer does not count against the limit");
    WorldEnd();
}

/* ---- Смена адреса ---------------------------------------------------------------------- */

typedef struct Roaming
{
    LaiueNetDriverV1 ports[2];
    uint32_t current;
} Roaming;

static int32_t RoamingSend(void *context, const LaiueNetAddress *to, const void *bytes,
                           uint32_t size)
{
    Roaming *roaming = context;
    const LaiueNetDriverV1 *port = &roaming->ports[roaming->current];
    return port->send(port->context, to, bytes, size);
}

static int32_t RoamingReceive(void *context, LaiueNetAddress *outFrom, void *buffer,
                              uint32_t capacity)
{
    Roaming *roaming = context;
    const LaiueNetDriverV1 *port = &roaming->ports[roaming->current];
    return port->receive(port->context, outFrom, buffer, capacity);
}

static void TestMigration(void)
{
    static Roaming roaming;
    uint32_t clientNext[CHANNELS] = {0u};
    uint32_t serverNext[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    const LaiueNetDriverV1 driver = {
        .structSize = sizeof(driver),
        .context = &roaming,
        .send = RoamingSend,
        .receive = RoamingReceive,
    };
    WorldBegin(14u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    Node *client = WorldAdd("10.0.0.2:5000", NULL);
    Node *elsewhere = WorldAdd("192.168.7.7:6000", NULL);
    roaming.ports[0] = client->driver;
    roaming.ports[1] = elsewhere->driver;
    roaming.current = 0u;
    desc.flags = 0u;
    desc.driver = &driver;
    desc.random = TestRandom;
    desc.randomContext = &client->randomState;
    Expect(LaiueNetEndpointCreate(&desc, &client->endpoint) == LAIUE_NET_OK, "endpoint creates");
    SetConditions(10000u, 0u, 0u, 0u, 0u);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), clientNext, 6u);
    Pump(server, serverFlow->peer, 2u, client->index, mixedSizes, COUNT_OF(mixedSizes), serverNext,
         6u);
    WorldRun(30u * TICK);
    /* Клиент уходит в другую сеть посреди передачи. */
    roaming.current = 1u;
    Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), clientNext, 22u);
    Pump(server, serverFlow->peer, 2u, client->index, mixedSizes, COUNT_OF(mixedSizes), serverNext,
         22u);
    const uint64_t until = world.now + 30u * SECOND;
    while ((!AllReceived(serverFlow, 22u) || !AllReceived(&client->flows[0], 22u)) &&
           world.now < until)
        WorldStep();
    Expect(AllReceived(serverFlow, 22u) && AllReceived(&client->flows[0], 22u),
           "the connection follows a client that changed its address");
    Expect(LaiueNetPeerGetInfo(server->endpoint, serverFlow->peer, &info) == LAIUE_NET_OK &&
               LaiueNetAddressEqual(&info.address, &elsewhere->address),
           "the server answers to the new address");
    Expect(client->disconnected == 0u && server->disconnected == 0u && server->connected == 1u,
           "a moved client is the same peer");
    WorldEnd();
}

/* ---- Давление незавершёнными рукопожатиями и проверка ответов ------------------------- */

static uint32_t PeerCount(const Node *node)
{
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    Expect(LaiueNetEndpointGetStats(node->endpoint, &stats) == LAIUE_NET_OK, "statistics");
    return stats.peerCount;
}

/* Отправляет запрос с сырого порта и возвращает размер ответа сервера. */
static int32_t Exchange(Node *raw, const Node *server, uint32_t size, uint8_t *reply,
                        uint32_t capacity)
{
    LaiueNetAddress from;
    Expect(raw->driver.send(raw->driver.context, &server->address, scratch, size) == 1,
           "handmade datagram enters the network");
    WorldStep();
    WorldStep();
    const int32_t answered = raw->driver.receive(raw->driver.context, &from, reply, capacity);
    Expect(answered == 0 || LaiueNetAddressEqual(&from, &server->address),
           "answers come from the server");
    return answered;
}

/* Клиенты, начавшие разом, не ждут повторов друг за другом: проверенные
 * запросы сверх бюджета обмена ключами откладываются до следующих
 * обновлений. */
static void TestHandshakeBurst(void)
{
    const uint32_t clients = 64u;
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    char text[32] = "10.0.3.";
    WorldBegin(25u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    SetConditions(5000u, 0u, 0u, 0u, 0u);
    for (uint32_t index = 0u; index < clients; ++index)
    {
        const uint32_t host = index + 1u;
        uint32_t length = 7u;
        if (host >= 10u)
        {
            text[length] = (char)('0' + host / 10u);
            ++length;
        }
        text[length] = (char)('0' + host % 10u);
        memcpy(text + length + 1u, ":6000", 6u);
        (void)BeginConnect(WorldAdd(text, &desc), server, NULL);
    }
    const uint64_t start = world.now;
    while (server->connected < clients && world.now < start + SECOND)
        WorldStep();
    ExpectEqual(server->connected, clients, "a burst of clients connects");
    Expect(world.now - start <= 60u * TICK,
           "requests beyond the update budget wait for the next update, not for a retry");
    WorldEnd();
}

static void TestHandshakePressure(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    uint8_t reply[128];
    uint8_t accepted[128];
    desc.maxPeers = 4u;
    WorldBegin(15u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Expect(LaiueNetSimulatorSetFilter(world.simulator, WireFilter, NULL) == LAIUE_NET_OK,
           "filter installs");
    /* Четыре клиента получают ответ сервера, но их первый пакет не доходит:
     * сервер держит четыре незавершённых рукопожатия. */
    dropShortFromPort = 7000u;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        static const char *const stalled[] = {"10.0.2.1:7000", "10.0.2.2:7000", "10.0.2.3:7000",
                                              "10.0.2.4:7000"};
        (void)BeginConnect(WorldAdd(stalled[index], &desc), server, NULL);
    }
    WorldRun(100u * TICK);
    Expect(server->connected == 0u && PeerCount(server) == 4u,
           "stalled clients hold places but are not announced");
    /* Таблица полна ими, но честный клиент всё равно входит: место уступает
     * старейшее из незавершённых. */
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    (void)Connect(client, server, SECOND);
    Expect(PeerCount(server) == 4u, "the oldest unfinished handshake gives way");

    /* Мусор остаётся без ответа. Исключение — датаграмма, похожая на запрос
     * соединения с чужой версией: ей уходит отказ, который в десятки раз
     * короче её самой. */
    Node *raw = WorldAdd("10.0.9.9:9000", NULL);
    for (uint32_t size = 1u; size < 1400u; size += 53u)
    {
        for (uint32_t index = 0u; index < size; ++index)
            scratch[index] = (uint8_t)(index * 37u + size);
        scratch[0] = (uint8_t)(size % 3u == 0u ? 0x81u : (size % 3u == 1u ? 0x40u : size));
        const int32_t answered = Exchange(raw, server, size, reply, sizeof(reply));
        if (scratch[0] == 0x81u && size >= 576u)
            Expect(answered == 16 && reply[0] == 0x84u && reply[11] == LAIUE_NET_REJECT_VERSION,
                   "a request of an unknown version gets a short refusal");
        else
            Expect(answered == 0, "garbage is not answered");
    }

    /* Запрос, собранный по описанию формата, получает метку и ничего больше:
     * ответ короче запроса, состояния на сервере не появляется. */
    memset(scratch, 0, 1200u);
    scratch[0] = 0x81u;
    scratch[1] = 1u;
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        scratch[4u + index] = (uint8_t)(PROTOCOL >> (index * 8u));
        scratch[12u + index] = (uint8_t)(0xC0u + index);
    }
    for (uint32_t index = 0u; index < 32u; ++index)
        scratch[20u + index] = (uint8_t)(index + 1u);
    scratch[52] = 0xB0u;
    scratch[53] = 0x04u;
    scratch[54] = 1u;
    Store32(scratch + 56, 1u << 20u);
    Store32(scratch + 60, 1u << 18u);
    Store32(scratch + 64, 30000u);
    Expect(Exchange(raw, server, 1200u, reply, sizeof(reply)) == 28 && reply[0] == 0x82u &&
               reply[1] == 1u && reply[3] == 0xC0u && reply[10] == 0xC7u,
           "a request without a token gets a token");
    Expect(Exchange(raw, server, 1199u, reply, sizeof(reply)) == 0,
           "a request shorter than the datagram size it declares is not answered");
    Expect(Exchange(raw, server, 575u, reply, sizeof(reply)) == 0,
           "a request shorter than the smallest datagram is not answered");
    scratch[68] = 17u;
    scratch[69] = reply[11];
    Expect(Exchange(raw, server, 1200u, accepted, sizeof(accepted)) == 28 && accepted[0] == 0x82u,
           "a forged token gets a fresh token, not a connection");
    /* С настоящей меткой сервер принимает запрос, и ответ всё ещё короче его.
     * Повтор запроса получает тот же ответ и не создаёт второго участника. */
    memcpy(scratch + 69, reply + 11, 17u);
    Expect(Exchange(raw, server, 1200u, accepted, sizeof(accepted)) == 83 && accepted[0] == 0x83u &&
               accepted[3] == 0xC0u && PeerCount(server) == 4u,
           "a request with its token is accepted");
    Expect(Exchange(raw, server, 1200u, reply, sizeof(reply)) == 83 &&
               memcmp(reply, accepted, 83u) == 0 && PeerCount(server) == 4u,
           "a repeated request gets the same answer and no second peer");
    Expect(server->connected == 1u && server->disconnected == 0u,
           "none of this reaches the application");

    /* Незавершённые рукопожатия истекают сами, метки тоже. */
    WorldRun(32u * SECOND);
    Expect(PeerCount(server) == 1u && server->connected == 1u && server->disconnected == 0u,
           "unfinished handshakes expire without a trace");
    Expect(Exchange(raw, server, 1200u, reply, sizeof(reply)) == 28 && reply[0] == 0x82u &&
               PeerCount(server) == 1u,
           "an expired token is refused");
    Node *late = WorldAdd("10.0.0.3:5000", &desc);
    (void)Connect(late, server, SECOND);
    dropShortFromPort = 0u;
    WorldEnd();
}

/* ---- Отказ драйвера и накопление событий ----------------------------------------------- */

static bool driverBroken;

static int32_t BrokenSend(void *context, const LaiueNetAddress *to, const void *bytes,
                          uint32_t size)
{
    const LaiueNetDriverV1 *port = context;
    return driverBroken ? -1 : port->send(port->context, to, bytes, size);
}

static int32_t BrokenReceive(void *context, LaiueNetAddress *outFrom, void *buffer,
                             uint32_t capacity)
{
    const LaiueNetDriverV1 *port = context;
    return driverBroken ? -1 : port->receive(port->context, outFrom, buffer, capacity);
}

static void TestDriverAndBacklog(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpoint *endpoint = NULL;
    uint32_t next[CHANNELS] = {0u};
    WorldBegin(16u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", NULL);
    const LaiueNetDriverV1 driver = {
        .structSize = sizeof(driver),
        .context = &client->driver,
        .send = BrokenSend,
        .receive = BrokenReceive,
    };
    desc.driver = &driver;
    desc.random = TestRandom;
    desc.randomContext = &client->randomState;
    Expect(LaiueNetEndpointCreate(&desc, &client->endpoint) == LAIUE_NET_OK, "endpoint creates");
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    driverBroken = true;
    Expect(LaiueNetUpdate(client->endpoint, world.now) == LAIUE_NET_IO_ERROR,
           "a broken driver is reported");
    driverBroken = false;
    WorldStep();
    Expect(client->disconnected == 0u, "a driver that recovers keeps its connections");

    /* Приложение не разбирает события несколько обновлений подряд: они ждут,
     * и байты сообщений остаются целыми. */
    server->muted = true;
    Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), next, 11u);
    WorldRun(SECOND);
    Expect(serverFlow->nextReliable[0] == 0u, "nothing is handled while events wait");
    server->muted = false;
    WorldStep();
    Expect(AllReceived(serverFlow, 11u), "waiting events are all delivered intact");

    desc.driver = NULL;
    Expect(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_INVALID_ARGUMENT &&
               endpoint == NULL,
           "an endpoint needs a driver");
    desc.driver = &driver;
    desc.channelCount = LAIUE_NET_MAX_CHANNELS + 1u;
    Expect(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_INVALID_ARGUMENT,
           "too many channels are refused");
    desc.channelCount = 1u;
    desc.maxDatagramBytes = 575u;
    Expect(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_INVALID_ARGUMENT,
           "a datagram limit below the minimum is refused");
    desc.maxDatagramBytes = 1453u;
    Expect(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_INVALID_ARGUMENT,
           "a datagram limit above the maximum is refused");
    desc.maxDatagramBytes = 0u;
    desc.structSize = 8u;
    Expect(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_INVALID_ARGUMENT,
           "a short descriptor is refused");
    LaiueNetEndpointDestroy(NULL);
    LaiueNetSimulatorDestroy(NULL);
    WorldEnd();
}

/* Приложение надолго перестало разбирать события. Память событий не растёт
 * сверх предела: конечная точка перестаёт читать сеть, а переполненная сеть
 * теряет лишнее. Когда разбор возобновляется, всё приходит целым — потерянное
 * отправитель повторяет сам. */
static void TestEventBacklog(void)
{
    const uint32_t total = 11u;
    uint32_t next[CHANNELS] = {0u};
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetEndpointStats stats = {.structSize = sizeof(stats)};
    desc.eventBacklogBytes = 65536u;
    WorldBeginLimited(18u, 256u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    desc.eventBacklogBytes = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    SetConditions(5000u, 0u, 0u, 0u, 0u);
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    Flow *serverFlow = ServerFlow(server, client->index);
    server->muted = true;
    uint64_t until = world.now + 3u * SECOND;
    while (world.now < until)
    {
        Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), next, total);
        WorldStep();
    }
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK, "statistics");
    const uint64_t received = stats.datagramsReceived;
    WorldRun(SECOND);
    Expect(LaiueNetEndpointGetStats(server->endpoint, &stats) == LAIUE_NET_OK &&
               stats.datagramsReceived == received,
           "an endpoint with a full backlog stops reading the network");
    Expect(serverFlow->nextReliable[0] == 0u && client->disconnected == 0u &&
               server->disconnected == 0u,
           "a stalled application keeps its connections");
    server->muted = false;
    until = world.now + 60u * SECOND;
    while (!AllReceived(serverFlow, total) && world.now < until)
    {
        Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), next, total);
        WorldStep();
    }
    Expect(AllReceived(serverFlow, total), "everything arrives once the application catches up");
    WorldEnd();
}

/* Стороны с разными пределами договариваются о меньших. */
static void TestNegotiation(void)
{
    LaiueNetEndpointDesc desc = DefaultDesc(LAIUE_NET_ENDPOINT_ACCEPT);
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    uint32_t next[CHANNELS] = {0u};
    desc.maxDatagramBytes = 1452u;
    desc.channelCount = 8u;
    desc.maxMessageBytes = 200000u;
    desc.receiveWindowBytes = 5000u;
    WorldBegin(17u);
    Node *server = WorldAdd("10.0.0.1:4000", &desc);
    desc.flags = 0u;
    desc.maxDatagramBytes = 576u;
    desc.channelCount = CHANNELS;
    desc.maxMessageBytes = 0u;
    desc.receiveWindowBytes = 0u;
    Node *client = WorldAdd("10.0.0.2:5000", &desc);
    const LaiueNetSimulatorConditions conditions = {
        .structSize = sizeof(conditions),
        .latencyMicroseconds = 5000u,
        .jitterMicroseconds = 5000u,
        .lossPerMillion = 30000u,
        .maxDatagramBytes = 576u,
    };
    const LaiueNetPeer peer = Connect(client, server, SECOND);
    /* Путь не пропускает датаграммы длиннее 576 байт: соединение живо, только
     * если обе стороны соблюдают меньший из заявленных размеров. */
    Expect(LaiueNetSimulatorSetConditions(world.simulator, &conditions) == LAIUE_NET_OK,
           "narrow path applies");
    Flow *serverFlow = ServerFlow(server, client->index);
    Expect(LaiueNetPeerGetInfo(client->endpoint, peer, &info) == LAIUE_NET_OK &&
               info.channelCount == CHANNELS && info.maxMessageBytes == 200000u &&
               info.maxUnreliableBytes == 576u - 43u,
           "the client sees the agreed limits");
    Expect(LaiueNetPeerGetInfo(server->endpoint, serverFlow->peer, &info) == LAIUE_NET_OK &&
               info.channelCount == CHANNELS && info.maxMessageBytes == (1u << 20u),
           "the server sees the agreed limits");
    /* Окно сервера — 8192 байта после округления: сообщения в полтораста
     * тысяч байт проходят через него по частям. */
    const uint64_t until = world.now + 120u * SECOND;
    uint32_t serverNext[CHANNELS] = {0u};
    while ((!AllReceived(serverFlow, 11u) || !AllReceived(&client->flows[0], 11u)) &&
           world.now < until)
    {
        Pump(client, peer, 1u, client->index, mixedSizes, COUNT_OF(mixedSizes), next, 11u);
        Pump(server, serverFlow->peer, 2u, client->index, mixedSizes, COUNT_OF(mixedSizes),
             serverNext, 11u);
        WorldStep();
    }
    Expect(AllReceived(serverFlow, 11u) && AllReceived(&client->flows[0], 11u),
           "messages larger than the receive window still arrive");
    WorldEnd();
}

static void ReadSeedOffset(void)
{
    char text[24] = {0};
#if defined(_WIN32)
    if (GetEnvironmentVariableA("LAIUE_NET_TEST_SEED", text, (DWORD)sizeof(text)) >= sizeof(text))
        return;
#else
    const char *value = getenv("LAIUE_NET_TEST_SEED");
    for (uint32_t index = 0u; value != NULL && value[index] != 0 && index + 1u < sizeof(text);
         ++index)
        text[index] = value[index];
#endif
    for (uint32_t index = 0u; text[index] >= '0' && text[index] <= '9'; ++index)
        seedOffset = seedOffset * 10u + (uint64_t)(text[index] - '0');
}

LAIUE_TEST_ENTRY(NetTestEntryPoint)
{
    ReadSeedOffset();
    TestAddresses();
    TestLifecycle();
    TestHandshakePolicies();
    TestLossyNetwork();
    TestDeepPipe();
    TestBottleneck();
    TestBackpressure();
    TestUnreliable();
    TestLiveness();
    TestConfirmation();
    TestTamperAndReplay();
    TestManyPeers();
    TestFlushAndLinger();
    TestMigration();
    TestHandshakePressure();
    TestHandshakeBurst();
    TestDriverAndBacklog();
    TestEventBacklog();
    TestNegotiation();
    LaiueTestRuntimeWrite("net transport: passed\n");
    LAIUE_TEST_SUCCESS();
}
