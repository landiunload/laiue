#include "repro.h"

static int GetBlock(void) { return 1; }
static int Rebase(void) { return 2; }

__declspec(noinline) void Init(State *state)
{
    *state = (State){0};
    state->api.context = state;
    state->api.getBlock = (void *)GetBlock;
    state->api.rebase = (void *)Rebase;
    state->world = Create(&state->api);
}
