#include "net/net.h"

#include "platform/socket.h"
#include "platform/system.h"

#include <string.h>

/* Драйвер датаграмм поверх сокета UDP платформы. */

#define RESOLVE_CAPACITY 16u

struct LaiueNetUdp
{
    PlatformUdpSocket socket;
};

static void ToPlatform(const LaiueNetAddress *address, PlatformSocketAddress *output)
{
    memset(output, 0, sizeof(*output));
    memcpy(output->ip, address->ip, sizeof(output->ip));
    output->scope = address->scope;
    output->port = address->port;
    output->family = address->family;
}

static void FromPlatform(const PlatformSocketAddress *address, LaiueNetAddress *output)
{
    memset(output, 0, sizeof(*output));
    memcpy(output->ip, address->ip, sizeof(output->ip));
    output->scope = address->scope;
    output->port = address->port;
    output->family = address->family;
}

static int32_t UdpSend(void *context, const LaiueNetAddress *to, const void *bytes, uint32_t size)
{
    LaiueNetUdp *udp = context;
    PlatformSocketAddress target;
    ToPlatform(to, &target);
    return PlatformUdpSend(&udp->socket, &target, bytes, size);
}

static int32_t UdpReceive(void *context, LaiueNetAddress *outFrom, void *buffer, uint32_t capacity)
{
    LaiueNetUdp *udp = context;
    PlatformSocketAddress source;
    memset(&source, 0, sizeof(source));
    const int32_t received = PlatformUdpReceive(&udp->socket, &source, buffer, capacity);
    if (received == PLATFORM_UDP_NONE)
        return 0;
    if (received < 0)
        return -1;
    FromPlatform(&source, outFrom);
    if (received != 0)
        return received;
    /* Пустую датаграмму таблица драйвера выразить не может: ноль в ней значит
     * «ничего нет». Транспорту она уходит одним нулевым байтом — такой пакет
     * он отбрасывает как чужой и засчитывает в свой предел приёма, поэтому
     * поток пустых датаграмм не останавливает чтение сокета. */
    *(uint8_t *)buffer = 0u;
    return 1;
}

LaiueNetStatus LaiueNetUdpOpen(const LaiueNetUdpDesc *desc, LaiueNetUdp **outUdp)
{
    if (outUdp == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outUdp = NULL;
    if (desc == NULL || desc->structSize < sizeof(*desc) ||
        (desc->bindAddress.family != LAIUE_NET_FAMILY_IPV4 &&
         desc->bindAddress.family != LAIUE_NET_FAMILY_IPV6))
        return LAIUE_NET_INVALID_ARGUMENT;
    LaiueNetUdp *udp = PlatformAllocate(sizeof(*udp), true);
    if (udp == NULL)
        return LAIUE_NET_OUT_OF_MEMORY;
    PlatformSocketAddress bindAddress;
    ToPlatform(&desc->bindAddress, &bindAddress);
    if (!PlatformUdpOpen(&bindAddress, desc->sendBufferBytes, desc->receiveBufferBytes,
                         &udp->socket))
    {
        PlatformFree(udp);
        return LAIUE_NET_IO_ERROR;
    }
    *outUdp = udp;
    return LAIUE_NET_OK;
}

void LaiueNetUdpClose(LaiueNetUdp *udp)
{
    if (udp == NULL)
        return;
    PlatformUdpClose(&udp->socket);
    PlatformFree(udp);
}

LaiueNetStatus LaiueNetUdpGetDriver(LaiueNetUdp *udp, LaiueNetDriverV1 *outDriver)
{
    if (udp == NULL || outDriver == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    outDriver->structSize = sizeof(*outDriver);
    outDriver->context = udp;
    outDriver->send = UdpSend;
    outDriver->receive = UdpReceive;
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetUdpGetLocalAddress(const LaiueNetUdp *udp, LaiueNetAddress *outAddress)
{
    PlatformSocketAddress local;
    if (udp == NULL || outAddress == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    if (!PlatformUdpLocalAddress(&udp->socket, &local))
        return LAIUE_NET_IO_ERROR;
    FromPlatform(&local, outAddress);
    return LAIUE_NET_OK;
}

LaiueNetStatus LaiueNetResolve(const char *host, uint16_t port, LaiueNetAddress *outAddresses,
                               uint32_t capacity, uint32_t *outCount)
{
    PlatformSocketAddress found[RESOLVE_CAPACITY];
    if (outCount == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outCount = 0u;
    if (host == NULL || outAddresses == NULL || capacity == 0u)
        return LAIUE_NET_INVALID_ARGUMENT;
    /* Текстовый адрес понятен без резолвера и без обращения к сети. */
    if (LaiueNetAddressParse(host, &outAddresses[0]) == LAIUE_NET_OK)
    {
        if (port != 0u)
            outAddresses[0].port = port;
        *outCount = 1u;
        return LAIUE_NET_OK;
    }
    const uint32_t count =
        PlatformResolveHost(host, found, capacity < RESOLVE_CAPACITY ? capacity : RESOLVE_CAPACITY);
    for (uint32_t index = 0u; index < count; ++index)
    {
        FromPlatform(&found[index], &outAddresses[index]);
        outAddresses[index].port = port;
    }
    *outCount = count;
    return count != 0u ? LAIUE_NET_OK : LAIUE_NET_IO_ERROR;
}
