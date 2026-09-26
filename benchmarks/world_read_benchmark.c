// Ручной benchmark точечного чтения мира и заполнения регионов. В ALL не
// входит и в CTest не регистрируется: его запускают осознанно и читают
// глазами.
//
// Две нагрузки, ради которых он написан:
//
//   * `WorldGetBlock` — то, чем физика обходит ядро тела: миллионы вызовов
//     за кадр в мире с локальными правками.
//   * `WorldFillRegion` — то, чем мешер берёт регион 66^3 вокруг чанка:
//     заполнение базового слоя, применение правок и разбор содержимого.
//
// Состояния мира подобраны так, чтобы отделить быстрый путь пустого мира от
// поиска в таблице и от разбора дельт. Печатается минимум и медиана по
// раундам и детерминированная контрольная сумма ответов: последняя должна
// совпадать у baseline и candidate побайтово, иначе сравнивать время нельзя.
//
// Числа здесь — про один вызов; долей кадра игры они не являются.

#include "platform/system.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define WBR_ROUNDS 13u

#define WBR_GETBLOCK_REPEATS 4u
#define WBR_FILL_REPEATS 150u

#define WBR_REGION_SPAN 66
#define WBR_REGION_CELLS ((size_t)WBR_REGION_SPAN * WBR_REGION_SPAN * WBR_REGION_SPAN)

static volatile uint64_t wbrSink;
static uint64_t wbrChecksum;

static BlockType wbrRegion[WBR_REGION_CELLS];

// === Вывод без CRT ===

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
    for (uint32_t index = 0; index < length; ++index)
    {
        text[index] = digits[length - index - 1u];
    }
    text[length] = '\0';
    WriteText(text);
}

// Четыре знака после запятой: разница между состояниями мира здесь —
// доли наносекунды на вызов.
static void WriteFixed(double value)
{
    if (!(value > 0.0))
    {
        WriteText("0.0000");
        return;
    }
    uint64_t scaled = (uint64_t)(value * 10000.0 + 0.5);
    WriteUnsigned(scaled / 10000u);
    WriteText(".");
    uint64_t fraction = scaled % 10000u;
    if (fraction < 1000u)
    {
        WriteText("0");
    }
    if (fraction < 100u)
    {
        WriteText("0");
    }
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

static void ReportCase(const char *name, const double *samples, uint32_t count,
                       uint64_t checksum)
{
    double sorted[WBR_ROUNDS];
    double minimum = samples[0];
    for (uint32_t index = 0u; index < count; ++index)
    {
        sorted[index] = samples[index];
        if (samples[index] < minimum)
        {
            minimum = samples[index];
        }
    }
    WriteText("WBR case=");
    WriteText(name);
    WriteText(" min_ns=");
    WriteFixed(minimum);
    WriteText(" med_ns=");
    WriteFixed(Median(sorted, count));
    WriteText(" max_ns=");
    WriteFixed(sorted[count - 1u]);
    WriteText(" checksum=");
    WriteUnsigned(checksum);
    WriteText("\n");
}

// === Провайдер ===

// Дёшево и детерминированно: ниже нуля сплошной блок одного материала,
// выше — воздух. Такой провайдер стоит около наносекунды на вызов, как в
// игре, и не прячет за собой обвязку мира.
typedef struct BenchProvider
{
    uint32_t unused;
} BenchProvider;

static BlockType BenchProviderGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    (void)rawContext;
    (void)x;
    (void)z;
    return y < 0 ? (BlockType)3U : BLOCK_AIR;
}

static WorldRegionContents BenchProviderFillRegion(void *rawContext, int64_t minBlockX,
                                                    int64_t minBlockY, int64_t minBlockZ, int32_t sizeX,
                                                    int32_t sizeY, int32_t sizeZ, BlockType *outBlocks)
{
    (void)rawContext;
    (void)minBlockX;
    (void)minBlockZ;
    bool anyAir = false;
    bool anySolid = false;
    size_t index = 0u;
    for (int32_t y = 0; y < sizeY; ++y)
    {
        BlockType value = (minBlockY + y) < 0 ? (BlockType)3U : BLOCK_AIR;
        anyAir |= value == BLOCK_AIR;
        anySolid |= value != BLOCK_AIR;
        for (int32_t x = 0; x < sizeX; ++x)
        {
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                outBlocks[index++] = value;
            }
        }
    }
    if (anyAir && anySolid)
    {
        return WORLD_REGION_MIXED;
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}

// === Мир с правками ===

static uint64_t MixChecksum(uint64_t checksum, uint64_t value)
{
    checksum ^= value;
    checksum *= UINT64_C(1099511628211);
    return checksum;
}

// Правки кладутся только непустыми: пустая правка поверх пустого базового
// слоя ничего бы не изменила и лишний раз трогала бы таблицу.
static void ApplyEdit(World *world, int64_t x, int64_t y, int64_t z, BlockType material)
{
    if (WorldGetBlock(world, x, y, z) == material)
    {
        return;
    }
    WorldSetBlock(world, x, y, z, material);
}

// Сплошная плита толщиной 24 блока в куске [low, high)^3: даёт длинные
// отрезки подряд идущих локальных индексов, на которых и держится быстрый
// путь записи региона.
static void FillSlabChunk(World *world, int64_t chunkX, int64_t chunkY, int64_t chunkZ)
{
    for (int64_t x = 0; x < CHUNK_SIZE; ++x)
    {
        for (int64_t z = 0; z < CHUNK_SIZE; ++z)
        {
            for (int64_t y = 0; y < 24; ++y)
            {
                BlockType material = (BlockType)(1u + (uint32_t)((x + z) & 3u));
                ApplyEdit(world, chunkX * CHUNK_SIZE + x, chunkY * CHUNK_SIZE + y,
                          chunkZ * CHUNK_SIZE + z, material);
            }
        }
    }
}

// Заполнить двадцать шесть соседей центрального чанка разреженно: проверяет
// обход узких кусков соседей, не раздувая подготовку.
static void FillSparseNeighbours(World *world)
{
    for (int64_t chunkX = -1; chunkX <= 1; ++chunkX)
    {
        for (int64_t chunkY = -1; chunkY <= 1; ++chunkY)
        {
            for (int64_t chunkZ = -1; chunkZ <= 1; ++chunkZ)
            {
                if (chunkX == 0 && chunkY == 0 && chunkZ == 0)
                {
                    continue;
                }
                for (int64_t x = 0; x < CHUNK_SIZE; x += 4)
                {
                    for (int64_t z = 0; z < CHUNK_SIZE; z += 4)
                    {
                        for (int64_t y = 0; y < CHUNK_SIZE; y += 2)
                        {
                            BlockType material = (BlockType)(5u + (uint32_t)((x + y + z) & 3u));
                            ApplyEdit(world, chunkX * CHUNK_SIZE + x, chunkY * CHUNK_SIZE + y,
                                      chunkZ * CHUNK_SIZE + z, material);
                        }
                    }
                }
            }
        }
    }
}

// === Точечное чтение ===

#define WBR_QUERY_X 128
#define WBR_QUERY_Z 128
#define WBR_QUERY_Y 5
#define WBR_QUERY_COUNT \
    ((size_t)WBR_QUERY_X * (size_t)WBR_QUERY_Z * (size_t)WBR_QUERY_Y)

static int64_t wbrQueries[WBR_QUERY_COUNT][3];

static void BuildQueries(void)
{
    size_t index = 0u;
    for (int32_t y = -2; y <= 2; ++y)
    {
        for (int32_t x = 0; x < WBR_QUERY_X; ++x)
        {
            for (int32_t z = 0; z < WBR_QUERY_Z; ++z)
            {
                wbrQueries[index][0] = x;
                wbrQueries[index][1] = y;
                wbrQueries[index][2] = z;
                ++index;
            }
        }
    }
}

static uint64_t RunGetBlockSample(World *world, double *outNanoseconds)
{
    uint64_t checksum = UINT64_C(1469598103934665603);
    double start = PlatformMonotonicSeconds();
    for (uint32_t repeat = 0u; repeat < WBR_GETBLOCK_REPEATS; ++repeat)
    {
        for (size_t index = 0u; index < WBR_QUERY_COUNT; ++index)
        {
            BlockType block = WorldGetBlock(world, wbrQueries[index][0], wbrQueries[index][1],
                                            wbrQueries[index][2]);
            checksum = MixChecksum(checksum, block);
        }
    }
    double elapsed = PlatformMonotonicSeconds() - start;
    *outNanoseconds =
        elapsed * 1.0e9 / ((double)WBR_QUERY_COUNT * (double)WBR_GETBLOCK_REPEATS);
    return checksum;
}

static void BenchmarkGetBlock(const char *name, World *world)
{
    double samples[WBR_ROUNDS];
    uint64_t checksum = 0u;
    for (uint32_t round = 0u; round < WBR_ROUNDS + 1u; ++round)
    {
        double nanoseconds = 0.0;
        uint64_t current = RunGetBlockSample(world, &nanoseconds);
        if (round == 0u)
        {
            continue; // прогрев
        }
        samples[round - 1u] = nanoseconds;
        checksum = current;
    }
    ReportCase(name, samples, WBR_ROUNDS, checksum);
    wbrSink += checksum;
}

// === Заполнение региона ===

static uint64_t ChecksumRegion(const BlockType *blocks, size_t count)
{
    uint64_t checksum = UINT64_C(1469598103934665603);
    for (size_t index = 0u; index < count; ++index)
    {
        checksum = MixChecksum(checksum, blocks[index]);
    }
    return checksum;
}

static void BenchmarkFillRegion(const char *name, World *world,
                                int64_t minX, int64_t minY, int64_t minZ, int32_t span)
{
    double samples[WBR_ROUNDS];
    WorldRegionContents lastContents = WORLD_REGION_ALL_AIR;
    for (uint32_t round = 0u; round < WBR_ROUNDS + 1u; ++round)
    {
        double start = PlatformMonotonicSeconds();
        for (uint32_t repeat = 0u; repeat < WBR_FILL_REPEATS; ++repeat)
        {
            lastContents = WorldFillRegion(world, minX, minY, minZ, span, span, span, wbrRegion);
        }
        double elapsed = PlatformMonotonicSeconds() - start;
        if (round == 0u)
        {
            continue; // прогрев
        }
        samples[round - 1u] = elapsed * 1.0e9 / (double)WBR_FILL_REPEATS;
    }
    uint64_t checksum = ChecksumRegion(wbrRegion,
        (size_t)span * (size_t)span * (size_t)span) ^ (uint64_t)lastContents;
    ReportCase(name, samples, WBR_ROUNDS, checksum);
    wbrSink += checksum;
}

// === Сценарии ===

static World *CreateProviderWorld(BenchProvider *provider, bool withFillRegion)
{
    WorldBaseProvider description = {0};
    description.context = provider;
    description.getBlock = BenchProviderGetBlock;
    if (withFillRegion)
    {
        description.fillRegion = BenchProviderFillRegion;
    }
    return WorldCreate(&description);
}

static void RunGetBlockCases(void)
{
    BenchProvider provider = {0};

    World *unedited = CreateProviderWorld(&provider, true);
    if (unedited == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    BenchmarkGetBlock("getblock.unedited", unedited);
    WorldDestroy(unedited);

    World *distantWorld = CreateProviderWorld(&provider, true);
    if (distantWorld == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    for (int64_t chunk = 0; chunk < 64; ++chunk)
    {
        ApplyEdit(distantWorld, 100000 + chunk * CHUNK_SIZE, 0, 200000 + chunk * CHUNK_SIZE,
                  (BlockType)(10u + (uint32_t)chunk));
    }
    BenchmarkGetBlock("getblock.far", distantWorld);
    WorldDestroy(distantWorld);

    World *zone = CreateProviderWorld(&provider, true);
    if (zone == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    for (int64_t chunk = 0; chunk < 16; ++chunk)
    {
        int64_t baseX = (chunk & 3) * 32;
        int64_t baseZ = (chunk >> 2) * 32;
        for (int64_t index = 0; index < 512; ++index)
        {
            int64_t x = baseX + (index % 32);
            int64_t z = baseZ + ((index / 32) % 32);
            int64_t y = index & 3;
            BlockType material = (BlockType)(20u + (uint32_t)(index & 127u));
            if (WorldGetBlock(zone, x, y, z) != material)
            {
                WorldSetBlock(zone, x, y, z, material);
            }
        }
    }
    BenchmarkGetBlock("getblock.zone", zone);
    WorldDestroy(zone);
}

static void RunFillCases(void)
{
    BenchProvider provider = {0};

    World *nullEmpty = WorldCreate(NULL);
    if (nullEmpty == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    BenchmarkFillRegion("fill.null_empty", nullEmpty, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(nullEmpty);

    // Единственная правка далеко за пределами региона: таблица непуста, но в
    // регион не попадает ни одна дельта, и весь он — воздух. Отделяет разбор
    // содержимого от обхода чанков и от записи базового слоя.
    World *nullFarEdit = WorldCreate(NULL);
    if (nullFarEdit == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    ApplyEdit(nullFarEdit, 100000, 0, 200000, (BlockType)17U);
    BenchmarkFillRegion("fill.null_faredit", nullFarEdit, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(nullFarEdit);

    // Регион ровно из одного сплошного чанка: весь он непуст, и разбор обязан
    // прочитать все ячейки. Это тот же однородный путь, что и у полностью
    // пустого региона, только маски совпадений с нулём нулевые.
    World *nullSolid = WorldCreate(NULL);
    if (nullSolid == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    for (int64_t x = 0; x < CHUNK_SIZE; ++x)
    {
        for (int64_t y = 0; y < CHUNK_SIZE; ++y)
        {
            for (int64_t z = 0; z < CHUNK_SIZE; ++z)
            {
                WorldSetBlock(nullSolid, x, y, z,
                              (BlockType)(1u + (uint32_t)((x + y + z) & 3u)));
            }
        }
    }
    BenchmarkFillRegion("fill.null_solid", nullSolid, 0, 0, 0, CHUNK_SIZE);
    WorldDestroy(nullSolid);

    World *nullCentral = WorldCreate(NULL);
    if (nullCentral == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    FillSlabChunk(nullCentral, 0, 0, 0);
    BenchmarkFillRegion("fill.null_central", nullCentral, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(nullCentral);

    World *nullNeighbours = WorldCreate(NULL);
    if (nullNeighbours == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    FillSlabChunk(nullNeighbours, 0, 0, 0);
    FillSparseNeighbours(nullNeighbours);
    BenchmarkFillRegion("fill.null_neighbours", nullNeighbours, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(nullNeighbours);

    World *providerEmpty = CreateProviderWorld(&provider, true);
    if (providerEmpty == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    BenchmarkFillRegion("fill.provider_empty", providerEmpty, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(providerEmpty);

    World *providerCentral = CreateProviderWorld(&provider, true);
    if (providerCentral == NULL)
    {
        LaiueTestRuntimeExit(1);
    }
    FillSlabChunk(providerCentral, 0, 0, 0);
    BenchmarkFillRegion("fill.provider_central", providerCentral, -1, -1, -1, WBR_REGION_SPAN);
    WorldDestroy(providerCentral);
}

LAIUE_TEST_ENTRY(WorldReadBenchmarkEntryPoint)
{
    char selection[64];
    uint32_t selectionLength = PlatformGetEnvironmentUtf8(
        "LAIUE_WORLD_BENCHMARK_GROUP", selection, (uint32_t)sizeof(selection));
    if (selectionLength >= sizeof(selection))
    {
        WriteText("world benchmark group name is too long\n");
        LaiueTestRuntimeExit(1);
    }

    WriteText("laiue world read benchmark\n");
    BuildQueries();

    bool runGetBlock = selectionLength == 0u || selection[0] == 'g';
    bool runFill = selectionLength == 0u || selection[0] == 'f';
    if (runGetBlock)
    {
        RunGetBlockCases();
    }
    if (runFill)
    {
        RunFillCases();
    }

    WriteText("world benchmark done sink=");
    WriteUnsigned(wbrSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
