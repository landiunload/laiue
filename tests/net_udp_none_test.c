/* Платформа без сокетов: готового драйвера UDP нет, а текстовые адреса
 * разбираются и без резолвера. Тест собирает заглушку из исходников, поэтому
 * она проверяется на любой платформе, а не только там, где её выбирает
 * сборка. */

#include "net/net.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>

static void Expect(bool condition, const char *text)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Net without sockets check failed: ");
    LaiueTestRuntimeWrite(text);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(NetUdpNoneTestEntryPoint)
{
    LaiueNetUdpDesc desc = {.structSize = sizeof(desc)};
    LaiueNetUdp *udp = (LaiueNetUdp *)&desc;
    LaiueNetDriverV1 driver = {.structSize = sizeof(driver)};
    LaiueNetAddress addresses[2];
    uint32_t count = 7u;
    Expect(LaiueNetAddressParse("127.0.0.1", &desc.bindAddress) == LAIUE_NET_OK, "address");
    Expect(LaiueNetUdpOpen(&desc, &udp) == LAIUE_NET_UNSUPPORTED && udp == NULL,
           "sockets are unsupported");
    Expect(LaiueNetUdpGetDriver(NULL, &driver) == LAIUE_NET_UNSUPPORTED &&
               LaiueNetUdpGetLocalAddress(NULL, &addresses[0]) == LAIUE_NET_UNSUPPORTED,
           "there is no driver and no local address");
    LaiueNetUdpClose(NULL);
    Expect(LaiueNetResolve("[2001:db8::5]:9", 0u, addresses, 2u, &count) == LAIUE_NET_OK &&
               count == 1u && addresses[0].port == 9u &&
               addresses[0].family == LAIUE_NET_FAMILY_IPV6,
           "a literal address resolves without a resolver");
    Expect(LaiueNetResolve("203.0.113.7", 4000u, addresses, 2u, &count) == LAIUE_NET_OK &&
               count == 1u && addresses[0].port == 4000u,
           "the given port replaces a missing one");
    Expect(LaiueNetResolve("example.com", 1u, addresses, 2u, &count) == LAIUE_NET_UNSUPPORTED &&
               count == 0u,
           "names need a resolver the platform does not have");
    Expect(LaiueNetResolve(NULL, 1u, addresses, 2u, &count) == LAIUE_NET_INVALID_ARGUMENT &&
               LaiueNetResolve("1.2.3.4", 1u, addresses, 2u, NULL) == LAIUE_NET_INVALID_ARGUMENT,
           "resolve checks its arguments");
    LaiueTestRuntimeWrite("net without sockets: passed\n");
    LAIUE_TEST_SUCCESS();
}
