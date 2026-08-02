#pragma once

#include "api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Потокобезопасность: обычные запросы мира можно выполнять параллельно.
// WorldRebase вызывается только после остановки рабочих потоков мешинга.
typedef struct World World;

typedef uint8_t BlockType;

#define BLOCK_AIR   0
#define BLOCK_EARTH 1
#define BLOCK_GRASS 2

#define CHUNK_SIZE      64
#define CHUNK_SIZE_LOG2 6

static inline uint32_t WorldHashChunkCoordinate(int64_t x, int64_t y, int64_t z)
{
    uint64_t hash = (uint64_t)x * 73856093ULL
                  ^ (uint64_t)y * 19349663ULL
                  ^ (uint64_t)z * 83492791ULL;
    return (uint32_t)(hash ^ (hash >> 33));
}

typedef enum WorldRegionContents
{
    WORLD_REGION_ALL_AIR,
    WORLD_REGION_ALL_SOLID,
    WORLD_REGION_MIXED
} WorldRegionContents;

typedef struct WorldChunkSummary
{
    int64_t chunk[3];
    uint64_t revision;
    uint32_t deltaCount;
} WorldChunkSummary;

typedef struct WorldChunkDelta
{
    uint32_t localIndex;
    BlockType block;
} WorldChunkDelta;

#define WORLD_MAX_ATOMIC_BLOCK_MUTATIONS 4096U
#define WORLD_MAX_BOUNDED_BLOCK_QUERY_CELLS 64U
#define WORLD_MAX_BOUNDED_BLOCK_RANGES 512U

typedef struct WorldBlockMutation
{
    int64_t block[3];
    BlockType expected;
    BlockType replacement;
} WorldBlockMutation;

typedef struct WorldBlockState
{
    BlockType block;
    bool edited;
} WorldBlockState;

typedef struct WorldBlockRange
{
    int64_t minimum[3];
    int64_t maximum[3];
} WorldBlockRange;

LAIUE_WORLD_API World* WorldCreate(int64_t seed);
LAIUE_WORLD_API void   WorldDestroy(World* world);

// Переносит локальное начало координат на целое число блоков.
// Сдвиги обязаны быть кратны CHUNK_SIZE. Абсолютные координаты произвольной
// точности растут динамически, а локальные координаты остаются маленькими.
LAIUE_WORLD_API bool WorldRebase(World* world,
    int64_t blockShiftX, int64_t blockShiftY, int64_t blockShiftZ);

// Возводит абсолютную координату X в квадрат. Возвращает смещение нового
// chunk-origin относительно старого, если оно помещается в int64.
LAIUE_WORLD_API bool WorldSquareAbsoluteX(
    World* world, int64_t localBlockX, int64_t* outLocalBlockX,
    bool* outChunkOriginDeltaFits, int64_t* outChunkOriginDeltaX);

LAIUE_WORLD_API void WorldFormatAbsoluteBlockCoordinate(World* world,
    int32_t axis, int64_t localBlock, wchar_t* outText, uint32_t capacity);

LAIUE_WORLD_API BlockType WorldGetBlock(World* world, int64_t x, int64_t y, int64_t z);
// Copies material and edit provenance under one shared lock. This is the
// per-cell primitive for bounded transactional gameplay operations.
LAIUE_WORLD_API bool WorldGetBlockState(
    World* world, int64_t x, int64_t y, int64_t z,
    WorldBlockState* outState);
// Inclusive, allocation-free solid occupancy query. Oversized ranges are
// rejected instead of turning a fixed-tick collision check into an unbounded
// world scan.
LAIUE_WORLD_API bool WorldAnySolidBlockInRange(
    World* world, const int64_t minimum[3], const int64_t maximum[3]);
// Evaluates all inclusive ranges under one shared table lock. Each range and
// the range count are bounded; false means invalid input and callers which use
// this for collision should fail closed. outSolid is always initialized.
LAIUE_WORLD_API bool WorldAnySolidBlockInRanges(
    World* world, const WorldBlockRange* ranges, uint32_t count,
    bool* outSolid);
// Возвращает true, если ячейка хранится как явная правка относительно
// процедурного terrain. Это provenance для bounded gameplay-операций; сам
// тип блока не кодирует, был ли он поставлен игроком.
LAIUE_WORLD_API bool WorldIsBlockEdited(
    World* world, int64_t x, int64_t y, int64_t z);
// Транзакционные системы используют bool-вариант, чтобы не продолжать после
// OOM. Совместимый WorldSetBlock оставлен для старых вызовов.
LAIUE_WORLD_API bool WorldTrySetBlock(
    World* world, int64_t x, int64_t y, int64_t z, BlockType block);
LAIUE_WORLD_API void      WorldSetBlock(World* world, int64_t x, int64_t y, int64_t z, BlockType block);
// Проверяет все expected и готовит новые delta-массивы до публикации хоть
// одной правки. При false содержимое блоков и revisions не меняются.
LAIUE_WORLD_API bool WorldApplyBlockBatch(
    World* world, const WorldBlockMutation* mutations, uint32_t count);

// Snapshot API для удалённого клиента. Сначала сервер копирует summaries в
// заданном окне, затем запрашивает deltas каждого чанка. Все буферы принадлежат
// вызывающему, поэтому allocator модуля world не выходит наружу.
LAIUE_WORLD_API uint64_t WorldGetRevision(World* world);
LAIUE_WORLD_API uint64_t WorldGetChunkRevision(
    World* world, const int64_t chunk[3]);
LAIUE_WORLD_API uint32_t WorldCopyEditedChunkSummaries(
    World* world, const int64_t minimumChunk[3], const int64_t maximumChunk[3],
    WorldChunkSummary* output, uint32_t capacity, bool* outTruncated,
    uint64_t* outWorldRevision);
LAIUE_WORLD_API bool WorldCopyChunkDeltas(
    World* world, const int64_t chunk[3], WorldChunkDelta* output,
    uint32_t capacity, uint32_t* outCount, uint64_t* outChunkRevision);
// Revision монотонна: snapshot старше уже применённого состояния считается
// успешно обработанным no-op и не заменяет содержимое чанка.
LAIUE_WORLD_API bool WorldReplaceChunkDeltas(
    World* world, const int64_t chunk[3], const WorldChunkDelta* deltas,
    uint32_t count, uint64_t chunkRevision, uint64_t worldRevision);

LAIUE_WORLD_API WorldRegionContents WorldFillRegion(World* world,
    int64_t minBlockX, int64_t minBlockY, int64_t minBlockZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ,
    BlockType* outBlocks,
    float* heightScratch, size_t heightScratchCount);

// Высота верхнего твёрдого блока в текущих локальных координатах.
LAIUE_WORLD_API int64_t WorldGetTerrainHeight(World* world, int64_t x, int64_t y);

// === Сохранение правок (Laiue World Format v1, docs/world_format.md) ===
//
// WorldSaveDeltas пишет seed, начало координат и все правки блоков
// (абсолютные координаты произвольной точности) — читать таблицу можно
// параллельно с рабочими потоками мешинга. WorldLoadDeltas вызывается
// на свежесозданном мире до запуска стриминга: сверяет seed,
// восстанавливает начало координат через WorldRebase (v1 требует его
// представимости в int64) и повторяет правки. Правки чанков, чьи
// абсолюты не представимы относительно восстановленного начала,
// в v1 пропускаются.
LAIUE_WORLD_API bool WorldSaveDeltas(World* world, const wchar_t* path);
LAIUE_WORLD_API bool WorldLoadDeltas(World* world, const wchar_t* path);
