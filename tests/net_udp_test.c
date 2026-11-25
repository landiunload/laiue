/* laiue.net поверх настоящих сокетов UDP на петлевом интерфейсе: драйвер,
 * адреса, двойной стек IPv6 и соединение под реальными часами. Где у
 * системы нет IPv6, проверки IPv6 пропускаются. */

#include "net/net.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PROTOCOL UINT64_C(0x6C61697565756470)
#define DEADLINE_MICROSECONDS UINT64_C(15000000)
#define LARGE_MESSAGE_BYTES 200000u

static uint8_t message[LARGE_MESSAGE_BYTES];

static void Expect(bool condition, const char *text)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net UDP check failed: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static LaiueNetAddress Address(const char *text)
{
    LaiueNetAddress address;
    Expect(LaiueNetAddressParse(text, &address) == LAIUE_NET_OK, text);
    return address;
}

static uint8_t Pattern(uint32_t index, uint32_t position)
{
    return (uint8_t)(index * 71u + position * 13u + (position >> 9u));
}

/* Сокет на петлевом адресе с выданным системой портом; NULL — семейства нет. */
static LaiueNetUdp *OpenLoopback(const char *bindText, LaiueNetAddress *outLocal)
{
    LaiueNetUdpDesc desc = {.structSize = sizeof(desc)};
    LaiueNetUdp *udp = NULL;
    desc.bindAddress = Address(bindText);
    desc.receiveBufferBytes = 1u << 20u;
    if (LaiueNetUdpOpen(&desc, &udp) != LAIUE_NET_OK)
        return NULL;
    Expect(udp != NULL && LaiueNetUdpGetLocalAddress(udp, outLocal) == LAIUE_NET_OK &&
               outLocal->port != 0u,
           "an open socket reports the port it was given");
    return udp;
}

/* Ждёт датаграмму на драйвере не дольше секунды. */
static int32_t ReceiveSoon(const LaiueNetDriverV1 *driver, LaiueNetAddress *from, uint8_t *buffer,
                           uint32_t capacity)
{
    const uint64_t until = LaiueNetClockMicroseconds() + UINT64_C(1000000);
    for (;;)
    {
        const int32_t received = driver->receive(driver->context, from, buffer, capacity);
        if (received != 0 || LaiueNetClockMicroseconds() > until)
            return received;
        PlatformSleepMilliseconds(1u);
    }
}

static void TestDriver(void)
{
    static uint8_t buffer[2048];
    LaiueNetAddress firstAddress;
    LaiueNetAddress secondAddress;
    LaiueNetAddress from;
    LaiueNetDriverV1 first = {.structSize = sizeof(first)};
    LaiueNetDriverV1 second = {.structSize = sizeof(second)};
    LaiueNetUdp *a = OpenLoopback("127.0.0.1", &firstAddress);
    LaiueNetUdp *b = OpenLoopback("127.0.0.1", &secondAddress);
    Expect(a != NULL && b != NULL, "loopback sockets open");
    Expect(firstAddress.family == LAIUE_NET_FAMILY_IPV4 && firstAddress.ip[0] == 127u &&
               firstAddress.port != secondAddress.port,
           "loopback sockets have their own ports");
    Expect(LaiueNetUdpGetDriver(a, &first) == LAIUE_NET_OK &&
               LaiueNetUdpGetDriver(b, &second) == LAIUE_NET_OK,
           "drivers are available");
    Expect(first.receive(first.context, &from, buffer, sizeof(buffer)) == 0,
           "an idle socket has nothing to receive");

    for (uint32_t index = 0u; index < 1400u; ++index)
        buffer[index] = Pattern(1u, index);
    Expect(first.send(first.context, &secondAddress, buffer, 1400u) == 1, "a datagram is sent");
    memset(buffer, 0, sizeof(buffer));
    Expect(ReceiveSoon(&second, &from, buffer, sizeof(buffer)) == 1400 &&
               LaiueNetAddressEqual(&from, &firstAddress),
           "the datagram arrives from the sender's address");
    for (uint32_t index = 0u; index < 1400u; ++index)
    {
        if (buffer[index] != Pattern(1u, index))
            Expect(false, "the datagram arrives intact");
    }

    /* Длинная датаграмма обрезается до буфера, и это видно по размеру. */
    Expect(first.send(first.context, &secondAddress, buffer, 2000u) == 1,
           "a long datagram is sent");
    Expect(ReceiveSoon(&second, &from, buffer, 1453u) == 1453,
           "a long datagram is cut to the buffer");
    /* Пустая датаграмма видна транспорту одним нулевым байтом. */
    Expect(first.send(first.context, &secondAddress, buffer, 0u) == 1, "an empty datagram is sent");
    buffer[0] = 0xFFu;
    Expect(ReceiveSoon(&second, &from, buffer, sizeof(buffer)) == 1 && buffer[0] == 0u,
           "an empty datagram arrives as one zero byte");

    /* Занятый порт второй раз не открывается. */
    LaiueNetUdpDesc busy = {.structSize = sizeof(busy)};
    LaiueNetUdp *twice = NULL;
    busy.bindAddress = firstAddress;
    Expect(LaiueNetUdpOpen(&busy, &twice) == LAIUE_NET_IO_ERROR && twice == NULL,
           "a port in use cannot be opened again");
    busy.bindAddress.family = 0u;
    Expect(LaiueNetUdpOpen(&busy, &twice) == LAIUE_NET_INVALID_ARGUMENT,
           "a socket needs an address family");
    LaiueNetUdpClose(a);
    LaiueNetUdpClose(b);
    LaiueNetUdpClose(NULL);
}

typedef struct Side
{
    LaiueNetUdp *udp;
    LaiueNetDriverV1 driver;
    LaiueNetAddress address;
    LaiueNetEndpoint *endpoint;
    LaiueNetPeer peer;
    uint32_t connected;
    uint32_t disconnected;
    uint32_t reason;
    uint32_t code;
    uint32_t reliable;
    uint32_t unreliable;
    LaiueNetAddress remote;
} Side;

static const uint32_t messageSizes[] = {0u, 1u, 1000u, 5000u, LARGE_MESSAGE_BYTES};
#define MESSAGE_COUNT ((uint32_t)(sizeof(messageSizes) / sizeof(messageSizes[0])))

static void Handle(Side *side, const LaiueNetEvent *event)
{
    if (event->type == LAIUE_NET_EVENT_CONNECTED)
    {
        LaiueNetPeerInfo info = {.structSize = sizeof(info)};
        ++side->connected;
        side->peer = event->peer;
        Expect(LaiueNetPeerGetInfo(side->endpoint, event->peer, &info) == LAIUE_NET_OK,
               "a connected peer is described");
        side->remote = info.address;
        return;
    }
    if (event->type == LAIUE_NET_EVENT_DISCONNECTED)
    {
        ++side->disconnected;
        side->reason = event->reason;
        side->code = event->code;
        return;
    }
    const uint8_t *bytes = event->data;
    if (event->delivery != LAIUE_NET_RELIABLE)
    {
        Expect(event->size == 100u && bytes[0] == 0xA5u, "the unreliable message is intact");
        ++side->unreliable;
        return;
    }
    Expect(side->reliable < MESSAGE_COUNT && event->size == messageSizes[side->reliable],
           "reliable messages arrive whole and in order");
    for (uint32_t position = 0u; position < event->size; ++position)
    {
        if (bytes[position] != Pattern(side->reliable, position))
            Expect(false, "reliable message bytes are intact");
    }
    ++side->reliable;
}

static void Pump(Side *sides, uint32_t count)
{
    const uint64_t now = LaiueNetClockMicroseconds();
    for (uint32_t index = 0u; index < count; ++index)
    {
        LaiueNetEvent event = {.structSize = sizeof(event)};
        Expect(LaiueNetUpdate(sides[index].endpoint, now) == LAIUE_NET_OK,
               "an endpoint over a healthy socket updates");
        while (LaiueNetPollEvent(sides[index].endpoint, &event))
            Handle(&sides[index], &event);
    }
    PlatformSleepMilliseconds(1u);
}

static void OpenSide(Side *side, const char *bindText, uint32_t flags)
{
    LaiueNetEndpointDesc desc = {
        .structSize = sizeof(desc),
        .flags = flags,
        .protocolId = PROTOCOL,
    };
    memset(side, 0, sizeof(*side));
    side->udp = OpenLoopback(bindText, &side->address);
    Expect(side->udp != NULL, "a socket for the endpoint opens");
    Expect(LaiueNetUdpGetDriver(side->udp, &side->driver) == LAIUE_NET_OK, "driver");
    desc.driver = &side->driver;
    Expect(LaiueNetEndpointCreate(&desc, &side->endpoint) == LAIUE_NET_OK, "endpoint creates");
}

static void CloseSide(Side *side)
{
    LaiueNetEndpointDestroy(side->endpoint);
    LaiueNetUdpClose(side->udp);
}

/* Полный обмен: рукопожатие, надёжные сообщения до 200 КБ, ненадёжное,
 * закрытие с кодом. target — адрес сервера, каким его видит клиент. */
static void Exchange(const char *serverBind, const char *clientBind, const char *target,
                     uint8_t expectedFamily)
{
    Side sides[2];
    Side *server = &sides[0];
    Side *client = &sides[1];
    OpenSide(server, serverBind, LAIUE_NET_ENDPOINT_ACCEPT);
    OpenSide(client, clientBind, 0u);
    LaiueNetConnectDesc connect = {.structSize = sizeof(connect)};
    connect.address = Address(target);
    connect.address.port = server->address.port;
    Expect(LaiueNetConnect(client->endpoint, &connect, &client->peer) == LAIUE_NET_OK,
           "connect starts");

    uint64_t until = LaiueNetClockMicroseconds() + DEADLINE_MICROSECONDS;
    while ((client->connected == 0u || server->connected == 0u) &&
           LaiueNetClockMicroseconds() < until)
        Pump(sides, 2u);
    Expect(client->connected == 1u && server->connected == 1u && client->disconnected == 0u,
           "both sides connect over UDP");
    Expect(server->remote.family == expectedFamily && server->remote.port == client->address.port,
           "the server sees the client's own address");

    for (uint32_t index = 0u; index < MESSAGE_COUNT; ++index)
    {
        for (uint32_t position = 0u; position < messageSizes[index]; ++position)
            message[position] = Pattern(index, position);
        Expect(LaiueNetSend(client->endpoint, client->peer, 0u, LAIUE_NET_RELIABLE, message,
                            messageSizes[index]) == LAIUE_NET_OK,
               "a reliable message is queued");
    }
    until = LaiueNetClockMicroseconds() + DEADLINE_MICROSECONDS;
    while (server->reliable != MESSAGE_COUNT && LaiueNetClockMicroseconds() < until)
        Pump(sides, 2u);
    Expect(server->reliable == MESSAGE_COUNT, "every reliable message arrives over UDP");

    /* Ненадёжное сообщение может потеряться и на петле: оно повторяется,
     * пока одно не дойдёт. */
    memset(message, 0xA5, 100u);
    until = LaiueNetClockMicroseconds() + DEADLINE_MICROSECONDS;
    while (server->unreliable == 0u && LaiueNetClockMicroseconds() < until)
    {
        Expect(LaiueNetSend(client->endpoint, client->peer, 0u, LAIUE_NET_UNRELIABLE, message,
                            100u) == LAIUE_NET_OK,
               "an unreliable message is queued");
        for (uint32_t step = 0u; step < 10u && server->unreliable == 0u; ++step)
            Pump(sides, 2u);
    }
    Expect(server->unreliable != 0u, "an unreliable message arrives over UDP");

    Expect(LaiueNetDisconnect(client->endpoint, client->peer, 42u, 0u) == LAIUE_NET_OK,
           "disconnect");
    until = LaiueNetClockMicroseconds() + DEADLINE_MICROSECONDS;
    while ((client->disconnected == 0u || server->disconnected == 0u) &&
           LaiueNetClockMicroseconds() < until)
        Pump(sides, 2u);
    Expect(client->disconnected == 1u && client->reason == LAIUE_NET_REASON_LOCAL,
           "the client closes");
    Expect(server->disconnected == 1u && server->reason == LAIUE_NET_REASON_REMOTE &&
               server->code == 42u,
           "the server learns the code");
    CloseSide(client);
    CloseSide(server);
}

static bool HasIpv6(void)
{
    LaiueNetAddress local;
    LaiueNetUdp *probe = OpenLoopback("::1", &local);
    if (probe == NULL)
        return false;
    LaiueNetUdpClose(probe);
    return true;
}

static bool IsLoopback(const LaiueNetAddress *address)
{
    static const uint8_t ipv6Loopback[16] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                             0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u};
    if (address->family == LAIUE_NET_FAMILY_IPV4)
        return address->ip[0] == 127u;
    return address->family == LAIUE_NET_FAMILY_IPV6 &&
           memcmp(address->ip, ipv6Loopback, sizeof(ipv6Loopback)) == 0;
}

static void TestResolve(void)
{
    LaiueNetAddress addresses[8];
    uint32_t count = 0u;
    Expect(LaiueNetResolve("127.0.0.1", 4000u, addresses, 8u, &count) == LAIUE_NET_OK &&
               count == 1u && addresses[0].port == 4000u && addresses[0].ip[0] == 127u,
           "a literal address resolves without a resolver");
    Expect(LaiueNetResolve("[::1]:7", 0u, addresses, 8u, &count) == LAIUE_NET_OK && count == 1u &&
               addresses[0].port == 7u && IsLoopback(&addresses[0]),
           "a literal keeps its own port when none is given");
    Expect(LaiueNetResolve(NULL, 1u, addresses, 8u, &count) == LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetResolve("localhost", 1u, addresses, 0u, &count) ==
                   LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetResolve("localhost", 1u, addresses, 8u, NULL) == LAIUE_NET_INVALID_ARGUMENT,
           "resolve checks its arguments");
    /* Резолвер может быть недоступен в песочнице; ответив, он называет петлю. */
    const LaiueNetStatus status = LaiueNetResolve("localhost", 9u, addresses, 8u, &count);
    Expect(status == LAIUE_NET_OK || status == LAIUE_NET_IO_ERROR, "localhost resolves or fails");
    for (uint32_t index = 0u; status == LAIUE_NET_OK && index < count; ++index)
        Expect(IsLoopback(&addresses[index]) && addresses[index].port == 9u,
               "localhost is a loopback address with the given port");
    Expect(LaiueNetResolve("name.invalid", 1u, addresses, 8u, &count) == LAIUE_NET_IO_ERROR &&
               count == 0u,
           "a name in the reserved .invalid domain does not resolve");
}

LAIUE_TEST_ENTRY(NetUdpTestEntryPoint)
{
    TestDriver();
    TestResolve();
    Exchange("127.0.0.1", "127.0.0.1", "127.0.0.1", LAIUE_NET_FAMILY_IPV4);
    if (HasIpv6())
    {
        Exchange("::1", "::1", "::1", LAIUE_NET_FAMILY_IPV6);
        /* Сокет IPv6 на всех интерфейсах принимает клиента IPv4 и видит его
         * обычным адресом IPv4, а не отображённым в IPv6. */
        Exchange("::", "127.0.0.1", "127.0.0.1", LAIUE_NET_FAMILY_IPV4);
    }
    else
    {
        LaiueTestRuntimeWrite("net udp: IPv6 is not available, its checks are skipped\n");
    }
    LaiueTestRuntimeWrite("net udp: passed\n");
    LAIUE_TEST_SUCCESS();
}
