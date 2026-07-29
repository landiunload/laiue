#include "server/player_input_queue.h"

#include <string.h>

static bool SequenceIsNewer(uint32_t candidate, uint32_t reference)
{
    uint32_t distance = candidate - reference;
    return distance != 0U && distance < UINT32_C(0x80000000);
}

void ServerPlayerInputQueueInitialize(ServerPlayerInputQueue *queue)
{
    if (queue != NULL)
    {
        memset(queue, 0, sizeof(*queue));
    }
}

ServerPlayerInputPushResult ServerPlayerInputQueuePush(ServerPlayerInputQueue *queue,
                                                       const NetworkInputCommand *command)
{
    if (queue == NULL || command == NULL || command->sequence == 0U)
    {
        return SERVER_PLAYER_INPUT_STALE;
    }
    if (queue->hasAcceptedSequence &&
        !SequenceIsNewer(command->sequence, queue->lastAcceptedSequence))
    {
        return SERVER_PLAYER_INPUT_STALE;
    }
    if (queue->hasAcceptedSequence)
    {
        uint32_t expected = queue->lastAcceptedSequence + 1U;
        if (expected == 0U)
        {
            expected = 1U;
        }
        if (command->sequence != expected)
        {
            return SERVER_PLAYER_INPUT_GAP;
        }
    }
    if (queue->count >= SERVER_PLAYER_INPUT_QUEUE_CAPACITY)
    {
        return SERVER_PLAYER_INPUT_OVERFLOW;
    }

    uint32_t slot = (queue->first + queue->count) % SERVER_PLAYER_INPUT_QUEUE_CAPACITY;
    queue->commands[slot] = *command;
    ++queue->count;
    queue->lastAcceptedSequence = command->sequence;
    queue->hasAcceptedSequence = true;
    return SERVER_PLAYER_INPUT_ACCEPTED;
}

bool ServerPlayerInputQueuePop(ServerPlayerInputQueue *queue, NetworkInputCommand *outCommand)
{
    if (queue == NULL || outCommand == NULL || queue->count == 0U)
    {
        return false;
    }
    *outCommand = queue->commands[queue->first];
    queue->first = (queue->first + 1U) % SERVER_PLAYER_INPUT_QUEUE_CAPACITY;
    --queue->count;
    return true;
}

bool ServerPlayerInputTimedOut(uint32_t serverTick, uint32_t lastAcceptedInputTick)
{
    return serverTick - lastAcceptedInputTick >= SERVER_PLAYER_INPUT_TIMEOUT_TICKS;
}
