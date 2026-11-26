// Ручной benchmark транспорта laiue.net: в ALL не входит, в CTest не
// регистрируется.
//
// В первых четырёх сценариях симулятор не теряет пакеты. Измеряется время
// работы транспорта и симулятора: шифрование, сборка кадров, подтверждения,
// таймеры и события.
//
// поток       — 256 МиБ надёжных сообщений по 64 КиБ от клиента серверу;
// мелочь      — миллион надёжных сообщений по 32 байта;
// рукопожатия — тысяча клиентов разом соединяются с одним сервером;
// толпа       — сервер и те же клиенты обмениваются ненадёжным сообщением
//               в каждом такте 60 Гц; меряется такт сервера.
// плохой путь — CUBIC и BBR передают один и тот же поток, измеряются
//               виртуальное время доставки и реальное время работы.

#include "net/net.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stddef.h>
#include <string.h>

#define PROTOCOL UINT64_C(0x6C61697565626E63)
#define MILLISECOND UINT64_C(1000)
#define STREAM_BYTES (UINT64_C(256) << 20u)
#define STREAM_MESSAGE (64u * 1024u)
#define SMALL_COUNT 1000000u
#define SMALL_MESSAGE 32u
#define CROWD 1000u
#define CROWD_TICKS 600u
#define CROWD_MESSAGE 64u
#define TICK_60HZ UINT64_C(16667)
#define PATH_MESSAGE (16u << 10u)
#define PATH_MESSAGES 256u
#define PATH_BUFFER_BYTES (1u << 20u)
#define PATH_QUEUE_DATAGRAMS 4096u
#define PATH_TIMEOUT (UINT64_C(90) * 1000000u)
/* Симулированное время, за которое сценарий обязан закончиться. */
#define SCENARIO_LIMIT (UINT64_C(600) * 1000000u)

typedef struct Totals
{
    uint64_t messages;
    uint64_t bytes;
    uint64_t checksum;
    uint32_t connected;
    uint32_t disconnected;
} Totals;

typedef struct Network
{
    LaiueNetSimulator *simulator;
    uint64_t now;
    uint32_t congestionControl;
    uint32_t streamBufferBytes;
} Network;

static uint64_t randomState = UINT64_C(0x9E3779B97F4A7C15);
static uint64_t checksum;
static uint8_t streamMessage[STREAM_MESSAGE];
static LaiueNetEndpoint *crowd[CROWD];
static LaiueNetPeer crowdPeers[CROWD];
static LaiueNetPeer serverPeers[CROWD];

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    do
    {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
        text[index] = digits[length - 1u - index];
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void Check(bool condition, const char *what)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net benchmark failed: ");
    LaiueTestRuntimeWrite(what);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

/* Детерминированные ключи и CID: контрольная сумма не зависит от них, но
 * прогон повторяется байт в байт. */
static bool Random(void *context, void *output, uint32_t size)
{
    (void)context;
    uint8_t *bytes = output;
    for (uint32_t index = 0u; index < size; ++index)
    {
        randomState ^= randomState << 13u;
        randomState ^= randomState >> 7u;
        randomState ^= randomState << 17u;
        bytes[index] = (uint8_t)(randomState >> 32u);
    }
    return true;
}

static LaiueNetAddress Address(uint32_t host, uint16_t port)
{
    LaiueNetAddress address;
    memset(&address, 0, sizeof(address));
    address.family = LAIUE_NET_FAMILY_IPV4;
    address.ip[0] = 10u;
    address.ip[1] = (uint8_t)(host >> 16u);
    address.ip[2] = (uint8_t)(host >> 8u);
    address.ip[3] = (uint8_t)host;
    address.port = port;
    return address;
}

static void NetworkStart(Network *network, const LaiueNetSimulatorConditions *conditions,
                         uint64_t seed, uint32_t queueDatagrams)
{
    memset(network, 0, sizeof(*network));
    const LaiueNetSimulatorDesc desc = {
        .structSize = sizeof(desc), .maxQueuedDatagrams = queueDatagrams, .seed = seed};
    Check(LaiueNetSimulatorCreate(&desc, &network->simulator) == LAIUE_NET_OK, "simulator");
    Check(LaiueNetSimulatorSetConditions(network->simulator, conditions) == LAIUE_NET_OK,
          "conditions");
}

static void NetworkBegin(Network *network, uint32_t latencyMicroseconds)
{
    const LaiueNetSimulatorConditions conditions = {.structSize = sizeof(conditions),
                                                    .latencyMicroseconds = latencyMicroseconds};
    NetworkStart(network, &conditions, 1u, 1u << 20u);
    /* На быстром пути замер упирается в процессор, не в окно потока. */
    network->streamBufferBytes = 16u << 20u;
}

static LaiueNetEndpoint *Open(Network *network, const LaiueNetAddress *address, uint32_t flags,
                              uint32_t maxPeers)
{
    LaiueNetDriverV1 driver;
    memset(&driver, 0, sizeof(driver));
    driver.structSize = sizeof(driver);
    Check(LaiueNetSimulatorOpen(network->simulator, address, &driver) == LAIUE_NET_OK, "port");
    LaiueNetEndpointDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.flags = flags;
    desc.driver = &driver;
    desc.protocolId = PROTOCOL;
    desc.maxPeers = maxPeers;
    desc.channelCount = 2u;
    desc.receiveWindowBytes = network->streamBufferBytes;
    desc.sendBufferBytes = network->streamBufferBytes;
    desc.congestionControl = network->congestionControl;
    desc.random = Random;
    LaiueNetEndpoint *endpoint = NULL;
    Check(LaiueNetEndpointCreate(&desc, &endpoint) == LAIUE_NET_OK, "endpoint");
    return endpoint;
}

static LaiueNetPeer Connect(LaiueNetEndpoint *client, const LaiueNetAddress *server)
{
    LaiueNetConnectDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.address = *server;
    LaiueNetPeer peer = 0u;
    Check(LaiueNetConnect(client, &desc, &peer) == LAIUE_NET_OK, "connect");
    return peer;
}

/* Забирает события; описатели входящих соединений пишет в peers. */
static void Drain(LaiueNetEndpoint *endpoint, Totals *totals, LaiueNetPeer *peers,
                  uint32_t capacity)
{
    LaiueNetEvent event;
    memset(&event, 0, sizeof(event));
    event.structSize = sizeof(event);
    while (LaiueNetPollEvent(endpoint, &event))
    {
        if (event.type == LAIUE_NET_EVENT_CONNECTED)
        {
            if (peers != NULL && totals->connected < capacity)
                peers[totals->connected] = event.peer;
            ++totals->connected;
        }
        else if (event.type == LAIUE_NET_EVENT_MESSAGE)
        {
            const uint8_t *data = event.data;
            ++totals->messages;
            totals->bytes += event.size;
            totals->checksum = totals->checksum * 31u + event.size +
                               (event.size != 0u ? data[0] + data[event.size - 1u] : 0u);
        }
        else
        {
            ++totals->disconnected;
        }
        memset(&event, 0, sizeof(event));
        event.structSize = sizeof(event);
    }
}

static void Step(Network *network, uint64_t step)
{
    network->now += step;
    Check(network->now < SCENARIO_LIMIT, "the scenario finishes in time");
    Check(LaiueNetSimulatorAdvance(network->simulator, network->now) == LAIUE_NET_OK, "advance");
}

static void Report(const char *label, double seconds, uint64_t amount, const char *unit)
{
    LaiueTestRuntimeWrite(label);
    LaiueTestRuntimeWrite(": ");
    WriteUnsigned((uint64_t)(seconds * 1000.0));
    LaiueTestRuntimeWrite(" ms");
    if (amount != 0u && seconds > 0.0)
    {
        LaiueTestRuntimeWrite(", ");
        WriteUnsigned((uint64_t)((double)amount / seconds));
        LaiueTestRuntimeWrite(unit);
    }
    LaiueTestRuntimeWrite("\n");
}

/* Один клиент шлёт серверу count сообщений по size байт, чередуя каналы. */
static void Transfer(const char *label, uint32_t size, uint64_t count)
{
    Network network;
    NetworkBegin(&network, 5u * (uint32_t)MILLISECOND);
    const LaiueNetAddress serverAddress = Address(1u, 4000u);
    const LaiueNetAddress clientAddress = Address(2u, 5000u);
    LaiueNetEndpoint *server = Open(&network, &serverAddress, LAIUE_NET_ENDPOINT_ACCEPT, 4u);
    LaiueNetEndpoint *client = Open(&network, &clientAddress, 0u, 4u);
    const LaiueNetPeer peer = Connect(client, &serverAddress);
    Totals serverTotals = {0};
    Totals clientTotals = {0};
    while (serverTotals.connected == 0u || clientTotals.connected == 0u)
    {
        Step(&network, MILLISECOND);
        Check(LaiueNetUpdate(client, network.now) == LAIUE_NET_OK &&
                  LaiueNetUpdate(server, network.now) == LAIUE_NET_OK,
              "update");
        Drain(client, &clientTotals, NULL, 0u);
        Drain(server, &serverTotals, NULL, 0u);
    }
    uint64_t queued = 0u;
    const double begin = PlatformMonotonicSeconds();
    while (serverTotals.messages < count)
    {
        while (queued < count)
        {
            streamMessage[0] = (uint8_t)queued;
            if (LaiueNetSend(client, peer, (uint32_t)(queued % 2u), LAIUE_NET_RELIABLE,
                             streamMessage, size) != LAIUE_NET_OK)
                break;
            ++queued;
        }
        Step(&network, MILLISECOND);
        Check(LaiueNetUpdate(client, network.now) == LAIUE_NET_OK &&
                  LaiueNetUpdate(server, network.now) == LAIUE_NET_OK,
              "update");
        Drain(client, &clientTotals, NULL, 0u);
        Drain(server, &serverTotals, NULL, 0u);
    }
    const double seconds = PlatformMonotonicSeconds() - begin;
    LaiueNetPeerInfo info;
    memset(&info, 0, sizeof(info));
    info.structSize = sizeof(info);
    Check(LaiueNetPeerGetInfo(client, peer, &info) == LAIUE_NET_OK && info.packetsLost == 0u,
          "a lossless path loses nothing");
    if (size >= 1024u)
        Report(label, seconds, serverTotals.bytes >> 20u, " MiB/s");
    else
        Report(label, seconds, serverTotals.messages, " messages/s");
    checksum = checksum * 1000003u + serverTotals.checksum + info.packetsSent;
    LaiueNetEndpointDestroy(client);
    LaiueNetEndpointDestroy(server);
    LaiueNetSimulatorDestroy(network.simulator);
}

/* Тысяча клиентов соединяются разом, затем 600 тактов 60 Гц обмениваются
 * ненадёжными сообщениями с сервером. */
static void Crowd(void)
{
    Network network;
    NetworkBegin(&network, 20u * (uint32_t)MILLISECOND);
    const LaiueNetAddress serverAddress = Address(1u, 4000u);
    LaiueNetEndpoint *server =
        Open(&network, &serverAddress, LAIUE_NET_ENDPOINT_ACCEPT, CROWD + 24u);
    Totals serverTotals = {0};
    Totals clientTotals = {0};
    for (uint32_t index = 0u; index < CROWD; ++index)
    {
        const LaiueNetAddress address = Address(100u + index, 5000u);
        crowd[index] = Open(&network, &address, 0u, 1u);
        crowdPeers[index] = Connect(crowd[index], &serverAddress);
    }
    double begin = PlatformMonotonicSeconds();
    while (serverTotals.connected < CROWD || clientTotals.connected < CROWD)
    {
        Step(&network, MILLISECOND);
        for (uint32_t index = 0u; index < CROWD; ++index)
        {
            Check(LaiueNetUpdate(crowd[index], network.now) == LAIUE_NET_OK, "update");
            Drain(crowd[index], &clientTotals, NULL, 0u);
        }
        Check(LaiueNetUpdate(server, network.now) == LAIUE_NET_OK, "update");
        Drain(server, &serverTotals, serverPeers, CROWD);
    }
    Report("1000 handshakes", PlatformMonotonicSeconds() - begin, CROWD, " handshakes/s");

    static uint8_t payload[CROWD_MESSAGE];
    double serverSeconds = 0.0;
    begin = PlatformMonotonicSeconds();
    for (uint32_t tick = 0u; tick < CROWD_TICKS; ++tick)
    {
        Step(&network, TICK_60HZ);
        payload[0] = (uint8_t)tick;
        for (uint32_t index = 0u; index < CROWD; ++index)
        {
            Check(LaiueNetSend(crowd[index], crowdPeers[index], 0u, LAIUE_NET_UNRELIABLE, payload,
                               CROWD_MESSAGE) == LAIUE_NET_OK,
                  "client send");
            Check(LaiueNetUpdate(crowd[index], network.now) == LAIUE_NET_OK, "update");
            Drain(crowd[index], &clientTotals, NULL, 0u);
        }
        const double serverBegin = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < CROWD; ++index)
            Check(LaiueNetSend(server, serverPeers[index], 0u, LAIUE_NET_UNRELIABLE, payload,
                               CROWD_MESSAGE) == LAIUE_NET_OK,
                  "server send");
        Check(LaiueNetUpdate(server, network.now) == LAIUE_NET_OK, "update");
        Drain(server, &serverTotals, NULL, 0u);
        serverSeconds += PlatformMonotonicSeconds() - serverBegin;
    }
    const double seconds = PlatformMonotonicSeconds() - begin;
    Check(serverTotals.disconnected == 0u && clientTotals.disconnected == 0u, "nobody disconnects");
    Report("crowd 1000 x 600 ticks, all sides", seconds, 0u, "");
    LaiueTestRuntimeWrite("crowd server tick: ");
    WriteUnsigned((uint64_t)(serverSeconds * 1000000.0 / CROWD_TICKS));
    LaiueTestRuntimeWrite(" us, ");
    WriteUnsigned((uint64_t)(serverSeconds * 1000000000.0 / CROWD_TICKS / CROWD));
    LaiueTestRuntimeWrite(" ns per peer\n");
    checksum = checksum * 1000003u + serverTotals.checksum + clientTotals.checksum +
               serverTotals.messages + clientTotals.messages;
    for (uint32_t index = 0u; index < CROWD; ++index)
        LaiueNetEndpointDestroy(crowd[index]);
    LaiueNetEndpointDestroy(server);
    LaiueNetSimulatorDestroy(network.simulator);
}

static void PrintPathValue(const char *name, uint64_t value)
{
    LaiueTestRuntimeWrite(" ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("=");
    WriteUnsigned(value);
}

static void DrainPath(LaiueNetEndpoint *server, uint32_t *received)
{
    LaiueNetEvent event = {.structSize = sizeof(event)};
    while (LaiueNetPollEvent(server, &event))
    {
        Check(event.type == LAIUE_NET_EVENT_MESSAGE && event.delivery == LAIUE_NET_RELIABLE &&
                  event.channel == 0u && event.size == PATH_MESSAGE && event.data != NULL &&
                  *received < PATH_MESSAGES,
              "one complete reliable path message (channel 0)");
        const uint8_t *data = event.data;
        for (uint32_t byte = 0u; byte < sizeof(*received); ++byte)
            Check(data[byte] == (uint8_t)(*received >> (8u * byte)), "path message order");
        Check(memcmp(data + sizeof(*received), streamMessage + sizeof(*received),
                     PATH_MESSAGE - sizeof(*received)) == 0,
              "path message bytes");
        ++*received;
    }
}

/* Доставить поток целиком и дождаться подтверждений. Пропускная
 * способность включает разгон, но отсчёт начинается после рукопожатия. */
static void PathTransfer(const LaiueNetSimulatorConditions *conditions, const char *label,
                         uint32_t control, uint64_t seed)
{
    randomState = UINT64_C(0x9E3779B97F4A7C15) ^ seed;
    Network network;
    NetworkStart(&network, conditions, seed, PATH_QUEUE_DATAGRAMS);
    network.streamBufferBytes = PATH_BUFFER_BYTES;
    network.congestionControl = control;
    const LaiueNetAddress serverAddress = Address(1u, 4000u);
    const LaiueNetAddress clientAddress = Address(2u, 5000u);
    LaiueNetEndpoint *server = Open(&network, &serverAddress, LAIUE_NET_ENDPOINT_ACCEPT, 1u);
    LaiueNetEndpoint *client = Open(&network, &clientAddress, 0u, 1u);
    const LaiueNetPeer peer = Connect(client, &serverAddress);
    Totals serverTotals = {0};
    Totals clientTotals = {0};
    while (serverTotals.connected == 0u || clientTotals.connected == 0u)
    {
        Check(network.now < PATH_TIMEOUT, "path handshake deadline");
        Step(&network, MILLISECOND);
        Check(LaiueNetUpdate(client, network.now) == LAIUE_NET_OK &&
                  LaiueNetUpdate(server, network.now) == LAIUE_NET_OK,
              "path handshake update");
        Drain(client, &clientTotals, NULL, 0u);
        Drain(server, &serverTotals, NULL, 0u);
        Check(clientTotals.disconnected == 0u && serverTotals.disconnected == 0u, "path connects");
    }

    uint32_t queued = 0u;
    uint32_t received = 0u;
    uint64_t deliveryUs = 0u;
    uint64_t rttSum = 0u;
    uint64_t rttSamples = 0u;
    LaiueNetPeerInfo info = {.structSize = sizeof(info)};
    const uint64_t startUs = network.now;
    const double wallStart = PlatformMonotonicSeconds();
    do
    {
        Check(network.now - startUs < PATH_TIMEOUT, "path transfer deadline");
        while (queued < PATH_MESSAGES)
        {
            for (uint32_t byte = 0u; byte < sizeof(queued); ++byte)
                streamMessage[byte] = (uint8_t)(queued >> (8u * byte));
            const LaiueNetStatus status =
                LaiueNetSend(client, peer, 0u, LAIUE_NET_RELIABLE, streamMessage, PATH_MESSAGE);
            if (status == LAIUE_NET_WOULD_BLOCK)
                break;
            Check(status == LAIUE_NET_OK, "queue path message");
            ++queued;
        }
        Step(&network, MILLISECOND);
        Check(LaiueNetUpdate(client, network.now) == LAIUE_NET_OK &&
                  LaiueNetUpdate(server, network.now) == LAIUE_NET_OK,
              "path update");
        Drain(client, &clientTotals, NULL, 0u);
        DrainPath(server, &received);
        Check(clientTotals.disconnected == 0u && clientTotals.messages == 0u &&
                  LaiueNetPeerGetInfo(client, peer, &info) == LAIUE_NET_OK,
              "path stays connected");
        if (deliveryUs == 0u)
        {
            if (info.roundTripMicroseconds != 0u)
            {
                rttSum += info.roundTripMicroseconds;
                ++rttSamples;
            }
            if (received == PATH_MESSAGES)
                deliveryUs = network.now - startUs;
        }
    } while (deliveryUs == 0u || info.queuedReliableBytes != 0u || info.bytesInFlight != 0u);
    Check(rttSamples != 0u, "path samples a round trip");
    const uint64_t wallUs = (uint64_t)((PlatformMonotonicSeconds() - wallStart) * 1000000.0);
    const uint64_t bytes = (uint64_t)PATH_MESSAGE * PATH_MESSAGES;
    LaiueTestRuntimeWrite(label);
    PrintPathValue("seed", seed);
    PrintPathValue("delivered_bytes", bytes);
    PrintPathValue("payload_bps", bytes * 8000000u / deliveryUs);
    PrintPathValue("delivery_us", deliveryUs);
    PrintPathValue("confirmed_us", network.now - startUs);
    PrintPathValue("mean_srtt_us", rttSum / rttSamples);
    PrintPathValue("packets_sent", info.packetsSent);
    PrintPathValue("packets_lost", info.packetsLost);
    PrintPathValue("wall_us", wallUs);
    LaiueTestRuntimeWrite("\n");
    LaiueNetEndpointDestroy(client);
    LaiueNetEndpointDestroy(server);
    LaiueNetSimulatorDestroy(network.simulator);
}

static void ImpairedPath(void)
{
    const LaiueNetSimulatorConditions conditions = {
        .structSize = sizeof(conditions),
        .latencyMicroseconds = 50000u,
        .lossPerMillion = 10000u,
        .bytesPerSecond = 1000000u,
        .queueMicroseconds = 100000u,
    };
    LaiueTestRuntimeWrite("\nImpaired path: one-thread simulator, fixed transfer after handshake");
    PrintPathValue("tick_us", MILLISECOND);
    PrintPathValue("one_way_us", conditions.latencyMicroseconds);
    PrintPathValue("link_bytes_per_s", conditions.bytesPerSecond);
    PrintPathValue("link_queue_us", conditions.queueMicroseconds);
    PrintPathValue("random_loss_ppm", conditions.lossPerMillion);
    PrintPathValue("simulator_queue_datagrams", PATH_QUEUE_DATAGRAMS);
    PrintPathValue("stream_buffer_bytes", PATH_BUFFER_BYTES);
    PrintPathValue("message_bytes", PATH_MESSAGE);
    PrintPathValue("messages", PATH_MESSAGES);
    LaiueTestRuntimeWrite("\n");
    for (uint64_t seed = 41u; seed <= 45u; ++seed)
    {
        PathTransfer(&conditions, "CUBIC", LAIUE_NET_CONGESTION_CUBIC, seed);
        PathTransfer(&conditions, "BBR", LAIUE_NET_CONGESTION_BBR, seed);
    }
}

LAIUE_TEST_ENTRY(NetBenchmarkEntryPoint)
{
    for (uint32_t index = 0u; index < STREAM_MESSAGE; ++index)
        streamMessage[index] = (uint8_t)(index * 7u + 3u);
    Transfer("stream 256 MiB in 64 KiB messages", STREAM_MESSAGE, STREAM_BYTES / STREAM_MESSAGE);
    Transfer("1M reliable 32-byte messages", SMALL_MESSAGE, SMALL_COUNT);
    Crowd();
    LaiueTestRuntimeWrite("checksum: ");
    WriteUnsigned(checksum);
    LaiueTestRuntimeWrite("\n");
    ImpairedPath();
    LAIUE_TEST_SUCCESS();
}
