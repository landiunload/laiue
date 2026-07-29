#include "core/player_command_mapper.h"
#include "network/network.h"
#include "test_runtime.h"

static void Expect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static void VerifyDirection(float forward, float right)
{
    const float pi = 3.14159265358979323846f;
    for (uint32_t sample = 0u; sample <= 4096u; ++sample)
    {
        float yaw = -pi + (2.0f * pi * (float)sample) / 4096.0f;
        double direction[2];
        PlayerCommandMapperBuildWorldDirection(forward, right, yaw, direction);
        double expectedLengthSquared =
            (double)forward * (double)forward + (double)right * (double)right;
        double actualLengthSquared = direction[0] * direction[0] + direction[1] * direction[1];
        double difference = actualLengthSquared - expectedLengthSquared;
        if (difference < 0.0)
        {
            difference = -difference;
        }
        Expect(difference <= 2e-6, "world direction did not preserve input length");

        NetworkInputCommand input = {
            .sequence = sample + 1u,
            .movementX = (float)direction[0],
            .movementY = (float)direction[1],
            .yaw = yaw,
        };
        NetworkInputCommand canonical;
        Expect(NetworkInputCanonicalize(&input, &canonical),
               "production protocol rejected mapped movement direction");
    }
}

LAIUE_TEST_ENTRY(PlayerCommandMapperTestEntryPoint)
{
    VerifyDirection(1.0f, 0.0f);
    VerifyDirection(0.0f, 1.0f);
    VerifyDirection(0.70710678f, 0.70710678f);
    LaiueTestRuntimeWrite("player command mapper: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
