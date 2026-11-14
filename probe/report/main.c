#include "repro.h"
#include <stdio.h>

static const void *seen;

void *Create(const Api *api)
{
    seen = api->context;
    return (void *)api;
}

int main(void)
{
    static State state;
    Init(&state);
    if (seen != &state)
    {
        puts("MISCOMPILED: api.context was overwritten by the zero-fill");
        return 1;
    }
    puts("ok");
    return 0;
}
