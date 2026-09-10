#include "physics/rigid_body.h"
#include "physics/rigid_broadphase.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Постоянный индекс на сценах, ради которых он и нужен: разреженной, с
 * телами разного размера и с большим числом неподвижных тел. Однородная
 * куча здесь намеренно не проверяется — на ней быстрее равномерная сетка,
 * и выбор режима остаётся за приложением.
 *
 * Тест одновременно проверка и benchmark. Проверяет он полноту: та же
 * сцена прогоняется дважды, индексом и сеткой, и состояния тел обязаны
 * совпасть до бита. Сетка ищет соседей независимо от дерева, поэтому
 * потерянный деревом кандидат немедленно разводит траектории. Поэтому в
 * каждой сцене тела собраны в тесные группы: сцена без единого контакта
 * ничего бы о полноте не доказала.
 *
 * Меряет он три величины. Две детерминированы и не зависят от машины:
 * `updatedProxyCount` — сколько листьев за шаг пришлось перевставить,
 * `visitedNodeCount` — сколько узлов обошли все запросы шага. Решение об
 * оптимизации принимается по ним. Третья — время этапов из профиля шага;
 * оно шумит и лишь подтверждает, что счётчики переводятся в секунды.
 * Память — `VoxelRigidBroadphaseBytes`, величина точная.
 *
 * Сцены намеренно небольшие: тест входит в обычный CTest и не должен
 * занимать машину. Команды для крупного замера — в
 * docs/broadphase_parallel_work.md.
 */

#define SCALE_BODIES 2048u
#define SCALE_STEPS 24u
#define SCALE_GROUP 4u
#define SCALE_MOVERS 32u
#define SCALE_SPACING 40.0

static uint32_t scaleChecks;

static void Expect(bool condition, const char *message)
{
    ++scaleChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Broadphase scale failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

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

// Микросекунды с одним знаком: миллисекунда слишком груба для этапа.
static void WriteMicroseconds(double seconds)
{
    double value = seconds * 1000000.0;
    if (!(value >= 0.0))
    {
        value = 0.0;
    }
    if (value > 4000000000.0)
    {
        value = 4000000000.0;
    }
    uint64_t whole = (uint64_t)value;
    WriteUnsigned(whole);
    WriteText(".");
    uint64_t fraction = (uint64_t)((value - (double)whole) * 10.0);
    WriteUnsigned(fraction > 9u ? 9u : fraction);
}

static double Absolute(double value)
{
    return value < 0.0 ? -value : value;
}

// Мир без блоков: сцены проверяют индекс тел, а не контакт с вокселями.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void QueryBlocks(void *context, int64_t x, int64_t y, int64_t z, VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    (void)z;
    outBlock->flags = 0u;
    outBlock->friction = 0.5f;
}

static uint64_t HashWord(uint64_t hash, uint64_t word)
{
    for (uint32_t byte = 0u; byte < 8u; ++byte)
    {
        hash = (hash ^ (word & 255u)) * UINT64_C(1099511628211);
        word >>= 8;
    }
    return hash;
}

static uint64_t HashDouble(uint64_t hash, double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return HashWord(hash, representation.bits);
}

static uint64_t HashCoordinate(uint64_t hash, const InfiniteCoord *coordinate)
{
    hash = HashWord(hash, (uint64_t)(int64_t)coordinate->sign);
    hash = HashWord(hash, coordinate->limbCount);
    for (uint32_t limb = 0u; limb < coordinate->limbCount; ++limb)
    {
        hash = HashWord(hash, coordinate->limbs[limb]);
    }
    return hash;
}

static uint64_t HashBodies(const VoxelRigidBody *bodies, uint32_t count)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint32_t index = 0u; index < count; ++index)
    {
        const VoxelRigidBody *body = &bodies[index];
        hash = HashWord(hash, body->stableId);
        hash = HashWord(hash, body->sleeping ? 1u : 0u);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            hash = HashCoordinate(hash, &body->position[axis]);
            hash = HashCoordinate(hash, &body->linearVelocity[axis]);
            hash = HashCoordinate(hash, &body->angularVelocity[axis]);
        }
        for (uint32_t axis = 0u; axis < 4u; ++axis)
        {
            hash = HashDouble(hash, body->orientation[axis]);
        }
    }
    return hash;
}

typedef enum ScaleScene
{
    // Группы далеко друг от друга, все тела движутся: запросы почти всегда
    // возвращают только свою группу, зато листья постоянно выходят за
    // собственную оболочку.
    SCALE_SCENE_SPARSE,
    // Полурёбра от 0.25 до 16 в одной сцене. Дерево обязано не терять
    // мелкие тела рядом с крупными и не раздувать внутренние узлы.
    SCALE_SCENE_MIXED,
    // Движется горстка тел, остальные стоят. Проверяет цену ежешагового
    // обхода неподвижных прокси.
    SCALE_SCENE_STATIC,
    SCALE_SCENE_COUNT
} ScaleScene;

static const char *SceneName(ScaleScene scene)
{
    if (scene == SCALE_SCENE_SPARSE)
    {
        return "sparse";
    }
    if (scene == SCALE_SCENE_MIXED)
    {
        return "mixed";
    }
    return "static";
}

static uint64_t SceneRandom(uint64_t *state)
{
    *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return *state >> 33;
}

static double SceneUnit(uint64_t *state)
{
    return (double)(SceneRandom(state) & 0xffffu) / 65535.0;
}

// Описание зависит только от номера тела: иначе сравнивать индекс с сеткой
// было бы нечего. Смещение сцены задаётся отдельно и целыми блоками.
static void DescribeSceneBody(ScaleScene scene, uint32_t index, const int64_t shift[3],
                              VoxelRigidBodyDescription *description)
{
    uint64_t state =
        UINT64_C(0x9e3779b97f4a7c15) ^ ((uint64_t)index * UINT64_C(0x2545f4914f6cdd1d));
    (void)SceneRandom(&state);

    double half = 0.5;
    if (scene == SCALE_SCENE_MIXED)
    {
        // Восемь ступеней размера, крупные тела редки: именно такое
        // распределение ломает деревья, выровненные только по высоте.
        static const double steps[8] = {0.25, 0.25, 0.4, 0.5, 0.5, 1.5, 4.0, 16.0};
        half = steps[index & 7u];
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        description->halfExtent[axis] = half;
    }

    // Группа из четырёх тел стоит вплотную, группы разнесены на 40 блоков.
    // Смещение внутри группы масштабируется размером, но ограничено блоком
    // с небольшим запасом: крупное тело не должно доставать до соседней
    // группы, иначе сцена перестанет быть разреженной.
    uint32_t group = index / SCALE_GROUP;
    uint32_t member = index % SCALE_GROUP;
    double reach = half < 1.0 ? half : 1.0;
    double offset = ((double)member - 1.5) * 0.9 * reach;
    if (scene == SCALE_SCENE_STATIC && index >= SCALE_MOVERS)
    {
        // Неподвижным телам нужно ещё и не касаться друг друга: иначе их
        // расталкивает решатель, прокси перевставляются, и сцена перестаёт
        // измерять то, ради чего она есть.
        offset = ((double)member - 1.5) * 6.0;
    }
    uint32_t side = 8u;
    double base[3];
    base[0] = (double)(group % side) * SCALE_SPACING + offset;
    base[1] = (double)((group / side) % side) * SCALE_SPACING + offset * 0.5;
    uint32_t layer = group / (side * side);
    base[2] = (double)layer * SCALE_SPACING + SceneUnit(&state) * 0.25;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        description->position[axis] = base[axis] - (double)shift[axis];
    }
    description->mass = 1.0;
    description->restitution = 0.0;
    description->friction = 0.5;
}

static void SceneVelocity(ScaleScene scene, uint32_t index, double velocity[3])
{
    uint64_t state =
        UINT64_C(0x0123456789abcdef) ^ ((uint64_t)index * UINT64_C(0x9e3779b97f4a7c15));
    bool mover = scene != SCALE_SCENE_STATIC || index < SCALE_MOVERS;
    double magnitude = mover ? 1.5 : 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        velocity[axis] = (SceneUnit(&state) * 2.0 - 1.0) * magnitude;
    }
}

typedef struct ScaleRun
{
    uint64_t hash;
    uint64_t updatedProxies;
    uint64_t visitedNodes;
    uint32_t midProxyCount;
    double stepSeconds;
    double broadphaseSeconds;
    double querySeconds;
} ScaleRun;

// Расписание ухода тел из симуляции: каждое третье выбывает на восьми шагах
// и возвращается. Зависит только от номера тела и номера шага, поэтому
// прогоны индексом и сеткой получают одинаковую последовательность.
static bool ChurnActive(uint32_t index, uint32_t step)
{
    if (index % 3u != 0u)
    {
        return true;
    }
    return step < 8u || step >= 16u;
}

static double ProfileClock(void *context)
{
    (void)context;
    return PlatformMonotonicSeconds();
}

static const int64_t scaleNoShift[3] = {0, 0, 0};

// Общий прогон сцены. indexed подключает дерево, иначе работает сетка.
// keepBodies оставляет тела живыми для проверок вызывающего.
static bool RunScene(ScaleScene scene, bool indexed, const int64_t shift[3], VoxelRigidBody *bodies,
                     void *scratch, uint32_t scratchBytes, VoxelRigidContactCache *cache,
                     VoxelRigidBroadphase *broadphase, bool keepBodies, bool churn,
                     ScaleRun *outRun)
{
    VoxelCollisionSource collision;
    collision.context = NULL;
    collision.queryBlockPhysics = QueryBlocks;
    collision.queryDynamicColliders = NULL;

    VoxelRigidStepSettings settings;
    VoxelRigidStepSettingsDefault(&settings);
    // Без тяжести сцена не оседает в кучу и остаётся той, что задумана.
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        settings.gravity[axis] = 0.0;
    }
    // Сон выключен намеренно: иначе неподвижная сцена засыпает целиком,
    // шаг выходит досрочно и мерить в ней становится нечего.
    settings.sleepFrames = 0u;

    for (uint32_t index = 0u; index < SCALE_BODIES; ++index)
    {
        VoxelRigidBodyDescription description;
        DescribeSceneBody(scene, index, shift, &description);
        if (!VoxelRigidBodyInitialize(&bodies[index], (uint64_t)index + 1u, &description))
        {
            return false;
        }
        double velocity[3];
        SceneVelocity(scene, index, velocity);
        if (!VoxelRigidBodyAddLinearVelocity(&bodies[index], velocity))
        {
            return false;
        }
    }

    VoxelRigidContactCacheReset(cache);
    if (indexed)
    {
        VoxelRigidBroadphaseReset(broadphase);
    }

    VoxelRigidStepProfile profile;
    VoxelRigidStepProfile emptyProfile = {0};
    profile = emptyProfile;
    profile.structSize = sizeof(profile);

    VoxelRigidStepOptions options;
    VoxelRigidStepOptions emptyOptions = {0};
    options = emptyOptions;
    options.structSize = sizeof(options);
    options.contactCache = cache;
    options.broadphase = indexed ? broadphase : NULL;
    options.solverOrder = VOXEL_RIGID_SOLVER_CANONICAL;
    options.profile = &profile;
    options.clockSeconds = ProfileClock;

    ScaleRun run = {0};
    double begin = PlatformMonotonicSeconds();
    for (uint32_t step = 0u; step < SCALE_STEPS; ++step)
    {
        if (churn)
        {
            // Тело, ушедшее из симуляции, обязано потерять прокси, а
            // вернувшееся — получить его заново. Это единственный путь, на
            // котором лист возвращается в пул и его слот переиспользуется.
            for (uint32_t index = 0u; index < SCALE_BODIES; ++index)
            {
                bodies[index].active = ChurnActive(index, step);
            }
        }
        if (!VoxelRigidBodyStepEx(bodies, SCALE_BODIES, &collision, &settings, scratch,
                                  scratchBytes, &options))
        {
            return false;
        }
        if (indexed && step == 12u)
        {
            run.midProxyCount = broadphase->proxyCount;
        }
        if (indexed)
        {
            run.updatedProxies += broadphase->updatedProxyCount;
            run.visitedNodes += broadphase->visitedNodeCount;
        }
        run.broadphaseSeconds += profile.seconds[VOXEL_RIGID_PROFILE_BROADPHASE];
        // Запросы индекса живут внутри поиска контактов и пробуждения;
        // отдельного этапа у них нет.
        run.querySeconds += profile.seconds[VOXEL_RIGID_PROFILE_BODY_CONTACTS] +
                            profile.seconds[VOXEL_RIGID_PROFILE_WAKE];
    }
    run.stepSeconds = PlatformMonotonicSeconds() - begin;
    run.hash = HashBodies(bodies, SCALE_BODIES);

    if (!keepBodies)
    {
        for (uint32_t index = 0u; index < SCALE_BODIES; ++index)
        {
            VoxelRigidBodyRelease(&bodies[index]);
        }
    }
    *outRun = run;
    return true;
}

static void ReleaseScene(VoxelRigidBody *bodies)
{
    for (uint32_t index = 0u; index < SCALE_BODIES; ++index)
    {
        VoxelRigidBodyRelease(&bodies[index]);
    }
}

static void ReportRun(ScaleScene scene, const ScaleRun *indexedRun, const ScaleRun *gridRun,
                      uint32_t indexBytes)
{
    WriteText("bp.scale ");
    WriteText(SceneName(scene));
    WriteText(" bodies=");
    WriteUnsigned(SCALE_BODIES);
    WriteText(" steps=");
    WriteUnsigned(SCALE_STEPS);
    WriteText(" index_bytes=");
    WriteUnsigned(indexBytes);
    WriteText(" updated_proxies=");
    WriteUnsigned(indexedRun->updatedProxies);
    WriteText(" visited_nodes=");
    WriteUnsigned(indexedRun->visitedNodes);
    WriteText(" tree_update_us=");
    WriteMicroseconds(indexedRun->broadphaseSeconds / (double)SCALE_STEPS);
    WriteText(" tree_query_us=");
    WriteMicroseconds(indexedRun->querySeconds / (double)SCALE_STEPS);
    WriteText(" tree_step_us=");
    WriteMicroseconds(indexedRun->stepSeconds / (double)SCALE_STEPS);
    WriteText(" grid_step_us=");
    WriteMicroseconds(gridRun->stepSeconds / (double)SCALE_STEPS);
    WriteText("\n");
}

LAIUE_TEST_ENTRY(RigidBroadphaseScaleTestEntryPoint)
{
    uint32_t scratchBytes = VoxelRigidBodyStepScratchBytes(SCALE_BODIES);
    uint32_t cacheBytes = VoxelRigidContactCacheBytes(SCALE_BODIES);
    uint32_t indexBytes = VoxelRigidBroadphaseBytes(SCALE_BODIES);
    Expect(scratchBytes != 0u && cacheBytes != 0u && indexBytes != 0u, "буферы описаны");

    VoxelRigidBody *bodies =
        (VoxelRigidBody *)PlatformAllocate((size_t)SCALE_BODIES * sizeof(*bodies), true);
    void *scratch = PlatformAllocate(scratchBytes, true);
    void *cacheStorage = PlatformAllocate(cacheBytes, true);
    void *indexStorage = PlatformAllocate(indexBytes, true);
    Expect(bodies != NULL && scratch != NULL && cacheStorage != NULL && indexStorage != NULL,
           "память выделена");

    VoxelRigidContactCache cache;
    VoxelRigidContactCache emptyCache = {0};
    cache = emptyCache;
    Expect(VoxelRigidContactCacheInitialize(&cache, cacheStorage, SCALE_BODIES, cacheBytes),
           "кэш импульсов готов");
    VoxelRigidBroadphase broadphase;
    VoxelRigidBroadphase emptyIndex = {0};
    broadphase = emptyIndex;
    Expect(VoxelRigidBroadphaseInitialize(&broadphase, indexStorage, SCALE_BODIES, indexBytes),
           "индекс готов");

    for (uint32_t sceneIndex = 0u; sceneIndex < (uint32_t)SCALE_SCENE_COUNT; ++sceneIndex)
    {
        ScaleScene scene = (ScaleScene)sceneIndex;
        ScaleRun indexedRun;
        ScaleRun gridRun;
        ScaleRun repeatRun;
        Expect(RunScene(scene, true, scaleNoShift, bodies, scratch, scratchBytes, &cache,
                        &broadphase, false, false, &indexedRun),
               "прогон с индексом выполнен");
        Expect(broadphase.indexedBodyCount == SCALE_BODIES, "индекс помнит длину массива тел");
        Expect(broadphase.proxyCount == SCALE_BODIES, "прокси заведены на все тела");
        Expect(RunScene(scene, false, scaleNoShift, bodies, scratch, scratchBytes, &cache, NULL,
                        false, false, &gridRun),
               "прогон с сеткой выполнен");
        // Полнота: сетка ищет соседей независимо от дерева. Пропущенный
        // деревом кандидат означает недостающий импульс и другую траекторию.
        Expect(indexedRun.hash == gridRun.hash, "индекс не теряет кандидатов");

        Expect(RunScene(scene, true, scaleNoShift, bodies, scratch, scratchBytes, &cache,
                        &broadphase, false, false, &repeatRun),
               "повторный прогон с индексом выполнен");
        Expect(repeatRun.hash == indexedRun.hash, "повторный прогон совпадает");
        // Счётчики — не статистика, а часть детерминизма: дерево обязано
        // строиться одинаково при одинаковой последовательности вызовов.
        Expect(repeatRun.updatedProxies == indexedRun.updatedProxies &&
                   repeatRun.visitedNodes == indexedRun.visitedNodes,
               "счётчики индекса воспроизводимы");
        Expect(indexedRun.updatedProxies >= SCALE_BODIES, "прокси действительно обновлялись");
        Expect(indexedRun.visitedNodes >= SCALE_BODIES, "запросы действительно обходили дерево");

        ReportRun(scene, &indexedRun, &gridRun, indexBytes);
    }

    // Rebasing: сцена, сдвинутая на целые блоки, обязана вести себя как
    // исходная. Индекс хранит локальные координаты, поэтому общий перенос
    // не должен ни терять прокси, ни менять набор кандидатов. Точное
    // сравнение делается внутри одного смещения — у разных смещений
    // округление абсолютных координат различается по последнему биту.
    ScaleRun shiftedIndexed;
    ScaleRun shiftedGrid;
    ScaleRun originRun;
    const int64_t shift[3] = {1024, -2048, 512};
    Expect(RunScene(SCALE_SCENE_MIXED, true, shift, bodies, scratch, scratchBytes, &cache,
                    &broadphase, true, false, &shiftedIndexed),
           "сдвинутая сцена с индексом");
    Expect(broadphase.proxyCount == SCALE_BODIES, "сдвиг сохранил все прокси");
    Expect(broadphase.indexedBodyCount == SCALE_BODIES, "сдвиг сохранил длину массива");
    double shiftedPositions[8][3];
    for (uint32_t sample = 0u; sample < 8u; ++sample)
    {
        Expect(
            VoxelRigidBodyLocalPosition(&bodies[(size_t)sample * 137u], shiftedPositions[sample]),
            "позиция тела сдвинутой сцены");
    }
    ReleaseScene(bodies);

    Expect(RunScene(SCALE_SCENE_MIXED, false, shift, bodies, scratch, scratchBytes, &cache, NULL,
                    false, false, &shiftedGrid),
           "сдвинутая сцена с сеткой");
    Expect(shiftedIndexed.hash == shiftedGrid.hash, "индекс не теряет кандидатов после сдвига");

    Expect(RunScene(SCALE_SCENE_MIXED, true, scaleNoShift, bodies, scratch, scratchBytes, &cache,
                    &broadphase, true, false, &originRun),
           "исходная сцена выполнена");
    for (uint32_t sample = 0u; sample < 8u; ++sample)
    {
        double origin[3];
        Expect(VoxelRigidBodyLocalPosition(&bodies[(size_t)sample * 137u], origin),
               "позиция тела исходной сцены");
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            Expect(Absolute((shiftedPositions[sample][axis] + (double)shift[axis]) - origin[axis]) <
                       1e-6,
                   "сдвинутая сцена совпала с исходной");
        }
    }
    ReleaseScene(bodies);

    // Уход и возвращение тел: лист возвращается в пул, его слот
    // переиспользуется, и номер тела в листе обязан пережить это без
    // единой потери. Сравнение с сеткой проверяет именно полноту, а не
    // только счётчик прокси.
    ScaleRun churnIndexed;
    ScaleRun churnGrid;
    Expect(RunScene(SCALE_SCENE_STATIC, true, scaleNoShift, bodies, scratch, scratchBytes, &cache,
                    &broadphase, false, true, &churnIndexed),
           "сцена с уходом тел выполнена индексом");
    Expect(broadphase.proxyCount == SCALE_BODIES, "вернувшиеся тела снова в индексе");
    uint32_t expectedMid = 0u;
    for (uint32_t index = 0u; index < SCALE_BODIES; ++index)
    {
        expectedMid += ChurnActive(index, 12u) ? 1u : 0u;
    }
    Expect(churnIndexed.midProxyCount == expectedMid,
           "во время ухода в индексе ровно оставшиеся тела");
    Expect(expectedMid < SCALE_BODIES, "расписание ухода действительно кого-то убирает");
    Expect(RunScene(SCALE_SCENE_STATIC, false, scaleNoShift, bodies, scratch, scratchBytes, &cache,
                    NULL, false, true, &churnGrid),
           "сцена с уходом тел выполнена сеткой");
    Expect(churnIndexed.hash == churnGrid.hash, "уход и возвращение тел не теряют кандидатов");

    PlatformFree(indexStorage);
    PlatformFree(cacheStorage);
    PlatformFree(scratch);
    PlatformFree(bodies);

    WriteText("Broadphase scale checks: ");
    WriteUnsigned(scaleChecks);
    WriteText("\n");
    WriteText("Broadphase scale tests passed.\n");
    LAIUE_TEST_SUCCESS();
}
