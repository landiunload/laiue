#include "repro.h"
#include <string.h>

static int GetBlock(void) { return 1; }
static int Rebase(void) { return 2; }

/* A: zeroing compound literal, then member stores (the miscompiled form). */
__declspec(noinline) void InitLiteral(State *state)
{
    *state = (State){0};
    state->api.context = state;
    state->api.getBlock = (void *)GetBlock;
    state->api.rebase = (void *)Rebase;
    state->world = Create(&state->api);
}

/* B: memset, then member stores. */
__declspec(noinline) void InitMemset(State *state)
{
    memset(state, 0, sizeof(*state));
    state->api.context = state;
    state->api.getBlock = (void *)GetBlock;
    state->api.rebase = (void *)Rebase;
    state->world = Create(&state->api);
}

/* C: one designated-initializer compound literal. */
__declspec(noinline) void InitDesignated(State *state)
{
    *state = (State){
        .api = {.context = state, .getBlock = (void *)GetBlock, .rebase = (void *)Rebase},
    };
    state->world = Create(&state->api);
}

/* D: build a local value, then copy it whole. */
__declspec(noinline) void InitLocal(State *state)
{
    State value = {0};
    value.api.context = state;
    value.api.getBlock = (void *)GetBlock;
    value.api.rebase = (void *)Rebase;
    *state = value;
    state->world = Create(&state->api);
}

/* E: designated initializer for the member struct only. */
__declspec(noinline) void InitMember(State *state)
{
    *state = (State){0};
    state->api = (Api){.context = state, .getBlock = (void *)GetBlock, .rebase = (void *)Rebase};
    state->world = Create(&state->api);
}

/* F: memset of a local, as in the engine's reset functions. */
__declspec(noinline) void InitPositions(State *state)
{
    memset(state, 0, sizeof(*state));
    state->position[0] = 0.5;
    state->position[1] = 0.5;
    state->position[2] = 2.751;
    state->hash = 14695981039346656037ULL;
    state->api.context = state;
    state->world = Create(&state->api);
}
