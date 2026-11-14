/* Opaque half of the repro: compiled without /GL, so whole-program
 * optimization cannot see through the calls. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ReproApi
{
    void *context;
    void *getBlock;
    void *fillRegion;
    void *rebase;
} ReproApi;

static const void *seenContext;
static const void *seenSmallContext;

void *ReproCreate(const ReproApi *api)
{
    seenContext = api->context;
    return (void *)api;
}

void *ReproCreateSmall(const ReproApi *api)
{
    seenSmallContext = api->context;
    return (void *)api;
}

typedef struct ReproSimStorage
{
    _Alignas(16) unsigned char bytes[0xC8];
} ReproSimStorage;

typedef struct ReproSmallStorage
{
    _Alignas(16) unsigned char bytes[0x38];
} ReproSmallStorage;

void ReproInit(void *simulation);
void *ReproInitSmall(void *value);

int main(void)
{
    static ReproSimStorage simulation;
    static ReproSmallStorage small;
    memset(&simulation, 0xAB, sizeof(simulation));
    memset(&small, 0xAB, sizeof(small));
    ReproInit(&simulation);
    (void)ReproInitSmall(&small);
    const int full = seenContext == (const void *)&simulation;
    const int reduced = seenSmallContext == (const void *)&small;
    printf("full %s, small %s\n", full ? "ok" : "MISCOMPILED", reduced ? "ok" : "MISCOMPILED");
    return full && reduced ? 0 : 1;
}
