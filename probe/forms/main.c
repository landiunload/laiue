#include "repro.h"
#include <stdio.h>

void InitLiteral(State *state);
void InitMemset(State *state);
void InitDesignated(State *state);
void InitLocal(State *state);
void InitMember(State *state);
void InitPositions(State *state);

static const void *seen;

void *Create(const Api *api)
{
    seen = api->context;
    return (void *)api;
}

static int Check(const char *name, void (*init)(State *))
{
    static State state;
    seen = NULL;
    init(&state);
    const int ok = seen == &state;
    printf("%s %s\n", name, ok ? "ok" : "MISCOMPILED");
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= Check("A literal", InitLiteral);
    ok &= Check("B memset", InitMemset);
    ok &= Check("C designated", InitDesignated);
    ok &= Check("D local", InitLocal);
    ok &= Check("E member", InitMember);
    ok &= Check("F positions", InitPositions);
    return ok ? 0 : 1;
}
