#include "voxel_render/chunk_streaming.h"
#include "scene/math_service.h"
#include "world/world.h"
#include "world/world_service.h"
#include "render/renderer.h"
#include "render/graphics_service.h"
#include "mesh/chunk_mesher.h"
#include "mesh/mesher_service.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>


#define MAX_WORKER_THREADS 4

// Бюджет загрузок на GPU за один кадр. Ограничивается не число мешей, а то,
// что реально стоит: байты, записанные в кольцо загрузки рендера (4 МиБ на
// кадр), и время процессора на разбор очереди. Число мешей выводится из
// этого само: меш чанка — порядка 100 КБ, то есть байтовый предел держит
// кадр в районе нескольких десятков загрузок, а не четырёх.
//
// Прежний предел MESH_UPLOADS_PER_FRAME = 4 появился, когда загрузка меша
// была дорогой. После кольца загрузки и крупной арены рендера она стала
// дешёвой, и счётчик мешей остался единственным ограничителем: при радиусе
// 12 полный куб — 625 непустых мешей, то есть 157 кадров пустоты.
#define CHUNK_UPLOAD_BYTES_PER_FRAME (4u * 1024u * 1024u)
#define MESH_UPLOAD_BUDGET_MILLISECONDS 2.0
#define CHUNK_MESH_BUILD_FAILED UINT32_MAX

/* Compatibility defaults used only by the legacy setter/create entry points.
 * Every new ChunkStreaming instance snapshots its own validated bindings. */
static const LaiueSceneMathServiceV1* legacySceneMathService;
static const LaiueWorldServiceV1* legacyWorldService;
static const LaiueMesherServiceV1* legacyMesherService;
static const LaiueGraphicsServiceV1* legacyGraphicsService;

static bool ServiceFieldPresent(uint32_t structSize, size_t offset, size_t size)
{
    return (size_t)structSize >= offset && (size_t)structSize - offset >= size;
}

static bool SceneMathServiceUsable(const LaiueSceneMathServiceV1* service)
{
    return service != NULL &&
           service->abiVersion == LAIUE_SCENE_MATH_SERVICE_ABI_VERSION_1 &&
           ServiceFieldPresent(service->structSize,
                               offsetof(LaiueSceneMathServiceV1,
                                        matrix4ExtractFrustumPlanes),
                               sizeof(service->matrix4ExtractFrustumPlanes));
}

static bool WorldServiceUsable(const LaiueWorldServiceV1* service)
{
    return service != NULL &&
           service->abiVersion == LAIUE_WORLD_SERVICE_ABI_VERSION_1 &&
           ServiceFieldPresent(service->structSize,
                               offsetof(LaiueWorldServiceV1, fillRegion),
                               sizeof(service->fillRegion));
}

static bool MesherServiceUsable(const LaiueMesherServiceV1* service)
{
    return service != NULL &&
           service->abiVersion == LAIUE_MESHER_SERVICE_ABI_VERSION_1 &&
           ServiceFieldPresent(service->structSize,
                               offsetof(LaiueMesherServiceV1, buildChunkMesh),
                               sizeof(service->buildChunkMesh));
}

static bool GraphicsServiceUsable(const LaiueGraphicsServiceV1* service)
{
    return service != NULL &&
           service->abiVersion == LAIUE_GRAPHICS_SERVICE_ABI_VERSION_1 &&
           ServiceFieldPresent(service->structSize,
                               offsetof(LaiueGraphicsServiceV1, drawMesh),
                               sizeof(service->drawMesh));
}

void ChunkStreamingSetSceneMathService(const LaiueSceneMathServiceV1* service)
{
    legacySceneMathService = SceneMathServiceUsable(service) ? service : NULL;
}

void ChunkStreamingSetWorldService(const LaiueWorldServiceV1* service)
{
    legacyWorldService = WorldServiceUsable(service) ? service : NULL;
}

void ChunkStreamingSetMesherService(const LaiueMesherServiceV1* service)
{
    legacyMesherService = MesherServiceUsable(service) ? service : NULL;
}

void ChunkStreamingSetGraphicsService(const LaiueGraphicsServiceV1* service)
{
    legacyGraphicsService = GraphicsServiceUsable(service) ? service : NULL;
}

typedef struct ChunkStreamingServiceBindings
{
    const LaiueSceneMathServiceV1* sceneMath;
    const LaiueWorldServiceV1* world;
    const LaiueMesherServiceV1* mesher;
    const LaiueGraphicsServiceV1* graphics;
} ChunkStreamingServiceBindings;

struct ChunkStreaming;

static WorldRegionContents VoxelRenderWorldFillRegion(
    const ChunkStreamingServiceBindings* services, World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ, BlockType* outBlocks)
{
    return services != NULL && services->world != NULL &&
           services->world->fillRegion != NULL && world != NULL
               ? services->world->fillRegion(world, minBlockX, minBlockY, minBlockZ,
                                          sizeX, sizeY, sizeZ, outBlocks)
               : WORLD_REGION_ALL_AIR;
}

static ChunkMesherScratch* VoxelRenderMesherScratchCreate(
    const ChunkStreamingServiceBindings* services)
{
    return services != NULL && services->mesher != NULL &&
           services->mesher->scratchCreate != NULL
               ? services->mesher->scratchCreate()
               : NULL;
}

static void VoxelRenderMesherScratchDestroy(
    const ChunkStreamingServiceBindings* services, ChunkMesherScratch* scratch)
{
    if (services != NULL && services->mesher != NULL &&
        services->mesher->scratchDestroy != NULL)
        services->mesher->scratchDestroy(scratch);
}

static bool VoxelRenderBuildChunkMesh(const ChunkStreamingServiceBindings* services,
    const ChunkMesherWorldSource* source,
    ChunkMesherScratch* scratch, int64_t chunkX, int64_t chunkY, int64_t chunkZ,
    ChunkQuad** outQuads, uint32_t* outQuadCount)
{
    return services != NULL && services->mesher != NULL &&
           services->mesher->buildChunkMesh != NULL &&
           services->mesher->buildChunkMesh(source, scratch, chunkX, chunkY, chunkZ,
                                         outQuads, outQuadCount);
}

static RendererMesh* VoxelRenderCreateMesh(const ChunkStreamingServiceBindings* services,
    Renderer* renderer,
    const ChunkQuad* quads, uint32_t quadCount)
{
    return services != NULL && services->graphics != NULL &&
           services->graphics->createMesh != NULL
               ? services->graphics->createMesh(renderer, quads, quadCount)
               : NULL;
}

static void VoxelRenderDestroyMesh(const ChunkStreamingServiceBindings* services,
    Renderer* renderer, RendererMesh* mesh)
{
    if (services != NULL && services->graphics != NULL &&
        services->graphics->destroyMesh != NULL)
        services->graphics->destroyMesh(renderer, mesh);
}

static void VoxelRenderDrawMesh(const ChunkStreamingServiceBindings* services,
    Renderer* renderer, const RendererMesh* mesh,
    const float chunkOriginRelative[3])
{
    if (services != NULL && services->graphics != NULL &&
        services->graphics->drawMesh != NULL)
        services->graphics->drawMesh(renderer, mesh, chunkOriginRelative);
}

/* The worker source is short-lived and carries both the stream instance and
 * its world.  This avoids a global callback context while preserving the
 * mesher's provider-only contract. */
typedef struct ChunkMesherWorldContext
{
    const ChunkStreamingServiceBindings* services;
    World* world;
} ChunkMesherWorldContext;

// Мешер принимает только абстрактный region provider. Voxel-render связывает
// его с выбранным world здесь, на границе технологии; сам mesher поэтому не
// импортирует world DLL.
static WorldRegionContents FillMesherRegionFromWorld(void* context,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ, BlockType* outBlocks)
{
    ChunkMesherWorldContext* worldContext = context;
    return VoxelRenderWorldFillRegion(worldContext != NULL ?
            worldContext->services : NULL,
        worldContext != NULL ? worldContext->world : NULL,
        minBlockX, minBlockY, minBlockZ, sizeX, sizeY, sizeZ, outBlocks);
}

static int64_t ChunkCoordinateFromBlock(int64_t block)
{
    int64_t chunk = block / CHUNK_SIZE;
    return block % CHUNK_SIZE < 0 ? chunk - 1 : chunk;
}

typedef enum ChunkEntryState
{
    CHUNK_ENTRY_EMPTY = 0,
    CHUNK_ENTRY_PENDING,
    CHUNK_ENTRY_READY,
} ChunkEntryState;

// PENDING не гасит отрисовку: mesh (если есть) — последняя готовая
// геометрия, она рисуется, пока рабочий поток строит замену.
typedef struct ChunkEntry
{
    int64_t x;
    int64_t y;
    int64_t z;
    RendererMesh* mesh;
    uint64_t revision;      // растёт при инвалидации: устаревшие результаты отбрасываются
    uint32_t drawSlotPlusOne; // 0 — меша нет, иначе позиция в плотном drawItems + 1
    ChunkEntryState state;
    bool requestQueued;     // есть ли в очереди заявка текущей ревизии
} ChunkEntry;

typedef struct ChunkRequest
{
    int64_t x;
    int64_t y;
    int64_t z;
    uint64_t revision;
    uint32_t centerEpoch;
} ChunkRequest;

typedef struct ChunkMeshResult
{
    int64_t x;
    int64_t y;
    int64_t z;
    ChunkQuad* quads;
    uint64_t revision;
    uint32_t quadCount;
} ChunkMeshResult;

typedef struct DrawItem
{
    float distanceSquared;
    uint32_t entryIndex;
} DrawItem;

struct ChunkStreaming
{
    World* world;
    Renderer* renderer;
    int32_t viewRadius;

    bool hasCenter;
    int64_t centerX;
    int64_t centerY;
    int64_t centerZ;

    // Кеш мешей: открытая адресация, таблица принадлежит главному потоку.
    ChunkEntry* entries;
    // Компактная запасная арена для пересборки таблицы после смены origin.
    // Origin меняет абсолютные координаты живущих чанков, а с ними и хеш
    // ключей, поэтому там таблицу действительно приходится строить заново.
    // Живущих в гистерезисном кубе радиуса + 1 не больше (2R+3)^3 записей,
    // поэтому арена ровно этого размера — а не второй таблицы на всю
    // ёмкость, которая была чистой памятью впустую на весь сеанс игры.
    ChunkEntry* rebuildScratch;
    uint32_t capacity;

    // Плотный список записей с мешами. Он же хранит кешированный порядок
    // от ближних к дальним: полную hash-таблицу Draw не обходит.
    DrawItem* drawItems;
    uint32_t drawItemCount;
    ChunkStreamingServiceBindings services;
    int64_t drawRenderOriginBlock[3];
    bool hasDrawRenderOrigin;
    bool drawOrderDirty;

    // true тогда и только тогда, когда возможна PENDING-запись без заявки.
    // Полный retry-скан таблицы выполняется лишь в таком случае.
    bool hasUnqueuedPending;

    // Кольцевые очереди под общим замком. Гарантия отсутствия потерь:
    // unfinishedWork (заявки + в работе + результаты) не превышает
    // ёмкость, поэтому очередь результатов переполниться не может.
    ChunkRequest* requests;
    uint32_t requestHead;
    uint32_t requestCount;
    ChunkMeshResult* results;
    uint32_t resultHead;
    uint32_t resultCount;
    uint32_t queueCapacity;
    uint32_t unfinishedWork;

    // Готовый результат, который рендер не принял в прошлом кадре (занята
    // его очередь загрузок): хранится до следующего кадра, чтобы не строить
    // чанк заново. Одновременно придерживается не более одного — разбор
    // очереди на нём останавливается.
    ChunkMeshResult heldResult;
    bool hasHeldResult;

    PlatformMutex queueLock;
    PlatformConditionVariable workAvailable;
    PlatformThread workerThreads[MAX_WORKER_THREADS];
    uint32_t workerThreadCount;
    uint32_t desiredWorkerThreadCount;
    uint32_t pausedWorkerCount;
    // Номер текущей паузы. Рабочий поток отчитывается о паузе один раз на
    // номер, а не один раз на «пока не вышел из ожидания»: иначе Pause сразу
    // после Resume при пустой очереди ждал бы отчёта вечно.
    uint32_t pauseGeneration;
    // Источник ревизий записей. Ревизия обязана быть уникальной во времени,
    // а не только внутри записи: запись, вытесненная из куба и заказанная
    // снова, раньше начинала с нуля, и результат прежней сборки той же
    // клетки мог совпасть с новой заявкой по координатам и ревизии, а
    // значит, быть принят уже после правки блока в промежутке.
    //
    // Счётчик 64-битный. 32-битного хватало бы на ~4,29 млрд присвоений, а
    // он растёт на каждую вставку записи и каждую инвалидацию (в том числе
    // когда кольцо заявок уже полно и заявка не ставится), поэтому обернуться
    // за время жизни процесса он мог бы. После оборота новая заявка получила
    // бы уже использованную ревизию, и устаревший результат, всё ещё висящий
    // в кольце, был бы принят как актуальный. 2^64 присвоений недостижимы.
    // Ноль зарезервирован как «ревизии нет» и пропускается, как и раньше.
    uint64_t nextRevision;
    bool pauseRequested;
    bool shutdownRequested;

    volatile int64_t queuedRequests;
    volatile int64_t completedBuilds;
    volatile int64_t cancelledBuilds;
    volatile int64_t discardedBuilds;
    volatile int64_t uploadedMeshes;
    // Время построения копится в микросекундах: монотонные часы платформы
    // выдают секунды с плавающей точкой, а счётчик обязан быть целым,
    // чтобы складываться атомарно из нескольких потоков.
    volatile int64_t totalBuildMicroseconds;
    uint32_t peakUnfinishedWork;
    volatile uint32_t centerEpoch;
};

// Выдаёт следующую ревизию записи. Ноль зарезервирован как «ревизии нет»:
// если 64-битный счётчик когда-нибудь дойдёт до переполнения, ноль
// пропускается, чтобы никакая запись и никакая заявка не получили его.
static uint64_t NextChunkRevision(ChunkStreaming* streaming)
{
    if (++streaming->nextRevision == 0u)
    {
        ++streaming->nextRevision;
    }
    return streaming->nextRevision;
}

static void AddMeshToDrawList(ChunkStreaming* streaming, ChunkEntry* entry)
{
    if (entry->drawSlotPlusOne != 0)
    {
        return;
    }

    uint32_t slot = streaming->drawItemCount++;
    streaming->drawItems[slot].entryIndex = (uint32_t)(entry - streaming->entries);
    entry->drawSlotPlusOne = slot + 1u;
    streaming->drawOrderDirty = true;
}

static void RemoveMeshFromDrawList(ChunkStreaming* streaming, ChunkEntry* entry)
{
    if (entry->drawSlotPlusOne == 0)
    {
        return;
    }

    uint32_t slot = entry->drawSlotPlusOne - 1u;
    uint32_t lastSlot = --streaming->drawItemCount;
    if (slot != lastSlot)
    {
        streaming->drawItems[slot] = streaming->drawItems[lastSlot];
        ChunkEntry* moved = &streaming->entries[streaming->drawItems[slot].entryIndex];
        moved->drawSlotPlusOne = slot + 1u;
    }
    entry->drawSlotPlusOne = 0;
    streaming->drawOrderDirty = true;
}

static void ResetDrawList(ChunkStreaming* streaming)
{
    streaming->drawItemCount = 0;
    streaming->drawOrderDirty = true;
}

static void SwapDrawItems(DrawItem* left, DrawItem* right)
{
    DrawItem temporary = *left;
    *left = *right;
    *right = temporary;
}

static void SiftDrawItemsDown(DrawItem* items, uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t leftChild = root * 2u + 1u;
        if (leftChild >= count)
        {
            return;
        }

        uint32_t largest = leftChild;
        uint32_t rightChild = leftChild + 1u;
        if (rightChild < count
            && items[rightChild].distanceSquared > items[leftChild].distanceSquared)
        {
            largest = rightChild;
        }

        if (items[root].distanceSquared >= items[largest].distanceSquared)
        {
            return;
        }

        SwapDrawItems(&items[root], &items[largest]);
        root = largest;
    }
}

static void SortDrawItemsFrontToBack(DrawItem* items, uint32_t count)
{
    if (count < 2)
    {
        return;
    }

    for (uint32_t start = count / 2u; start > 0; --start)
    {
        SiftDrawItemsDown(items, start - 1u, count);
    }

    for (uint32_t end = count; end > 1; --end)
    {
        SwapDrawItems(&items[0], &items[end - 1u]);
        SiftDrawItemsDown(items, 0, end - 1u);
    }
}

static ChunkEntry* FindEntry(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    uint32_t mask = streaming->capacity - 1;
    uint32_t index = WorldHashChunkCoordinate(x, y, z) & mask;

    for (uint32_t probe = 0; probe < streaming->capacity; ++probe)
    {
        ChunkEntry* entry = &streaming->entries[index];
        if (entry->state == CHUNK_ENTRY_EMPTY)
        {
            return NULL;
        }
        if (entry->x == x && entry->y == y && entry->z == z)
        {
            return entry;
        }
        index = (index + 1) & mask;
    }

    return NULL;
}

static ChunkEntry* InsertEntry(ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    uint32_t mask = streaming->capacity - 1;
    uint32_t index = WorldHashChunkCoordinate(x, y, z) & mask;

    while (streaming->entries[index].state != CHUNK_ENTRY_EMPTY)
    {
        index = (index + 1) & mask;
    }

    ChunkEntry* entry = &streaming->entries[index];
    entry->x = x;
    entry->y = y;
    entry->z = z;
    entry->mesh = NULL;
    entry->revision = NextChunkRevision(streaming);
    entry->drawSlotPlusOne = 0;
    entry->requestQueued = false;
    return entry;
}

static bool IsInsideRadius(const ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z, int64_t radius)
{
    int64_t deltaX = x - streaming->centerX;
    int64_t deltaY = y - streaming->centerY;
    int64_t deltaZ = z - streaming->centerZ;
    if (deltaX < 0) deltaX = -deltaX;
    if (deltaY < 0) deltaY = -deltaY;
    if (deltaZ < 0) deltaZ = -deltaZ;
    return deltaX <= radius && deltaY <= radius && deltaZ <= radius;
}

// Удаляет запись из таблицы открытой адресации, сдвигая кластер назад,
// чтобы не заводить надгробий (иначе таблица со временем набивается
// мёртвыми слотами и её пришлось бы периодически пересобирать целиком).
// Сдвиг меняет индекс записи в массиве entries, поэтому у каждой
// перемещённой записи обязательно чинится ссылка в плотном списке
// отрисовки: drawItems хранит именно индекс, а не указатель.
static void EraseEntry(ChunkStreaming* streaming, ChunkEntry* entry)
{
    const uint32_t mask = streaming->capacity - 1u;
    uint32_t hole = (uint32_t)(entry - streaming->entries);

    for (uint32_t scan = hole;;)
    {
        scan = (scan + 1u) & mask;
        ChunkEntry* candidate = &streaming->entries[scan];
        if (candidate->state == CHUNK_ENTRY_EMPTY)
        {
            break;
        }

        // Кандидат переезжает в дыру только если дыра лежит на его пути
        // пробирования: прямое расстояние от home до дыры не больше, чем
        // до текущего слота. Иначе home оказался бы за дырой и FindEntry
        // его больше не нашёл бы.
        const uint32_t home =
            WorldHashChunkCoordinate(candidate->x, candidate->y, candidate->z) & mask;
        const uint32_t homeToHole = (hole - home) & mask;
        const uint32_t homeToScan = (scan - home) & mask;
        if (homeToHole <= homeToScan)
        {
            streaming->entries[hole] = *candidate;
            ChunkEntry* moved = &streaming->entries[hole];
            if (moved->drawSlotPlusOne != 0)
            {
                streaming->drawItems[moved->drawSlotPlusOne - 1u].entryIndex = hole;
            }
            hole = scan;
        }
    }

    ChunkEntry* vacated = &streaming->entries[hole];
    vacated->state = CHUNK_ENTRY_EMPTY;
    vacated->mesh = NULL;
    vacated->drawSlotPlusOne = 0;
}

#ifndef NDEBUG
static void VerifyFail(const char* message)
{
    PlatformWriteConsoleUtf8(message);
    PlatformWriteConsoleUtf8("\r\n");
    volatile int* fault = (volatile int*)0;
    *fault = 1;
}

// Проверка целостности после каждого перехода. Ловит потерянную запись
// (дыра в кластере делает последующие ключи недостижимыми), дубликат
// ключа и рассинхронизацию плотного списка отрисовки/очереди заявок с
// таблицей. Работает только в отладочной сборке: полный проход по таблице
// в релизе недопустим.
static void VerifyStreamingIntegrity(ChunkStreaming* streaming)
{
    const uint32_t mask = streaming->capacity - 1u;

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        const ChunkEntry* entry = &streaming->entries[index];
        if (entry->state == CHUNK_ENTRY_EMPTY)
        {
            continue;
        }

        uint32_t probe = WorldHashChunkCoordinate(entry->x, entry->y, entry->z) & mask;
        bool reachable = false;
        for (uint32_t step = 0; step < streaming->capacity; ++step)
        {
            if (probe == index)
            {
                reachable = true;
                break;
            }
            const ChunkEntry* other = &streaming->entries[probe];
            if (other->state == CHUNK_ENTRY_EMPTY)
            {
                break;
            }
            if (other->x == entry->x && other->y == entry->y && other->z == entry->z)
            {
                VerifyFail("chunk streaming integrity: duplicate chunk key in table");
            }
            probe = (probe + 1u) & mask;
        }
        if (!reachable)
        {
            VerifyFail("chunk streaming integrity: entry unreachable through FindEntry");
        }
    }

    for (uint32_t slot = 0; slot < streaming->drawItemCount; ++slot)
    {
        const DrawItem* item = &streaming->drawItems[slot];
        if (item->entryIndex >= streaming->capacity)
        {
            VerifyFail("chunk streaming integrity: draw item index out of range");
        }
        const ChunkEntry* entry = &streaming->entries[item->entryIndex];
        if (entry->mesh == NULL)
        {
            VerifyFail("chunk streaming integrity: draw item references an entry without mesh");
        }
        if (entry->drawSlotPlusOne != slot + 1u)
        {
            VerifyFail("chunk streaming integrity: draw slot does not match its position");
        }
        if (FindEntry(streaming, entry->x, entry->y, entry->z) != entry)
        {
            VerifyFail("chunk streaming integrity: drawn entry is not the table's own record");
        }
    }

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        const ChunkEntry* entry = &streaming->entries[index];
        if (entry->mesh == NULL)
        {
            continue;
        }
        if (entry->drawSlotPlusOne == 0 || entry->drawSlotPlusOne > streaming->drawItemCount)
        {
            VerifyFail("chunk streaming integrity: mesh is missing from the draw list");
        }
        if (streaming->drawItems[entry->drawSlotPlusOne - 1u].entryIndex != index)
        {
            VerifyFail("chunk streaming integrity: draw list points to the wrong entry");
        }
    }

    PlatformMutexLock(&streaming->queueLock);
    const uint32_t requestCount = streaming->requestCount;
    const uint32_t resultCount = streaming->resultCount;
    const uint32_t unfinishedWork = streaming->unfinishedWork;
    const uint32_t queueCapacity = streaming->queueCapacity;
    PlatformMutexUnlock(&streaming->queueLock);

    if (requestCount > queueCapacity || resultCount > queueCapacity
        || unfinishedWork > queueCapacity
        || unfinishedWork < requestCount + resultCount)
    {
        VerifyFail("chunk streaming integrity: request/result ring is inconsistent");
    }
}

// Единственная тестовая точка входа: VerifyStreamingIntegrity статична,
// а стресс-тесту в tests/ нужна работающая проверка в отладочной сборке.
// Публичный заголовок ради неё не заводится, и в Release символа нет.
LAIUE_VOXEL_RENDER_API void ChunkStreamingVerifyIntegrityForTesting(
    ChunkStreaming* streaming)
{
    VerifyStreamingIntegrity(streaming);
}

// Тестовые лазейки для проверки переполнения счётчика ревизий. Нужны, чтобы
// стресс-тест мог подвести счётчик к границе и подложить в кольцо результатов
// синтетический результат с заданной ревизией. Отдельного заголовка нет, в
// Release символов тоже нет.
LAIUE_VOXEL_RENDER_API void ChunkStreamingSetNextRevisionForTesting(
    ChunkStreaming* streaming, uint64_t value)
{
    streaming->nextRevision = value;
}

LAIUE_VOXEL_RENDER_API uint64_t ChunkStreamingGetEntryRevisionForTesting(
    ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    const ChunkEntry* entry = FindEntry(streaming, x, y, z);
    return entry != NULL ? entry->revision : 0u;
}

// Кладёт в очередь результатов пустой (ноль квадов) результат с заданной
// ревизией, как если бы его вернул рабочий поток. unfinishedWork растёт
// вместе с resultCount, чтобы инварианты кольца не нарушались: Pump затем
// уменьшит его при разборе.
LAIUE_VOXEL_RENDER_API void ChunkStreamingPushEmptyResultForTesting(
    ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z, uint64_t revision)
{
    PlatformMutexLock(&streaming->queueLock);
    const uint32_t queueMask = streaming->queueCapacity - 1u;
    ChunkMeshResult* result = &streaming->results[
        (streaming->resultHead + streaming->resultCount) & queueMask];
    result->x = x;
    result->y = y;
    result->z = z;
    result->quads = NULL;
    result->revision = revision;
    result->quadCount = 0u;
    streaming->resultCount++;
    streaming->unfinishedWork++;
    PlatformMutexUnlock(&streaming->queueLock);
}
#endif

// Ставит заявку текущей ревизии записи; false — очередь занята,
// повторная попытка произойдёт в ChunkStreamingPump.
static bool TryEnqueueRequest(ChunkStreaming* streaming, ChunkEntry* entry)
{
    bool enqueued = false;

    PlatformMutexLock(&streaming->queueLock);
    if (streaming->unfinishedWork < streaming->queueCapacity)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest* request = &streaming->requests[(streaming->requestHead + streaming->requestCount) & queueMask];
        request->x = entry->x;
        request->y = entry->y;
        request->z = entry->z;
        request->revision = entry->revision;
        request->centerEpoch = streaming->centerEpoch;
        streaming->requestCount++;
        streaming->unfinishedWork++;
        if (streaming->unfinishedWork > streaming->peakUnfinishedWork)
        {
            streaming->peakUnfinishedWork = streaming->unfinishedWork;
        }
        enqueued = true;
    }
    PlatformMutexUnlock(&streaming->queueLock);

    if (enqueued)
    {
        PlatformAtomicIncrementI64(&streaming->queuedRequests);
        PlatformConditionVariableWakeOne(&streaming->workAvailable);
    }

    entry->requestQueued = enqueued;
    if (!enqueued)
    {
        streaming->hasUnqueuedPending = true;
    }
    return enqueued;
}

// Пакетная постановка заявок для обхода куба/грани. Одиночный путь берёт
// замок и будит рабочий поток на каждую заявку; на первом заполнении куба
// это десятки тысяч пар lock/unlock и столько же пробуждений подряд.
// Пакет пишет заявки в кольцо под одним замком и будит рабочих один раз.
// Порядок заявок — порядок обхода, тот же, что и у одиночных вызовов.
//
// Инвариант: unfinishedWork меняет только главный поток (здесь и в
// ChunkStreamingPump), между пакетами он не убывает, поэтому разбиение
// потока заявок на пакеты не меняет, сколько именно заявок поместилось.
#define CHUNK_REQUEST_ENQUEUE_BATCH 64

typedef struct ChunkEnqueueBatch
{
    ChunkEntry* entries[CHUNK_REQUEST_ENQUEUE_BATCH];
    uint32_t count;
} ChunkEnqueueBatch;

static void FlushEnqueueBatch(ChunkStreaming* streaming, ChunkEnqueueBatch* batch)
{
    if (batch->count == 0u)
    {
        return;
    }

    uint32_t enqueued = 0u;
    const uint32_t queueMask = streaming->queueCapacity - 1u;
    PlatformMutexLock(&streaming->queueLock);
    // Пока requestCount > 0, рабочий поток не засыпает: из цикла ожидания он
    // выходит, видит непустую очередь и берётся за работу. Значит, будить
    // нужно ровно в переходе «очередь была пуста → стала непуста», а не на
    // каждом пакете: иначе WakeAll держит всех рабочих горячими, даже когда
    // работы ещё нет, и забивает замок.
    const bool queueWasEmpty = streaming->requestCount == 0u;
    while (enqueued < batch->count
        && streaming->unfinishedWork < streaming->queueCapacity)
    {
        ChunkEntry* entry = batch->entries[enqueued];
        ChunkRequest* request = &streaming->requests[
            (streaming->requestHead + streaming->requestCount) & queueMask];
        request->x = entry->x;
        request->y = entry->y;
        request->z = entry->z;
        request->revision = entry->revision;
        request->centerEpoch = streaming->centerEpoch;
        streaming->requestCount++;
        streaming->unfinishedWork++;
        if (streaming->unfinishedWork > streaming->peakUnfinishedWork)
        {
            streaming->peakUnfinishedWork = streaming->unfinishedWork;
        }
        entry->requestQueued = true;
        enqueued++;
    }
    if (enqueued < batch->count)
    {
        // Не поместилось — повторная попытка будет в условном сканировании
        // ChunkStreamingPump, как и у одиночного пути.
        streaming->hasUnqueuedPending = true;
        for (uint32_t index = enqueued; index < batch->count; ++index)
        {
            batch->entries[index]->requestQueued = false;
        }
    }
    PlatformMutexUnlock(&streaming->queueLock);

    if (enqueued != 0u)
    {
        PlatformAtomicAddI64(&streaming->queuedRequests, (int64_t)enqueued);
        if (queueWasEmpty)
        {
            PlatformConditionVariableWakeAll(&streaming->workAvailable);
        }
    }
    batch->count = 0u;
}

static uint32_t WorkerThreadProcedure(void* parameter)
{
    ChunkStreaming* streaming = parameter;

    ChunkMesherScratch* scratch = VoxelRenderMesherScratchCreate(&streaming->services);
    if (scratch == NULL)
    {
        return 1;
    }

    // Номер паузы, о которой этот поток уже отчитался. Ноль — ни о какой:
    // счёт пауз начинается с единицы.
    uint32_t reportedPauseGeneration = 0;

    for (;;)
    {
        PlatformMutexLock(&streaming->queueLock);
        while (!streaming->shutdownRequested
            && (streaming->pauseRequested || streaming->requestCount == 0))
        {
            if (streaming->pauseRequested
                && reportedPauseGeneration != streaming->pauseGeneration)
            {
                reportedPauseGeneration = streaming->pauseGeneration;
                streaming->pausedWorkerCount++;
                PlatformConditionVariableWakeAll(&streaming->workAvailable);
            }
            PlatformConditionVariableWait(&streaming->workAvailable, &streaming->queueLock);
        }
        if (streaming->shutdownRequested)
        {
            PlatformMutexUnlock(&streaming->queueLock);
            VoxelRenderMesherScratchDestroy(&streaming->services, scratch);
            return 0;
        }

        uint32_t queueMask = streaming->queueCapacity - 1;
        ChunkRequest request = streaming->requests[streaming->requestHead & queueMask];
        streaming->requestHead++;
        streaming->requestCount--;
        PlatformMutexUnlock(&streaming->queueLock);

        // Тяжёлая работа — без замка.
        bool cancelled =
            request.centerEpoch != PlatformAtomicLoadU32Acquire(&streaming->centerEpoch);
        double buildStart = PlatformMonotonicSeconds();
        ChunkMeshResult result = { .x = request.x, .y = request.y, .z = request.z, .revision = request.revision };
        ChunkMesherWorldContext worldContext = {
            .services = &streaming->services,
            .world = streaming->world,
        };
        ChunkMesherWorldSource source = {
            .context = &worldContext,
            .fillRegion = FillMesherRegionFromWorld,
        };
        if (cancelled || !VoxelRenderBuildChunkMesh(&streaming->services,
            &source, scratch,
            request.x, request.y, request.z, &result.quads, &result.quadCount))
        {
            result.quadCount = CHUNK_MESH_BUILD_FAILED;
        }
        double buildEnd = PlatformMonotonicSeconds();
        PlatformAtomicAddI64(&streaming->totalBuildMicroseconds,
            (int64_t)((buildEnd - buildStart) * 1000000.0));
        PlatformAtomicIncrementI64(&streaming->completedBuilds);
        if (cancelled) PlatformAtomicIncrementI64(&streaming->cancelledBuilds);

        // Очередь результатов переполниться не может: unfinishedWork
        // ограничен её ёмкостью.
        PlatformMutexLock(&streaming->queueLock);
        streaming->results[(streaming->resultHead + streaming->resultCount) & queueMask] = result;
        streaming->resultCount++;
        PlatformMutexUnlock(&streaming->queueLock);
    }
}

static bool StartWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount != 0) return true;
    streaming->shutdownRequested = false;

    for (uint32_t index = 0;
         index < streaming->desiredWorkerThreadCount; ++index)
    {
        PlatformThread thread;
        if (PlatformThreadStart(&thread, WorkerThreadProcedure, streaming))
        {
            streaming->workerThreads[streaming->workerThreadCount++] = thread;
        }
    }
    return streaming->workerThreadCount != 0;
}

static bool StopWorkerThreads(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount == 0) return true;

    PlatformMutexLock(&streaming->queueLock);
    streaming->shutdownRequested = true;
    streaming->pauseRequested = false;
    streaming->pausedWorkerCount = 0;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    PlatformMutexUnlock(&streaming->queueLock);

    for (uint32_t index = 0;
         index < streaming->workerThreadCount; ++index)
    {
        PlatformThreadJoin(&streaming->workerThreads[index]);
    }
    streaming->workerThreadCount = 0;
    streaming->shutdownRequested = false;
    return true;
}

static void ResumeWorkerThreads(ChunkStreaming* streaming)
{
    PlatformMutexLock(&streaming->queueLock);
    streaming->pausedWorkerCount = 0;
    streaming->pauseRequested = false;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    PlatformMutexUnlock(&streaming->queueLock);
}

bool ChunkStreamingPause(ChunkStreaming* streaming)
{
    if (streaming->workerThreadCount == 0) return false;

    PlatformMutexLock(&streaming->queueLock);
    // Каждая пауза — новый номер и новый счёт: рабочие, отчитавшиеся о
    // прошлой паузе и так и не вышедшие из ожидания, обязаны отчитаться
    // снова.
    streaming->pauseGeneration++;
    streaming->pausedWorkerCount = 0;
    streaming->pauseRequested = true;
    PlatformConditionVariableWakeAll(&streaming->workAvailable);
    while (streaming->pausedWorkerCount < streaming->workerThreadCount)
    {
        PlatformConditionVariableWait(&streaming->workAvailable, &streaming->queueLock);
    }

    // Все рабочие потоки стоят на condition variable и больше не читают World.
    uint32_t queueMask = streaming->queueCapacity - 1;
    for (uint32_t index = 0; index < streaming->resultCount; ++index)
    {
        ChunkMeshResult* result = &streaming->results[
            (streaming->resultHead + index) & queueMask];
        if (result->quads != NULL)
        {
            PlatformFree(result->quads);
            result->quads = NULL;
        }
    }

    if (streaming->hasHeldResult)
    {
        if (streaming->heldResult.quads != NULL)
        {
            PlatformFree(streaming->heldResult.quads);
        }
        streaming->heldResult.quads = NULL;
        streaming->hasHeldResult = false;
    }

    streaming->requestHead = 0;
    streaming->requestCount = 0;
    streaming->resultHead = 0;
    streaming->resultCount = 0;
    streaming->unfinishedWork = 0;
    streaming->hasUnqueuedPending = false;

    PlatformMutexUnlock(&streaming->queueLock);

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        ChunkEntry* entry = &streaming->entries[index];
        entry->requestQueued = false;
        if (entry->state == CHUNK_ENTRY_PENDING && entry->mesh != NULL)
        {
            entry->state = CHUNK_ENTRY_READY;
        }
        else if (entry->state == CHUNK_ENTRY_PENDING)
        {
            streaming->hasUnqueuedPending = true;
        }
    }
    return true;
}

static bool TrySubtractInt64(
    int64_t value, int64_t difference, int64_t* outValue)
{
    if (difference > 0 && value < INT64_MIN + difference) return false;
    if (difference < 0 && value > INT64_MAX + difference) return false;
    *outValue = value - difference;
    return true;
}

static void QueueChunkIfMissing(ChunkStreaming* streaming,
    int64_t x, int64_t y, int64_t z, ChunkEnqueueBatch* batch)
{
    if (FindEntry(streaming, x, y, z) != NULL) return;

    ChunkEntry* entry = InsertEntry(streaming, x, y, z);
    entry->state = CHUNK_ENTRY_PENDING;
    batch->entries[batch->count++] = entry;
    if (batch->count == CHUNK_REQUEST_ENQUEUE_BATCH)
    {
        FlushEnqueueBatch(streaming, batch);
    }
}

static void QueueMissingChunks(
    ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    ChunkEnqueueBatch batch = { .count = 0u };
    for (int64_t shell = 0; shell <= streaming->viewRadius; ++shell)
    {
        for (int64_t deltaZ = -shell; deltaZ <= shell; ++deltaZ)
        {
            for (int64_t deltaY = -shell; deltaY <= shell; ++deltaY)
            {
                for (int64_t deltaX = -shell; deltaX <= shell; ++deltaX)
                {
                    int64_t absoluteX = deltaX < 0 ? -deltaX : deltaX;
                    int64_t absoluteY = deltaY < 0 ? -deltaY : deltaY;
                    int64_t absoluteZ = deltaZ < 0 ? -deltaZ : deltaZ;
                    int64_t chebyshev = absoluteX > absoluteY ? absoluteX : absoluteY;
                    if (absoluteZ > chebyshev) chebyshev = absoluteZ;
                    if (chebyshev != shell) continue;

                    QueueChunkIfMissing(streaming,
                        chunkX + deltaX, chunkY + deltaY, chunkZ + deltaZ, &batch);
                }
            }
        }
    }
    FlushEnqueueBatch(streaming, &batch);
}

// За один шаг на соседний чанк в кубе радиуса viewRadius появляется лишь
// дальняя грань (а при движении по диагонали — две или три), всё остальное
// уже стояло в таблице. Точечный обход этих граней заменяет перебор всего
// куба: на радиусе R это (2R+1) обращений вместо (2R+1)^3.
static void QueueMissingLeadingFace(ChunkStreaming* streaming,
    int64_t previousX, int64_t previousY, int64_t previousZ,
    int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    const int64_t radius = streaming->viewRadius;
    const int64_t delta[3] = {
        chunkX - previousX, chunkY - previousY, chunkZ - previousZ
    };

    ChunkEnqueueBatch batch = { .count = 0u };
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (delta[axis] == 0) continue;

        const int64_t sign = delta[axis] > 0 ? 1 : -1;
        const int32_t axisA = (axis + 1) % 3;
        const int32_t axisB = (axis + 2) % 3;

        for (int64_t offsetA = -radius; offsetA <= radius; ++offsetA)
        {
            for (int64_t offsetB = -radius; offsetB <= radius; ++offsetB)
            {
                int64_t offset[3] = { 0, 0, 0 };
                offset[axis] = sign * radius;
                offset[axisA] = offsetA;
                offset[axisB] = offsetB;
                QueueChunkIfMissing(streaming,
                    chunkX + offset[0], chunkY + offset[1], chunkZ + offset[2],
                    &batch);
            }
        }
    }
    FlushEnqueueBatch(streaming, &batch);
}

// Выбрасывает один ушедший чанк: снимает меш со списка отрисовки,
// освобождает его и удаляет запись из таблицы. Повторный вызов для того же
// ключа безвреден: FindEntry уже вернёт NULL.
static void EvictChunk(ChunkStreaming* streaming, int64_t x, int64_t y, int64_t z)
{
    ChunkEntry* entry = FindEntry(streaming, x, y, z);
    if (entry == NULL)
    {
        return;
    }

    if (entry->mesh != NULL)
    {
        RemoveMeshFromDrawList(streaming, entry);
        VoxelRenderDestroyMesh(&streaming->services, streaming->renderer, entry->mesh);
        entry->mesh = NULL;
    }
    EraseEntry(streaming, entry);
}

// Убирает из таблицы всё, что вышло за радиус + 1 вокруг нового центра.
// Пересборка в свежую таблицу не нужна: хеш ключа от центра не зависит,
// поэтому дом каждой оставшейся записи тот же, а удаление сдвигом кластера
// сохраняет достижимость соседей. Приёмник-таблица тут не требуется вовсе.
//
// Индекс не увеличивается после удаления: EraseEntry сдвигает кластер, и на
// место только что освобождённого слота встаёт следующая запись — её и надо
// разобрать тем же индексом.
static void PruneEntriesOutsideRadius(ChunkStreaming* streaming, int64_t radius)
{
    uint32_t index = 0u;
    while (index < streaming->capacity)
    {
        ChunkEntry* entry = &streaming->entries[index];
        if (entry->state == CHUNK_ENTRY_EMPTY
            || IsInsideRadius(streaming, entry->x, entry->y, entry->z, radius))
        {
            ++index;
            continue;
        }

        if (entry->mesh != NULL)
        {
            RemoveMeshFromDrawList(streaming, entry);
            VoxelRenderDestroyMesh(&streaming->services, streaming->renderer, entry->mesh);
            entry->mesh = NULL;
        }
        EraseEntry(streaming, entry);
    }
}

// При переходе ровно на соседний чанк за пределами нового радиуса+1
// оказываются только уходящие грани старого куба: по одной на каждую ось,
// изменившую знак. Перебирать всю таблицу, как при первом вызове или
// телепорте, не нужно.
static void EvictDepartedChunks(ChunkStreaming* streaming,
    int64_t previousX, int64_t previousY, int64_t previousZ)
{
    const int64_t radius = (int64_t)streaming->viewRadius + 1;
    const int64_t delta[3] = {
        streaming->centerX - previousX,
        streaming->centerY - previousY,
        streaming->centerZ - previousZ
    };
    const int64_t previous[3] = { previousX, previousY, previousZ };

    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (delta[axis] == 0)
        {
            continue;
        }

        const int64_t sign = delta[axis] > 0 ? 1 : -1;
        const int32_t axisA = (axis + 1) % 3;
        const int32_t axisB = (axis + 2) % 3;
        const int64_t faceCoordinate = previous[axis] - sign * radius;

        for (int64_t offsetA = -radius; offsetA <= radius; ++offsetA)
        {
            for (int64_t offsetB = -radius; offsetB <= radius; ++offsetB)
            {
                int64_t chunk[3];
                chunk[axis] = faceCoordinate;
                chunk[axisA] = previous[axisA] + offsetA;
                chunk[axisB] = previous[axisB] + offsetB;

                if (IsInsideRadius(streaming, chunk[0], chunk[1], chunk[2], radius))
                {
                    continue;
                }
                EvictChunk(streaming, chunk[0], chunk[1], chunk[2]);
            }
        }
    }
}

bool ChunkStreamingResumeAfterOriginChange(ChunkStreaming* streaming,
    bool originDeltaFits,
    int64_t chunkOriginDeltaX, int64_t chunkOriginDeltaY, int64_t chunkOriginDeltaZ,
    int64_t newCenterX, int64_t newCenterY, int64_t newCenterZ)
{
    streaming->hasCenter = true;
    streaming->centerX = newCenterX;
    streaming->centerY = newCenterY;
    streaming->centerZ = newCenterZ;
    streaming->hasUnqueuedPending = false;

    // Origin сменился: абсолютные координаты живущих чанков пересчитываются
    // вместе с их хешем, поэтому таблицу приходится строить заново. Приёмник
    // берём из компактной арены: живущих в радиусе + 1 не больше (2R+3)^3,
    // так что второй таблицы на всю ёмкость для этого не нужно.
    ChunkEntry* scratch = streaming->rebuildScratch;
    uint32_t scratchCount = 0u;

    for (uint32_t index = 0; index < streaming->capacity; ++index)
    {
        ChunkEntry* previous = &streaming->entries[index];
        if (previous->mesh == NULL) continue;

        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        bool keep = originDeltaFits
            && TrySubtractInt64(previous->x, chunkOriginDeltaX, &x)
            && TrySubtractInt64(previous->y, chunkOriginDeltaY, &y)
            && TrySubtractInt64(previous->z, chunkOriginDeltaZ, &z)
            && IsInsideRadius(streaming, x, y, z,
                (int64_t)streaming->viewRadius + 1);

        if (keep)
        {
            ChunkEntry* moved = &scratch[scratchCount++];
            moved->x = x;
            moved->y = y;
            moved->z = z;
            moved->mesh = previous->mesh;
            moved->state = CHUNK_ENTRY_READY;
            moved->requestQueued = false;
            moved->drawSlotPlusOne = 0;
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
        else
        {
            VoxelRenderDestroyMesh(&streaming->services, streaming->renderer, previous->mesh);
            previous->mesh = NULL;
            previous->drawSlotPlusOne = 0;
        }
    }

    memset(streaming->entries, 0,
        (size_t)streaming->capacity * sizeof(ChunkEntry));
    ResetDrawList(streaming);

    for (uint32_t index = 0; index < scratchCount; ++index)
    {
        ChunkEntry* moved = InsertEntry(streaming,
            scratch[index].x, scratch[index].y, scratch[index].z);
        moved->mesh = scratch[index].mesh;
        moved->state = CHUNK_ENTRY_READY;
        AddMeshToDrawList(streaming, moved);
    }

    QueueMissingChunks(streaming, newCenterX, newCenterY, newCenterZ);

#ifndef NDEBUG
    VerifyStreamingIntegrity(streaming);
#endif

    ResumeWorkerThreads(streaming);
    return true;
}

ChunkStreaming* ChunkStreamingCreate(World* world, Renderer* renderer, int32_t viewRadiusChunks)
{
    return ChunkStreamingCreateWithServices(world, renderer, viewRadiusChunks,
        legacySceneMathService, legacyWorldService, legacyMesherService,
        legacyGraphicsService);
}

ChunkStreaming* ChunkStreamingCreateWithServices(World* world, Renderer* renderer,
    int32_t viewRadiusChunks, const LaiueSceneMathServiceV1* sceneMath,
    const LaiueWorldServiceV1* worldService, const LaiueMesherServiceV1* mesher,
    const LaiueGraphicsServiceV1* graphics)
{
    ChunkStreaming* streaming = PlatformAllocate(sizeof(*streaming), true);
    if (streaming == NULL)
    {
        return NULL;
    }

    streaming->services.sceneMath = SceneMathServiceUsable(sceneMath) ? sceneMath : NULL;
    streaming->services.world = WorldServiceUsable(worldService) ? worldService : NULL;
    streaming->services.mesher = MesherServiceUsable(mesher) ? mesher : NULL;
    streaming->services.graphics = GraphicsServiceUsable(graphics) ? graphics : NULL;
    streaming->world = world;
    streaming->renderer = renderer;
    streaming->viewRadius = viewRadiusChunks;

    // Гистерезис держит максимум куб радиуса + 1. Загрузка около 54%
    // достаточна для быстрой открытой адресации без лишнего удвоения памяти.
    uint32_t diameter = (uint32_t)(viewRadiusChunks * 2 + 3);
    uint32_t volume = diameter * diameter * diameter;
    uint32_t minimumCapacity = volume + volume / 2u;
    uint32_t capacity = 1;
    while (capacity < minimumCapacity)
    {
        capacity <<= 1;
    }

    // Очередям достаточно вместить весь активный куб радиуса viewRadius.
    // Hash-таблица больше из-за гистерезиса, но переносить этот запас в две
    // очереди нет смысла: переполнение всё равно корректно retry-ится.
    // Степень двойки поверх этого — не только маска вместо деления, но и
    // запас на ведущие грани шага: пока не разобраны заявки старого куба,
    // новые заявки уходящих граней должны поместиться сразу (см. проверку
    // ведущих граней в voxel_raycast_test.c).
    uint32_t activeDiameter = (uint32_t)(viewRadiusChunks * 2 + 1);
    uint32_t activeVolume = activeDiameter * activeDiameter * activeDiameter;
    uint32_t queueCapacity = 1;
    while (queueCapacity < activeVolume)
    {
        queueCapacity <<= 1;
    }

    streaming->capacity = capacity;
    streaming->queueCapacity = queueCapacity;
    streaming->entries = PlatformAllocate((size_t)capacity * sizeof(ChunkEntry), true);
    streaming->rebuildScratch = PlatformAllocate((size_t)volume * sizeof(ChunkEntry), false);
    streaming->drawItems = PlatformAllocate((size_t)volume * sizeof(DrawItem), false);
    streaming->requests = PlatformAllocate((size_t)queueCapacity * sizeof(ChunkRequest), false);
    streaming->results = PlatformAllocate((size_t)queueCapacity * sizeof(ChunkMeshResult), false);

    if (!PlatformMutexInitialize(&streaming->queueLock)
        || !PlatformConditionVariableInitialize(&streaming->workAvailable))
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    if (streaming->entries == NULL || streaming->rebuildScratch == NULL
        || streaming->drawItems == NULL
        || streaming->requests == NULL || streaming->results == NULL)
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    // Пул потоков мешинга: масштабируется по ядрам процессора.
    uint32_t processorCount = PlatformLogicalProcessorCount();
    streaming->desiredWorkerThreadCount =
        processorCount > 2 ? processorCount - 2 : 1;
    if (streaming->desiredWorkerThreadCount > MAX_WORKER_THREADS)
    {
        streaming->desiredWorkerThreadCount = MAX_WORKER_THREADS;
    }

    if (!StartWorkerThreads(streaming))
    {
        ChunkStreamingDestroy(streaming);
        return NULL;
    }

    return streaming;
}

void ChunkStreamingDestroy(ChunkStreaming* streaming)
{
    if (streaming == NULL)
    {
        return;
    }

    StopWorkerThreads(streaming);

    // Остаточные результаты: освободить CPU-массивы.
    if (streaming->results != NULL)
    {
        uint32_t queueMask = streaming->queueCapacity - 1;
        for (uint32_t i = 0; i < streaming->resultCount; ++i)
        {
            ChunkMeshResult* result = &streaming->results[(streaming->resultHead + i) & queueMask];
            if (result->quads != NULL) PlatformFree(result->quads);
        }
    }

    if (streaming->hasHeldResult && streaming->heldResult.quads != NULL)
    {
        PlatformFree(streaming->heldResult.quads);
        streaming->heldResult.quads = NULL;
        streaming->hasHeldResult = false;
    }

    if (streaming->entries != NULL)
    {
        for (uint32_t i = 0; i < streaming->capacity; ++i)
        {
            if (streaming->entries[i].mesh != NULL)
            {
                VoxelRenderDestroyMesh(&streaming->services, streaming->renderer,
                    streaming->entries[i].mesh);
            }
        }
        PlatformFree(streaming->entries);
    }

    if (streaming->rebuildScratch != NULL)
    {
        PlatformFree(streaming->rebuildScratch);
    }
    if (streaming->drawItems != NULL) PlatformFree(streaming->drawItems);
    if (streaming->requests != NULL) PlatformFree(streaming->requests);
    if (streaming->results != NULL) PlatformFree(streaming->results);
    // Потоки уже остановлены выше, поэтому примитивы синхронизации никто
    // не держит и их можно разрушить.
    PlatformConditionVariableDestroy(&streaming->workAvailable);
    PlatformMutexDestroy(&streaming->queueLock);
    PlatformFree(streaming);
}

void ChunkStreamingSetCenter(ChunkStreaming* streaming, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    if (streaming->hasCenter &&
        streaming->centerX == chunkX && streaming->centerY == chunkY && streaming->centerZ == chunkZ)
    {
        return;
    }

    const int64_t previousX = streaming->centerX;
    const int64_t previousY = streaming->centerY;
    const int64_t previousZ = streaming->centerZ;
    const bool hadCenter = streaming->hasCenter;

    const int64_t deltaX = chunkX - previousX;
    const int64_t deltaY = chunkY - previousY;
    const int64_t deltaZ = chunkZ - previousZ;
    const bool unitStep = hadCenter
        && deltaX >= -1 && deltaX <= 1
        && deltaY >= -1 && deltaY <= 1
        && deltaZ >= -1 && deltaZ <= 1;

    // Переход на соседний чанк: таблицу не пересобираем. Уходят только
    // уходящие грани, приходят только дальние — их и трогаем точечно.
    // Раньше здесь memset-илась запасная таблица и в неё заливались все
    // ≈(2R+3)^3 записи ради (2R+1)^2 изменившихся.
    if (unitStep)
    {
        streaming->hasCenter = true;
        streaming->centerX = chunkX;
        streaming->centerY = chunkY;
        streaming->centerZ = chunkZ;
        PlatformAtomicIncrementU32(&streaming->centerEpoch);

        EvictDepartedChunks(streaming, previousX, previousY, previousZ);
        QueueMissingLeadingFace(streaming, previousX, previousY, previousZ,
            chunkX, chunkY, chunkZ);

#ifndef NDEBUG
        VerifyStreamingIntegrity(streaming);
#endif
        return;
    }

    // Первый вызов, телепорт или смена центра больше чем на чанк. Хеш ключа
    // от центра не зависит, поэтому дом каждой оставшейся записи тот же:
    // таблицу не пересобираем, а прямо в ней вытесняем всё, что вышло за
    // радиус + 1. Вторая таблица на всю ёмкость (прежний приёмник) больше
    // не нужна — это и была чистая потеря памяти на весь сеанс игры.
    streaming->hasCenter = true;
    streaming->centerX = chunkX;
    streaming->centerY = chunkY;
    streaming->centerZ = chunkZ;
    PlatformAtomicIncrementU32(&streaming->centerEpoch);
    streaming->drawOrderDirty = true;

    PruneEntriesOutsideRadius(streaming,
        (int64_t)streaming->viewRadius + 1);

    streaming->hasUnqueuedPending = false;
    for (uint32_t i = 0; i < streaming->capacity; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[i];
        if (entry->state == CHUNK_ENTRY_PENDING && !entry->requestQueued)
        {
            streaming->hasUnqueuedPending = true;
            break;
        }
    }

    // Сюда попадают только первый вызов, телепорт и смена origin:
    // для шага на соседний чанк выше есть точечный путь.
    QueueMissingChunks(streaming, chunkX, chunkY, chunkZ);

#ifndef NDEBUG
    VerifyStreamingIntegrity(streaming);
#endif
}

void ChunkStreamingInvalidateBlock(ChunkStreaming* streaming, int64_t blockX, int64_t blockY, int64_t blockZ)
{
    int64_t chunkX = ChunkCoordinateFromBlock(blockX);
    int64_t chunkY = ChunkCoordinateFromBlock(blockY);
    int64_t chunkZ = ChunkCoordinateFromBlock(blockZ);
    int64_t localX = blockX - chunkX * CHUNK_SIZE;
    int64_t localY = blockY - chunkY * CHUNK_SIZE;
    int64_t localZ = blockZ - chunkZ * CHUNK_SIZE;

    // Блок на границе чанка входит в расширенный регион соседа —
    // соседние чанки перестраиваются тоже.
    int64_t offsetsX[2] = { 0, localX == 0 ? -1 : (localX == CHUNK_SIZE - 1 ? 1 : 0) };
    int64_t offsetsY[2] = { 0, localY == 0 ? -1 : (localY == CHUNK_SIZE - 1 ? 1 : 0) };
    int64_t offsetsZ[2] = { 0, localZ == 0 ? -1 : (localZ == CHUNK_SIZE - 1 ? 1 : 0) };

    for (int32_t indexZ = 0; indexZ < 2; ++indexZ)
    {
        if (indexZ == 1 && offsetsZ[1] == 0) continue;
        for (int32_t indexY = 0; indexY < 2; ++indexY)
        {
            if (indexY == 1 && offsetsY[1] == 0) continue;
            for (int32_t indexX = 0; indexX < 2; ++indexX)
            {
                if (indexX == 1 && offsetsX[1] == 0) continue;

                ChunkEntry* entry = FindEntry(streaming,
                    chunkX + offsetsX[indexX], chunkY + offsetsY[indexY], chunkZ + offsetsZ[indexZ]);
                if (entry == NULL)
                {
                    continue;
                }

                // Старый меш НЕ удаляем здесь: он остаётся последней готовой
                // геометрией и продолжает рисоваться, пока рабочий поток строит
                // замену. Свап и освобождение — в ChunkStreamingPump, когда новый
                // меш загружен. Иначе чанк мигал бы дырой те кадр-два, что идёт
                // перестройка.
                entry->state = CHUNK_ENTRY_PENDING;
                entry->revision = NextChunkRevision(streaming);
                TryEnqueueRequest(streaming, entry);
            }
        }
    }
}

// Пытается загрузить придержанный с прошлого кадра готовый меш.
// false — рендер снова не принял (оставляем до следующего кадра).
// Байты успешной загрузки прибавляются к бюджету текущего кадра: кольцо
// рендера у придержанного и у новых мешей одно и то же.
static bool TryUploadHeldResult(ChunkStreaming* streaming,
    uint64_t* uploadBytes, bool* uploadedAny)
{
    ChunkMeshResult* held = &streaming->heldResult;
    ChunkEntry* entry = FindEntry(streaming, held->x, held->y, held->z);
    if (entry == NULL || entry->state != CHUNK_ENTRY_PENDING
        || entry->revision != held->revision)
    {
        // Запись ушла, была перестроена или сменила ревизию — держать
        // нечего.
        if (held->quads != NULL) PlatformFree(held->quads);
        held->quads = NULL;
        streaming->hasHeldResult = false;
        return true;
    }

    RendererMesh* mesh = VoxelRenderCreateMesh(&streaming->services,
        streaming->renderer,
        held->quads, held->quadCount);
    if (mesh == NULL)
    {
        return false;
    }

    bool hadMesh = entry->mesh != NULL;
    if (hadMesh)
    {
        VoxelRenderDestroyMesh(&streaming->services, streaming->renderer, entry->mesh);
    }
    entry->mesh = mesh;
    if (!hadMesh)
    {
        AddMeshToDrawList(streaming, entry);
    }
    entry->state = CHUNK_ENTRY_READY;
    entry->requestQueued = false;
    PlatformAtomicIncrementI64(&streaming->uploadedMeshes);
    *uploadBytes +=
        ((uint64_t)held->quadCount * sizeof(ChunkQuad) + 15u) & ~(uint64_t)15u;
    *uploadedAny = true;

    if (held->quads != NULL) PlatformFree(held->quads);
    held->quads = NULL;
    streaming->hasHeldResult = false;
    return true;
}

void ChunkStreamingPump(ChunkStreaming* streaming)
{
    uint32_t queueMask = streaming->queueCapacity - 1;
    uint64_t uploadBytes = 0;
    bool uploadedAny = false;
    bool holdResult = false;
    double pumpStart = PlatformMonotonicSeconds();

    // Придержанный результат пробуем первым: рендер принимает не больше
    // своей очереди загрузок на кадр, поэтому переполнение случается.
    if (streaming->hasHeldResult
        && !TryUploadHeldResult(streaming, &uploadBytes, &uploadedAny))
    {
        return;
    }

    for (;;)
    {
        // Заглянуть в очередь: результат с геометрией берём только
        // при оставшемся байтовом и временном бюджете, пустые — бесплатны.
        PlatformMutexLock(&streaming->queueLock);
        if (streaming->resultCount == 0)
        {
            PlatformMutexUnlock(&streaming->queueLock);
            break;
        }

        ChunkMeshResult result = streaming->results[streaming->resultHead & queueMask];
        bool hasGeometry = result.quadCount != CHUNK_MESH_BUILD_FAILED
            && result.quadCount > 0;
        uint64_t resultBytes = 0;
        if (hasGeometry)
        {
            // Считаем ровно байты кольца рендера: размер с выравниванием.
            resultBytes =
                ((uint64_t)result.quadCount * sizeof(ChunkQuad) + 15u) & ~(uint64_t)15u;
            if (uploadedAny)
            {
                if (uploadBytes >= (uint64_t)CHUNK_UPLOAD_BYTES_PER_FRAME
                    || resultBytes
                        > (uint64_t)CHUNK_UPLOAD_BYTES_PER_FRAME - uploadBytes)
                {
                    PlatformMutexUnlock(&streaming->queueLock);
                    break;
                }
                if ((PlatformMonotonicSeconds() - pumpStart) * 1000.0
                    >= MESH_UPLOAD_BUDGET_MILLISECONDS)
                {
                    PlatformMutexUnlock(&streaming->queueLock);
                    break;
                }
            }
        }

        streaming->resultHead++;
        streaming->resultCount--;
        streaming->unfinishedWork--;
        PlatformMutexUnlock(&streaming->queueLock);

        ChunkEntry* entry = FindEntry(streaming, result.x, result.y, result.z);
        if (entry != NULL && entry->state == CHUNK_ENTRY_PENDING && entry->revision == result.revision)
        {
            // Заявка этой ревизии завершена; повторное выставление ниже нужно
            // только если построение не удалось.
            entry->requestQueued = false;
            if (result.quadCount == CHUNK_MESH_BUILD_FAILED)
            {
                // Сбой построения (нехватка памяти): повторная попытка
                // через условное сканирование ниже.
                streaming->hasUnqueuedPending = true;
            }
            else if (result.quadCount > 0)
            {
                RendererMesh* mesh = VoxelRenderCreateMesh(&streaming->services,
                    streaming->renderer, result.quads, result.quadCount);
                if (mesh != NULL)
                {
                    // Свап готов: старый меш освобождаем только теперь (отложенно
                    // под fence — кадр с ним ещё может быть в полёте на GPU).
                    bool hadMesh = entry->mesh != NULL;
                    if (hadMesh)
                    {
                        VoxelRenderDestroyMesh(&streaming->services, streaming->renderer,
                            entry->mesh);
                    }
                    entry->mesh = mesh;
                    if (!hadMesh)
                    {
                        AddMeshToDrawList(streaming, entry);
                    }
                    entry->state = CHUNK_ENTRY_READY;
                    uploadBytes += resultBytes;
                    uploadedAny = true;
                    PlatformAtomicIncrementI64(&streaming->uploadedMeshes);
                }
                else
                {
                    // Рендер не принял меш (занята его очередь загрузок).
                    // Придерживаем готовый результат до следующего кадра:
                    // перестраивать чанк с нуля незачем. requestQueued
                    // удерживает условное сканирование от повторной заявки.
                    entry->requestQueued = true;
                    streaming->heldResult = result;
                    streaming->hasHeldResult = true;
                    holdResult = true;
                }
            }
            else
            {
                // Чанк стал пустым (все блоки убраны): снимаем старый меш.
                if (entry->mesh != NULL)
                {
                    RemoveMeshFromDrawList(streaming, entry);
                    VoxelRenderDestroyMesh(&streaming->services, streaming->renderer,
                        entry->mesh);
                    entry->mesh = NULL;
                }
                entry->state = CHUNK_ENTRY_READY;
            }
        }
        else
        {
            PlatformAtomicIncrementI64(&streaming->discardedBuilds);
        }

        if (result.quads != NULL && !holdResult)
        {
            PlatformFree(result.quads);
        }

        if (holdResult)
        {
            break;
        }
    }

    // Повторные заявки: полный проход нужен только после фактического
    // переполнения очереди, сбоя построения или загрузки.
    if (streaming->hasUnqueuedPending)
    {
        streaming->hasUnqueuedPending = false;
        for (uint32_t i = 0; i < streaming->capacity; ++i)
        {
            ChunkEntry* entry = &streaming->entries[i];
            if (entry->state == CHUNK_ENTRY_PENDING && !entry->requestQueued)
            {
                // После первого отказа unfinishedWork уже достиг ёмкости:
                // остальные попытки в этом кадре гарантированно не пройдут.
                if (!TryEnqueueRequest(streaming, entry))
                {
                    break;
                }
            }
        }
    }
}

void ChunkStreamingGetStats(ChunkStreaming* streaming,
    ChunkStreamingStats* outStats)
{
    if (streaming == NULL || outStats == NULL) return;

    outStats->queuedRequests = (uint64_t)PlatformAtomicLoadI64(&streaming->queuedRequests);
    outStats->completedBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->completedBuilds);
    outStats->cancelledBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->cancelledBuilds);
    outStats->discardedBuilds = (uint64_t)PlatformAtomicLoadI64(&streaming->discardedBuilds);
    outStats->uploadedMeshes = (uint64_t)PlatformAtomicLoadI64(&streaming->uploadedMeshes);
    int64_t totalMicroseconds = PlatformAtomicLoadI64(&streaming->totalBuildMicroseconds);

    PlatformMutexLock(&streaming->queueLock);
    outStats->pendingRequests = streaming->requestCount;
    outStats->pendingResults = streaming->resultCount;
    outStats->peakUnfinishedWork = streaming->peakUnfinishedWork;
    PlatformMutexUnlock(&streaming->queueLock);

    outStats->averageBuildMilliseconds =
        outStats->completedBuilds > 0
            ? (double)totalMicroseconds / (1000.0 * (double)outStats->completedBuilds)
            : 0.0;
}

static void ExpandFrustumPlanesForChunk(float planes[6][4])
{
    const float halfExtent = (float)(CHUNK_SIZE / 2);
    for (uint32_t plane = 0; plane < 6; ++plane)
    {
        float absoluteX = planes[plane][0] < 0.0f ? -planes[plane][0] : planes[plane][0];
        float absoluteY = planes[plane][1] < 0.0f ? -planes[plane][1] : planes[plane][1];
        float absoluteZ = planes[plane][2] < 0.0f ? -planes[plane][2] : planes[plane][2];
        planes[plane][3] += halfExtent * (absoluteX + absoluteY + absoluteZ);
    }
}

static bool FrustumContainsChunkCenter(const float planes[6][4], const float center[3])
{
    for (uint32_t plane = 0; plane < 6; ++plane)
    {
        if (planes[plane][0] * center[0]
            + planes[plane][1] * center[1]
            + planes[plane][2] * center[2]
            + planes[plane][3] < 0.0f)
        {
            return false;
        }
    }
    return true;
}

void ChunkStreamingDraw(ChunkStreaming* streaming, const float viewProjection[16],
    const int64_t renderOriginBlock[3])
{
    if (streaming == NULL || viewProjection == NULL || renderOriginBlock == NULL ||
        streaming->services.sceneMath == NULL ||
        streaming->services.sceneMath->matrix4ExtractFrustumPlanes == NULL)
    {
        return;
    }
    bool renderOriginChanged = !streaming->hasDrawRenderOrigin
        || streaming->drawRenderOriginBlock[0] != renderOriginBlock[0]
        || streaming->drawRenderOriginBlock[1] != renderOriginBlock[1]
        || streaming->drawRenderOriginBlock[2] != renderOriginBlock[2];

    // Distances and order are relative to the caller-selected render origin.
    // Recompute only when that origin or the mesh set changes.
    if (streaming->drawOrderDirty || renderOriginChanged)
    {
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            DrawItem* item = &streaming->drawItems[i];
            const ChunkEntry* entry = &streaming->entries[item->entryIndex];
            float centerX = (float)(entry->x * CHUNK_SIZE - renderOriginBlock[0])
                + (float)(CHUNK_SIZE / 2);
            float centerY = (float)(entry->y * CHUNK_SIZE - renderOriginBlock[1])
                + (float)(CHUNK_SIZE / 2);
            float centerZ = (float)(entry->z * CHUNK_SIZE - renderOriginBlock[2])
                + (float)(CHUNK_SIZE / 2);
            item->distanceSquared = centerX * centerX + centerY * centerY + centerZ * centerZ;
        }

        SortDrawItemsFrontToBack(streaming->drawItems, streaming->drawItemCount);
        for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
        {
            ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];
            entry->drawSlotPlusOne = i + 1u;
        }

        streaming->drawRenderOriginBlock[0] = renderOriginBlock[0];
        streaming->drawRenderOriginBlock[1] = renderOriginBlock[1];
        streaming->drawRenderOriginBlock[2] = renderOriginBlock[2];
        streaming->hasDrawRenderOrigin = true;
        streaming->drawOrderDirty = false;
    }

    float planes[6][4];
    streaming->services.sceneMath->matrix4ExtractFrustumPlanes(viewProjection, planes);
    ExpandFrustumPlanesForChunk(planes);

    // Frustum зависит от поворота камеры, поэтому отсечение остаётся
    // покадровым. Плотный список исключает обход пустых слотов hash-таблицы.
    for (uint32_t i = 0; i < streaming->drawItemCount; ++i)
    {
        const ChunkEntry* entry = &streaming->entries[streaming->drawItems[i].entryIndex];

        float chunkOriginRelative[3] = {
            (float)(entry->x * CHUNK_SIZE - renderOriginBlock[0]),
            (float)(entry->y * CHUNK_SIZE - renderOriginBlock[1]),
            (float)(entry->z * CHUNK_SIZE - renderOriginBlock[2]),
        };
        float center[3] = {
            chunkOriginRelative[0] + (float)(CHUNK_SIZE / 2),
            chunkOriginRelative[1] + (float)(CHUNK_SIZE / 2),
            chunkOriginRelative[2] + (float)(CHUNK_SIZE / 2),
        };
        // До C23 массив float[6][4] не приводится к const float(*)[4]
        // неявно, поэтому квалификатор добавляется явно.
        if (!FrustumContainsChunkCenter((const float (*)[4])planes, center))
        {
            continue;
        }

        VoxelRenderDrawMesh(&streaming->services, streaming->renderer, entry->mesh,
            chunkOriginRelative);
    }
}
