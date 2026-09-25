// Ручной benchmark мешера чанка. В ALL не входит и в CTest не регистрируется:
// его запускают осознанно и читают глазами.
//
// Наполнения подобраны по тому, что они делают с greedy-слиянием, а не по
// правдоподобию: от «сливается всё» до «слить нельзя ничего». Один и тот же
// чанк на разных наполнениях стоит по-разному в десятки раз, поэтому одна
// усреднённая цифра о мешере не говорит ничего.
//
// Числа здесь — про построение меша одного чанка. Называть их процентами
// кадра игры нельзя: сколько чанков строится за кадр и сколько при этом
// делает рендер, здесь не измеряется вовсе.

#include "mesh/chunk_mesher.h"
#include "platform/system.h"
#include "render/chunk_geometry.h"
#include "test_runtime.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define MESHER_SAMPLES 7u
#define MESHER_ITERATIONS 40u

static volatile uint64_t mesherSink;

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

// Четыре знака после запятой: разница между вариантами здесь измеряется
// сотыми долями миллисекунды.
static void WriteMilliseconds(double value)
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

typedef enum MesherFill
{
    MESHER_EMPTY,
    MESHER_SLAB,          // сплошная плита одного материала: сливается всё
    MESHER_TERRAIN_RUNS,  // ступенчатая поверхность, материал постоянен по колонне
    MESHER_TERRAIN_MIXED, // та же поверхность, материал меняется на каждом блоке
    MESHER_TERRAIN_BANDS, // та же поверхность, материал меняется каждые восемь блоков
    MESHER_SPARSE,        // редкие блоки: много мелких квадов
    MESHER_CHECKER,       // шахматный порядок: слить нельзя ничего
    // Те же наполнения, но заселены и все двадцать шесть соседних чанков.
    // Регион мешера — 66^3 вокруг чанка, поэтому он задевает по одному слою
    // блоков от каждого соседа, и выборка мира обязана их вернуть. Сколько
    // это стоит, на одиноком чанке не видно вовсе.
    MESHER_TERRAIN_RUNS_NEIGHBOURS,
    MESHER_CHECKER_NEIGHBOURS,
    MESHER_SINGLE_BLOCK, // одна занятая колонна: цена подготовки масок без плотной работы
    MESHER_EDGE_LINES,   // три ребра от общего угла: 190 блоков
    MESHER_RARE_BLOCKS,  // 64 несоприкасающихся блока, разнесённых по чанку
    MESHER_SPARSE_2048,
    MESHER_SPARSE_4096,
    MESHER_SPARSE_8192,
    MESHER_SPARSE_16384,
    MESHER_FILL_COUNT
} MesherFill;

static const char *FillName(MesherFill fill)
{
    static const char *names[MESHER_FILL_COUNT] = {
        "empty",
        "slab",
        "terrain_runs",
        "terrain_mixed",
        "terrain_bands",
        "sparse",
        "checker",
        "terrain_runs+neighbours",
        "checker+neighbours",
        "single_block",
        "edge_lines",
        "rare_blocks",
        "sparse_2048",
        "sparse_4096",
        "sparse_8192",
        "sparse_16384",
    };
    return names[fill];
}

static BlockType FillBlock(MesherFill fill, int64_t x, int64_t y, int64_t z)
{
    int64_t height = 20 + (((x * 7 + y * 13) % 24 + 24) % 24);
    BlockType columnMaterial = (BlockType)(1u + (uint32_t)(((x + y) % 4 + 4) % 4));
    switch (fill)
    {
    case MESHER_EMPTY:
        return BLOCK_AIR;
    case MESHER_SLAB:
        return z >= 0 && z < 24 ? (BlockType)3u : BLOCK_AIR;
    case MESHER_TERRAIN_RUNS:
    case MESHER_TERRAIN_RUNS_NEIGHBOURS:
        return z < height ? columnMaterial : BLOCK_AIR;
    case MESHER_TERRAIN_MIXED:
        return z < height ? (BlockType)(1u + (uint32_t)(((x + y + z) % 4 + 4) % 4)) : BLOCK_AIR;
    case MESHER_TERRAIN_BANDS:
        // Слои по восемь блоков: полоса граней длинная, но обрывается
        // материалом, а не концом поверхности.
        return z < height ? (BlockType)(1u + (uint32_t)((z & 63) >> 3) +
                                        (uint32_t)(((x + y) % 3 + 3) % 3) * 8u)
                          : BLOCK_AIR;
    case MESHER_SPARSE:
        return ((x % 2) + 2) % 2 == 0 && ((y % 2) + 2) % 2 == 0 && ((z % 2) + 2) % 2 == 0
                   ? (BlockType)5u
                   : BLOCK_AIR;
    case MESHER_SINGLE_BLOCK:
        return x == 32 && y == 32 && z == 32 ? (BlockType)7u : BLOCK_AIR;
    case MESHER_EDGE_LINES:
        return (y == 0 && z == 0) || (x == 0 && z == 0) || (x == 0 && y == 0) ? (BlockType)9u
                                                                              : BLOCK_AIR;
    case MESHER_RARE_BLOCKS:
        return x % 16 == 0 && y % 16 == 0 && z % 16 == 0 ? (BlockType)11u : BLOCK_AIR;
    case MESHER_SPARSE_2048:
    case MESHER_SPARSE_4096:
    case MESHER_SPARSE_8192:
    case MESHER_SPARSE_16384:
    {
        uint32_t spacingZ = 8u >> (uint32_t)(fill - MESHER_SPARSE_2048);
        return x % 4 == 0 && y % 4 == 0 && z % spacingZ == 0 ? (BlockType)13u : BLOCK_AIR;
    }
    default:
        return (((x + y + z) & 1) == 0) ? (BlockType)4u : BLOCK_AIR;
    }
}

static bool FillsNeighbours(MesherFill fill)
{
    return fill == MESHER_TERRAIN_RUNS_NEIGHBOURS || fill == MESHER_CHECKER_NEIGHBOURS;
}

static void Fill(World *world, MesherFill fill)
{
    int64_t low = FillsNeighbours(fill) ? -CHUNK_SIZE : 0;
    int64_t high = FillsNeighbours(fill) ? 2 * CHUNK_SIZE : CHUNK_SIZE;
    for (int64_t x = low; x < high; ++x)
    {
        for (int64_t y = low; y < high; ++y)
        {
            for (int64_t z = low; z < high; ++z)
            {
                BlockType block = FillBlock(fill, x, y, z);
                if (block != BLOCK_AIR)
                {
                    WorldSetBlock(world, x, y, z, block);
                }
            }
        }
    }
}

// Независимый счёт граней чанка: для каждого непустого блока и каждого из
// шести направлений грань есть ровно тогда, когда сосед пуст. Столько же
// элементарных граней видит и мешер до greedy-слияния, поэтому это ровно та
// ёмкость, которую BuildChunkMesh выделяет под выдачу до укорачивания.
static uint32_t CountVisibleFaces(World *world)
{
    static const int32_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    uint32_t total = 0u;
    for (int64_t x = 0; x < CHUNK_SIZE; ++x)
    {
        for (int64_t y = 0; y < CHUNK_SIZE; ++y)
        {
            for (int64_t z = 0; z < CHUNK_SIZE; ++z)
            {
                if (WorldGetBlock(world, x, y, z) == BLOCK_AIR)
                {
                    continue;
                }
                for (uint32_t face = 0u; face < 6u; ++face)
                {
                    if (WorldGetBlock(world, x + offsets[face][0], y + offsets[face][1],
                            z + offsets[face][2]) == BLOCK_AIR)
                    {
                        ++total;
                    }
                }
            }
        }
    }
    return total;
}

static void RunCase(MesherFill fill)
{
    World *world = WorldCreate(NULL);
    ChunkMesherScratch *scratch = ChunkMesherScratchCreate();
    if (world == NULL || scratch == NULL)
    {
        WriteText("mesher benchmark setup failed\n");
        LaiueTestRuntimeExit(1);
    }
    Fill(world, fill);

    // Прогрев: первый проход платит за страницы и кэш.
    ChunkQuad *quads = NULL;
    uint32_t quadCount = 0u;
    if (!BuildChunkMesh(world, scratch, 0, 0, 0, &quads, &quadCount))
    {
        WriteText("mesher benchmark build failed\n");
        LaiueTestRuntimeExit(1);
    }
    uint32_t reported = quadCount;
    if (quads != NULL)
    {
        PlatformFree(quads);
    }

    double best = 0.0;
    for (uint32_t sample = 0u; sample < MESHER_SAMPLES; ++sample)
    {
        double begin = PlatformMonotonicSeconds();
        for (uint32_t iteration = 0u; iteration < MESHER_ITERATIONS; ++iteration)
        {
            ChunkQuad *pass = NULL;
            uint32_t passCount = 0u;
            if (!BuildChunkMesh(world, scratch, 0, 0, 0, &pass, &passCount))
            {
                WriteText("mesher benchmark build failed\n");
                LaiueTestRuntimeExit(1);
            }
            mesherSink += passCount;
            if (pass != NULL)
            {
                PlatformFree(pass);
            }
        }
        double elapsed = PlatformMonotonicSeconds() - begin;
        if (sample == 0u || elapsed < best)
        {
            best = elapsed;
        }
    }

    uint32_t faces = CountVisibleFaces(world);
    WriteText("mesher fill=");
    WriteText(FillName(fill));
    WriteText(" quads=");
    WriteUnsigned(reported);
    WriteText(" faces=");
    WriteUnsigned(faces);
    WriteText(" quad_bytes=");
    WriteUnsigned((uint64_t)reported * sizeof(ChunkQuad));
    WriteText(" capacity_bytes=");
    WriteUnsigned((uint64_t)faces * sizeof(ChunkQuad));
    WriteText(" best_ms=");
    WriteMilliseconds(best * 1000.0 / (double)MESHER_ITERATIONS);
    WriteText("\n");

    ChunkMesherScratchDestroy(scratch);
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(MesherBenchmarkEntryPoint)
{
    // Для A/B отдельного наполнения без истории аллокаций предыдущих сцен.
    char selected[64];
    uint32_t selectedLength = PlatformGetEnvironmentUtf8("LAIUE_MESHER_BENCHMARK_FILL", selected,
                                                         (uint32_t)sizeof(selected));
    if (selectedLength >= sizeof(selected))
    {
        WriteText("mesher benchmark fill name is too long\n");
        LaiueTestRuntimeExit(1);
    }
    bool matched = false;
    WriteText("laiue mesher benchmark\n");
    for (uint32_t fill = 0u; fill < (uint32_t)MESHER_FILL_COUNT; ++fill)
    {
        if (selectedLength != 0u)
        {
            const char *name = FillName((MesherFill)fill);
            uint32_t index = 0u;
            while (index < selectedLength && name[index] != '\0' && name[index] == selected[index])
            {
                ++index;
            }
            if (index != selectedLength || name[index] != '\0')
            {
                continue;
            }
        }
        matched = true;
        RunCase((MesherFill)fill);
    }
    if (!matched)
    {
        WriteText("mesher benchmark unknown fill\n");
        LaiueTestRuntimeExit(1);
    }
    WriteText("mesher benchmark done sink=");
    WriteUnsigned(mesherSink != 0u ? 1u : 0u);
    WriteText("\n");
    LAIUE_TEST_SUCCESS();
}
