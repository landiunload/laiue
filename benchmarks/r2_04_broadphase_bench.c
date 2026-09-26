// ROUND 2, agent 04-broadphase: измерительный стенд постоянного broadphase.
//
// Зачем свой стенд, а не существующий laiue_broadphase_benchmark: прошлая волна
// измеряла сцены, которые для этой задачи узки. Нужны (а) кластеризованное и
// однородное размещение, (б) смешанные размеры тел, (в) статические и
// движущиеся прокси, (г) малая и крупная сцена, (д) полный цикл
// RemoveProxy/SetProxy. Кроме того, здесь замеряется ВРЕМЯ ОБХОДА, а хеши и
// проверка полноты вынесены во вневременной проход финального состояния:
// хеширование кандидатов внутри замера тонуло бы в стоимости FNV и искажало
// сравнение.
//
// production src/physics/rigid_broadphase.c компилируется прямо в exe: SetProxy,
// RemoveProxy и Query намеренно не входят в экспорт модуля.
//
// Сцены детерминированы; движение — целочисленно-дробные отражения внутри
// границ. Для каждой сцены печатается строка на прогон (rep) с временем
// обновления и запроса, счётчиками updated/visited/candidates, и одна строка
// верификации финального состояния с missing (полнота против прямого перебора)
// и двумя контрольными суммами кандидатов.

#include "physics/rigid_broadphase.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>

#define RB_MAX_BODIES 8192u
#define RB_STORAGE_BYTES 1200000u
#define RB_WARMUP 12u
#define RB_REPS 1u

static uint8_t rbStorage[RB_STORAGE_BYTES];
static VoxelRigidBroadphase rbIndex;
static VoxelRigidBroadphase rbEmpty = {0};
static uint32_t rbOut[RB_MAX_BODIES];
static uint8_t rbSeen[RB_MAX_BODIES];

typedef struct RbBody
{
    double center[3];
    double velocity[3];
    double half[3];
} RbBody;

static RbBody rbBodies[RB_MAX_BODIES];
static double rbMinimum[RB_MAX_BODIES][3];
static double rbMaximum[RB_MAX_BODIES][3];
static double rbLimitMinimum[3];
static double rbLimitMaximum[3];

typedef enum RbScene
{
    RB_SCENE_UNIFORM,
    RB_SCENE_CLUSTERED,
    RB_SCENE_STATIC,
    RB_SCENE_LARGE,
    RB_SCENE_SMALL,
    RB_SCENE_CHURN,
    RB_SCENE_COUNT
} RbScene;

static const char *SceneName(RbScene scene)
{
    if (scene == RB_SCENE_UNIFORM) return "uniform";
    if (scene == RB_SCENE_CLUSTERED) return "clustered";
    if (scene == RB_SCENE_STATIC) return "static";
    if (scene == RB_SCENE_LARGE) return "large";
    if (scene == RB_SCENE_SMALL) return "small";
    return "churn";
}

static uint32_t SceneBodies(RbScene scene)
{
    if (scene == RB_SCENE_LARGE) return 8192u;
    if (scene == RB_SCENE_SMALL) return 48u;
    return 2048u;
}

static uint32_t SceneSteps(RbScene scene)
{
    if (scene == RB_SCENE_STATIC) return 150u;
    if (scene == RB_SCENE_LARGE) return 25u;
    if (scene == RB_SCENE_SMALL) return 3000u;
    if (scene == RB_SCENE_CHURN) return 50u;
    return 120u;
}

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

// Полнота и контрольные суммы — дорогой O(count^2) проход. В A/B он нужен
// один раз на версию, поэтому его можно выключить переменной окружения, чтобы
// замеряемые запуски не платили за него. По умолчанию (переменная не задана)
// проверка включена.
static bool VerificationEnabled(void)
{
#if defined(_WIN32)
    char value[8];
    DWORD length = GetEnvironmentVariableA("R2BP_VERIFY", value, (DWORD)sizeof(value));
    if (length == 0u)
    {
        return true;
    }
    return value[0] != '0';
#else
    const char *value = getenv("R2BP_VERIFY");
    if (value == NULL)
    {
        return true;
    }
    return value[0] != '0';
#endif
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

static void WriteTag(const char *tag)
{
    WriteText(tag);
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

// Раскладка зависит только от сцены и номера тела. Смещение и размер задают
// характер нагрузки: однородная сетка, тесные группы смешанных размеров,
// почти неподвижная сцена, крупная разреженная сцена и малая сцена.
static uint32_t ConfigureScene(RbScene scene)
{
    uint64_t random = UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)scene * UINT64_C(0x2545f4914f6cdd1d));
    uint32_t count = SceneBodies(scene);
    double speed = 0.0;
    for (uint32_t index = 0u; index < count; ++index)
    {
        RbBody *body = &rbBodies[index];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            body->half[axis] = 0.5;
        }
        if (scene == RB_SCENE_UNIFORM || scene == RB_SCENE_CHURN)
        {
            uint32_t x = index % 16u;
            uint32_t y = (index / 16u) % 16u;
            uint32_t z = index / 256u;
            body->center[0] = (double)x * 2.0;
            body->center[1] = (double)y * 2.0;
            body->center[2] = (double)z * 2.0;
            speed = 0.6;
        }
        else if (scene == RB_SCENE_CLUSTERED)
        {
            static const double sizes[6] = {0.25, 0.4, 0.5, 0.9, 1.5, 3.0};
            double half = sizes[index % 6u];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                body->half[axis] = half;
            }
            uint32_t group = index / 8u;
            uint32_t member = index % 8u;
            double offset = ((double)member - 3.5) * 0.9;
            body->center[0] = (double)(group % 8u) * 40.0 + offset;
            body->center[1] = (double)((group / 8u) % 8u) * 40.0 + offset * 0.5;
            body->center[2] = (double)(group / 64u) * 40.0 + UnitRandom(&random) * 0.25;
            speed = 0.8;
        }
        else if (scene == RB_SCENE_STATIC)
        {
            uint32_t x = index % 16u;
            uint32_t y = (index / 16u) % 16u;
            uint32_t z = index / 256u;
            body->center[0] = (double)x * 4.0;
            body->center[1] = (double)y * 4.0;
            body->center[2] = (double)z * 4.0;
            speed = index < 64u ? 1.0 : 0.0;
        }
        else if (scene == RB_SCENE_LARGE)
        {
            static const double sizes[8] = {0.25, 0.25, 0.4, 0.5, 0.5, 1.5, 4.0, 16.0};
            double half = sizes[index & 7u];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                body->half[axis] = half;
            }
            uint32_t x = index % 16u;
            uint32_t y = (index / 16u) % 16u;
            uint32_t z = index / 256u;
            body->center[0] = (double)x * 8.0;
            body->center[1] = (double)y * 8.0;
            body->center[2] = (double)z * 8.0;
            speed = 1.0;
        }
        else
        {
            uint32_t x = index % 4u;
            uint32_t y = (index / 4u) % 3u;
            uint32_t z = index / 12u;
            body->center[0] = (double)x * 1.5;
            body->center[1] = (double)y * 1.5;
            body->center[2] = (double)z * 1.5;
            speed = 0.5;
        }
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            body->velocity[axis] = SignedRandom(&random) * speed;
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        rbLimitMinimum[axis] = rbBodies[0].center[axis];
        rbLimitMaximum[axis] = rbBodies[0].center[axis];
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (rbBodies[index].center[axis] < rbLimitMinimum[axis])
            {
                rbLimitMinimum[axis] = rbBodies[index].center[axis];
            }
            if (rbBodies[index].center[axis] > rbLimitMaximum[axis])
            {
                rbLimitMaximum[axis] = rbBodies[index].center[axis];
            }
        }
        rbLimitMinimum[axis] -= 3.0;
        rbLimitMaximum[axis] += 3.0;
    }
    return count;
}

static void ComputeBounds(uint32_t index)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        rbMinimum[index][axis] = rbBodies[index].center[axis] - rbBodies[index].half[axis];
        rbMaximum[index][axis] = rbBodies[index].center[axis] + rbBodies[index].half[axis];
    }
}

static void AdvanceBody(RbBody *body)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double position = body->center[axis] + body->velocity[axis];
        if (position < rbLimitMinimum[axis])
        {
            position = rbLimitMinimum[axis] + (rbLimitMinimum[axis] - position);
            body->velocity[axis] = -body->velocity[axis];
        }
        else if (position > rbLimitMaximum[axis])
        {
            position = rbLimitMaximum[axis] - (position - rbLimitMaximum[axis]);
            body->velocity[axis] = -body->velocity[axis];
        }
        body->center[axis] = position;
    }
}

static bool ExactOverlap(uint32_t first, uint32_t second)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (rbMinimum[first][axis] > rbMaximum[second][axis] ||
            rbMaximum[first][axis] < rbMinimum[second][axis])
        {
            return false;
        }
    }
    return true;
}

typedef struct RbRepResult
{
    uint64_t updateNanoseconds;
    uint64_t queryNanoseconds;
    uint64_t candidates;
    uint32_t updatedProxies;
    uint32_t visitedNodes;
} RbRepResult;

typedef struct RbVerifyResult
{
    uint64_t candidates;
    uint64_t missing;
    uint64_t rawHash;
    uint64_t setHash;
} RbVerifyResult;

// Один прогон: конфигурация, инициализация, прогрев, затем замеp steps шагов.
// verify=true дополнительно считает полноту и контрольные суммы финального
// состояния (дорого, поэтому только на rep 0).
static bool RunRep(RbScene scene, uint32_t count, uint32_t steps, bool verify, bool churn,
                   RbRepResult *outResult, RbVerifyResult *outVerify)
{
    ConfigureScene(scene);
    rbIndex = rbEmpty;
    uint32_t indexBytes = VoxelRigidBroadphaseBytes(count);
    if (indexBytes == 0u || indexBytes > RB_STORAGE_BYTES)
    {
        return false;
    }
    if (!VoxelRigidBroadphaseInitialize(&rbIndex, rbStorage, count, indexBytes))
    {
        return false;
    }
    for (uint32_t index = 0u; index < count; ++index)
    {
        ComputeBounds(index);
    }

    for (uint32_t step = 0u; step < RB_WARMUP; ++step)
    {
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (churn) RigidBroadphaseRemoveProxy(&rbIndex, index);
            AdvanceBody(&rbBodies[index]);
            ComputeBounds(index);
            if (!RigidBroadphaseSetProxy(&rbIndex, index, rbMinimum[index], rbMaximum[index]))
            {
                return false;
            }
        }
        for (uint32_t index = 0u; index < count; ++index)
        {
            uint32_t found = 0u;
            if (!RigidBroadphaseQuery(&rbIndex, rbMinimum[index], rbMaximum[index], rbOut, count, &found))
            {
                return false;
            }
        }
    }

    uint32_t updatedBefore = rbIndex.updatedProxyCount;
    uint32_t visitedBefore = rbIndex.visitedNodeCount;

    RbRepResult result = {0};
    for (uint32_t step = 0u; step < steps; ++step)
    {
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (churn) RigidBroadphaseRemoveProxy(&rbIndex, index);
            AdvanceBody(&rbBodies[index]);
            ComputeBounds(index);
        }
        double start = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < count; ++index)
        {
            if (!RigidBroadphaseSetProxy(&rbIndex, index, rbMinimum[index], rbMaximum[index]))
            {
                return false;
            }
        }
        double afterUpdate = PlatformMonotonicSeconds();
        for (uint32_t index = 0u; index < count; ++index)
        {
            uint32_t found = 0u;
            if (!RigidBroadphaseQuery(&rbIndex, rbMinimum[index], rbMaximum[index], rbOut, count, &found))
            {
                return false;
            }
            result.candidates += found;
        }
        double afterQuery = PlatformMonotonicSeconds();
        result.updateNanoseconds += (uint64_t)((afterUpdate - start) * 1e9);
        result.queryNanoseconds += (uint64_t)((afterQuery - afterUpdate) * 1e9);
    }
    result.updatedProxies = rbIndex.updatedProxyCount - updatedBefore;
    result.visitedNodes = rbIndex.visitedNodeCount - visitedBefore;

    if (verify)
    {
        RbVerifyResult verification = {0};
        verification.rawHash = UINT64_C(14695981039346656037);
        // Один проход: полнота против прямого перебора и контрольные суммы
        // множества кандидатов считаются вместе с тем же обходом.
        for (uint32_t first = 0u; first < count; ++first)
        {
            uint32_t found = 0u;
            if (!RigidBroadphaseQuery(&rbIndex, rbMinimum[first], rbMaximum[first], rbOut, count,
                                      &found))
            {
                return false;
            }
            verification.candidates += found;
            verification.rawHash = HashWord(verification.rawHash, (uint64_t)first);
            verification.rawHash = HashWord(verification.rawHash, (uint64_t)found);
            for (uint32_t candidate = 0u; candidate < found; ++candidate)
            {
                uint32_t slot = rbOut[candidate];
                verification.rawHash = HashWord(verification.rawHash, (uint64_t)slot);
                verification.setHash ^=
                    Mix64(((uint64_t)first * UINT64_C(0x9e3779b97f4a7c15)) ^ Mix64((uint64_t)slot));
                rbSeen[slot] = 1u;
            }
            for (uint32_t second = 0u; second < count; ++second)
            {
                if (second != first && rbSeen[second] == 0u && ExactOverlap(first, second))
                {
                    ++verification.missing;
                }
            }
            for (uint32_t candidate = 0u; candidate < found; ++candidate)
            {
                rbSeen[rbOut[candidate]] = 0u;
            }
        }
        *outVerify = verification;
    }
    else
    {
        RbVerifyResult empty = {0};
        *outVerify = empty;
    }
    *outResult = result;
    return true;
}

static void ReportRep(RbScene scene, uint32_t count, uint32_t steps, uint32_t rep,
                      const RbRepResult *result)
{
    WriteTag("r2bp scene=");
    WriteText(SceneName(scene));
    WriteTag(" count=");
    WriteUnsigned(count);
    WriteTag(" steps=");
    WriteUnsigned(steps);
    WriteTag(" rep=");
    WriteUnsigned(rep);
    WriteTag(" update_ns=");
    WriteUnsigned(result->updateNanoseconds);
    WriteTag(" query_ns=");
    WriteUnsigned(result->queryNanoseconds);
    WriteTag(" candidates=");
    WriteUnsigned(result->candidates);
    WriteTag(" updated=");
    WriteUnsigned(result->updatedProxies);
    WriteTag(" visited=");
    WriteUnsigned(result->visitedNodes);
    WriteTag("\n");
}

static void ReportVerify(RbScene scene, uint32_t count, const RbVerifyResult *verification)
{
    WriteTag("r2bp.verify scene=");
    WriteText(SceneName(scene));
    WriteTag(" count=");
    WriteUnsigned(count);
    WriteTag(" candidates=");
    WriteUnsigned(verification->candidates);
    WriteTag(" missing=");
    WriteUnsigned(verification->missing);
    WriteTag(" raw_hash=");
    WriteUnsigned(verification->rawHash);
    WriteTag(" set_hash=");
    WriteUnsigned(verification->setHash);
    WriteTag("\n");
}

LAIUE_TEST_ENTRY(R2BroadphaseBenchEntryPoint)
{
    WriteText("laiue r2_04_broadphase benchmark\n");
    // Тот же target/флаги, что у production TU: маркер ISA нужен, чтобы
    // отличить действительно активную векторную ветку от скалярного отката.
#if defined(__AVX2__)
    WriteText("r2bp.isa=avx2\n");
#elif defined(__SSE2__)
    WriteText("r2bp.isa=sse2\n");
#else
    WriteText("r2bp.isa=scalar\n");
#endif
    for (uint32_t sceneIndex = 0u; sceneIndex < (uint32_t)RB_SCENE_COUNT; ++sceneIndex)
    {
        RbScene scene = (RbScene)sceneIndex;
        uint32_t count = SceneBodies(scene);
        uint32_t steps = SceneSteps(scene);
        uint32_t indexBytes = VoxelRigidBroadphaseBytes(count);
        bool churn = scene == RB_SCENE_CHURN;
        bool verifyEnabled = VerificationEnabled();
        RbRepResult firstResult = {0};
        RbVerifyResult verification = {0};
        for (uint32_t rep = 0u; rep < RB_REPS; ++rep)
        {
            RbRepResult result = {0};
            RbVerifyResult localVerify = {0};
            if (!RunRep(scene, count, steps, rep == 0u && verifyEnabled, churn, &result, &localVerify))
            {
                WriteText("r2bp run failed\n");
                LaiueTestRuntimeExit(2);
            }
            if (rep == 0u)
            {
                firstResult = result;
                verification = localVerify;
            }
            else if (result.candidates != firstResult.candidates ||
                     result.updatedProxies != firstResult.updatedProxies ||
                     result.visitedNodes != firstResult.visitedNodes)
            {
                WriteText("r2bp nondeterministic across reps\n");
                LaiueTestRuntimeExit(3);
            }
            ReportRep(scene, count, steps, rep, &result);
        }
        if (verifyEnabled)
        {
            if (verification.missing != 0u)
            {
                WriteText("r2bp lost candidates\n");
                LaiueTestRuntimeExit(4);
            }
            ReportVerify(scene, count, &verification);
        }
        WriteTag("r2bp.scene scene=");
        WriteText(SceneName(scene));
        WriteTag(" count=");
        WriteUnsigned(count);
        WriteTag(" index_bytes=");
        WriteUnsigned(indexBytes);
        WriteTag("\n");
    }
    LaiueTestRuntimeExit(0);
}
