#pragma once

#include "network/network.h"

#include <stdbool.h>
#include <stdint.h>

// A little over four seconds at the shared 60 Hz simulation rate.  The
// queue is fixed-size so a delayed or malicious peer cannot allocate memory
// from the server simulation thread.
#define SERVER_PLAYER_INPUT_QUEUE_CAPACITY 256U
#define SERVER_PLAYER_INPUT_TIMEOUT_TICKS 15U

typedef struct ServerPlayerInputQueue
{
    NetworkInputCommand commands[SERVER_PLAYER_INPUT_QUEUE_CAPACITY];
    uint32_t first;
    uint32_t count;
    uint32_t lastAcceptedSequence;
    bool hasAcceptedSequence;
} ServerPlayerInputQueue;

typedef enum ServerPlayerInputPushResult
{
    SERVER_PLAYER_INPUT_ACCEPTED = 0,
    SERVER_PLAYER_INPUT_STALE = 1,
    SERVER_PLAYER_INPUT_GAP = 2,
    SERVER_PLAYER_INPUT_OVERFLOW = 3,
} ServerPlayerInputPushResult;

void ServerPlayerInputQueueInitialize(ServerPlayerInputQueue *queue);

// Inputs are ordered with wrapping uint32 serial arithmetic. Duplicate and
// old inputs are harmlessly ignored. A forward gap is a protocol violation:
// QUIC delivers the one reliable gameplay stream in order, so a legitimate
// client cannot lose an intermediate command. Capacity pressure is reported
// separately so only the offending peer can be disconnected.
ServerPlayerInputPushResult ServerPlayerInputQueuePush(ServerPlayerInputQueue *queue,
                                                       const NetworkInputCommand *command);

bool ServerPlayerInputQueuePop(ServerPlayerInputQueue *queue, NetworkInputCommand *outCommand);

// Wrap-safe for the server's uint32 tick counter. The held command is
// neutralized on the fifteenth tick without an accepted input.
bool ServerPlayerInputTimedOut(uint32_t serverTick, uint32_t lastAcceptedInputTick);
