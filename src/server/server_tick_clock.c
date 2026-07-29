#include "server/server_tick_clock.h"

#include <stddef.h>

static bool IsFinitePositive(double value)
{
    return value == value && value > 0.0 && value <= 3600.0;
}

static double TickEpsilon(double tickSeconds)
{
    return tickSeconds * 1e-10;
}

void ServerTickClockInitialize(ServerTickClock *clock)
{
    if (clock != NULL)
    {
        clock->accumulatorSeconds = 0.0;
    }
}

void ServerTickClockAccumulate(ServerTickClock *clock, double elapsedSeconds,
                               double maximumBacklogSeconds)
{
    if (clock == NULL || !(elapsedSeconds >= 0.0) || !IsFinitePositive(maximumBacklogSeconds))
    {
        return;
    }
    if (elapsedSeconds >= maximumBacklogSeconds ||
        clock->accumulatorSeconds >= maximumBacklogSeconds - elapsedSeconds)
    {
        clock->accumulatorSeconds = maximumBacklogSeconds;
        return;
    }
    clock->accumulatorSeconds += elapsedSeconds;
}

bool ServerTickClockHasTick(const ServerTickClock *clock, double tickSeconds)
{
    return clock != NULL && IsFinitePositive(tickSeconds) &&
           clock->accumulatorSeconds + TickEpsilon(tickSeconds) >= tickSeconds;
}

bool ServerTickClockConsumeTick(ServerTickClock *clock, double tickSeconds)
{
    if (!ServerTickClockHasTick(clock, tickSeconds))
    {
        return false;
    }
    if (clock->accumulatorSeconds <= tickSeconds)
    {
        clock->accumulatorSeconds = 0.0;
    }
    else
    {
        clock->accumulatorSeconds -= tickSeconds;
    }
    return true;
}
