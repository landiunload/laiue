/* Сокеты платформы не дают системе дробить датаграммы: размер пути
 * транспорт ищет сам, и фрагменты скрыли бы от него настоящий. На петлевом
 * интерфейсе слишком большой датаграммы не бывает, поэтому проверка читает
 * настройки открытого сокета у самой системы. */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#define __APPLE_USE_RFC_3542 1
#endif
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "platform/socket.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Platform socket check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

/* Сокет на всех интерфейсах с выданным системой портом; false — у системы
 * нет такого семейства. */
static bool Open(uint8_t family, PlatformUdpSocket *outSocket)
{
    const PlatformSocketAddress any = {.family = family};
    return PlatformUdpOpen(&any, 0u, 0u, outSocket);
}

/* Целая настройка сокета, как её хранит система. */
static bool Option(const PlatformUdpSocket *socket, int level, int name, int *outValue)
{
#if defined(_WIN32)
    DWORD value = 0u;
    int length = (int)sizeof(value);
    if (getsockopt((SOCKET)socket->opaque[0], level, name, (char *)&value, &length) != 0)
        return false;
#else
    int value = 0;
    socklen_t length = (socklen_t)sizeof(value);
    if (getsockopt((int)socket->opaque[0], level, name, &value, &length) != 0)
        return false;
#endif
    *outValue = (int)value;
    return true;
}

/* Система не дробит датаграммы IPv4, которые шлёт сокет, а Windows и Linux
 * к тому же не сверяют их с размером пути, который запомнили по ICMP. */
static bool WholeIpv4(const PlatformUdpSocket *socket)
{
    int value = 0;
#if defined(_WIN32)
    return Option(socket, IPPROTO_IP, IP_DONTFRAGMENT, &value) && value != 0 &&
           Option(socket, IPPROTO_IP, IP_MTU_DISCOVER, &value) && value == IP_PMTUDISC_PROBE;
#elif defined(IP_MTU_DISCOVER)
    return Option(socket, IPPROTO_IP, IP_MTU_DISCOVER, &value) && value == IP_PMTUDISC_PROBE;
#else
    return Option(socket, IPPROTO_IP, IP_DONTFRAG, &value) && value != 0;
#endif
}

/* То же о датаграммах IPv6. */
static bool WholeIpv6(const PlatformUdpSocket *socket)
{
    int value = 0;
#if defined(_WIN32)
    return Option(socket, IPPROTO_IPV6, IPV6_DONTFRAG, &value) && value != 0 &&
           Option(socket, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &value) && value == IP_PMTUDISC_PROBE;
#elif defined(IPV6_MTU_DISCOVER)
    return Option(socket, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &value) &&
           value == IPV6_PMTUDISC_PROBE;
#else
    return Option(socket, IPPROTO_IPV6, IPV6_DONTFRAG, &value) && value != 0;
#endif
}

LAIUE_TEST_ENTRY(PlatformSocketTestEntryPoint)
{
    PlatformUdpSocket socket = {{0u, 0u}};
    Expect(Open(PLATFORM_SOCKET_IPV4, &socket), "an IPv4 socket opens");
    Expect(WholeIpv4(&socket), "an IPv4 socket does not let the system fragment its datagrams");
    PlatformUdpClose(&socket);

    /* Где у системы нет IPv6, его проверки пропускаются. */
    if (Open(PLATFORM_SOCKET_IPV6, &socket))
    {
        Expect(WholeIpv6(&socket), "nor does an IPv6 socket");
#if !defined(__APPLE__)
        /* Сокет IPv6 шлёт и IPv4, отображёнными адресами. Darwin запрета для
         * них не знает. */
        Expect(WholeIpv4(&socket), "nor the IPv4 datagrams an IPv6 socket sends");
#endif
        PlatformUdpClose(&socket);
    }
    LaiueTestRuntimeWrite("platform socket: passed\n");
    LAIUE_TEST_SUCCESS();
}
