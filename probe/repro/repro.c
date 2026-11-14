/* MSVC ARM64 store-order repro. Mirrors SimulationInitialize from
 * tests/physics_determinism_test.c: a zeroing compound literal followed by
 * member stores, then the address of a member escapes to an opaque call. */
#include <stdint.h>

typedef struct ReproApi
{
    void *context;
    int (*getBlock)(void *, int64_t, int64_t, int64_t);
    void *fillRegion;
    int (*rebase)(void *, int64_t, int64_t, int64_t);
} ReproApi;

typedef struct ReproCollision
{
    void *context;
    void *queryBlock;
    void *queryDynamic;
} ReproCollision;

typedef struct ReproCoord
{
    uint64_t *limbs;
    uint32_t count;
    int32_t sign;
} ReproCoord;

typedef struct ReproSim
{
    int64_t origin[3];
    ReproApi api;
    void *world;
    ReproCollision collision;
    ReproCoord coords[3];
    double position[3];
    double velocity[3];
    uint64_t hash;
    uint32_t mask;
} ReproSim;

void *ReproCreate(const ReproApi *api);

static int ReproBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    return (int)(x + y + z);
}

static int ReproRebase(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    return (int)(x - y - z);
}

__declspec(noinline) void ReproInit(ReproSim *simulation)
{
    *simulation = (ReproSim){0};
    simulation->api.context = &simulation->origin;
    simulation->api.getBlock = ReproBlock;
    simulation->api.rebase = ReproRebase;
    simulation->world = ReproCreate(&simulation->api);
}

typedef struct ReproSmall
{
    int64_t origin[3];
    ReproApi api;
} ReproSmall;

void *ReproCreateSmall(const ReproApi *api);

__declspec(noinline) void *ReproInitSmall(ReproSmall *value)
{
    *value = (ReproSmall){0};
    value->api.context = value;
    value->api.getBlock = ReproBlock;
    return ReproCreateSmall(&value->api);
}
