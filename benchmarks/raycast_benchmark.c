// Ручной бенчмарк горячих путей scene: DDA-луч по вокселям (VoxelRaycast) и
// покадровые преобразования камеры. В обычную сборку и в CTest не входит:
// включается опцией LAIUE_BUILD_BENCHMARKS и запускается руками.
//
// Измеряются сценарии, которые определяют стоимость выбора блока и кадра:
// пустой мир (быстрый путь WorldGetBlock без правок), мир с правками (полный
// поиск в таблице под shared lock на каждый воксель), провайдер базового слоя
// и преобразования камеры. Каждый сценарий фиксирован seed-ом-константой,
// печатает контрольную сумму результата и складывает её в volatile, чтобы
// компилятор не выбросил работу.
//
// Методика повторяет engine_benchmark: медиана нечётного числа выборок вместо
// среднего, медленные и быстрые крайние значения остаются в отчёте.

#include "platform/system.h"
#include "scene/camera.h"
#include "scene/math.h"
#include "scene/voxel_raycast.h"
#include "world/world.h"

// Харнесс без CRT общий с тестами: Windows собирает движок с
// /NODEFAULTLIB, поэтому ни printf, ни exit здесь недоступны.
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SAMPLE_COUNT   9u
#define RAY_COUNT      400u
#define CAMERA_FRAMES  200000u
#define CAMERA_ACTIONS 64u

static volatile uint64_t benchmarkSink;

// === Вывод без CRT ===

static void WriteText(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void WriteUnsigned(uint64_t value)
{
    char digits[21];
    uint32_t length = 0u;
    if (value == 0u) digits[length++] = '0';
    while (value != 0u)
    {
        digits[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    char text[22];
    for (uint32_t index = 0; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

static void WriteHex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[19];
    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0; index < 16u; ++index)
    {
        text[2u + index] = digits[(value >> (60u - 4u * index)) & 0xfu];
    }
    text[18] = '\0';
    WriteText(text);
}

// Три знака после запятой: больше не несёт смысла при разбросе выборок.
static void WriteMilliseconds(double value)
{
    if (value < 0.0) value = 0.0;
    uint64_t thousandths = (uint64_t)(value * 1000.0 + 0.5);
    WriteUnsigned(thousandths / 1000u);
    WriteText(".");
    uint64_t fraction = thousandths % 1000u;
    if (fraction < 100u) WriteText("0");
    if (fraction < 10u) WriteText("0");
    WriteUnsigned(fraction);
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Сортировка вставками: девять элементов, а qsort из CRT недоступна.
static double Median(double *samples, uint32_t count)
{
    for (uint32_t index = 1; index < count; ++index)
    {
        double value = samples[index];
        uint32_t insertion = index;
        while (insertion > 0u && CompareDouble(&samples[insertion - 1u], &value) > 0)
        {
            samples[insertion] = samples[insertion - 1u];
            --insertion;
        }
        samples[insertion] = value;
    }
    return samples[count / 2u];
}

// === Контрольная сумма результатов ===

static uint64_t HashBytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t HashInt64(uint64_t hash, int64_t value)
{
    return HashBytes(hash, &value, sizeof(value));
}

static uint64_t HashHit(uint64_t hash, bool hit, const VoxelRaycastHit *hitData)
{
    hash = HashBytes(hash, &hit, sizeof(hit));
    if (!hit)
    {
        return hash;
    }
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        hash = HashInt64(hash, hitData->block[axis]);
        hash = HashInt64(hash, hitData->previousBlock[axis]);
    }
    hash = HashBytes(hash, hitData->normal, sizeof(hitData->normal));
    hash = HashBytes(hash, &hitData->distance, sizeof(hitData->distance));
    return hash;
}

// === Луч по вокселям ===

typedef struct RayScenario
{
    const char *name;
    World *world;
    const float *direction;
    double originX;
    double originY;
    double originZ;
    float maximumDistance;
    bool expectHit;
    uint64_t operations;
} RayScenario;

static bool RunRayScenario(const RayScenario *scenario)
{
    if (scenario->world == NULL) return false;

    double samples[SAMPLE_COUNT];
    uint64_t checksum = UINT64_C(14695981039346656037);
    double origin[] = { scenario->originX, scenario->originY, scenario->originZ };
    VoxelRaycastHit hit;
    memset(&hit, 0, sizeof(hit));

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t ray = 0; ray < RAY_COUNT; ++ray)
        {
            // Небольшой детерминированный сдвиг начала: лучи не повторяют
            // ровно один и тот же путь от одной точки.
            double axisOrigin[3] = {
                origin[0] + (double)(ray % 7u) * 0.01,
                origin[1] + (double)(ray % 5u) * 0.01,
                origin[2] + (double)(ray % 3u) * 0.01,
            };
            bool hitFound = VoxelRaycast(scenario->world, axisOrigin,
                scenario->direction, scenario->maximumDistance, &hit);
            if (hitFound != scenario->expectHit)
            {
                return false;
            }
            checksum = HashHit(checksum, hitFound, &hit);
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
    }

    benchmarkSink ^= checksum;
    double median = Median(samples, SAMPLE_COUNT);
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        WriteText("SAMPLE ");
        WriteText(scenario->name);
        WriteText(" ");
        WriteMilliseconds(samples[sample]);
        WriteText("\n");
    }
    WriteText("RESULT ");
    WriteText(scenario->name);
    WriteText(" ");
    WriteMilliseconds(median);
    WriteText(" ");
    WriteMilliseconds(samples[0]);
    WriteText(" ");
    WriteMilliseconds(samples[SAMPLE_COUNT - 1u]);
    WriteText(" ");
    WriteUnsigned((uint64_t)RAY_COUNT * scenario->operations);
    WriteText(" ");
    WriteHex(checksum);
    WriteText("\n");
    return true;
}

// Провайдер базового слоя: камень ниже нуля, воздух выше. Меш не строится,
// поэтому вызывается только WorldGetBlock -> provider->getBlock.
static uint8_t TerrainSolidBelowZero(void *context, int64_t x, int64_t y, int64_t z)
{
    (void)context;
    (void)x;
    (void)z;
    return y < 0 ? 1u : 0u;
}

static bool RunRayBenchmarks(void)
{
    const float diagonal[3] = { 0.57735027f, 0.57735027f, 0.57735027f };
    const float positiveZ[3] = { 0.0f, 0.0f, 1.0f };
    const float positiveX[3] = { 1.0f, 0.0f, 0.0f };

    World *empty = WorldCreate(NULL);
    if (empty == NULL) return false;

    RayScenario scenario;
    scenario.world = empty;
    scenario.direction = diagonal;
    scenario.originX = 0.5; scenario.originY = 0.5; scenario.originZ = 0.5;
    scenario.maximumDistance = VOXEL_RAYCAST_MAX_DISTANCE;
    scenario.expectHit = false;
    scenario.operations = 0u;   // заполняется приблизительно ниже
    scenario.name = "raycast.empty_diagonal";
    // 1024 * (|dx|+|dy|+|dz|) воксельных пересечений для диагонали.
    scenario.operations = 1774u;
    if (!RunRayScenario(&scenario)) return false;

    scenario.name = "raycast.empty_axis";
    scenario.direction = positiveX;
    scenario.operations = 1024u;
    if (!RunRayScenario(&scenario)) return false;

    // Мир с одной правкой в стороне: WorldGetBlock больше не может идти
    // быстрым путём и на каждом вокселе ищет запись в таблице под lock.
    World *edited = WorldCreate(NULL);
    if (edited == NULL) return false;
    if (!WorldTrySetBlock(edited, 1000, 0, 0, (BlockType)9U)) return false;
    scenario.world = edited;
    scenario.direction = positiveZ;
    scenario.originX = 0.5; scenario.originY = 200.5; scenario.originZ = 0.5;
    scenario.expectHit = false;
    scenario.operations = 1024u;
    scenario.name = "raycast.edits_miss";
    if (!RunRayScenario(&scenario)) return false;

    // Та же таблица правок, но луч упирается в блок на полпути.
    if (!WorldTrySetBlock(edited, 512, 0, 0, (BlockType)9U)) return false;
    scenario.name = "raycast.edits_hit";
    scenario.direction = positiveX;
    scenario.originX = 0.5; scenario.originY = 0.5; scenario.originZ = 0.5;
    scenario.expectHit = true;
    scenario.operations = 512u;
    if (!RunRayScenario(&scenario)) return false;

    // Базовый слой: правок нет, но каждый воксель зовёт provider->getBlock.
    WorldBaseProvider provider;
    provider.context = NULL;
    provider.getBlock = TerrainSolidBelowZero;
    provider.fillRegion = NULL;
    provider.rebase = NULL;
    World *terrain = WorldCreate(&provider);
    if (terrain == NULL) return false;
    scenario.world = terrain;
    scenario.name = "raycast.provider";
    scenario.direction = positiveX;
    scenario.originX = 0.5; scenario.originY = 50.5; scenario.originZ = 0.5;
    scenario.expectHit = false;
    scenario.operations = 1024u;
    if (!RunRayScenario(&scenario)) return false;

    WorldDestroy(terrain);
    WorldDestroy(edited);
    WorldDestroy(empty);
    return true;
}

// === Преобразования камеры ===

static bool RunCameraBenchmark(void)
{
    Camera camera;
    CameraInit(&camera, 12.25, 70.5, -3.75, 0.3f, 0.1f);

    const float eye[3] = { 12.25f, 70.5f, -3.75f };
    float view[16];
    float projection[16];
    float viewProjection[16];
    double samples[SAMPLE_COUNT];
    uint64_t checksum = UINT64_C(14695981039346656037);

    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t frame = 0; frame < CAMERA_FRAMES; ++frame)
        {
            CameraUpdate(&camera, 0.016f,
                (frame & 1u) != 0u, (frame & 2u) != 0u,
                (frame & 4u) != 0u, (frame & 8u) != 0u, (frame & 16u) != 0u,
                (int32_t)(frame % 9u) - 4, (int32_t)(frame % 5u) - 2,
                4.5f, 0.0022f);
            for (uint32_t step = 0; step < CAMERA_ACTIONS; ++step)
            {
                CameraGetViewMatrix(&camera, eye, view);
                CameraGetProjectionMatrix(1.7777778f, 1.2f, 0.05f, 512.0f,
                    projection);
                Matrix4Multiply(view, projection, viewProjection);
            }
        }
        samples[sample] = (PlatformMonotonicSeconds() - start) * 1000.0;
        checksum = HashBytes(checksum, viewProjection, sizeof(viewProjection));
        checksum = HashBytes(checksum, &camera, sizeof(camera));
    }

    benchmarkSink ^= checksum;
    double median = Median(samples, SAMPLE_COUNT);
    for (uint32_t sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        WriteText("SAMPLE camera.frame ");
        WriteMilliseconds(samples[sample]);
        WriteText("\n");
    }
    WriteText("RESULT camera.frame ");
    WriteMilliseconds(median);
    WriteText(" ");
    WriteMilliseconds(samples[0]);
    WriteText(" ");
    WriteMilliseconds(samples[SAMPLE_COUNT - 1u]);
    WriteText(" ");
    WriteUnsigned((uint64_t)CAMERA_FRAMES * CAMERA_ACTIONS);
    WriteText(" ");
    WriteHex(checksum);
    WriteText("\n");
    return true;
}

LAIUE_TEST_ENTRY(RaycastBenchmarkEntryPoint)
{
    WriteText("laiue raycast/camera benchmark\n");
    WriteText("samples=");
    WriteUnsigned(SAMPLE_COUNT);
    WriteText(" rays=");
    WriteUnsigned(RAY_COUNT);
    WriteText(" camera_frames=");
    WriteUnsigned(CAMERA_FRAMES);
    WriteText(" camera_actions=");
    WriteUnsigned(CAMERA_ACTIONS);
    WriteText("\n");

    if (!RunRayBenchmarks())
    {
        WriteText("raycast benchmark could not run\n");
        LaiueTestRuntimeExit(1);
    }
    if (!RunCameraBenchmark())
    {
        WriteText("camera benchmark could not run\n");
        LaiueTestRuntimeExit(1);
    }

    // Ссылка на sink не даёт компилятору выбросить измеряемую работу.
    if (benchmarkSink == UINT64_MAX) WriteText("");
    LAIUE_TEST_SUCCESS();
}
