#include "server/player_input_queue.h"
#include "test_runtime.h"

#include <string.h>

// Keep the bounded 256-command queue out of the no-CRT Windows stack frame;
// otherwise MSVC emits a __chkstk dependency for this standalone harness.
static ServerPlayerInputQueue g_queue;

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

static NetworkInputCommand Command(uint32_t sequence, bool jumpPressed)
{
    NetworkInputCommand command;
    memset(&command, 0, sizeof(command));
    command.sequence = sequence;
    command.jumpPressed = jumpPressed;
    return command;
}

LAIUE_TEST_ENTRY(ServerInputQueueTestEntryPoint)
{
    ServerPlayerInputQueueInitialize(&g_queue);

    NetworkInputCommand first = Command(1U, true);
    NetworkInputCommand second = Command(2U, false);
    NetworkInputCommand third = Command(3U, false);
    NetworkInputCommand stale = Command(1U, false);
    Expect(ServerPlayerInputQueuePush(&g_queue, &first) == SERVER_PLAYER_INPUT_ACCEPTED,
           "first input was not accepted");
    Expect(ServerPlayerInputQueuePush(&g_queue, &third) == SERVER_PLAYER_INPUT_GAP,
           "forward input gap was not rejected");
    Expect(ServerPlayerInputQueuePush(&g_queue, &stale) == SERVER_PLAYER_INPUT_STALE,
           "duplicate input was not rejected as stale");
    Expect(ServerPlayerInputQueuePush(&g_queue, &second) == SERVER_PLAYER_INPUT_ACCEPTED,
           "sequential input after rejected gap was not accepted");

    NetworkInputCommand popped;
    Expect(ServerPlayerInputQueuePop(&g_queue, &popped) && popped.sequence == 1U &&
               popped.jumpPressed,
           "jump edge or FIFO order was lost");
    Expect(ServerPlayerInputQueuePop(&g_queue, &popped) && popped.sequence == 2U &&
               !popped.jumpPressed,
           "second queued input was corrupted");
    Expect(!ServerPlayerInputQueuePop(&g_queue, &popped), "empty input queue produced a command");
    Expect(!ServerPlayerInputTimedOut(114U, 100U),
           "held input timed out before the fifteenth tick");
    Expect(ServerPlayerInputTimedOut(115U, 100U),
           "held input was not neutralized on the fifteenth tick");
    Expect(ServerPlayerInputTimedOut(5U, UINT32_MAX - 9U),
           "input timeout did not survive server tick wrap");

    ServerPlayerInputQueueInitialize(&g_queue);
    NetworkInputCommand beforeWrap = Command(UINT32_MAX, false);
    NetworkInputCommand afterWrap = Command(1U, false);
    Expect(ServerPlayerInputQueuePush(&g_queue, &beforeWrap) == SERVER_PLAYER_INPUT_ACCEPTED &&
               ServerPlayerInputQueuePush(&g_queue, &afterWrap) == SERVER_PLAYER_INPUT_ACCEPTED,
           "uint32 sequence wrap was not ordered");

    ServerPlayerInputQueueInitialize(&g_queue);
    for (uint32_t index = 0; index < SERVER_PLAYER_INPUT_QUEUE_CAPACITY; ++index)
    {
        NetworkInputCommand command = Command(index + 1U, false);
        Expect(ServerPlayerInputQueuePush(&g_queue, &command) == SERVER_PLAYER_INPUT_ACCEPTED,
               "bounded queue filled too early");
    }
    NetworkInputCommand overflow = Command(SERVER_PLAYER_INPUT_QUEUE_CAPACITY + 1U, false);
    Expect(ServerPlayerInputQueuePush(&g_queue, &overflow) == SERVER_PLAYER_INPUT_OVERFLOW,
           "bounded queue did not report peer-local overflow");

    LaiueTestRuntimeWrite("Server input queue: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
