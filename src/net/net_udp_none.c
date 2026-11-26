#include "net/net.h"

#include <stddef.h>

/* Платформа без сокетов: транспорт работает с драйвером приложения, а
 * готового драйвера UDP и резолвера имён нет. */

LaiueNetStatus LaiueNetUdpOpen(const LaiueNetUdpDesc *desc, LaiueNetUdp **outUdp)
{
    (void)desc;
    if (outUdp != NULL)
        *outUdp = NULL;
    return LAIUE_NET_UNSUPPORTED;
}

void LaiueNetUdpClose(LaiueNetUdp *udp)
{
    (void)udp;
}

LaiueNetStatus LaiueNetUdpGetDriver(LaiueNetUdp *udp, LaiueNetDriverV1 *outDriver)
{
    (void)udp;
    (void)outDriver;
    return LAIUE_NET_UNSUPPORTED;
}

LaiueNetStatus LaiueNetUdpGetLocalAddress(const LaiueNetUdp *udp, LaiueNetAddress *outAddress)
{
    (void)udp;
    (void)outAddress;
    return LAIUE_NET_UNSUPPORTED;
}

LaiueNetStatus LaiueNetResolve(const char *host, uint16_t port, LaiueNetAddress *outAddresses,
                               uint32_t capacity, uint32_t *outCount)
{
    if (outCount == NULL)
        return LAIUE_NET_INVALID_ARGUMENT;
    *outCount = 0u;
    if (host == NULL || outAddresses == NULL || capacity == 0u)
        return LAIUE_NET_INVALID_ARGUMENT;
    /* Текстовый адрес разбирается и здесь: резолвер для него не нужен. */
    if (LaiueNetAddressParse(host, &outAddresses[0]) != LAIUE_NET_OK)
        return LAIUE_NET_UNSUPPORTED;
    if (port != 0u)
        outAddresses[0].port = port;
    *outCount = 1u;
    return LAIUE_NET_OK;
}
