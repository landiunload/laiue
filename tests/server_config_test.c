#include <stdbool.h>
#include "platform/system.h"
#include "server/server_config.h"
#include "test_runtime.h"

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("server config test failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool TextEquals(const char *left, const char *right)
{
    uint32_t index = 0;
    while (left[index] != '\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == '\0' && right[index] == '\0';
}

LAIUE_TEST_ENTRY(ServerConfigTestEntryPoint)
{
    char testCase[32];
    uint32_t testCaseLength = PlatformGetEnvironmentUtf8(
        "LAIUE_TEST_CASE", testCase, sizeof(testCase));
    Expect(testCaseLength != 0, "missing LAIUE_TEST_CASE");

    ServerConfiguration configuration;
    ServerConfigurationLoad(&configuration);
    if (TextEquals(testCase, "valid"))
    {
        Expect(configuration.port == 30000U, "custom UDP port");
        Expect(configuration.maximumPeers == 7U, "maximum peers override");
        Expect(configuration.worldSeed == -12345, "world seed override");
        Expect(configuration.addressFamily ==
                   SERVER_ADDRESS_FAMILY_IPV6,
               "IPv6 family");
        Expect(TextEquals(configuration.listenAddress, "::1"),
               "IPv6 listen address");
        Expect(configuration.allowContentDownloads,
               "content downloads override");
        Expect(TextEquals(
                   configuration.certificateFile, "cert.pem"),
               "certificate path");
        Expect(TextEquals(
                   configuration.privateKeyFile, "key.pem"),
               "private key path");
    }
    else if (TextEquals(testCase, "invalid-dual"))
    {
        Expect(configuration.addressFamily ==
                   SERVER_ADDRESS_FAMILY_DUAL,
               "dual family");
        Expect(configuration.listenAddress[0] == '\0',
               "concrete address must invalidate dual mode");
    }
    else
    {
        Expect(false, "unknown test case");
    }

    LAIUE_TEST_SUCCESS();
}
