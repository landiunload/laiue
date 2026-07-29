#include "server/server_tick_clock.h"
#include "test_runtime.h"

#include <stdint.h>

#define TEST_TICK_RATE 60U
#define TEST_TICK_SECONDS (1.0 / (double)TEST_TICK_RATE)
#define TEST_MAXIMUM_CATCH_UP_TICKS 8U
#define TEST_MAXIMUM_BACKLOG_SECONDS (256.0 / (double)TEST_TICK_RATE)

static void ClockExpect(bool condition, const char *message)
{
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t ConsumeBatch(ServerTickClock *clock)
{
    uint32_t steps = 0;
    while (steps < TEST_MAXIMUM_CATCH_UP_TICKS &&
           ServerTickClockConsumeTick(clock, TEST_TICK_SECONDS))
    {
        ++steps;
    }
    return steps;
}

static void TestRenderPartitions(void)
{
    static const uint32_t rates[] = {30U, 60U, 144U};
    for (uint32_t rateIndex = 0; rateIndex < sizeof(rates) / sizeof(rates[0]); ++rateIndex)
    {
        ServerTickClock clock;
        ServerTickClockInitialize(&clock);
        uint32_t ticks = 0;
        for (uint32_t frame = 0; frame < rates[rateIndex] * 10U; ++frame)
        {
            ServerTickClockAccumulate(&clock, 1.0 / (double)rates[rateIndex],
                                      TEST_MAXIMUM_BACKLOG_SECONDS);
            ticks += ConsumeBatch(&clock);
        }
        ClockExpect(ticks == TEST_TICK_RATE * 10U, "server clock drifted across render partitions");
        ClockExpect(!ServerTickClockHasTick(&clock, TEST_TICK_SECONDS),
                    "server clock retained an extra partition tick");
    }
}

static void TestHitchTailIsRetained(void)
{
    ServerTickClock clock;
    ServerTickClockInitialize(&clock);
    ServerTickClockAccumulate(&clock, 0.25, TEST_MAXIMUM_BACKLOG_SECONDS);

    ClockExpect(ConsumeBatch(&clock) == 8U, "first hitch catch-up batch is not bounded");
    ClockExpect(ServerTickClockHasTick(&clock, TEST_TICK_SECONDS),
                "bounded catch-up discarded the hitch tail");
    ClockExpect(ConsumeBatch(&clock) == 7U,
                "second catch-up batch did not drain the retained tail");
    ClockExpect(!ServerTickClockHasTick(&clock, TEST_TICK_SECONDS),
                "hitch backlog did not converge");
}

static void TestSafetyBacklogIsBounded(void)
{
    ServerTickClock clock;
    ServerTickClockInitialize(&clock);
    ServerTickClockAccumulate(&clock, 60.0, TEST_MAXIMUM_BACKLOG_SECONDS);

    uint32_t ticks = 0;
    while (ServerTickClockConsumeTick(&clock, TEST_TICK_SECONDS))
    {
        ++ticks;
    }
    ClockExpect(ticks == 256U, "server safety backlog does not match the input FIFO horizon");
}

LAIUE_TEST_ENTRY(ServerTickClockTestEntryPoint)
{
    TestRenderPartitions();
    TestHitchTailIsRetained();
    TestSafetyBacklogIsBounded();
    LaiueTestRuntimeWrite("Server fixed-tick clock: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
