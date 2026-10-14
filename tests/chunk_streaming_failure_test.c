/* Only copied streaming/mesher objects substitute Platform* with these
 * wrappers. World and the wrappers themselves use the normal platform ABI.
 * Mesher callbacks point at unchanged production mesher bodies, including
 * its real scratch allocation/cleanup and greedy build implementation. */
#include "voxel_render/chunk_streaming.h"
#include "mesh/mesher_service.h"
#include "render/graphics_service.h"
#include "world/numeric_provider.h"
#include "world/world_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct Allocation
{
    void *memory;
    size_t size;
} Allocation;

typedef struct ThreadRecord
{
    PlatformThread thread;
    bool live;
} ThreadRecord;

static PlatformMutex trackerLock;
static Allocation allocations[64];
static ThreadRecord threads[4];
static uint32_t allocateCalls, reallocateCalls, liveAllocations;
static uint32_t mutexInitCalls, mutexDestroyCalls, conditionInitCalls, conditionDestroyCalls;
static uint32_t threadStartCalls, threadJoinCalls, startedThreads;
static uint32_t failAllocateCall, failThreadStartCall;
static bool failMutexInit, failConditionInit, failEveryThreadStart;
static PlatformMutex *initializedMutex;
static PlatformConditionVariable *initializedCondition;
static uint64_t rendererPlaceholder, meshPlaceholder;
static uint32_t meshAttempts, meshCreates, meshDestroys;
static bool rejectMesh, meshLive;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Chunk streaming failure check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static Allocation *FindAllocation(void *memory)
{
    for (uint32_t index = 0u; index < 64u; ++index)
        if (allocations[index].memory == memory)
            return &allocations[index];
    return NULL;
}

void *StreamingTestAllocate(size_t size, bool zero)
{
    PlatformMutexLock(&trackerLock);
    ++allocateCalls;
    if (allocateCalls == failAllocateCall)
    {
        PlatformMutexUnlock(&trackerLock);
        return NULL;
    }
    void *memory = PlatformAllocate(size, zero);
    Expect(memory != NULL, "ordinary fixture allocation");
    Allocation *record = FindAllocation(NULL);
    Expect(record != NULL, "tracking capacity");
    record->memory = memory;
    record->size = size;
    ++liveAllocations;
    PlatformMutexUnlock(&trackerLock);
    return memory;
}

void *StreamingTestReallocate(void *memory, size_t size, bool zeroNew)
{
    PlatformMutexLock(&trackerLock);
    ++reallocateCalls;
    Allocation *record = FindAllocation(memory);
    Expect(memory != NULL && record != NULL, "reallocate preserves ownership");
    void *moved = PlatformReallocate(memory, size, zeroNew);
    if (moved != NULL)
    {
        record->memory = moved;
        record->size = size;
    }
    PlatformMutexUnlock(&trackerLock);
    return moved;
}

void StreamingTestFree(void *memory)
{
    if (memory == NULL)
        return;
    PlatformMutexLock(&trackerLock);
    Allocation *record = FindAllocation(memory);
    Expect(record != NULL, "free preserves ownership and is not repeated");
    record->memory = NULL;
    record->size = 0u;
    --liveAllocations;
    PlatformFree(memory);
    PlatformMutexUnlock(&trackerLock);
}

bool StreamingTestMutexInitialize(PlatformMutex *mutex)
{
    ++mutexInitCalls;
    Expect(initializedMutex == NULL, "one queue mutex per fixture");
    if (failMutexInit)
        return false;
    if (!PlatformMutexInitialize(mutex))
        return false;
    initializedMutex = mutex;
    return true;
}

void StreamingTestMutexDestroy(PlatformMutex *mutex)
{
    Expect(mutex == initializedMutex && initializedMutex != NULL,
           "destroy only a successfully initialized mutex");
    ++mutexDestroyCalls;
    PlatformMutexDestroy(mutex);
    initializedMutex = NULL;
}

bool StreamingTestConditionInitialize(PlatformConditionVariable *condition)
{
    ++conditionInitCalls;
    Expect(initializedMutex != NULL && initializedCondition == NULL,
           "condition initialization follows successful mutex initialization");
    if (failConditionInit)
        return false;
    if (!PlatformConditionVariableInitialize(condition))
        return false;
    initializedCondition = condition;
    return true;
}

void StreamingTestConditionDestroy(PlatformConditionVariable *condition)
{
    Expect(condition == initializedCondition && initializedCondition != NULL,
           "destroy only a successfully initialized condition");
    ++conditionDestroyCalls;
    PlatformConditionVariableDestroy(condition);
    initializedCondition = NULL;
}

bool StreamingTestThreadStart(PlatformThread *thread, PlatformThreadEntry entry, void *context)
{
    ++threadStartCalls;
    Expect(initializedMutex != NULL && initializedCondition != NULL,
           "worker starts only after synchronization initialization");
    if (failEveryThreadStart || threadStartCalls == failThreadStartCall)
        return false;
    if (!PlatformThreadStart(thread, entry, context))
        return false;
    Expect(startedThreads < 4u, "worker count stays within its bounded pool");
    threads[startedThreads].thread = *thread;
    threads[startedThreads].live = true;
    ++startedThreads;
    return true;
}

void StreamingTestThreadJoin(PlatformThread *thread)
{
    ThreadRecord *record = NULL;
    for (uint32_t index = 0u; index < startedThreads; ++index)
        if (threads[index].live && memcmp(&threads[index].thread, thread, sizeof(*thread)) == 0)
            record = &threads[index];
    Expect(record != NULL, "join exactly one successfully started worker");
    PlatformThreadJoin(thread);
    record->live = false;
    ++threadJoinCalls;
}

static RendererMesh *CreateMesh(Renderer *renderer, const ChunkQuad *quads, uint32_t count)
{
    Expect(renderer == (Renderer *)&rendererPlaceholder && quads != NULL && count != 0u,
           "real mesher produces geometry for the real world block");
    ++meshAttempts;
    if (rejectMesh)
        return NULL;
    Expect(!meshLive, "single-chunk fixture has one mesh owner");
    meshLive = true;
    ++meshCreates;
    return (RendererMesh *)&meshPlaceholder;
}

static void DestroyMesh(Renderer *renderer, RendererMesh *mesh)
{
    Expect(renderer == (Renderer *)&rendererPlaceholder &&
               mesh == (RendererMesh *)&meshPlaceholder && meshLive,
           "normal destruction releases the actual published mesh handle once");
    meshLive = false;
    ++meshDestroys;
}

static const LaiueMesherServiceV1 mesher = {
    .structSize = sizeof(LaiueMesherServiceV1),
    .abiVersion = LAIUE_MESHER_SERVICE_ABI_VERSION_1,
    .scratchCreate = ChunkMesherScratchCreate,
    .scratchDestroy = ChunkMesherScratchDestroy,
    .buildChunkMesh = BuildChunkMesh,
};

static const LaiueGraphicsServiceV1 graphics = {
    .structSize = sizeof(LaiueGraphicsServiceV1),
    .abiVersion = LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1,
    .createMesh = CreateMesh,
    .destroyMesh = DestroyMesh,
};

static uint32_t DesiredWorkers(void)
{
    uint32_t count = PlatformLogicalProcessorCount();
    count = count > 2u ? count - 2u : 1u;
    return count > 4u ? 4u : count;
}

static void ExpectReleased(void)
{
    // Every constructor failure has no live workers; successful fixtures
    // destroy/join before this assertion. Worker frees are tracked under lock.
    Expect(liveAllocations == 0u && initializedMutex == NULL && initializedCondition == NULL,
           "all subject allocations and initialized synchronization are released");
    Expect(threadJoinCalls == startedThreads, "all started workers are joined");
    for (uint32_t index = 0u; index < 4u; ++index)
        Expect(!threads[index].live, "no worker survives fixture cleanup");
    Expect(!meshLive, "no mesh survives fixture cleanup");
}

static void Reset(void)
{
    ExpectReleased();
    memset(allocations, 0, sizeof(allocations));
    memset(threads, 0, sizeof(threads));
    allocateCalls = reallocateCalls = liveAllocations = 0u;
    mutexInitCalls = mutexDestroyCalls = conditionInitCalls = conditionDestroyCalls = 0u;
    threadStartCalls = threadJoinCalls = startedThreads = 0u;
    failAllocateCall = failThreadStartCall = 0u;
    failMutexInit = failConditionInit = failEveryThreadStart = false;
    initializedMutex = NULL;
    initializedCondition = NULL;
    meshAttempts = meshCreates = meshDestroys = 0u;
    rejectMesh = meshLive = false;
}

static ChunkStreaming *Create(World *world, int32_t radius)
{
    return ChunkStreamingCreateWithServices(world, (Renderer *)&rendererPlaceholder, radius, NULL,
                                            LaiueWorldGetStaticServiceV1(), &mesher, &graphics);
}

static void TestRadiusPreflight(World *world)
{
    const int32_t invalid[] = {-1, INT32_MIN, INT32_MAX, INT32_MAX / 2, 563, 1000, 65535};
    for (uint32_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
    {
        Reset();
        // The old constructor exits on this fake allocation failure, so RED
        // detects its first allocation without executing its overflowing loop.
        failAllocateCall = 1u;
        Expect(Create(world, invalid[index]) == NULL, "invalid radius is rejected");
        Expect(allocateCalls == 0u && mutexInitCalls == 0u && conditionInitCalls == 0u &&
                   threadStartCalls == 0u,
               "invalid radius rejected before allocation, scratch, sync or threads");
        ExpectReleased();
    }
    const int32_t ordinary[] = {0, 1, 12, 16};
    for (uint32_t index = 0u; index < sizeof(ordinary) / sizeof(ordinary[0]); ++index)
    {
        Reset();
        failAllocateCall = 1u;
        Expect(Create(world, ordinary[index]) == NULL && allocateCalls == 1u,
               "ordinary nonnegative radii reach allocation unchanged");
        ExpectReleased();
    }
    Reset();
    failAllocateCall = 1u;
    Expect(Create(world, 562) == NULL, "upper arithmetic boundary avoids real allocation");
    Expect(allocateCalls == (SIZE_MAX > UINT32_MAX ? 1u : 0u),
           "2^31-capacity boundary also honors the target size_t");
    ExpectReleased();
#if SIZE_MAX == UINT32_MAX
    Reset();
    failAllocateCall = 1u;
    Expect(Create(world, 177) == NULL && allocateCalls == 0u,
           "32-bit array byte overflow rejected before allocation");
    ExpectReleased();
#endif
}

static void TestIncompleteMesher(World *world)
{
    for (uint32_t missing = 0u; missing < 3u; ++missing)
    {
        Reset();
        LaiueMesherServiceV1 incomplete = mesher;
        if (missing == 0u)
            incomplete.scratchCreate = NULL;
        else if (missing == 1u)
            incomplete.scratchDestroy = NULL;
        else
            incomplete.buildChunkMesh = NULL;
        failEveryThreadStart = true;
        Expect(ChunkStreamingCreateWithServices(world, (Renderer *)&rendererPlaceholder, 1, NULL,
                                                LaiueWorldGetStaticServiceV1(), &incomplete,
                                                &graphics) == NULL,
               "incomplete mesher cannot create a streaming instance");
        Expect(threadStartCalls == 0u && allocateCalls <= 6u,
               "incomplete mesher rejects before scratch allocation or thread start");
        ExpectReleased();
    }
}

static void TestIncompleteGraphics(World *world)
{
    for (uint32_t missing = 0u; missing < 2u; ++missing)
    {
        Reset();
        LaiueGraphicsServiceV1 incomplete = graphics;
        if (missing == 0u)
            incomplete.createMesh = NULL;
        else
            incomplete.destroyMesh = NULL;
        Expect(ChunkStreamingCreateWithServices(world, (Renderer *)&rendererPlaceholder, 0, NULL,
                                                LaiueWorldGetStaticServiceV1(), &mesher,
                                                &incomplete) == NULL,
               "incomplete graphics ownership is rejected");
        Expect(allocateCalls == 0u && threadStartCalls == 0u && meshAttempts == 0u,
               "incomplete graphics rejects before allocating or creating resources");
        ExpectReleased();
    }
    Reset();
    ChunkStreaming *streaming = ChunkStreamingCreateWithServices(
        world, (Renderer *)&rendererPlaceholder, 0, NULL, LaiueWorldGetStaticServiceV1(),
        &mesher, NULL);
    Expect(streaming != NULL, "NULL graphics retains CPU-only operation");
    ChunkStreamingDestroy(streaming);
    ExpectReleased();
}

static void TestArrayFailures(World *world)
{
    for (uint32_t failure = 1u; failure <= 6u; ++failure)
    {
        Reset();
        failAllocateCall = failure;
        Expect(Create(world, 1) == NULL, "constructor allocation failure");
        Expect(allocateCalls == failure && mutexInitCalls == 0u && conditionInitCalls == 0u &&
                   threadStartCalls == 0u,
               "array failure stops construction before synchronization or workers");
        ExpectReleased();
    }
}

static void TestMutexFailure(World *world)
{
    Reset();
    failMutexInit = true;
    Expect(Create(world, 1) == NULL, "mutex failure aborts construction");
    Expect(allocateCalls == 6u && mutexInitCalls == 1u && mutexDestroyCalls == 0u &&
               conditionInitCalls == 0u && conditionDestroyCalls == 0u && threadStartCalls == 0u,
           "failed mutex neither destroyed nor followed by condition/worker creation");
    ExpectReleased();
}

static void TestConditionFailure(World *world)
{
    Reset();
    failConditionInit = true;
    Expect(Create(world, 1) == NULL, "condition failure aborts construction");
    Expect(mutexInitCalls == 1u && mutexDestroyCalls == 1u && conditionInitCalls == 1u &&
               conditionDestroyCalls == 0u && threadStartCalls == 0u,
           "condition failure destroys only the successfully initialized mutex");
    ExpectReleased();
}

static void TestThreadFailures(World *world)
{
    Reset();
    failEveryThreadStart = true;
    Expect(Create(world, 1) == NULL, "no successfully started worker aborts creation");
    Expect(threadStartCalls == DesiredWorkers() && startedThreads == 0u &&
               mutexDestroyCalls == 1u && conditionDestroyCalls == 1u,
           "failed thread starts release real scratch and synchronization");
    ExpectReleased();

    Reset();
    failThreadStartCall = 1u;
    ChunkStreaming *streaming = Create(world, 1);
    Expect(threadStartCalls == DesiredWorkers() && startedThreads == DesiredWorkers() - 1u &&
               ((streaming != NULL) == (startedThreads != 0u)),
           "first worker failure preserves any later successfully started workers");
    if (streaming != NULL)
    {
        Expect(ChunkStreamingPause(streaming), "partial worker pool remains usable");
        ChunkStreamingDestroy(streaming);
    }
    Expect(mutexDestroyCalls == 1u && conditionDestroyCalls == 1u,
           "partial worker pool releases synchronization once");
    ExpectReleased();
}

static void TestRealScratchFailures(World *world)
{
    // First real scratch: header plus its four normal production buffers.
    for (uint32_t failure = 7u; failure <= 11u; ++failure)
    {
        Reset();
        failAllocateCall = failure;
        failEveryThreadStart = true;
        Expect(Create(world, 1) == NULL, "real mesher scratch allocation failure");
        Expect(allocateCalls >= failure && threadStartCalls == DesiredWorkers() - 1u,
               "failed real scratch is cleaned and never counted as a live worker");
        ExpectReleased();
    }
}

static void TestLiveCleanup(World *world, uint32_t mode)
{
    Reset();
    rejectMesh = mode == 1u;
    ChunkStreaming *streaming = Create(world, 0);
    Expect(streaming != NULL && startedThreads == DesiredWorkers(), "real worker pool starts");
    ChunkStreamingSetCenter(streaming, 0, 0, 0);
    const double begin = PlatformMonotonicSeconds();
    for (;;)
    {
        ChunkStreamingStats stats;
        if (mode != 2u)
            ChunkStreamingPump(streaming);
        ChunkStreamingGetStats(streaming, &stats);
        if ((mode == 0u && meshCreates == 1u) || (mode == 1u && meshAttempts != 0u) ||
            (mode == 2u && stats.pendingResults != 0u))
            break;
        Expect(PlatformMonotonicSeconds() - begin < 10.0, "live cleanup fixture makes progress");
        PlatformSleepMilliseconds(1u);
    }
    ChunkStreamingDestroy(streaming);
    Expect(mutexDestroyCalls == 1u && conditionDestroyCalls == 1u,
           "normal destruction releases initialized synchronization once");
    Expect(meshCreates == (mode == 0u ? 1u : 0u) && meshDestroys == meshCreates,
           "normal destruction retains mesh handle cleanup");
    ExpectReleased();
}

static bool TextEqual(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static bool CaseEnabled(const char *selected, const char *name)
{
    return selected[0] == '\0' || TextEqual(selected, "all") || TextEqual(selected, name);
}

LAIUE_TEST_ENTRY(ChunkStreamingFailureTestEntryPoint)
{
    Expect(PlatformMutexInitialize(&trackerLock), "tracking mutex initialization");
    World *world = WorldCreateWithNumericService(NULL, LaiueNumericGetStaticServiceV1());
    Expect(world != NULL, "real empty world creation");
    char selected[32] = {0};
    const uint32_t length = PlatformGetEnvironmentUtf8("LAIUE_STREAMING_FAILURE_CASE", selected,
                                                       (uint32_t)sizeof(selected));
    Expect(length < sizeof(selected), "selected case fits its buffer");
    bool ran = false;
    if (CaseEnabled(selected, "radius"))
    {
        TestRadiusPreflight(world);
        ran = true;
    }
    if (CaseEnabled(selected, "arrays"))
    {
        TestArrayFailures(world);
        ran = true;
    }
    if (CaseEnabled(selected, "provider"))
    {
        TestIncompleteMesher(world);
        ran = true;
    }
    if (CaseEnabled(selected, "graphics"))
    {
        TestIncompleteGraphics(world);
        ran = true;
    }
    if (CaseEnabled(selected, "mutex"))
    {
        TestMutexFailure(world);
        ran = true;
    }
    if (CaseEnabled(selected, "condition"))
    {
        TestConditionFailure(world);
        ran = true;
    }
    if (CaseEnabled(selected, "threads"))
    {
        TestThreadFailures(world);
        ran = true;
    }
    if (CaseEnabled(selected, "scratch"))
    {
        TestRealScratchFailures(world);
        ran = true;
    }
    WorldSetBlock(world, 0, 0, 0, 1u);
    if (CaseEnabled(selected, "live"))
    {
        TestLiveCleanup(world, 0u);
        ran = true;
    }
    if (CaseEnabled(selected, "held"))
    {
        TestLiveCleanup(world, 1u);
        ran = true;
    }
    if (CaseEnabled(selected, "queued"))
    {
        TestLiveCleanup(world, 2u);
        ran = true;
    }
    Expect(ran, "selected case is recognized");
    WorldDestroy(world);
    ExpectReleased();
    PlatformMutexDestroy(&trackerLock);
    LaiueTestRuntimeWrite("chunk_streaming_failure_test passed\n");
    LAIUE_TEST_SUCCESS();
}
