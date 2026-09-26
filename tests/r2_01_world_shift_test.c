/* Узкий регрессионный тест сдвига массива дельт (ROUND 2, agent 01-world).
 *
 * Проверяет через публичный API, что вставка и удаление тысяч правок в один
 * чанк в произвольном порядке сохраняют и отсортированность (по ней ищет
 * ChunkGetDelta), и точное содержимое. Это прямое покрытие ускоренного сдвига
 * ChunkShiftRight/ChunkShiftLeft: ошибка в направлении или границах блока
 * потеряла бы или перепутала правку, и проверка значения это поймала бы.
 *
 * Отдельно проверяются граничные позиции (первый и последний локальный
 * индекс чанка), отрицательные координаты, ветка «правка равна базе
 * провайдера» (не должна оставлять override) и пакетный путь с тем же
 * буфером дельт.
 */
#include "world/world.h"
#include "world/numeric_provider.h"
#include "numeric/numeric_service.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <string.h>

#define R2_WORLD_DELTA_COUNT 5000U
#define R2_WORLD_CHUNK_CELLS (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

static uint32_t r2WorldChecks;

static void R2WorldExpect(bool condition, const char *name)
{
    ++r2WorldChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("r2_01 world shift check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t R2WorldNext(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

/* Проверяет, что правки, сложенные в один чанк в порядке order, читаются
 * ровно как ожидается, и что после удаления в обратном порядке не остаётся
 * ни одной правки. */
static void R2WorldToggleChunk(uint32_t seed)
{
    static uint32_t order[R2_WORLD_DELTA_COUNT];
    static BlockType expected[R2_WORLD_CHUNK_CELLS];
    static uint8_t used[R2_WORLD_CHUNK_CELLS];

    memset(expected, BLOCK_AIR, sizeof(expected));
    memset(used, 0, sizeof(used));

    uint32_t state = seed;
    uint32_t filled = 0U;
    while (filled < R2_WORLD_DELTA_COUNT)
    {
        uint32_t cell = R2WorldNext(&state) % R2_WORLD_CHUNK_CELLS;
        if (used[cell] != 0U)
        {
            continue;
        }
        used[cell] = 1U;
        order[filled] = cell;
        ++filled;
    }

    World *world = WorldCreate(NULL);
    R2WorldExpect(world != NULL, "world create");

    for (uint32_t index = 0U; index < R2_WORLD_DELTA_COUNT; ++index)
    {
        uint32_t cell = order[index];
        int64_t x = (int64_t)(cell / (CHUNK_SIZE * CHUNK_SIZE));
        int64_t y = (int64_t)((cell / CHUNK_SIZE) % CHUNK_SIZE);
        int64_t z = (int64_t)(cell % CHUNK_SIZE);
        BlockType block = (BlockType)(1U + (index % 255U));
        R2WorldExpect(WorldTrySetBlock(world, x, y, z, block),
            "single set succeeds");
        expected[cell] = block;
    }

    for (uint32_t index = 0U; index < R2_WORLD_DELTA_COUNT; ++index)
    {
        uint32_t cell = order[index];
        int64_t x = (int64_t)(cell / (CHUNK_SIZE * CHUNK_SIZE));
        int64_t y = (int64_t)((cell / CHUNK_SIZE) % CHUNK_SIZE);
        int64_t z = (int64_t)(cell % CHUNK_SIZE);
        R2WorldExpect(WorldGetBlock(world, x, y, z) == expected[cell],
            "single read matches written value");
    }

    /* Удаление в обратном порядке: если сдвиг влево теряет или повторяет
     * элемент, хотя бы одно чтение ниже разойдётся. */
    for (uint32_t index = R2_WORLD_DELTA_COUNT; index > 0U; --index)
    {
        uint32_t cell = order[index - 1U];
        int64_t x = (int64_t)(cell / (CHUNK_SIZE * CHUNK_SIZE));
        int64_t y = (int64_t)((cell / CHUNK_SIZE) % CHUNK_SIZE);
        int64_t z = (int64_t)(cell % CHUNK_SIZE);
        R2WorldExpect(WorldTrySetBlock(world, x, y, z, BLOCK_AIR),
            "single clear succeeds");
        R2WorldExpect(WorldGetBlock(world, x, y, z) == BLOCK_AIR,
            "cleared block is air");
    }

    static BlockType region[CHUNK_SIZE * CHUNK_SIZE];
    WorldRegionContents contents = WorldFillRegion(world, 0, 0, 0,
        CHUNK_SIZE, CHUNK_SIZE, 1, region);
    R2WorldExpect(contents == WORLD_REGION_ALL_AIR,
        "emptied chunk region is all air");

    WorldDestroy(world);
}

/* Первый и последний локальный индекс чанка — крайние позиции сдвига. */
static void R2WorldBoundaryCells(void)
{
    World *world = WorldCreate(NULL);
    R2WorldExpect(world != NULL, "boundary world create");

    R2WorldExpect(WorldTrySetBlock(world, 0, 0, 0, 3), "set first cell");
    R2WorldExpect(WorldTrySetBlock(world, CHUNK_SIZE - 1, CHUNK_SIZE - 1,
        CHUNK_SIZE - 1, 4), "set last cell");
    R2WorldExpect(WorldGetBlock(world, 0, 0, 0) == 3, "first cell value");
    R2WorldExpect(WorldGetBlock(world, CHUNK_SIZE - 1, CHUNK_SIZE - 1,
        CHUNK_SIZE - 1) == 4, "last cell value");

    /* Отрицательные координаты: деление с округлением вниз и локальный
     * индекс ровно на границе чанка. */
    R2WorldExpect(WorldTrySetBlock(world, -1, -1, -1, 6), "set negative cell");
    R2WorldExpect(WorldTrySetBlock(world, -CHUNK_SIZE, -CHUNK_SIZE, -CHUNK_SIZE, 7),
        "set negative chunk origin");
    R2WorldExpect(WorldGetBlock(world, -1, -1, -1) == 6, "negative cell value");
    R2WorldExpect(WorldGetBlock(world, -CHUNK_SIZE, -CHUNK_SIZE, -CHUNK_SIZE) == 7,
        "negative chunk origin value");
    R2WorldExpect(WorldGetBlock(world, -1, -1, 0) == BLOCK_AIR,
        "neighbour of negative cell is air");

    WorldDestroy(world);
}

static int64_t r2BaseCalls;

static BlockType R2WorldBaseGetBlock(void *context, int64_t x, int64_t y,
    int64_t z)
{
    (void)context;
    ++r2BaseCalls;
    return (BlockType)((x + y + z) % 3 == 0 ? 5U : BLOCK_AIR);
}

/* Правка, равная значению провайдера, не должна оставлять sparse override. */
static void R2WorldProviderBaseEqual(void)
{
    WorldBaseProvider provider;
    provider.context = NULL;
    provider.getBlock = R2WorldBaseGetBlock;
    provider.fillRegion = NULL;
    provider.rebase = NULL;

    World *world = WorldCreate(&provider);
    R2WorldExpect(world != NULL, "provider world create");

    R2WorldExpect(WorldGetBlock(world, 0, 0, 0) == 5, "provider base value");
    R2WorldExpect(WorldTrySetBlock(world, 0, 0, 0, 5), "set equal to base");
    R2WorldExpect(WorldGetBlock(world, 0, 0, 0) == 5, "value stays base");
    R2WorldExpect(WorldTrySetBlock(world, 0, 0, 0, 8), "set override");
    R2WorldExpect(WorldGetBlock(world, 0, 0, 0) == 8, "override value");
    R2WorldExpect(WorldTrySetBlock(world, 0, 0, 0, 5), "clear back to base");
    R2WorldExpect(WorldGetBlock(world, 0, 0, 0) == 5, "base value restored");

    WorldDestroy(world);
}

/* Пакетный путь использует те же сдвиги на буфере дельт; берём и крупный
 * (хеш-ветка), и мелкий (линейная ветка) пакеты. */
static void R2WorldBatchRoundTrip(uint32_t count)
{
    WorldBlockMutation *mutations = PlatformAllocate(
        (size_t)count * sizeof(*mutations), false);
    R2WorldExpect(mutations != NULL, "batch mutations allocated");
    if (mutations == NULL)
    {
        return;
    }

    World *world = WorldCreate(NULL);
    R2WorldExpect(world != NULL, "batch world create");

    for (uint32_t index = 0U; index < count; ++index)
    {
        int64_t x = (int64_t)(index % CHUNK_SIZE);
        int64_t y = (int64_t)((index / CHUNK_SIZE) % CHUNK_SIZE);
        int64_t z = (int64_t)((index / (CHUNK_SIZE * CHUNK_SIZE)) % CHUNK_SIZE);
        mutations[index].block[0] = x;
        mutations[index].block[1] = y;
        mutations[index].block[2] = z;
        mutations[index].expected = BLOCK_AIR;
        mutations[index].replacement = (BlockType)(1U + (index % 255U));
    }
    R2WorldExpect(WorldApplyBlockBatch(world, mutations, count),
        "batch apply succeeds");
    for (uint32_t index = 0U; index < count; ++index)
    {
        R2WorldExpect(WorldGetBlock(world, mutations[index].block[0],
            mutations[index].block[1], mutations[index].block[2])
            == mutations[index].replacement, "batch value present");
    }

    for (uint32_t index = 0U; index < count; ++index)
    {
        mutations[index].expected = mutations[index].replacement;
        mutations[index].replacement = BLOCK_AIR;
    }
    R2WorldExpect(WorldApplyBlockBatch(world, mutations, count),
        "batch revert succeeds");
    for (uint32_t index = 0U; index < count; ++index)
    {
        R2WorldExpect(WorldGetBlock(world, mutations[index].block[0],
            mutations[index].block[1], mutations[index].block[2]) == BLOCK_AIR,
            "batch revert air");
    }

    WorldDestroy(world);
    PlatformFree(mutations);
}

LAIUE_TEST_ENTRY(R2WorldShiftTestEntryPoint)
{
    WorldSetNumericService(LaiueNumericGetStaticServiceV1());
    R2WorldToggleChunk(0x12345678U);
    R2WorldToggleChunk(0x9E3779B9U);
    R2WorldBoundaryCells();
    R2WorldProviderBaseEqual();
    R2WorldBatchRoundTrip(4000U); // крупный пакет: хеш-ветка
    R2WorldBatchRoundTrip(8U);    // мелкий пакет: линейная ветка

    if (r2WorldChecks == 0U)
    {
        LaiueTestRuntimeWrite("r2_01 world shift: no checks ran\r\n");
        LaiueTestRuntimeExit(1);
    }
    LAIUE_TEST_SUCCESS();
}
