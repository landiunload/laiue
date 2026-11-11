#include "platform/socket.h"
#include "platform/system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string.h>

/* Из mstcpip.h. Без этого ICMP-ответ «порт недоступен» на любую отправленную
 * датаграмму превращается в ошибку WSAECONNRESET следующего recvfrom. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* opaque[0] — SOCKET, opaque[1] — семейство адресов; ноль значит «закрыт». */
_Static_assert(sizeof(SOCKET) <= sizeof(uintptr_t), "PlatformUdpSocket storage must cover SOCKET");

static uint16_t SwapPort(uint16_t port)
{
    return (uint16_t)((port << 8u) | (port >> 8u));
}

/* Длина родного адреса или 0, если сокет этого семейства до него не достаёт. */
static int ToNative(const PlatformSocketAddress *address, uintptr_t socketFamily,
                    SOCKADDR_STORAGE *storage)
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
        return (int)sizeof(*native);
    }
    if (address->family != PLATFORM_SOCKET_IPV4)
        return 0;
    struct sockaddr_in *native = (struct sockaddr_in *)storage;
    native->sin_family = AF_INET;
    native->sin_port = SwapPort(address->port);
    memcpy(&native->sin_addr, address->ip, 4u);
    return (int)sizeof(*native);
}

static bool FromNative(const SOCKADDR_STORAGE *storage, PlatformSocketAddress *address)
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

static int ClampBufferSize(uint32_t bytes)
{
    return bytes > (uint32_t)INT32_MAX ? INT32_MAX : (int)bytes;
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
    WSADATA startup;
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0)
        return false;
    const uintptr_t family = bindAddress->family;
    const SOCKET handle =
        socket(family == PLATFORM_SOCKET_IPV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }
    u_long nonBlocking = 1u;
    bool succeeded = ioctlsocket(handle, (long)FIONBIO, &nonBlocking) == 0;
    BOOL reportResets = FALSE;
    DWORD returned = 0u;
    /* Старые системы этого управляющего кода не знают; сокет годится и так. */
    (void)WSAIoctl(handle, SIO_UDP_CONNRESET, &reportResets, (DWORD)sizeof(reportResets), NULL, 0u,
                   &returned, NULL, NULL);
    if (family == PLATFORM_SOCKET_IPV6)
    {
        const DWORD onlyIpv6 = 0u;
        succeeded = succeeded && setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY,
                                            (const char *)&onlyIpv6, (int)sizeof(onlyIpv6)) == 0;
    }
    if (sendBufferBytes != 0u)
    {
        const int size = ClampBufferSize(sendBufferBytes);
        (void)setsockopt(handle, SOL_SOCKET, SO_SNDBUF, (const char *)&size, (int)sizeof(size));
    }
    if (receiveBufferBytes != 0u)
    {
        const int size = ClampBufferSize(receiveBufferBytes);
        (void)setsockopt(handle, SOL_SOCKET, SO_RCVBUF, (const char *)&size, (int)sizeof(size));
    }
    SOCKADDR_STORAGE native;
    const int nativeLength = ToNative(bindAddress, family, &native);
    succeeded = succeeded && nativeLength != 0 &&
                bind(handle, (const struct sockaddr *)&native, nativeLength) == 0;
    if (!succeeded)
    {
        closesocket(handle);
        WSACleanup();
        return false;
    }
    outSocket->opaque[0] = (uintptr_t)handle;
    outSocket->opaque[1] = family;
    return true;
}

void PlatformUdpClose(PlatformUdpSocket *socket)
{
    if (!IsOpen(socket))
        return;
    closesocket((SOCKET)socket->opaque[0]);
    WSACleanup();
    socket->opaque[0] = 0u;
    socket->opaque[1] = 0u;
}

bool PlatformUdpLocalAddress(const PlatformUdpSocket *socket, PlatformSocketAddress *outAddress)
{
    if (!IsOpen(socket) || outAddress == NULL)
        return false;
    SOCKADDR_STORAGE native;
    int nativeLength = (int)sizeof(native);
    memset(&native, 0, sizeof(native));
    return getsockname((SOCKET)socket->opaque[0], (struct sockaddr *)&native, &nativeLength) == 0 &&
           FromNative(&native, outAddress);
}

int32_t PlatformUdpSend(PlatformUdpSocket *socket, const PlatformSocketAddress *to,
                        const void *bytes, uint32_t size)
{
    if (!IsOpen(socket) || to == NULL || (bytes == NULL && size != 0u))
        return -1;
    SOCKADDR_STORAGE native;
    const int nativeLength = ToNative(to, socket->opaque[1], &native);
    if (nativeLength == 0 || size > UINT16_MAX)
        return 0;
    if (sendto((SOCKET)socket->opaque[0], (const char *)bytes, (int)size, 0,
               (const struct sockaddr *)&native, nativeLength) != SOCKET_ERROR)
        return 1;
    switch (WSAGetLastError())
    {
    case WSAEWOULDBLOCK:
    case WSAENOBUFS:
    case WSAEMSGSIZE:
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAEADDRNOTAVAIL:
    case WSAEACCES:
    case WSAECONNRESET:
    case WSAENETRESET:
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
    SOCKADDR_STORAGE native;
    int nativeLength = (int)sizeof(native);
    memset(&native, 0, sizeof(native));
    const int received = recvfrom((SOCKET)socket->opaque[0], (char *)buffer, (int)capacity, 0,
                                  (struct sockaddr *)&native, &nativeLength);
    if (received == SOCKET_ERROR)
    {
        switch (WSAGetLastError())
        {
        case WSAEWOULDBLOCK:
            return PLATFORM_UDP_NONE;
        case WSAEMSGSIZE:
            /* Датаграмма длиннее буфера: начало уже в нём, остаток потерян. */
            return FromNative(&native, outFrom) ? (int32_t)capacity : 0;
        case WSAECONNRESET:
        case WSAENETRESET:
            /* Отголоски прежних отправок: ICMP-ответы, сброс сети. */
            return 0;
        default:
            return PLATFORM_UDP_BROKEN;
        }
    }
    /* Датаграмма с адресом неизвестного семейства никому не адресована. */
    return FromNative(&native, outFrom) ? received : 0;
}

uint32_t PlatformResolveHost(const char *host, PlatformSocketAddress *outAddresses,
                             uint32_t capacity)
{
    /* Имя узла DNS не длиннее 253 символов. */
    wchar_t wideHost[256];
    uint32_t length = 0u;
    if (host == NULL || outAddresses == NULL || capacity == 0u)
        return 0u;
    while (host[length] != '\0' && length < 256u)
        ++length;
    if (length == 0u || length >= 256u || !PlatformUtf8ToWide(host, length, wideHost, 256u, NULL))
        return 0u;
    WSADATA startup;
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0)
        return 0u;
    ADDRINFOW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    ADDRINFOW *found = NULL;
    uint32_t count = 0u;
    if (GetAddrInfoW(wideHost, NULL, &hints, &found) == 0)
    {
        for (const ADDRINFOW *entry = found; entry != NULL && count < capacity;
             entry = entry->ai_next)
        {
            SOCKADDR_STORAGE native;
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
        FreeAddrInfoW(found);
    }
    WSACleanup();
    return count;
}
