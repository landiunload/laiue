#pragma once

#include <stdbool.h>

// Wall-clock accumulator for the authoritative server. Unlike the render
// clock, it retains work that did not fit into one bounded catch-up batch:
// dropping that tail while accepting one input per 60 Hz tick would create a
// permanent FIFO delay after every hitch.
typedef struct ServerTickClock
{
    double accumulatorSeconds;
} ServerTickClock;

void ServerTickClockInitialize(ServerTickClock *clock);

// Adds elapsed monotonic time and clamps only the total safety backlog.
// Invalid/negative elapsed values are ignored.
void ServerTickClockAccumulate(ServerTickClock *clock, double elapsedSeconds,
                               double maximumBacklogSeconds);

bool ServerTickClockHasTick(const ServerTickClock *clock, double tickSeconds);
bool ServerTickClockConsumeTick(ServerTickClock *clock, double tickSeconds);
