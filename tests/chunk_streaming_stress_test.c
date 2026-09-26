// Стресс-тест таблицы стриминга чанков с независимым эталоном целостности.
//
// src/scene/chunk_streaming.c держит меши чанков в открытой адресации и
// удаляет записи сдвигом кластера (backward-shift), не оставляя надгробий.
// Удаление меняет индекс записи в массиве entries, а плотный список
// отрисовки хранит именно индекс, поэтому каждое перемещение обязано чинить
// ссылку. Проверка целостности VerifyStreamingIntegrity живёт в самом
// файле под !NDEBUG и в Release выключена.
//
// Тест белый: публичного API для обхода таблицы нет, а независимый эталон
// обязан видеть настоящие ключи. Поэтому здесь продублирована раскладка
// ChunkStreaming до drawItemCount (порядок и типы полей совпадают с
// оригиналом) и построена собственная копия FindEntry. Эталон множества
// ключей считается отдельно — по геометрии куба вокруг центра с той же
// политикой гистерезиса, что и в движке.
//
// Рендер фиктивный (как в voxel_raycast_test.c): настоящий Renderer никогда
// не вызывается. Мир пуст, поэтому любой построенный чанк даёт ноль квадов
// и ChunkStreamingPump не доходит до RendererCreateMesh. Сценарий со
// списком отрисовки дополнительно подставляет в записи непрозрачные «меши»
// и держит рабочие потоки на паузе, чтобы backward-shift прошёл по живой
// записи со слотом отрисовки.
//
// Путь смены origin (ResumeAfterOriginChange) заканчивается возобновлением
// рабочих потоков. Немедленный ChunkStreamingPause после него в движке
// повисает: ResumeWorkerThreads обнуляет pausedWorkerCount, но не сбрасывает
// уже взведённый во worker-потоке флаг reportedPaused, если поток не успел
// выйти из цикла ожидания (см. отчёт). Поэтому сценарий смены origin не
// ставит потоки обратно на паузу сразу, а даёт очереди опустеть.

#include "scene/chunk_streaming.h"
#include "render/renderer.h"
#include "world/world.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef NDEBUG
// Единственная тестовая точка входа, добавленная в chunk_streaming.c под
// !NDEBUG: тест не видит static VerifyStreamingIntegrity, отдельный
// публичный заголовок для неё не заводится.
LAIUE_SCENE_API void ChunkStreamingVerifyIntegrityForTesting(
    ChunkStreaming* streaming);
// Точки входа для проверки переполнения счётчика ревизий. Тоже только под
// !NDEBUG и тоже без публичного заголовка.
LAIUE_SCENE_API void ChunkStreamingSetNextRevisionForTesting(
    ChunkStreaming* streaming, uint64_t value);
LAIUE_SCENE_API uint64_t ChunkStreamingGetEntryRevisionForTesting(
    ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z);
LAIUE_SCENE_API void ChunkStreamingPushEmptyResultForTesting(
    ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z, uint64_t revision);
#endif

#define STRESS_SET_CAPACITY 8192u
#define STRESS_MAX_INJECTED 8192u

static uint32_t stressStep;
static uint64_t stressSeed;
static uint32_t stressQueueCapacity;
static int32_t stressRadius;

// Фиктивный рендерер: указатель хранится стримингом, но ни один вызов
// Renderer* не происходит — мир пуст, меши не создаются, а подставленные
// «меши» снимаются до уничтожения стриминга. Диспетчер рендера читает тип
// бэкенда из начала объекта, а нули означают AUTO, поэтому даже случайный
// вызов остался бы no-op.
static uint64_t stressRendererPlaceholder;
static uint64_t stressFakeMeshes[STRESS_SET_CAPACITY];

// === Детерминированный ГПСЧ ===

static uint64_t StressNext(uint64_t* state)
{
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

static uint32_t StressBelow(uint64_t* state, uint32_t bound)
{
    return (uint32_t)((StressNext(state) >> 33) % (uint64_t)bound);
}

static int64_t StressSigned(uint64_t* state, int64_t magnitude)
{
    uint32_t span = (uint32_t)(magnitude * 2 + 1);
    return (int64_t)StressBelow(state, span) - magnitude;
}

// === Вывод сообщений без CRT ===

static void StressWriteUnsigned(uint64_t value)
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
        text[index] = digits[length - 1u - index];
    }
    text[length] = '\0';
    LaiueTestRuntimeWrite(text);
}

static void StressFail(const char* message, uint32_t line)
{
    LaiueTestRuntimeWrite("chunk streaming stress failure: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite(" [line ");
    StressWriteUnsigned((uint64_t)line);
    LaiueTestRuntimeWrite(", step ");
    StressWriteUnsigned((uint64_t)stressStep);
    LaiueTestRuntimeWrite(", seed ");
    StressWriteUnsigned(stressSeed);
    LaiueTestRuntimeWrite(", radius ");
    StressWriteUnsigned((uint64_t)stressRadius);
    LaiueTestRuntimeWrite("]\r\n");
    LaiueTestRuntimeExit(1);
}

#define EXPECT(condition, message)                                       \
    do                                                                   \
    {                                                                    \
        if (!(condition))                                                \
        {                                                                \
            StressFail((message), (uint32_t)__LINE__);                   \
        }                                                                \
    } while (0)

// === Зеркало внутренней раскладки ===
//
// Поля до drawItemCount обязаны совпадать с ChunkStreaming по порядку и
// типу: иначе чтение настоящей таблицы через этот вид сломается. Дальше
// структура не читается (там mutex, condition variable и потоки).
typedef struct StressChunkEntry
{
    int64_t x;
    int64_t y;
    int64_t z;
    RendererMesh* mesh;
    uint64_t revision;
    uint32_t drawSlotPlusOne;
    int state;
    bool requestQueued;
} StressChunkEntry;

typedef struct StressDrawItem
{
    float distanceSquared;
    uint32_t entryIndex;
} StressDrawItem;

typedef struct StressChunkStreaming
{
    World* world;
    Renderer* renderer;
    int32_t viewRadius;
    bool hasCenter;
    int64_t centerX;
    int64_t centerY;
    int64_t centerZ;
    StressChunkEntry* entries;
    StressChunkEntry* rebuildScratch;
    uint32_t capacity;
    StressDrawItem* drawItems;
    uint32_t drawItemCount;
} StressChunkStreaming;

// === Множество ключей на открытой адресации ===
//
// Хеш и пробирование повторяют WorldHashChunkCoordinate из world.h.

typedef struct StressSet
{
    uint32_t count;
    uint8_t used[STRESS_SET_CAPACITY];
    int64_t x[STRESS_SET_CAPACITY];
    int64_t y[STRESS_SET_CAPACITY];
    int64_t z[STRESS_SET_CAPACITY];
} StressSet;

static StressSet stressActual;
static StressSet stressSetA;
static StressSet stressSetB;

static int64_t stressShiftKeyX[STRESS_MAX_INJECTED];
static int64_t stressShiftKeyY[STRESS_MAX_INJECTED];
static int64_t stressShiftKeyZ[STRESS_MAX_INJECTED];
static uint32_t stressShiftIndex[STRESS_MAX_INJECTED];

static uint32_t StressSetSlot(int64_t x, int64_t y, int64_t z)
{
    return WorldHashChunkCoordinate(x, y, z) & (STRESS_SET_CAPACITY - 1u);
}

static void StressSetClear(StressSet* set)
{
    set->count = 0u;
    memset(set->used, 0, sizeof(set->used));
}

static bool StressSetContains(const StressSet* set, int64_t x, int64_t y, int64_t z)
{
    uint32_t index = StressSetSlot(x, y, z);
    for (uint32_t probe = 0u; probe < STRESS_SET_CAPACITY; ++probe)
    {
        if (!set->used[index])
        {
            return false;
        }
        if (set->x[index] == x && set->y[index] == y && set->z[index] == z)
        {
            return true;
        }
        index = (index + 1u) & (STRESS_SET_CAPACITY - 1u);
    }
    return false;
}

// true — ключ добавлен, false — уже был или множество переполнено.
static bool StressSetAdd(StressSet* set, int64_t x, int64_t y, int64_t z)
{
    uint32_t index = StressSetSlot(x, y, z);
    for (uint32_t probe = 0u; probe < STRESS_SET_CAPACITY; ++probe)
    {
        if (!set->used[index])
        {
            set->used[index] = 1u;
            set->x[index] = x;
            set->y[index] = y;
            set->z[index] = z;
            set->count++;
            return true;
        }
        if (set->x[index] == x && set->y[index] == y && set->z[index] == z)
        {
            return false;
        }
        index = (index + 1u) & (STRESS_SET_CAPACITY - 1u);
    }
    return false;
}

static bool StressInsideCube(
    int64_t x, int64_t y, int64_t z,
    int64_t centerX, int64_t centerY, int64_t centerZ, int64_t radius)
{
    int64_t deltaX = x - centerX;
    int64_t deltaY = y - centerY;
    int64_t deltaZ = z - centerZ;
    if (deltaX < 0) deltaX = -deltaX;
    if (deltaY < 0) deltaY = -deltaY;
    if (deltaZ < 0) deltaZ = -deltaZ;
    return deltaX <= radius && deltaY <= radius && deltaZ <= radius;
}

static void StressAddCube(
    StressSet* set, int64_t centerX, int64_t centerY, int64_t centerZ, int64_t radius)
{
    for (int64_t deltaZ = -radius; deltaZ <= radius; ++deltaZ)
    {
        for (int64_t deltaY = -radius; deltaY <= radius; ++deltaY)
        {
            for (int64_t deltaX = -radius; deltaX <= radius; ++deltaX)
            {
                (void)StressSetAdd(set,
                    centerX + deltaX, centerY + deltaY, centerZ + deltaZ);
            }
        }
    }
}

// Политика движка: при смене центра остаются записи в радиусе viewRadius+1
// вокруг нового центра, а куб viewRadius вокруг него заказывается заново.
// Формула одинакова для соседнего шага, телепорта и первого вызова.
static void StressShadowAdvance(
    StressSet* destination, const StressSet* previous,
    int64_t centerX, int64_t centerY, int64_t centerZ, int64_t radius)
{
    StressSetClear(destination);
    for (uint32_t index = 0u; index < STRESS_SET_CAPACITY; ++index)
    {
        if (!previous->used[index])
        {
            continue;
        }
        if (StressInsideCube(previous->x[index], previous->y[index], previous->z[index],
                centerX, centerY, centerZ, radius + 1))
        {
            (void)StressSetAdd(destination,
                previous->x[index], previous->y[index], previous->z[index]);
        }
    }
    StressAddCube(destination, centerX, centerY, centerZ, radius);
}

static uint32_t StressQueueCapacityFor(int32_t radius)
{
    uint32_t diameter = (uint32_t)(radius * 2 + 1);
    uint32_t volume = diameter * diameter * diameter;
    uint32_t capacity = 1u;
    while (capacity < volume)
    {
        capacity <<= 1u;
    }
    return capacity;
}

// === Независимая копия FindEntry ===

static int64_t StressFindIndex(
    const StressChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    const uint32_t mask = streaming->capacity - 1u;
    uint32_t index = WorldHashChunkCoordinate(x, y, z) & mask;
    for (uint32_t probe = 0u; probe < streaming->capacity; ++probe)
    {
        const StressChunkEntry* entry = &streaming->entries[index];
        if (entry->state == 0)
        {
            return -1;
        }
        if (entry->x == x && entry->y == y && entry->z == z)
        {
            return (int64_t)index;
        }
        index = (index + 1u) & mask;
    }
    return -1;
}

// === Полная проверка после каждого шага ===

static void StressVerify(ChunkStreaming* handle, const StressSet* shadow, bool paused)
{
    StressChunkStreaming* streaming = (StressChunkStreaming*)handle;

    StressSetClear(&stressActual);

    for (uint32_t index = 0u; index < streaming->capacity; ++index)
    {
        const StressChunkEntry* entry = &streaming->entries[index];
        if (entry->state == 0)
        {
            EXPECT(entry->mesh == NULL, "empty table slot kept a mesh");
            EXPECT(entry->drawSlotPlusOne == 0u, "empty table slot kept a draw slot");
            continue;
        }
        EXPECT(!StressSetContains(&stressActual, entry->x, entry->y, entry->z),
            "duplicate chunk key in table");
        (void)StressSetAdd(&stressActual, entry->x, entry->y, entry->z);
    }

    EXPECT(stressActual.count == shadow->count,
        "live chunk count differs from the expected cube");
    for (uint32_t index = 0u; index < STRESS_SET_CAPACITY; ++index)
    {
        if (!shadow->used[index])
        {
            continue;
        }
        EXPECT(StressSetContains(&stressActual,
                   shadow->x[index], shadow->y[index], shadow->z[index]),
            "expected chunk key is missing from the table");
    }

    for (uint32_t index = 0u; index < streaming->capacity; ++index)
    {
        const StressChunkEntry* entry = &streaming->entries[index];
        if (entry->state == 0)
        {
            continue;
        }
        EXPECT(StressFindIndex(streaming, entry->x, entry->y, entry->z) == (int64_t)index,
            "table entry is unreachable through probing search");
    }

    for (uint32_t slot = 0u; slot < streaming->drawItemCount; ++slot)
    {
        uint32_t entryIndex = streaming->drawItems[slot].entryIndex;
        EXPECT(entryIndex < streaming->capacity, "draw item index is out of range");
        if (entryIndex >= streaming->capacity)
        {
            continue;
        }
        const StressChunkEntry* entry = &streaming->entries[entryIndex];
        EXPECT(entry->mesh != NULL, "draw item references an entry without a mesh");
        EXPECT(entry->drawSlotPlusOne == slot + 1u,
            "draw slot does not match its position");
        EXPECT(StressFindIndex(streaming, entry->x, entry->y, entry->z)
                   == (int64_t)entryIndex,
            "draw item does not point at the table's own record");
    }

    for (uint32_t index = 0u; index < streaming->capacity; ++index)
    {
        const StressChunkEntry* entry = &streaming->entries[index];
        if (entry->mesh == NULL)
        {
            continue;
        }
        EXPECT(entry->drawSlotPlusOne >= 1u
                && entry->drawSlotPlusOne <= streaming->drawItemCount,
            "a mesh is missing from the draw list");
        if (entry->drawSlotPlusOne < 1u
            || entry->drawSlotPlusOne > streaming->drawItemCount)
        {
            continue;
        }
        EXPECT(streaming->drawItems[entry->drawSlotPlusOne - 1u].entryIndex == index,
            "draw list points at the wrong entry");
    }

    ChunkStreamingStats stats;
    ChunkStreamingGetStats(handle, &stats);
    if (paused)
    {
        EXPECT(stats.pendingResults == 0u,
            "paused streaming must have no pending results");
    }
    EXPECT(stats.pendingRequests <= stressQueueCapacity,
        "request ring exceeded its capacity");

#ifndef NDEBUG
    ChunkStreamingVerifyIntegrityForTesting(handle);
#endif
}

// === Подстановка фиктивных мешей для backward-shift ===

// Помечает «мешами» только внутренность куба (радиус viewRadius-1): при
// шаге на соседний чанк уходящая грань лежит на радиусе viewRadius+1 и её
// записи остаются без меша, поэтому RendererDestroyMesh не вызывается.
static void StressInjectInteriorMeshes(
    ChunkStreaming* handle, int64_t centerX, int64_t centerY, int64_t centerZ)
{
    StressChunkStreaming* streaming = (StressChunkStreaming*)handle;
    const int64_t interior = (int64_t)streaming->viewRadius - 1;
    uint32_t slot = 0u;

    for (uint32_t index = 0u; index < streaming->capacity; ++index)
    {
        StressChunkEntry* entry = &streaming->entries[index];
        if (entry->state == 0 || entry->mesh != NULL)
        {
            continue;
        }
        if (!StressInsideCube(entry->x, entry->y, entry->z,
                centerX, centerY, centerZ, interior))
        {
            continue;
        }
        entry->mesh = (RendererMesh*)&stressFakeMeshes[index];
        entry->state = 2; // CHUNK_ENTRY_READY
        entry->requestQueued = false;
        entry->drawSlotPlusOne = slot + 1u;
        streaming->drawItems[slot].entryIndex = index;
        streaming->drawItems[slot].distanceSquared = 0.0f;
        ++slot;
    }
    streaming->drawItemCount = slot;
}

static void StressClearInjectedMeshes(ChunkStreaming* handle)
{
    StressChunkStreaming* streaming = (StressChunkStreaming*)handle;
    for (uint32_t index = 0u; index < streaming->capacity; ++index)
    {
        streaming->entries[index].mesh = NULL;
        streaming->entries[index].drawSlotPlusOne = 0u;
    }
    streaming->drawItemCount = 0u;
}

// Пустой мир даёт нулевые меши, поэтому Pump не зовёт рендерер. Ждём, пока
// рабочие разберут очередь заявок и результатов, чтобы сценарий смены
// origin не копил незавершённую работу.
// Потолок — по времени, а не по числу прокачек: горячий цикл прокачки на
// быстром главном потоке съедал миллион итераций за доли секунды и на
// двухъядерном ARM64-раннере в Debug вытеснял рабочих раньше, чем те
// разбирали очередь. Между прокачками главный поток уступает процессор.
static void StressSettle(ChunkStreaming* handle)
{
    const double start = PlatformMonotonicSeconds();
    for (;;)
    {
        ChunkStreamingPump(handle);
        ChunkStreamingStats stats;
        ChunkStreamingGetStats(handle, &stats);
        if (stats.pendingRequests == 0u && stats.pendingResults == 0u)
        {
            break;
        }
        EXPECT(PlatformMonotonicSeconds() - start < 60.0, "streaming did not settle");
        PlatformSleepMilliseconds(0u);
    }
}

// === Сценарии ===

static void RunRandomScenario(int32_t radius, uint64_t seed, uint32_t steps)
{
    stressSeed = seed;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");
    EXPECT(ChunkStreamingPause(handle), "streaming was not paused");

    StressSet* shadow = &stressSetA;
    StressSet* scratch = &stressSetB;
    StressSetClear(shadow);
    StressSetClear(scratch);

    ChunkStreamingSetCenter(handle, 0, 0, 0);
    StressAddCube(shadow, 0, 0, 0, radius);
    StressVerify(handle, shadow, true);

    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;
    uint64_t state = seed ^ 0x9E3779B97F4A7C15ULL;

    for (uint32_t step = 0u; step < steps; ++step)
    {
        stressStep = step + 1u;
        const uint32_t choice = StressBelow(&state, 18u);

        if (choice < 6u)
        {
            int64_t nextX = centerX;
            int64_t nextY = centerY;
            int64_t nextZ = centerZ;
            const uint32_t axis = StressBelow(&state, 3u);
            const int64_t direction = StressBelow(&state, 2u) == 0u ? -1 : 1;
            if (axis == 0u) nextX += direction;
            else if (axis == 1u) nextY += direction;
            else nextZ += direction;

            StressShadowAdvance(scratch, shadow, nextX, nextY, nextZ, radius);
            StressSet* swap = shadow; shadow = scratch; scratch = swap;
            ChunkStreamingSetCenter(handle, nextX, nextY, nextZ);
            centerX = nextX; centerY = nextY; centerZ = nextZ;
        }
        else if (choice < 9u)
        {
            int64_t nextX = centerX;
            int64_t nextY = centerY;
            int64_t nextZ = centerZ;
            const uint32_t first = StressBelow(&state, 3u);
            const uint32_t second = (first + 1u + StressBelow(&state, 2u)) % 3u;
            const int64_t firstDirection = StressBelow(&state, 2u) == 0u ? -1 : 1;
            const int64_t secondDirection = StressBelow(&state, 2u) == 0u ? -1 : 1;
            int64_t* coordinates[3] = { &nextX, &nextY, &nextZ };
            *coordinates[first] += firstDirection;
            *coordinates[second] += secondDirection;

            StressShadowAdvance(scratch, shadow, nextX, nextY, nextZ, radius);
            StressSet* swap = shadow; shadow = scratch; scratch = swap;
            ChunkStreamingSetCenter(handle, nextX, nextY, nextZ);
            centerX = nextX; centerY = nextY; centerZ = nextZ;
        }
        else if (choice < 10u)
        {
            int64_t nextX = centerX + (StressBelow(&state, 2u) == 0u ? -1 : 1);
            int64_t nextY = centerY + (StressBelow(&state, 2u) == 0u ? -1 : 1);
            int64_t nextZ = centerZ + (StressBelow(&state, 2u) == 0u ? -1 : 1);

            StressShadowAdvance(scratch, shadow, nextX, nextY, nextZ, radius);
            StressSet* swap = shadow; shadow = scratch; scratch = swap;
            ChunkStreamingSetCenter(handle, nextX, nextY, nextZ);
            centerX = nextX; centerY = nextY; centerZ = nextZ;
        }
        else if (choice < 12u)
        {
            int64_t deltaX = StressSigned(&state, 3);
            int64_t deltaY = StressSigned(&state, 3);
            int64_t deltaZ = StressSigned(&state, 3);
            if (deltaX > -2 && deltaX < 2 && deltaY > -2 && deltaY < 2
                && deltaZ > -2 && deltaZ < 2)
            {
                deltaX = 3;
            }
            const int64_t nextX = centerX + deltaX;
            const int64_t nextY = centerY + deltaY;
            const int64_t nextZ = centerZ + deltaZ;

            StressShadowAdvance(scratch, shadow, nextX, nextY, nextZ, radius);
            StressSet* swap = shadow; shadow = scratch; scratch = swap;
            ChunkStreamingSetCenter(handle, nextX, nextY, nextZ);
            centerX = nextX; centerY = nextY; centerZ = nextZ;
        }
        else if (choice < 14u)
        {
            const int64_t offsetX = StressSigned(&state, radius);
            const int64_t offsetY = StressSigned(&state, radius);
            const int64_t offsetZ = StressSigned(&state, radius);
            const int64_t localX = (int64_t)StressBelow(&state, (uint32_t)CHUNK_SIZE);
            const int64_t localY = (int64_t)StressBelow(&state, (uint32_t)CHUNK_SIZE);
            const int64_t localZ = (int64_t)StressBelow(&state, (uint32_t)CHUNK_SIZE);
            ChunkStreamingInvalidateBlock(handle,
                (centerX + offsetX) * CHUNK_SIZE + localX,
                (centerY + offsetY) * CHUNK_SIZE + localY,
                (centerZ + offsetZ) * CHUNK_SIZE + localZ);
        }
        else if (choice < 15u)
        {
            const int64_t offsetX = 100 + StressSigned(&state, 8);
            const int64_t offsetY = StressSigned(&state, 8);
            const int64_t offsetZ = StressSigned(&state, 8);
            ChunkStreamingInvalidateBlock(handle,
                (centerX + offsetX) * CHUNK_SIZE,
                (centerY + offsetY) * CHUNK_SIZE,
                (centerZ + offsetZ) * CHUNK_SIZE);
        }
        else if (choice < 17u)
        {
            EXPECT(ChunkStreamingPause(handle), "streaming did not pause");
        }
        else
        {
            ChunkStreamingPump(handle);
        }

        StressVerify(handle, shadow, true);
    }

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

// Блуждание по замкнутому кольцу: центр возвращается в исходную точку
// каждые восемь шагов. Если удаление сдвигом не чистит таблицу, живое
// множество ключей разойдётся с эталоном, и тест это поймает.
static const int64_t stressRing[8][3] = {
    { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { -1, 1, 0 },
    { -1, 0, 0 }, { -1, -1, 0 }, { 0, -1, 0 }, { 1, -1, 0 },
};

static void RunCircleWalkScenario(int32_t radius, uint32_t steps)
{
    stressSeed = 0xC1AC1E01ULL;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");
    EXPECT(ChunkStreamingPause(handle), "streaming was not paused");

    StressSet* shadow = &stressSetA;
    StressSet* scratch = &stressSetB;
    StressSetClear(shadow);
    StressSetClear(scratch);
    ChunkStreamingSetCenter(handle, 0, 0, 0);
    StressAddCube(shadow, 0, 0, 0, radius);

    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;

    for (uint32_t step = 0u; step < steps; ++step)
    {
        stressStep = step + 1u;
        const int64_t* offset = stressRing[step % 8u];
        centerX += offset[0];
        centerY += offset[1];
        centerZ += offset[2];

        StressShadowAdvance(scratch, shadow, centerX, centerY, centerZ, radius);
        StressSet* swap = shadow; shadow = scratch; scratch = swap;
        ChunkStreamingSetCenter(handle, centerX, centerY, centerZ);
        StressVerify(handle, shadow, true);
    }

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

// Сценарий списка отрисовки: перед каждым шагом внутренность куба
// помечается фиктивными мешами, затем центр уходит на соседний чанк.
// Уходящая грань остаётся без мешей, а backward-shift обязан перенести
// живые записи со слотами отрисовки и починить их индексы. Проверка
// ловит отсутствие этой починки в Release, где встроенная проверка спит.
static void RunDrawListShiftScenario(int32_t radius, uint32_t steps)
{
    stressSeed = 0x51F7ED00ULL;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");
    EXPECT(ChunkStreamingPause(handle), "streaming was not paused");

    StressChunkStreaming* streaming = (StressChunkStreaming*)handle;
    StressSet* shadow = &stressSetA;
    StressSet* scratch = &stressSetB;
    StressSetClear(shadow);
    StressSetClear(scratch);

    ChunkStreamingSetCenter(handle, 0, 0, 0);
    StressAddCube(shadow, 0, 0, 0, radius);

    int64_t centerX = 0;
    bool sawShift = false;

    for (uint32_t step = 0u; step < steps; ++step)
    {
        stressStep = step + 1u;

        StressInjectInteriorMeshes(handle, centerX, 0, 0);

        uint32_t snapshotCount = 0u;
        for (uint32_t index = 0u; index < streaming->capacity; ++index)
        {
            const StressChunkEntry* entry = &streaming->entries[index];
            if (entry->mesh == NULL)
            {
                continue;
            }
            EXPECT(snapshotCount < STRESS_MAX_INJECTED, "too many injected meshes");
            stressShiftKeyX[snapshotCount] = entry->x;
            stressShiftKeyY[snapshotCount] = entry->y;
            stressShiftKeyZ[snapshotCount] = entry->z;
            stressShiftIndex[snapshotCount] = index;
            ++snapshotCount;
        }

        const int64_t nextX = centerX + 1;
        StressShadowAdvance(scratch, shadow, nextX, 0, 0, radius);
        StressSet* swap = shadow; shadow = scratch; scratch = swap;
        ChunkStreamingSetCenter(handle, nextX, 0, 0);
        centerX = nextX;

        // Проверяем до снятия фиктивных мешей: список отрисовки непуст.
        StressVerify(handle, shadow, true);

        for (uint32_t index = 0u; index < snapshotCount; ++index)
        {
            int64_t moved = StressFindIndex(streaming,
                stressShiftKeyX[index], stressShiftKeyY[index], stressShiftKeyZ[index]);
            if (moved >= 0 && moved != (int64_t)stressShiftIndex[index])
            {
                sawShift = true;
            }
        }

        StressClearInjectedMeshes(handle);
    }

    EXPECT(sawShift, "the scenario never moved a meshed entry");

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

// Смена origin: ResumeAfterOriginChange возобновляет рабочие потоки, и
// ставить их на паузу сразу после этого нельзя (см. заголовок файла), поэтому
// сценарий идёт с работающими потоками, а очередь разбирается StressSettle.
// Мешей нет, значит таблицу меняет только поток теста.
static void RunOriginChangeScenario(int32_t radius, uint32_t iterations)
{
    stressSeed = 0x0F1C0DE0ULL;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");

    StressSet* shadow = &stressSetA;
    StressSet* scratch = &stressSetB;
    StressSetClear(shadow);
    StressSetClear(scratch);

    ChunkStreamingSetCenter(handle, 0, 0, 0);
    StressAddCube(shadow, 0, 0, 0, radius);
    StressSettle(handle);
    StressVerify(handle, shadow, false);

    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;
    uint64_t state = 0x00DDF00DULL;

    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
    {
        stressStep = iteration + 1u;
        const bool originDeltaFits = StressBelow(&state, 2u) == 0u;
        const int64_t originDeltaX = StressSigned(&state, 5);
        const int64_t originDeltaY = StressSigned(&state, 5);
        const int64_t originDeltaZ = StressSigned(&state, 5);
        const int64_t nextX = centerX + StressSigned(&state, 4);
        const int64_t nextY = centerY + StressSigned(&state, 4);
        const int64_t nextZ = centerZ + StressSigned(&state, 4);

        // Мешей нет, поэтому смена origin собирает таблицу заново из куба.
        StressSetClear(shadow);
        StressAddCube(shadow, nextX, nextY, nextZ, radius);
        (void)ChunkStreamingResumeAfterOriginChange(handle, originDeltaFits,
            originDeltaX, originDeltaY, originDeltaZ, nextX, nextY, nextZ);
        centerX = nextX; centerY = nextY; centerZ = nextZ;

        StressSettle(handle);
        StressVerify(handle, shadow, false);
    }

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

// Пауза сразу после возобновления при пустой очереди. Раньше рабочий поток
// отчитывался о паузе флагом, который сбрасывался только после выхода из
// ожидания; при пустой очереди он из ожидания не выходил, второй Pause
// его не досчитывался и ждал вечно. Зависание здесь ловится таймаутом
// CTest, поэтому сценарий короткий и повторяется несколько раз подряд.
static void RunPauseAfterResumeScenario(int32_t radius, uint32_t repeats)
{
    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");

    EXPECT(ChunkStreamingPause(handle), "first pause did not complete");
    ChunkStreamingSetCenter(handle, 0, 0, 0);
    EXPECT(ChunkStreamingPause(handle), "pause with a queued cube did not complete");

    for (uint32_t repeat = 0u; repeat < repeats; ++repeat)
    {
        // Смена origin возобновляет рабочих; заказов при том же центре
        // нет, поэтому очередь остаётся пустой и потоки не выходят из
        // ожидания. Следующий Pause обязан завершиться.
        (void)ChunkStreamingResumeAfterOriginChange(handle, true, 0, 0, 0, 0, 0, 0);
        EXPECT(ChunkStreamingPause(handle), "pause right after resume did not complete");
    }

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

// Центр двигается, пока рабочие потоки строят и складывают результаты в
// очередь. Главный поток разбирает их в Pump и параллельно инвалидирует
// блоки. Здесь проверяется протокол очереди и номера эпохи под настоящей
// многопоточностью: заявка, уехавшая рабочему, может завершиться уже после
// того, как запись сменила ревизию или была сдвинута EraseEntry. Эталон
// множества ключей считается по-прежнему только по политике центра.
static void RunConcurrentCenterScenario(int32_t radius, uint64_t seed, uint32_t moves)
{
    stressSeed = seed;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");
    // Потоки намеренно не останавливаются: протокол очереди проверяется в
    // рабочем режиме.

    StressSet* shadow = &stressSetA;
    StressSet* next = &stressSetB;
    StressSetClear(shadow);
    StressSetClear(next);

    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;
    ChunkStreamingSetCenter(handle, centerX, centerY, centerZ);
    StressAddCube(shadow, centerX, centerY, centerZ, radius);

    uint64_t state = seed;
    for (uint32_t move = 0u; move < moves; ++move)
    {
        stressStep = move + 1u;
        centerX += StressSigned(&state, 2);
        centerY += StressSigned(&state, 2);
        centerZ += StressSigned(&state, 2);
        ChunkStreamingSetCenter(handle, centerX, centerY, centerZ);
        StressShadowAdvance(next, shadow, centerX, centerY, centerZ, radius);
        StressSet* swap = shadow;
        shadow = next;
        next = swap;

        for (uint32_t invalid = 0u; invalid < 3u; ++invalid)
        {
            int64_t blockX = centerX * CHUNK_SIZE + StressSigned(&state, CHUNK_SIZE + 1);
            int64_t blockY = centerY * CHUNK_SIZE + StressSigned(&state, CHUNK_SIZE + 1);
            int64_t blockZ = centerZ * CHUNK_SIZE + StressSigned(&state, CHUNK_SIZE + 1);
            ChunkStreamingInvalidateBlock(handle, blockX, blockY, blockZ);
        }
        ChunkStreamingPump(handle);
        ChunkStreamingPump(handle);
    }

    StressSettle(handle);
    StressVerify(handle, shadow, false);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}

#ifndef NDEBUG
// Переполнение счётчика ревизий. Счётчик подводится к 64-битной границе, и
// проверяется, что переход через неё не путает устаревшие и актуальные
// результаты: ревизия до переполнения и после обязана различаться, ноль
// зарезервирован и не выдаётся, а синтетический результат со старой ревизией
// отбрасывается, тогда как результат с текущей — принимается. Работает
// только в Debug: тестовые лазейки в Release не компилируются.
static void RunRevisionOverflowScenario(int32_t radius)
{
    stressSeed = 0xA11CE0FFULL;
    stressStep = 0u;
    stressRadius = radius;
    stressQueueCapacity = StressQueueCapacityFor(radius);

    World* world = WorldCreate(NULL);
    EXPECT(world != NULL, "world was not created");
    ChunkStreaming* handle = ChunkStreamingCreate(
        world, (Renderer*)&stressRendererPlaceholder, radius);
    EXPECT(handle != NULL, "streaming was not created");
    EXPECT(ChunkStreamingPause(handle), "streaming was not paused");

    ChunkStreamingSetCenter(handle, 0, 0, 0);
    EXPECT(ChunkStreamingPause(handle), "streaming was not paused");

    StressChunkStreaming* streaming = (StressChunkStreaming*)handle;
    const int64_t chunkX = 0;
    const int64_t chunkY = 0;
    const int64_t chunkZ = 0;

    // Блок внутри чанка: инвалидация трогает ровно одну запись.
    const int64_t blockX = 1;
    const int64_t blockY = 1;
    const int64_t blockZ = 1;

    // Одно присвоение ещё даёт UINT64_MAX, следующее переполняется в ноль,
    // который обязан быть пропущен.
    ChunkStreamingSetNextRevisionForTesting(handle, UINT64_MAX - 1u);
    ChunkStreamingInvalidateBlock(handle, blockX, blockY, blockZ);
    const uint64_t beforeWrap =
        ChunkStreamingGetEntryRevisionForTesting(handle, chunkX, chunkY, chunkZ);
    EXPECT(beforeWrap == UINT64_MAX, "revision before wrap was not the maximum");

    ChunkStreamingInvalidateBlock(handle, blockX, blockY, blockZ);
    const uint64_t afterWrap =
        ChunkStreamingGetEntryRevisionForTesting(handle, chunkX, chunkY, chunkZ);
    EXPECT(afterWrap != 0u, "revision after wrap reused the reserved zero");
    EXPECT(afterWrap == 1u, "revision after wrap must be one");
    EXPECT(afterWrap != beforeWrap, "wrap reused the previous revision");

    const int64_t entryIndex = StressFindIndex(streaming, chunkX, chunkY, chunkZ);
    EXPECT(entryIndex >= 0, "chunk entry disappeared from the table");
    EXPECT(streaming->entries[entryIndex].state == 1,
        "entry must be pending after invalidation");

    // Устаревший результат с ревизией до переполнения не должен приниматься
    // записью, которой после переполнения выдана новая ревизия.
    ChunkStreamingStats statsBefore;
    ChunkStreamingGetStats(handle, &statsBefore);

    ChunkStreamingPushEmptyResultForTesting(
        handle, chunkX, chunkY, chunkZ, beforeWrap);
    ChunkStreamingPump(handle);

    ChunkStreamingStats statsAfterStale;
    ChunkStreamingGetStats(handle, &statsAfterStale);
    EXPECT(statsAfterStale.discardedBuilds == statsBefore.discardedBuilds + 1u,
        "stale pre-wrap result was not discarded");
    EXPECT(streaming->entries[entryIndex].state == 1,
        "stale pre-wrap result was accepted as ready");

    // Актуальный результат с текущей ревизией принимается.
    ChunkStreamingPushEmptyResultForTesting(
        handle, chunkX, chunkY, chunkZ, afterWrap);
    ChunkStreamingPump(handle);
    EXPECT(streaming->entries[entryIndex].state == 2,
        "current post-wrap result was not accepted");

    StressClearInjectedMeshes(handle);
    ChunkStreamingDestroy(handle);
    WorldDestroy(world);
}
#endif

LAIUE_TEST_ENTRY(ChunkStreamingStressTestEntryPoint)
{
    RunConcurrentCenterScenario(2, 0x0C0FFEE0ULL, 400u);
    RunConcurrentCenterScenario(3, 0x0FFFFFFFFULL, 150u);
    RunRandomScenario(2, 0x1111111122222222ULL, 2000u);
    RunRandomScenario(3, 0x3333333344444444ULL, 1500u);
    RunRandomScenario(4, 0x5555555566666666ULL, 800u);
    RunCircleWalkScenario(2, 20000u);
    RunDrawListShiftScenario(3, 256u);
    RunOriginChangeScenario(2, 16u);
    RunPauseAfterResumeScenario(2, 8u);
#ifndef NDEBUG
    RunRevisionOverflowScenario(2);
#endif

    LaiueTestRuntimeWrite("Chunk streaming stress tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
