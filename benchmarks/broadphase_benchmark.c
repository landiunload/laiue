// Узкий ручной бенчмарк постоянного broadphase. В CTest не входит: его
// запускают явно, чтобы сравнивать варианты на одной машине.
//
// Он компилирует src/physics/rigid_broadphase.c прямо в исполняемый файл:
// RigidBroadphaseSetProxy/RemoveProxy/Query намеренно не входят в экспорт
// модуля, а мерить нужно именно их, а не полный шаг физики, где улучшение
// тонет в решателе и узкой фазе. RigidBroadphaseQuery не выделяет память и не
// рекурсирует, поэтому время запроса здесь — чистая стоимость обхода.
//
// Сцены детерминированы (фиксированные seeds и целочисленно-дробное
// движение). На выходе для каждой сцены: медианы по прогонам времени
// обновления и запроса на шаг, точные счётчики updated/visited, контрольные
// суммы кандидатов и полнота, проверенная прямым перебором на финальном
// состоянии (пропущенный пересекающийся прокси увеличивает missing).

#include "physics/rigid_broadphase.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define BPB_MAX_BODIES 2048u
#define BPB_WARMUP 8u
#define BPB_STEPS 32u
#define BPB_REPS 7u
#define BPB_STORAGE_BYTES 600000u

static uint8_t bpbStorage[BPB_STORAGE_BYTES];
static VoxelRigidBroadphase bpbIndex;
static VoxelRigidBroadphase bpbEmpty = {0};
static uint32_t bpbOut[BPB_MAX_BODIES];
static uint8_t bpbSeen[BPB_MAX_BODIES];
static volatile uint64_t bpbSink;

typedef struct BpbBody
{
    double center[3];
    double velocity[3];
    double half[3];
} BpbBody;

static BpbBody bpbBodies[BPB_MAX_BODIES];
static double bpbMinimum[BPB_MAX_BODIES][3];
static double bpbMaximum[BPB_MAX_BODIES][3];
static double bpbLimitMinimum[3];
static double bpbLimitMaximum[3];

typedef enum BpbScene
{
    BPB_SCENE_SMALL,
    BPB_SCENE_DENSE,
    BPB_SCENE_SPARSE,
    BPB_SCENE_STATIC,
    BPB_SCENE_MOVING,
    BPB_SCENE_COUNT
} BpbScene;

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

static uint64_t Mix64(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t HashWord(uint64_t hash, uint64_t word)
{
    for (uint32_t byte = 0u; byte < 8u; ++byte)
    {
        hash = (hash ^ (word & 255u)) * UINT64_C(1099511628211);
        word >>= 8u;
    }
    return hash;
}

static uint64_t NextRandom(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state;
}

static double UnitRandom(uint64_t *state)
{
    return (double)((NextRandom(state) >> 33) & 0xffffu) / 65535.0;
}

static double SignedRandom(uint64_t *state)
{
    return UnitRandom(state) * 2.0 - 1.0;
}

static const char *SceneName(BpbScene scene)
{
    if (scene == BPB_SCENE_SMALL)
    {
        return "small";
    }
    if (scene == BPB_SCENE_DENSE)
    {
        return "dense";
    }
    if (scene == BPB_SCENE_SPARSE)
    {
        return "sparse";
    }
    if (scene == BPB_SCENE_STATIC)
    {
        return "static";
    }
    return "moving";
}

// Раскладка зависит только от сцены и номера тела. Смещение и размер задают
// характер нагрузки: плотная куча, разреженные группы, почти неподвижная
// сцена и быстрый равномерный ход.
static uint32_t ConfigureScene(BpbScene scene)
{
    uint64_t random = UINT64_C(0x9e3779b97f4a7c15) ^
                      ((uint64_t)scene * UINT64_C(0x2545f4914f6cdd1d));
    uint32_t count = scene == BPB_SCENE_SMALL ? 16u : BPB_MAX_BODIES;
    double speed = 0.0;
    for (uint32_t index = 0u; index < count; ++index)
    {
        BpbBody *body = &bpbBodies[index];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            body->half[axis] = 0.5;
        }
        if (scene == BPB_SCENE_SMALL)
        {
            uint32_t x = index % 4u;
            uint32_t y = (index / 4u) % 2u;
            uint32_t z = index / 8u;
            body->center[0] = (double)x * 1.1 - 1.65;
            body->center[1] = (double)y * 1.1 - 0.55;
            body->center[2] = (double)z * 1.1 + 2.0;
            speed = 0.35;
        }
        else if (scene == BPB_SCENE_DENSE)
        {
            uint32_t x = index % 16u;
            uint32_t y = (index / 16u) % 16u;
            uint32_t z = index / 256u;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                body->half[axis] = 0.45;
            }
            body->center[0] = (double)x;
            body->center[1] = (double)y;
            body->center[2] = 0.44 + (double)z * 0.89;
            speed = 0.2;
        }
        else if (scene == BPB_SCENE_SPARSE || scene == BPB_SCENE_STATIC)
        {
            uint32_t group = index / 4u;
            uint32_t member = index % 4u;
            double offset = ((double)member - 1.5) * 0.9;
            body->center[0] = (double)(group % 8u) * 40.0 + offset;
            body->center[1] = (double)((group / 8u) % 8u) * 40.0 + offset * 0.5;
            body->center[2] = (double)(group / 64u) * 40.0 + UnitRandom(&random) * 0.25;
            speed = (scene == BPB_SCENE_STATIC && index >= 32u) ? 0.0 : 1.5;
        }
        else
        {
            uint32_t x = index % 16u;
            uint32_t y = (index / 16u) % 16u;
            uint32_t z = index / 256u;
            body->center[0] = (double)x * 2.0;
            body->center[1] = (double)y * 2.0;
            body->center[2] = (double)z * 2.0;
            speed = 2.0;
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            body->velocity[axis] = SignedRandom(&random) * speed;
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        bpbLimitMinimum[axis] = bpbBodies[0].center[axis];
        bpbLimitMaximum[axis] = bpbBodies[0].center[axis];
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (bpbBodies[index].center[axis] < bpbLimitMinimum[axis])
            {
                bpbLimitMinimum[axis] = bpbBodies[index].center[axis];
            }
            if (bpbBodies[index].center[axis] > bpbLimitMaximum[axis])
            {
                bpbLimitMaximum[axis] = bpbBodies[index].center[axis];
            }
        }
        bpbLimitMinimum[axis] -= 2.0;
        bpbLimitMaximum[axis] += 2.0;
    }
    return count;
}

static void ComputeBounds(uint32_t index)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        bpbMinimum[index][axis] = bpbBodies[index].center[axis] - bpbBodies[index].half[axis];
        bpbMaximum[index][axis] = bpbBodies[index].center[axis] + bpbBodies[index].half[axis];
    }
}

// Отражение от границ сцены: движение остаётся ограниченным и детерминированным,
// а равномерный ход сохраняет упреждающее вытягивание толстой коробки.
static void AdvanceBody(BpbBody *body)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double position = body->center[axis] + body->velocity[axis];
        if (position < bpbLimitMinimum[axis])
        {
            position = bpbLimitMinimum[axis] + (bpbLimitMinimum[axis] - position);
            body->velocity[axis] = -body->velocity[axis];
        }
        else if (position > bpbLimitMaximum[axis])
        {
            position = bpbLimitMaximum[axis] - (position - bpbLimitMaximum[axis]);
            body->velocity[axis] = -body->velocity[axis];
        }
        body->center[axis] = position;
    }
}

static bool ExactOverlap(uint32_t first, uint32_t second)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (bpbMinimum[first][axis] > bpbMaximum[second][axis] ||
            bpbMaximum[first][axis] < bpbMinimum[second][axis])
        {
            return false;
        }
    }
    return true;
}

typedef struct BpbRepResult
{
    uint64_t updateNanoseconds;
    uint64_t queryNanoseconds;
    uint64_t candidates;
    uint64_t missing;
    uint64_t rawHash;
    uint64_t setHash;
    uint32_t updatedProxies;
    uint32_t visitedNodes;
    uint32_t proxyCount;
} BpbRepResult;

static uint64_t MedianU64(uint64_t *values, uint32_t count)
{
    for (uint32_t index = 1u; index < count; ++index)
    {
        uint64_t value = values[index];
        uint32_t insertion = index;
        while (insertion > 0u && values[insertion - 1u] > value)
        {
            values[insertion] = values[insertion - 1u];
            --insertion;
        }
        values[insertion] = value;
    }
    return values[count / 2u];
}

static bool RunRep(BpbScene scene, uint32_t count, uint32_t indexBytes, BpbRepResult *outResult)
{
    ConfigureScene(scene);
    bpbIndex = bpbEmpty;
    if (!VoxelRigidBroadphaseInitialize(&bpbIndex, bpbStorage, count, indexBytes))
    {
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        ComputeBounds(index);
    }

    for (uint32_t step = 0u; step < BPB_WARMUP; ++step)
    {
        for (uint32_t index = 0u; index < count; ++index)
        {
            AdvanceBody(&bpbBodies[index]);
            ComputeBounds(index);
            if (!RigidBroadphaseSetProxy(&bpbIndex, index, bpbMinimum[index], bpbMaximum[index]))
            {
                return false;
            }
        }
        for (uint32_t index = 0u; index < count; ++index)
        {
            uint32_t found = 0u;
            if (!RigidBroadphaseQuery(&bpbIndex, bpbMinimum[index], bpbMaximum[index], bpbOut, count,
                                      &found))
            {
                return false;
            }
        }
    }

    BpbRepResult result = {0};
    result.rawHash = UINT64_C(14695981039346656037);
    for (uint32_t step = 0u; step < BPB_STEPS; ++step)
    {
        for (uint32_t index = 0u; index < count; ++index)
        {
            AdvanceBody(&bpbBodies[index]);
            ComputeBounds(index);
        }
        double start = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (!RigidBroadphaseSetProxy(&bpbIndex, index, bpbMinimum[index], bpbMaximum[index]))
            {
                return false;
            }
        }
        double afterUpdate = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < count; ++index)
        {
            uint32_t found = 0u;
            if (!RigidBroadphaseQuery(&bpbIndex, bpbMinimum[index], bpbMaximum[index], bpbOut,
                                      count, &found))
            {
                return false;
            }
            result.candidates += found;
            result.rawHash = HashWord(result.rawHash, (uint64_t)step);
            result.rawHash = HashWord(result.rawHash, (uint64_t)index);
            for (uint32_t candidate = 0u; candidate < found; ++candidate)
            {
                result.rawHash = HashWord(result.rawHash, (uint64_t)bpbOut[candidate]);
                result.setHash ^= Mix64(((uint64_t)step << 32) ^ (uint64_t)index ^
                                        Mix64((uint64_t)bpbOut[candidate]));
            }
        }
        double afterQuery = PlatformMonotonicSeconds();
        result.updateNanoseconds += (uint64_t)((afterUpdate - start) * 1e9);
        result.queryNanoseconds += (uint64_t)((afterQuery - afterUpdate) * 1e9);
    }

    // Полнота: прямой перебор по точным коробкам. Жирная коробка листа обязана
    // содержать точную, поэтому любой пересекающий запрос кандидат обязан
    // вернуться. Пропуск считается в missing.
    for (uint32_t first = 0u; first < count; ++first)
    {
        uint32_t found = 0u;
        if (!RigidBroadphaseQuery(&bpbIndex, bpbMinimum[first], bpbMaximum[first], bpbOut, count,
                                  &found))
        {
            return false;
        }
        for (uint32_t index = 0u; index < count; ++index)
        {
            bpbSeen[index] = 0u;
        }
        for (uint32_t candidate = 0u; candidate < found; ++candidate)
        {
            bpbSeen[bpbOut[candidate]] = 1u;
        }
        for (uint32_t second = 0u; second < count; ++second)
        {
            if (second != first && !bpbSeen[second] && ExactOverlap(first, second))
            {
                ++result.missing;
            }
        }
    }

    result.updatedProxies = bpbIndex.updatedProxyCount;
    result.visitedNodes = bpbIndex.visitedNodeCount;
    result.proxyCount = bpbIndex.proxyCount;
    bpbSink ^= result.rawHash ^ result.setHash ^ result.candidates;
    *outResult = result;
    return true;
}

static void ReportScene(BpbScene scene, uint32_t count, uint32_t indexBytes,
                        const BpbRepResult *result, uint64_t updateMedian, uint64_t queryMedian)
{
    WriteText("bpbench ");
    WriteText(SceneName(scene));
    WriteText(" bodies=");
    WriteUnsigned(count);
    WriteText(" steps=");
    WriteUnsigned(BPB_STEPS);
    WriteText(" reps=");
    WriteUnsigned(BPB_REPS);
    WriteText(" index_bytes=");
    WriteUnsigned(indexBytes);
    WriteText(" update_ns=");
    WriteUnsigned(updateMedian);
    WriteText(" query_ns=");
    WriteUnsigned(queryMedian);
    WriteText(" total_ns=");
    WriteUnsigned(updateMedian + queryMedian);
    WriteText(" candidates=");
    WriteUnsigned(result->candidates);
    WriteText(" updated=");
    WriteUnsigned(result->updatedProxies);
    WriteText(" visited=");
    WriteUnsigned(result->visitedNodes);
    WriteText(" proxies=");
    WriteUnsigned(result->proxyCount);
    WriteText(" missing=");
    WriteUnsigned(result->missing);
    WriteText(" raw_hash=");
    WriteUnsigned(result->rawHash);
    WriteText(" set_hash=");
    WriteUnsigned(result->setHash);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(BroadphaseBenchmarkEntryPoint)
{
    WriteText("laiue broadphase benchmark\n");
    for (uint32_t sceneIndex = 0u; sceneIndex < (uint32_t)BPB_SCENE_COUNT; ++sceneIndex)
    {
        BpbScene scene = (BpbScene)sceneIndex;
        uint32_t count = ConfigureScene(scene);
        uint32_t indexBytes = VoxelRigidBroadphaseBytes(count);
        if (indexBytes == 0u || indexBytes > BPB_STORAGE_BYTES)
        {
            WriteText("broadphase benchmark storage unavailable\n");
            LaiueTestRuntimeExit(2);
        }
        BpbRepResult results[BPB_REPS];
        uint64_t updateSamples[BPB_REPS];
        uint64_t querySamples[BPB_REPS];
        for (uint32_t rep = 0u; rep < BPB_REPS; ++rep)
        {
            if (!RunRep(scene, count, indexBytes, &results[rep]))
            {
                WriteText("broadphase benchmark run failed\n");
                LaiueTestRuntimeExit(2);
            }
            updateSamples[rep] = results[rep].updateNanoseconds / BPB_STEPS;
            querySamples[rep] = results[rep].queryNanoseconds / BPB_STEPS;
        }
        for (uint32_t rep = 1u; rep < BPB_REPS; ++rep)
        {
            if (results[rep].candidates != results[0].candidates ||
                results[rep].updatedProxies != results[0].updatedProxies ||
                results[rep].visitedNodes != results[0].visitedNodes ||
                results[rep].missing != results[0].missing ||
                results[rep].rawHash != results[0].rawHash ||
                results[rep].setHash != results[0].setHash ||
                results[rep].proxyCount != count)
            {
                WriteText("broadphase benchmark nondeterministic across reps\n");
                LaiueTestRuntimeExit(3);
            }
        }
        if (results[0].missing != 0u)
        {
            WriteText("broadphase benchmark lost candidates\n");
            LaiueTestRuntimeExit(4);
        }
        uint64_t updateMedian = MedianU64(updateSamples, BPB_REPS);
        uint64_t queryMedian = MedianU64(querySamples, BPB_REPS);
        ReportScene(scene, count, indexBytes, &results[0], updateMedian, queryMedian);
    }
    if (bpbSink == UINT64_MAX)
    {
        WriteText("");
    }
    LAIUE_TEST_SUCCESS();
}
