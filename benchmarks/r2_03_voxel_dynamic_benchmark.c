// Ручной A/B-стенд обработки dynamic candidates VoxelBody: цена
// QueryDynamicColliderBatch (валидация, проверка дубликатов stableId,
// детерминированная сортировка) и последующего clipping в VoxelBodyMoveAxis /
// ground contact. Мир блоков пуст, поэтому время сценария определяется только
// обработкой выборки коллайдеров, а не block-запросами.
//
// Печатает ns на операцию, число block-запросов на операцию и checksum
// результата. Baseline и candidate обязаны напечатать одинаковый checksum:
// изменена только стоимость, а не результат.
//
// Экспериментальный harness ROUND2 (03-voxel). В CTest не регистрируется и в
// ALL не входит; собирается явно и запускается A/B-скриптом.

#include "physics/voxel_body.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_COUNT 9u
#define WARMUP_DIVISOR 3u
#define MAX_COLLIDERS 32u

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

// FNV-1a-подобное смешивание: XOR с последующим умножением не сокращается при
// чётном числе одинаковых операций, поэтому checksum реально ловит различие.
static void MixSink(uint64_t *sink, uint64_t value)
{
    *sink ^= value;
    *sink *= 1099511628211ULL;
}

typedef enum DynOrder
{
    DYN_ORDER_SORTED = 0,
    DYN_ORDER_REVERSED = 1,
    DYN_ORDER_SHUFFLED = 2,
} DynOrder;

typedef struct DynContext
{
    uint32_t count;
    DynOrder order;
    uint64_t blockQueries;
    VoxelCollisionSource source;
    VoxelBodyShape shape;
} DynContext;

static void QueryEmptyBlock(void *rawContext, int64_t x, int64_t y, int64_t z,
                            VoxelBlockPhysics *outBlock)
{
    (void)x;
    (void)y;
    (void)z;
    DynContext *context = (DynContext *)rawContext;
    ++context->blockQueries;
    outBlock->flags = 0u;
    outBlock->friction = 0.0f;
}

// Детерминированная перестановка: стабильный паттерн, одинаковый между
// baseline и candidate. Индексы 0..count-1 переставляются так, чтобы
// unsorted-порядок не совпадал с возрастанием stableId.
static uint32_t ShuffledIndex(uint32_t index, uint32_t count)
{
    return (index * 7u + 3u) % count;
}

static void FillCollider(VoxelDynamicCollider *collider, uint32_t stableId)
{
    *collider = (VoxelDynamicCollider){0};
    double offset = (double)(stableId - 1u) * 0.25;
    collider->bounds.minimum[0] = 0.85 + offset;
    collider->bounds.maximum[0] = 1.35 + offset;
    collider->bounds.minimum[1] = -4.0;
    collider->bounds.maximum[1] = 4.0;
    collider->bounds.minimum[2] = 0.25;
    collider->bounds.maximum[2] = 2.75;
    collider->velocity[0] = 0.0;
    collider->velocity[1] = 0.0;
    collider->velocity[2] = 0.0;
    collider->friction = 0.5f;
    collider->stableId = stableId;
}

static bool QueryDynColliders(void *rawContext, const VoxelBodyBounds *queryBounds,
                              VoxelDynamicCollider *outColliders, uint32_t colliderCapacity,
                              uint32_t *outColliderCount)
{
    (void)queryBounds;
    DynContext *context = (DynContext *)rawContext;
    uint32_t count = context->count;
    if (count > colliderCapacity || count > MAX_COLLIDERS)
    {
        *outColliderCount = 0u;
        return false;
    }
    for (uint32_t slot = 0u; slot < count; ++slot)
    {
        uint32_t id;
        switch (context->order)
        {
        case DYN_ORDER_REVERSED:
            id = count - slot;
            break;
        case DYN_ORDER_SHUFFLED:
            id = ShuffledIndex(slot, count) + 1u;
            break;
        case DYN_ORDER_SORTED:
        default:
            id = slot + 1u;
            break;
        }
        FillCollider(&outColliders[slot], id);
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

typedef struct DynState
{
    double position[3];
} DynState;

typedef void (*BenchBody)(DynContext *context, void *state, uint32_t ops, uint64_t *sink);

static void RunMoveBody(DynContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    DynState *state = (DynState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        state->position[0] = 0.5;
        state->position[1] = 0.5;
        state->position[2] = 2.751;
        bool collided =
            VoxelBodyMoveAxis(&context->source, state->position, &context->shape, 0, 0.125);
        MixSink(sink, (uint64_t)(collided ? 1u : 0u));
        MixSink(sink, DoubleBits(state->position[0]));
    }
}

static void RunGroundBody(DynContext *context, void *rawState, uint32_t ops, uint64_t *sink)
{
    DynState *state = (DynState *)rawState;
    for (uint32_t op = 0u; op < ops; ++op)
    {
        state->position[0] = 1.5;
        state->position[1] = 0.5;
        state->position[2] = 4.51;
        VoxelGroundContact contact;
        VoxelBodyQueryGroundContact(&context->source, state->position, &context->shape, 0.01,
                                    &contact);
        MixSink(sink, (uint64_t)(contact.supported ? 1u : 0u));
        MixSink(sink, (uint64_t)contact.surfaceStableId);
        MixSink(sink, DoubleBits((double)contact.friction));
    }
}

static void RunMode(const char *label, BenchBody body, DynContext *context, void *state,
                    uint32_t ops)
{
    uint64_t sink = 0u;
    body(context, state, ops / WARMUP_DIVISOR + 1u, &sink);

    double samples[SAMPLE_COUNT];
    uint64_t queries = 0u;
    for (uint32_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
    {
        context->blockQueries = 0u;
        double start = PlatformMonotonicSeconds();
        body(context, state, ops, &sink);
        double elapsed = PlatformMonotonicSeconds() - start;
        samples[sample] = elapsed * 1.0e9 / (double)ops;
        queries = context->blockQueries;
    }
    benchmarkSink ^= sink;

    WriteText(label);
    WriteText(" ns_per_op=");
    WriteFixed2(Median(samples, SAMPLE_COUNT));
    WriteText(" block_queries=");
    WriteUnsigned(queries);
    WriteText(" checksum=");
    WriteUnsigned(sink);
    WriteText("\n");
}

static void RunScenario(const char *label, BenchBody body, uint32_t count, DynOrder order,
                        uint32_t ops)
{
    DynContext context;
    context.count = count;
    context.order = order;
    context.blockQueries = 0u;
    context.shape = BenchShape();
    context.source.context = &context;
    context.source.queryBlockPhysics = QueryEmptyBlock;
    context.source.queryDynamicColliders = count > 0u ? QueryDynColliders : NULL;
    DynState state = {{0.5, 0.5, 2.751}};
    RunMode(label, body, &context, &state, ops);
}

LAIUE_TEST_ENTRY(R2VoxelDynamicBenchmarkEntryPoint)
{
    WriteText("laiue r2 voxel dynamic-candidates benchmark\n");

    // Typical: маленькая выборка, порядок как у большинства приложений.
    RunScenario("move.n1.sorted", RunMoveBody, 1u, DYN_ORDER_SORTED, 300000u);
    RunScenario("move.n8.sorted", RunMoveBody, 8u, DYN_ORDER_SORTED, 300000u);
    RunScenario("move.n16.sorted", RunMoveBody, 16u, DYN_ORDER_SORTED, 200000u);
    // Boundary: ровно ёмкость.
    RunScenario("move.n32.sorted", RunMoveBody, 32u, DYN_ORDER_SORTED, 100000u);
    // Adversarial: обратный порядок заставляет insertion sort работать в полную
    // силу, случайный — дополнительно перемешивает выборку.
    RunScenario("move.n32.reversed", RunMoveBody, 32u, DYN_ORDER_REVERSED, 100000u);
    RunScenario("move.n32.shuffled", RunMoveBody, 32u, DYN_ORDER_SHUFFLED, 100000u);
    RunScenario("ground.n32.reversed", RunGroundBody, 32u, DYN_ORDER_REVERSED, 100000u);
    RunScenario("ground.n8.sorted", RunGroundBody, 8u, DYN_ORDER_SORTED, 200000u);

    if (benchmarkSink == UINT64_MAX)
    {
        WriteText("");
    }
    LAIUE_TEST_SUCCESS();
}
