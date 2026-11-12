#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
/* Под голым _POSIX_C_SOURCE Darwin прячет часть сетевых объявлений, а
 * IPV6_DONTFRAG объявляет только с набором параметров RFC 3542. */
#define _DARWIN_C_SOURCE 1
#define __APPLE_USE_RFC_3542 1
#endif

#include "platform/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* opaque[0] — дескриптор, opaque[1] — семейство адресов; ноль значит «закрыт». */

static uint16_t SwapPort(uint16_t port)
{
    const uint16_t probe = 1u;
    /* Порт в родном адресе идёт в сетевом порядке; на big-endian он уже таков. */
    return *(const uint8_t *)&probe == 1u ? (uint16_t)((port << 8u) | (port >> 8u)) : port;
}

/* Длина родного адреса или 0, если сокет этого семейства до него не достаёт. */
static socklen_t ToNative(const PlatformSocketAddress *address, uintptr_t socketFamily,
                          struct sockaddr_storage *storage)
{
    memset(storage, 0, sizeof(*storage));
    if (socketFamily == PLATFORM_SOCKET_IPV6)
    {
        struct sockaddr_in6 *native = (struct sockaddr_in6 *)storage;
        native->sin6_family = AF_INET6;
        native->sin6_port = SwapPort(address->port);
        if (address->family == PLATFORM_SOCKET_IPV6)
        {
            memcpy(&native->sin6_addr, address->ip, 16u);
            native->sin6_scope_id = address->scope;
        }
        else if (address->family == PLATFORM_SOCKET_IPV4)
        {
            /* ::ffff:a.b.c.d */
            native->sin6_addr.s6_addr[10] = 0xFFu;
            native->sin6_addr.s6_addr[11] = 0xFFu;
            memcpy(&native->sin6_addr.s6_addr[12], address->ip, 4u);
        }
        else
        {
            return 0;
        }
        return (socklen_t)sizeof(*native);
    }
    if (address->family != PLATFORM_SOCKET_IPV4)
        return 0;
    struct sockaddr_in *native = (struct sockaddr_in *)storage;
    native->sin_family = AF_INET;
    native->sin_port = SwapPort(address->port);
    memcpy(&native->sin_addr, address->ip, 4u);
    return (socklen_t)sizeof(*native);
}

static bool FromNative(const struct sockaddr_storage *storage, PlatformSocketAddress *address)
{
    static const uint8_t mappedPrefix[12] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xFFu, 0xFFu};
    memset(address, 0, sizeof(*address));
    if (storage->ss_family == AF_INET)
    {
        const struct sockaddr_in *native = (const struct sockaddr_in *)storage;
        address->family = PLATFORM_SOCKET_IPV4;
        address->port = SwapPort(native->sin_port);
        memcpy(address->ip, &native->sin_addr, 4u);
        return true;
    }
    if (storage->ss_family != AF_INET6)
        return false;
    const struct sockaddr_in6 *native = (const struct sockaddr_in6 *)storage;
    address->port = SwapPort(native->sin6_port);
    if (memcmp(native->sin6_addr.s6_addr, mappedPrefix, sizeof(mappedPrefix)) == 0)
    {
        address->family = PLATFORM_SOCKET_IPV4;
        memcpy(address->ip, &native->sin6_addr.s6_addr[12], 4u);
        return true;
    }
    address->family = PLATFORM_SOCKET_IPV6;
    address->scope = native->sin6_scope_id;
    memcpy(address->ip, &native->sin6_addr, 16u);
    return true;
}

static bool IsOpen(const PlatformUdpSocket *socket)
{
    return socket != NULL && socket->opaque[1] != 0u;
}

static bool IsWouldBlock(int error)
{
#if EWOULDBLOCK != EAGAIN
    if (error == EWOULDBLOCK)
        return true;
#endif
    return error == EAGAIN;
}

static bool MakeNonBlocking(int descriptor)
{
    const int status = fcntl(descriptor, F_GETFL, 0);
    const int flags = fcntl(descriptor, F_GETFD, 0);
    return status >= 0 && flags >= 0 && fcntl(descriptor, F_SETFL, status | O_NONBLOCK) == 0 &&
           fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static void SetBufferSize(int descriptor, int option, uint32_t bytes)
{
    if (bytes != 0u)
    {
        const int size = bytes > (uint32_t)INT32_MAX ? INT32_MAX : (int)bytes;
        (void)setsockopt(descriptor, SOL_SOCKET, option, &size, (socklen_t)sizeof(size));
    }
}

/* Датаграмму больше пути система не дробит, а отвергает: размер пути
 * транспорт ищет сам, и фрагменты скрыли бы от него настоящий. Система,
 * которая запрета не знает, остаётся при дроблении. */
static void ForbidFragmentation(int descriptor, uintptr_t family)
{
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_PROBE)
    /* Linux ставит DF и не сверяется с размером пути, который ядро запомнило
     * по ICMP: подложное сообщение иначе запирало бы и пакеты базового
     * размера. Сокету IPv6 нужны обе настройки: IPv4 идёт через него
     * отображёнными адресами. */
    const int probe = IP_PMTUDISC_PROBE;
    (void)setsockopt(descriptor, IPPROTO_IP, IP_MTU_DISCOVER, &probe, (socklen_t)sizeof(probe));
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_PROBE)
    if (family == PLATFORM_SOCKET_IPV6)
    {
        const int probeIpv6 = IPV6_PMTUDISC_PROBE;
        (void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &probeIpv6,
                         (socklen_t)sizeof(probeIpv6));
    }
#else
    (void)family;
#endif
#else
    /* Darwin запрещает дробление только своему семейству сокета: IPv4 через
     * сокет IPv6 дробится по-прежнему. */
    const int wholeDatagrams = 1;
#if defined(IP_DONTFRAG)
    if (family == PLATFORM_SOCKET_IPV4)
        (void)setsockopt(descriptor, IPPROTO_IP, IP_DONTFRAG, &wholeDatagrams,
                         (socklen_t)sizeof(wholeDatagrams));
#endif
#if defined(IPV6_DONTFRAG)
    if (family == PLATFORM_SOCKET_IPV6)
        (void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_DONTFRAG, &wholeDatagrams,
                         (socklen_t)sizeof(wholeDatagrams));
#endif
    (void)descriptor;
    (void)family;
    (void)wholeDatagrams;
#endif
}

bool PlatformUdpOpen(const PlatformSocketAddress *bindAddress, uint32_t sendBufferBytes,
                     uint32_t receiveBufferBytes, PlatformUdpSocket *outSocket)
{
    if (outSocket == NULL)
        return false;
    outSocket->opaque[0] = 0u;
    outSocket->opaque[1] = 0u;
    if (bindAddress == NULL || (bindAddress->family != PLATFORM_SOCKET_IPV4 &&
                                bindAddress->family != PLATFORM_SOCKET_IPV6))
        return false;
    const uintptr_t family = bindAddress->family;
    const int descriptor =
        socket(family == PLATFORM_SOCKET_IPV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0)
        return false;
    bool succeeded = MakeNonBlocking(descriptor);
    if (family == PLATFORM_SOCKET_IPV6)
    {
        const int onlyIpv6 = 0;
        succeeded = succeeded && setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &onlyIpv6,
                                            (socklen_t)sizeof(onlyIpv6)) == 0;
    }
    ForbidFragmentation(descriptor, family);
    SetBufferSize(descriptor, SO_SNDBUF, sendBufferBytes);
    SetBufferSize(descriptor, SO_RCVBUF, receiveBufferBytes);
    struct sockaddr_storage native;
    const socklen_t nativeLength = ToNative(bindAddress, family, &native);
    succeeded = succeeded && nativeLength != 0 &&
                bind(descriptor, (const struct sockaddr *)&native, nativeLength) == 0;
    if (!succeeded)
    {
        close(descriptor);
        return false;
    }
    outSocket->opaque[0] = (uintptr_t)descriptor;
    outSocket->opaque[1] = family;
    return true;
}

void PlatformUdpClose(PlatformUdpSocket *socket)
{
    if (!IsOpen(socket))
        return;
    close((int)socket->opaque[0]);
    socket->opaque[0] = 0u;
    socket->opaque[1] = 0u;
}

bool PlatformUdpLocalAddress(const PlatformUdpSocket *socket, PlatformSocketAddress *outAddress)
{
    if (!IsOpen(socket) || outAddress == NULL)
        return false;
    struct sockaddr_storage native;
    socklen_t nativeLength = (socklen_t)sizeof(native);
    memset(&native, 0, sizeof(native));
    return getsockname((int)socket->opaque[0], (struct sockaddr *)&native, &nativeLength) == 0 &&
           FromNative(&native, outAddress);
}

int32_t PlatformUdpSend(PlatformUdpSocket *socket, const PlatformSocketAddress *to,
                        const void *bytes, uint32_t size)
{
    if (!IsOpen(socket) || to == NULL || (bytes == NULL && size != 0u))
        return -1;
    struct sockaddr_storage native;
    const socklen_t nativeLength = ToNative(to, socket->opaque[1], &native);
    if (nativeLength == 0 || size > UINT16_MAX)
        return 0;
    /* Пустая датаграмма допустима и без буфера, но Bionic объявляет буфер
     * sendto ненулевым. */
    static const uint8_t empty = 0u;
    const void *payload = bytes != NULL ? bytes : &empty;
    for (;;)
    {
        if (sendto((int)socket->opaque[0], payload, (size_t)size, 0,
                   (const struct sockaddr *)&native, nativeLength) >= 0)
            return 1;
        if (errno != EINTR)
            break;
    }
    if (IsWouldBlock(errno))
        return 0;
    switch (errno)
    {
    case ENOBUFS:
    case ENOMEM:
    case EMSGSIZE:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case EADDRNOTAVAIL:
    case EACCES:
    case EPERM:
    case ECONNREFUSED:
    case ECONNRESET:
        return 0;
    default:
        return -1;
    }
}

int32_t PlatformUdpReceive(PlatformUdpSocket *socket, PlatformSocketAddress *outFrom, void *buffer,
                           uint32_t capacity)
{
    if (!IsOpen(socket) || outFrom == NULL || buffer == NULL || capacity == 0u ||
        capacity > (uint32_t)INT32_MAX)
        return PLATFORM_UDP_BROKEN;
    struct sockaddr_storage native;
    socklen_t nativeLength = (socklen_t)sizeof(native);
    ssize_t received = 0;
    do
    {
        nativeLength = (socklen_t)sizeof(native);
        memset(&native, 0, sizeof(native));
        received = recvfrom((int)socket->opaque[0], buffer, (size_t)capacity, 0,
                            (struct sockaddr *)&native, &nativeLength);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
    {
        if (IsWouldBlock(errno))
            return PLATFORM_UDP_NONE;
        /* Отголоски прежних отправок: ICMP-ответы. */
        if (errno == ECONNREFUSED || errno == ECONNRESET)
            return 0;
        return PLATFORM_UDP_BROKEN;
    }
    /* Датаграмму длиннее буфера система обрезает до capacity молча, а
     * датаграмма с адресом неизвестного семейства никому не адресована. */
    return FromNative(&native, outFrom) ? (int32_t)received : 0;
}

uint32_t PlatformResolveHost(const char *host, PlatformSocketAddress *outAddresses,
                             uint32_t capacity)
{
    if (host == NULL || host[0] == '\0' || outAddresses == NULL || capacity == 0u)
        return 0u;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo *found = NULL;
    if (getaddrinfo(host, NULL, &hints, &found) != 0)
        return 0u;
    uint32_t count = 0u;
    for (const struct addrinfo *entry = found; entry != NULL && count < capacity;
         entry = entry->ai_next)
    {
        struct sockaddr_storage native;
        if (entry->ai_addr == NULL || entry->ai_addrlen > sizeof(native))
            continue;
        memset(&native, 0, sizeof(native));
        memcpy(&native, entry->ai_addr, entry->ai_addrlen);
        if (FromNative(&native, &outAddresses[count]))
        {
            outAddresses[count].port = 0u;
            ++count;
        }
    }
    freeaddrinfo(found);
    return count;
}
