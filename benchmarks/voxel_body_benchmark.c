// Ручной benchmark горячего пути AABB-контроллера VoxelBody: обычный шаг
// персонажа, длинные свипы по плоскостям, ground/stable/sneak-запросы,
// candidate traversal и динамический broadphase. Не входит в CTest и не
// собирается по умолчанию; запускается явно для сравнения baseline и
// candidate на одной машине.
//
// Печатает ns на операцию, число обращений к queryBlockPhysics и checksum
// результата. Baseline и candidate обязаны напечатать одинаковый checksum:
// это подтверждает, что изменена только стоимость, а не результат.

#include "physics/voxel_body.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 7u
#define WARMUP_DIVISOR 4u

static volatile uint64_t benchmarkSink;

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u)
    {
        digits[length++] = '0';
    }
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0u; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void WriteFixed2(double value)
{
    uint64_t hundredths = (uint64_t)(value * 100.0 + 0.5);
    WriteUnsigned(hundredths / 100u);
    WriteText(".");
    uint64_t fraction = hundredths % 100u;
    if (fraction < 10u)
    {
        WriteText("0");
    }
    WriteUnsigned(fraction);
}

static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && samples[insertion - 1u] > value)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

static uint64_t DoubleBits(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return representation.bits;
}

typedef enum BenchWorldMode
{
    BENCH_WORLD_EMPTY = 0,
    BENCH_WORLD_FLAT = 1,
    BENCH_WORLD_WALLS = 2,
    BENCH_WORLD_HOLE = 3,
} BenchWorldMode;

typedef struct BenchWorld
{
    BenchWorldMode mode;
    uint64_t queries;
} BenchWorld;

typedef struct BenchDynamic
{
    uint32_t count;
} BenchDynamic;

typedef struct BenchContext
{
    BenchWorld world;
    BenchDynamic dynamic;
    VoxelCollisionSource source;
    VoxelBodyShape shape;
} BenchContext;

static bool BenchBlockIsSolid(const BenchWorld *world, int64_t x, int64_t y, int64_t z)
{
    (void)y;
    switch (world->mode)
    {
    case BENCH_WORLD_FLAT:
        return z <= 0;
    case BENCH_WORLD_WALLS:
    {
        int64_t cell = x % 16;
        if (cell < 0)
        {
            cell += 16;
        }
        return z <= 0 || (cell == 0 && z >= 1 && z <= 3);
    }
    case BENCH_WORLD_HOLE:
        // Широкая дыра справа: центр тела ещё стоит на полу, а сдвинутая
        // стопа уже уходит в пустоту и заставляет sneak-цикл уменьшать ход.
        return z <= 0 && !(x >= 6 && x <= 13);
    case BENCH_WORLD_EMPTY:
    default:
        return false;
    }
}

static void BenchQueryBlock(void *rawContext, int64_t x, int64_t y, int64_t z,
                            VoxelBlockPhysics *outBlock)
{
    BenchContext *context = (BenchContext *)rawContext;
    ++context->world.queries;
    bool solid = BenchBlockIsSolid(&context->world, x, y, z);
    outBlock->flags = solid ? (uint32_t)VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = solid ? 0.6f : 0.0f;
}

static bool BenchQueryDynamic(void *rawContext, const VoxelBodyBounds *queryBounds,
                              VoxelDynamicCollider *outColliders, uint32_t colliderCapacity,
                              uint32_t *outColliderCount)
{
    BenchContext *context = (BenchContext *)rawContext;
    (void)queryBounds;
    uint32_t count = context->dynamic.count;
    if (count > colliderCapacity)
    {
        *outColliderCount = 0u;
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        VoxelDynamicCollider collider = {0};
        collider.bounds.minimum[0] = 1.0 + (double)index * 0.4;
        collider.bounds.maximum[0] = 2.0 + (double)index * 0.4;
        collider.bounds.minimum[1] = -1.0;
        collider.bounds.maximum[1] = 1.0;
        collider.bounds.minimum[2] = 0.5;
        collider.bounds.maximum[2] = 2.5;
        collider.velocity[0] = 0.125 + (double)index * 0.5;
        collider.velocity[1] = -0.25;
        collider.velocity[2] = 0.0;
        collider.friction = 0.5f;
        collider.stableId = ((uint64_t)index + 1u) * 3u;
        outColliders[index] = collider;
    }
    *outColliderCount = count;
    return true;
}

static VoxelBodyShape BenchShape(void)
{
    VoxelBodyShape shape = {
        .radius = 0.3,
        .height = 1.8,
        .eyeHeight = 1.75,
        .collisionEpsilon = 0.001,
    };
    return shape;
}

static void BenchContextInitialize(BenchContext *context, BenchWorldMode mode,
                                   uint32_t dynamicCount)
{
    context->world.mode = mode;
    context->world.queries = 0u;
    context->dynamic.count = dynamicCount;
    context->shape = BenchShape();
    context->source.context = context;
    context->source.queryBlockPhysics = BenchQueryBlock;
    context->source.queryDynamicColliders = dynamicCount > 0u ? BenchQueryDynamic : NULL;
}

static void BenchPosition(uint32_t index, double *outPosition)
{
    uint32_t cellX = index % 64u;
    uint32_t cellY = (index / 64u) % 64u;
    outPosition[0] = (double)cellX * 0.5 + 0.1;
    outPosition[1] = (double)cellY * 0.5 + 0.1;
    outPosition[2] = 2.751;
}

typedef struct WalkState
{
    double position[3];
    double velocity[3];
} WalkState;

typedef void (*BenchBody)(BenchContext *context, void *state, uint32_t ops, uint64_t *sink);

static void RunWalkBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    WalkState *state = (WalkState *)rawState;

    for (uint32_t op = 0u; op < ops; ++op)
    {
        VoxelGroundContact contact;
        VoxelBodyQueryGroundContact(&context->source, state->position, &context->shape, 0.01,
                                    &contact);
        if (contact.supported)
        {
            state->velocity[2] = 0.0;
        }
        else if (state->velocity[2] > -1.0 / 16.0)
        {
            state->velocity[2] -= 1.0 / 1024.0;
        }
        if (VoxelBodyMoveAxis(&context->source, state->position, &context->shape, 0,
                              state->velocity[0]))
        {
            state->velocity[0] = -state->velocity[0];
        }
        if (VoxelBodyMoveAxis(&context->source, state->position, &context->shape, 1,
                              state->velocity[1]))
        {
            state->velocity[1] = -state->velocity[1];
        }
        if (VoxelBodyMoveAxis(&context->source, state->position, &context->shape, 2,
                              state->velocity[2]))
        {
            state->velocity[2] = 0.0;
        }
        *sink ^= DoubleBits(state->position[0]);
        *sink ^= DoubleBits(state->position[1]);
        *sink ^= DoubleBits(state->position[2]);
    }
}

typedef struct SweepState
{
    double origin[3];
    int32_t axis;
    double distance;
} SweepState;

static void RunSweepBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    SweepState *state = (SweepState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        double position[3] = {state->origin[0], state->origin[1], state->origin[2]};
        bool collided = VoxelBodyMoveAxis(&context->source, position, &context->shape, state->axis,
                                          state->distance);
        *sink ^= (uint64_t)(collided ? 1u : 0u);
        *sink ^= DoubleBits(position[state->axis]);
    }
}

typedef struct IndexState
{
    uint32_t index;
    double supportRadius;
} IndexState;

static void RunCollidesBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    IndexState *state = (IndexState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        double position[3];
        BenchPosition(state->index++, position);
        bool collided = VoxelBodyCollides(&context->source, position, &context->shape);
        *sink ^= (uint64_t)(collided ? 1u : 0u);
        *sink ^= DoubleBits(position[0]);
    }
}

static void RunStableBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    IndexState *state = (IndexState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        double position[3];
        BenchPosition(state->index++, position);
        bool stable = VoxelBodyHasStableGround(&context->source, position, &context->shape, 0.01,
                                               state->supportRadius);
        *sink ^= (uint64_t)(stable ? 1u : 0u);
        *sink ^= DoubleBits(position[1]);
    }
}

typedef struct SneakState
{
    uint32_t index;
    double xDistance;
    double yDistance;
} SneakState;

static void RunSneakBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    SneakState *state = (SneakState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        uint32_t index = state->index++;
        double position[3];
        position[0] = 5.70 + (double)(index % 9u) * 0.03;
        position[1] = 0.5;
        position[2] = 2.751;
        double x = state->xDistance;
        double y = state->yDistance;
        VoxelBodyClipSneakingMovement(&context->source, position, &context->shape, 0.01, &x, &y);
        *sink ^= DoubleBits(x);
        *sink ^= DoubleBits(y);
    }
}

typedef struct DynamicState
{
    double position[3];
} DynamicState;

static void RunDynamicBody(BenchContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    DynamicState *state = (DynamicState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        state->position[0] = 0.5;
        state->position[1] = 0.5;
        state->position[2] = 2.751;
        bool collided = VoxelBodyMoveAxis(&context->source, state->position, &context->shape, 0,
                                          0.25);
        *sink ^= (uint64_t)(collided ? 1u : 0u);
        *sink ^= DoubleBits(state->position[0]);
    }
}

static void RunMode(const char *label, BenchBody body, BenchContext *context, void *state,
                    uint32_t ops)
{
    uint64_t sink = 0u;
    body(context, state, ops / WARMUP_DIVISOR + 1u, &sink);

    double samples[SAMPLE_COUNT];
    uint64_t queries = 0u;
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
    {
        context->world.queries = 0u;
        double start = PlatformMonotonicSeconds();
        body(context, state, ops, &sink);
        double elapsed = PlatformMonotonicSeconds() - start;
        samples[sample] = elapsed * 1.0e9 / (double)ops;
        queries = context->world.queries;
    }
    benchmarkSink ^= sink;

    WriteText(label);
    WriteText(" ns_per_op=");
    WriteFixed2(Median(samples, SAMPLE_COUNT));
    WriteText(" queries_per_op=");
    WriteFixed2((double)queries / (double)ops);
    WriteText(" checksum=");
    WriteUnsigned(sink);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(VoxelBodyBenchmarkEntryPoint)
{
    WriteText("laiue voxel-body benchmark\n");

    BenchContext context;
    WalkState walk;

    BenchContextInitialize(&context, BENCH_WORLD_WALLS, 0u);
    walk.position[0] = 0.5;
    walk.position[1] = 0.5;
    walk.position[2] = 2.751;
    walk.velocity[0] = 5.0 / 256.0;
    walk.velocity[1] = -3.0 / 256.0;
    walk.velocity[2] = 0.0;
    RunMode("walk.walls", RunWalkBody, &context, &walk, 150000u);

    BenchContextInitialize(&context, BENCH_WORLD_EMPTY, 0u);
    SweepState sweep;
    sweep.origin[0] = 0.5;
    sweep.origin[1] = 0.5;
    sweep.origin[2] = 2.751;
    sweep.axis = 0;
    sweep.distance = 64.0;
    RunMode("sweep.free", RunSweepBody, &context, &sweep, 4000u);

    BenchContextInitialize(&context, BENCH_WORLD_WALLS, 0u);
    sweep.origin[0] = 0.5;
    sweep.origin[1] = 0.5;
    sweep.origin[2] = 2.751;
    sweep.axis = 0;
    sweep.distance = 20.0;
    RunMode("sweep.wall", RunSweepBody, &context, &sweep, 20000u);

    BenchContextInitialize(&context, BENCH_WORLD_WALLS, 0u);
    IndexState indexState = {0u, 0.2};
    RunMode("collides.walls", RunCollidesBody, &context, &indexState, 300000u);

    BenchContextInitialize(&context, BENCH_WORLD_WALLS, 0u);
    indexState.index = 0u;
    indexState.supportRadius = 0.2;
    RunMode("stable.walls", RunStableBody, &context, &indexState, 200000u);

    // Опора отсутствует: до правки один и тот же блок мог быть спрошен до
    // девяти раз. Покрывает случай падения/прыжка над пустотой.
    BenchContextInitialize(&context, BENCH_WORLD_EMPTY, 0u);
    indexState.index = 0u;
    indexState.supportRadius = 0.2;
    RunMode("stable.air", RunStableBody, &context, &indexState, 200000u);

    BenchContextInitialize(&context, BENCH_WORLD_HOLE, 0u);
    SneakState sneak = {0u, 0.6, 0.6};
    RunMode("sneak.hole", RunSneakBody, &context, &sneak, 200000u);

    BenchContextInitialize(&context, BENCH_WORLD_EMPTY, 2u);
    DynamicState dynamic = {{0.5, 0.5, 2.751}};
    RunMode("dynamic.move", RunDynamicBody, &context, &dynamic, 200000u);

    if (benchmarkSink == UINT64_MAX)
    {
        WriteText("");
    }
    LAIUE_TEST_SUCCESS();
}
