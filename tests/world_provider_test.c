#include "world/world.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <limits.h>

typedef struct ProviderContext
{
    int64_t origin[3];
    uint32_t getCalls;
    uint32_t fillCalls;
    uint32_t rebaseCalls;
    bool rejectRebase;
} ProviderContext;

static uint32_t worldProviderChecks;

static void ProviderExpect(bool condition, const char *name)
{
    ++worldProviderChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("World provider check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0U;
    while (left[index] != L'\0' || right[index] != L'\0')
    {
        if (left[index] != right[index])
        {
            return false;
        }
        ++index;
    }
    return true;
}

static BlockType ProviderPattern(const ProviderContext *context, int64_t x, int64_t y, int64_t z)
{
    int64_t absoluteX = context->origin[0] + x;
    int64_t absoluteY = context->origin[1] + y;
    int64_t absoluteZ = context->origin[2] + z;
    int64_t sum = absoluteX + absoluteY * 3 + absoluteZ * 5;
    return sum % 4 == 0 ? BLOCK_AIR : (BlockType)9U;
}

static BlockType ProviderGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->getCalls;
    return ProviderPattern(context, x, y, z);
}

static WorldRegionContents ProviderFillRegion(void *rawContext, int64_t minBlockX,
                                              int64_t minBlockY, int64_t minBlockZ, int32_t sizeX,
                                              int32_t sizeY, int32_t sizeZ, BlockType *outBlocks)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->fillCalls;
    bool anyAir = false;
    bool anySolid = false;
    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                size_t index =
                    (((size_t)y * (size_t)sizeX) + (size_t)x) * (size_t)sizeZ + (size_t)z;
                BlockType block =
                    ProviderPattern(context, minBlockX + x, minBlockY + y, minBlockZ + z);
                outBlocks[index] = block;
                anyAir |= block == BLOCK_AIR;
                anySolid |= block != BLOCK_AIR;
            }
        }
    }
    if (anyAir && anySolid)
    {
        return WORLD_REGION_MIXED;
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}

static bool ProviderRebase(void *rawContext, int64_t blockShiftX, int64_t blockShiftY,
                           int64_t blockShiftZ)
{
    ProviderContext *context = (ProviderContext *)rawContext;
    ++context->rebaseCalls;
    if (context->rejectRebase)
    {
        return false;
    }
    context->origin[0] += blockShiftX;
    context->origin[1] += blockShiftY;
    context->origin[2] += blockShiftZ;
    return true;
}

static void TestEmptyWorld(void)
{
    World *world = WorldCreate(NULL);
    ProviderExpect(world != NULL, "NULL provider did not create a world");
    ProviderExpect(WorldGetBlock(world, -9000000, 17, 9000000) == BLOCK_AIR,
                   "empty world returned a material");

    BlockType region[8];
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        region[index] = (BlockType)0xffU;
    }
    ProviderExpect(WorldFillRegion(world, -1, -1, -1, 2, 2, 2, region) == WORLD_REGION_ALL_AIR,
                   "empty region classification is not all-air");
    for (uint32_t index = 0U; index < 8U; ++index)
    {
        ProviderExpect(region[index] == BLOCK_AIR, "empty provider did not clear the region");
    }

    BlockType guard = (BlockType)0x7fU;
    ProviderExpect(WorldFillRegion(world, 0, 0, 0, 1, 0, 1, &guard) == WORLD_REGION_ALL_AIR &&
                       guard == (BlockType)0x7fU,
                   "invalid region changed its output buffer");
    WorldDestroy(world);

    WorldBaseProvider invalid = {0};
    ProviderExpect(WorldCreate(&invalid) == NULL, "provider without getBlock was accepted");
}

static void TestProviderAndMutations(void)
{
    ProviderContext context = {0};
    WorldBaseProvider provider = {
        .context = &context,
        .getBlock = ProviderGetBlock,
        .fillRegion = ProviderFillRegion,
        .rebase = ProviderRebase,
    };
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "provider world was not created");

    BlockType base = ProviderPattern(&context, 1, 2, 3);
    ProviderExpect(WorldGetBlock(world, 1, 2, 3) == base && context.getCalls == 1U,
                   "getBlock provider was not used");
    ProviderExpect(WorldGetRevision(world) == 0U, "new world revision is not zero");

    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, (BlockType)255U) &&
                       WorldGetBlock(world, 1, 2, 3) == (BlockType)255U &&
                       WorldGetRevision(world) == 1U,
                   "application material override was not published");
    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, base) &&
                       WorldGetBlock(world, 1, 2, 3) == base && WorldGetRevision(world) == 2U,
                   "restoring the provider value failed");
    ProviderExpect(WorldTrySetBlock(world, 1, 2, 3, base) && WorldGetRevision(world) == 2U,
                   "no-op mutation advanced revision");

    BlockType region[8];
    WorldRegionContents contents = WorldFillRegion(world, -1, 2, 3, 2, 2, 2, region);
    ProviderExpect(context.fillCalls == 1U, "fillRegion provider was not used");
    bool anyAir = false;
    bool anySolid = false;
    for (int32_t y = 0; y < 2; ++y)
    {
        for (int32_t x = 0; x < 2; ++x)
        {
            for (int32_t z = 0; z < 2; ++z)
            {
                size_t index = (((size_t)y * 2U) + (size_t)x) * 2U + (size_t)z;
                BlockType expected = ProviderPattern(&context, -1 + x, 2 + y, 3 + z);
                ProviderExpect(region[index] == expected,
                               "fillRegion layout or provider value is wrong");
                anyAir |= expected == BLOCK_AIR;
                anySolid |= expected != BLOCK_AIR;
            }
        }
    }
    WorldRegionContents expectedContents =
        anyAir && anySolid ? WORLD_REGION_MIXED
                           : (anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR);
    ProviderExpect(contents == expectedContents, "filled region classification is wrong");

    const int64_t first[3] = {17, 4, -2};
    const int64_t second[3] = {CHUNK_SIZE + 17, 4, -2};
    BlockType firstBase = ProviderPattern(&context, first[0], first[1], first[2]);
    BlockType secondBase = ProviderPattern(&context, second[0], second[1], second[2]);
    WorldBlockMutation batch[2] = {
        {
            .block = {first[0], first[1], first[2]},
            .expected = firstBase,
            .replacement = (BlockType)21U,
        },
        {
            .block = {second[0], second[1], second[2]},
            .expected = secondBase,
            .replacement = (BlockType)22U,
        },
    };
    uint64_t beforeBatch = WorldGetRevision(world);
    ProviderExpect(WorldApplyBlockBatch(world, batch, 2U) &&
                       WorldGetRevision(world) == beforeBatch + 2U &&
                       WorldGetBlock(world, first[0], first[1], first[2]) == (BlockType)21U &&
                       WorldGetBlock(world, second[0], second[1], second[2]) == (BlockType)22U,
                   "atomic batch did not publish all values");

    WorldBlockMutation rejected[2] = {
        {
            .block = {first[0], first[1], first[2]},
            .expected = (BlockType)21U,
            .replacement = (BlockType)31U,
        },
        {
            .block = {second[0], second[1], second[2]},
            .expected = (BlockType)99U,
            .replacement = (BlockType)32U,
        },
    };
    uint64_t beforeRejected = WorldGetRevision(world);
    ProviderExpect(!WorldApplyBlockBatch(world, rejected, 2U) &&
                       WorldGetRevision(world) == beforeRejected &&
                       WorldGetBlock(world, first[0], first[1], first[2]) == (BlockType)21U &&
                       WorldGetBlock(world, second[0], second[1], second[2]) == (BlockType)22U,
                   "rejected batch changed world state");
    ProviderExpect(WorldApplyBlockBatch(world, NULL, 0U) &&
                       WorldGetRevision(world) == beforeRejected,
                   "empty batch was not a no-op");

    WorldDestroy(world);
}

static void TestRebaseAndFormatting(void)
{
    ProviderContext context = {0};
    WorldBaseProvider provider = {
        .context = &context,
        .getBlock = ProviderGetBlock,
        .fillRegion = ProviderFillRegion,
        .rebase = ProviderRebase,
    };
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "rebase world was not created");

    const int64_t original[3] = {5, 6, 7};
    ProviderExpect(WorldTrySetBlock(world, original[0], original[1], original[2], (BlockType)42U),
                   "global override was not created");
    wchar_t before[32];
    WorldFormatAbsoluteBlockCoordinate(world, 0, original[0], before, 32U);
    ProviderExpect(WideEquals(before, L"5"), "initial coordinate formatting is wrong");

    ProviderExpect(WorldRebase(world, CHUNK_SIZE, -2 * CHUNK_SIZE, 3 * CHUNK_SIZE) &&
                       context.rebaseCalls == 1U,
                   "chunk-aligned rebase failed");
    const int64_t rebased[3] = {
        original[0] - CHUNK_SIZE,
        original[1] + 2 * CHUNK_SIZE,
        original[2] - 3 * CHUNK_SIZE,
    };
    ProviderExpect(WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "override did not retain its absolute coordinate");
    ProviderExpect(ProviderPattern(&context, rebased[0] + 1, rebased[1], rebased[2]) ==
                       WorldGetBlock(world, rebased[0] + 1, rebased[1], rebased[2]),
                   "provider did not follow the rebased local origin");
    wchar_t after[32];
    WorldFormatAbsoluteBlockCoordinate(world, 0, rebased[0], after, 32U);
    ProviderExpect(WideEquals(after, L"5"), "absolute coordinate changed after rebase");

    uint32_t callsBeforeUnaligned = context.rebaseCalls;
    ProviderExpect(!WorldRebase(world, 1, 0, 0) && context.rebaseCalls == callsBeforeUnaligned &&
                       WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "unaligned rebase was not rejected atomically");

    context.rejectRebase = true;
    wchar_t beforeRejected[32];
    WorldFormatAbsoluteBlockCoordinate(world, 1, rebased[1], beforeRejected, 32U);
    uint64_t revisionBeforeRejected = WorldGetRevision(world);
    ProviderExpect(!WorldRebase(world, CHUNK_SIZE, 0, 0) &&
                       context.rebaseCalls == callsBeforeUnaligned + 1U &&
                       WorldGetRevision(world) == revisionBeforeRejected &&
                       WorldGetBlock(world, rebased[0], rebased[1], rebased[2]) == (BlockType)42U,
                   "provider-rejected rebase changed world state");
    wchar_t afterRejected[32];
    WorldFormatAbsoluteBlockCoordinate(world, 1, rebased[1], afterRejected, 32U);
    ProviderExpect(WideEquals(beforeRejected, afterRejected),
                   "provider-rejected rebase changed coordinate origin");
    WorldDestroy(world);

    World *farWorld = WorldCreate(NULL);
    const int64_t hugeAlignedShift = INT64_MAX - (CHUNK_SIZE - 1);
    ProviderExpect(farWorld != NULL && WorldRebase(farWorld, hugeAlignedShift, 0, 0) &&
                       WorldRebase(farWorld, hugeAlignedShift, 0, 0) &&
                       WorldRebase(farWorld, hugeAlignedShift, 0, 0),
                   "infinite origin did not grow beyond 64 bits");
    wchar_t farText[32];
    WorldFormatAbsoluteBlockCoordinate(farWorld, 0, 0, farText, 32U);
    ProviderExpect(WideEquals(farText, L"~2^64"), "far coordinate formatting is wrong");
    WorldDestroy(farWorld);
}

// === Разбор региона ===
//
// WorldFillRegion заканчивается разбором содержимого: есть ли пустые ячейки,
// есть ли непустые. Разбор идёт по восемь ячеек за раз, поэтому проверять
// его надо не на круглых размерах, а ровно на хвостах: длина 1..17 и вокруг
// кратных восьми, буфер со смещённым на 1..7 адресом, ноль в каждом байте
// слова и непустые значения на всех четырёх характерных битовых картинах.
//
// Эталон здесь побайтный и намеренно тупой: пройти буфер после вызова и
// посмотреть, встретился ли ноль и встретилось ли что-то кроме нуля.

#define REGION_BUFFER_BYTES 160U
#define REGION_GUARD 8U

static BlockType regionPattern[REGION_BUFFER_BYTES];
static uint32_t regionPatternCount;

static WorldRegionContents PatternFillRegion(void *rawContext, int64_t minBlockX, int64_t minBlockY,
                                             int64_t minBlockZ, int32_t sizeX, int32_t sizeY,
                                             int32_t sizeZ, BlockType *outBlocks)
{
    (void)rawContext;
    (void)minBlockX;
    (void)minBlockY;
    (void)minBlockZ;
    size_t count = (size_t)sizeX * (size_t)sizeY * (size_t)sizeZ;
    for (size_t index = 0U; index < count; ++index)
    {
        outBlocks[index] = index < regionPatternCount ? regionPattern[index] : BLOCK_AIR;
    }
    return WORLD_REGION_MIXED;
}

static BlockType PatternGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    (void)rawContext;
    (void)x;
    (void)y;
    int64_t index = z;
    if (index < 0 || (uint64_t)index >= regionPatternCount)
    {
        return BLOCK_AIR;
    }
    return regionPattern[index];
}

static WorldRegionContents ClassifyBytes(const BlockType *cells, uint32_t count)
{
    bool anyAir = false;
    bool anySolid = false;
    for (uint32_t index = 0U; index < count; ++index)
    {
        if (cells[index] == BLOCK_AIR)
        {
            anyAir = true;
        }
        else
        {
            anySolid = true;
        }
    }
    if (anyAir && anySolid)
    {
        return WORLD_REGION_MIXED;
    }
    return anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR;
}

// Один прогон: длина, смещение адреса буфера и значение сторожевых байтов.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void RunRegionCase(World *world, uint32_t count, uint32_t offset, BlockType canary,
                          bool useFillRegion)
{
    static BlockType buffer[REGION_BUFFER_BYTES];
    for (uint32_t index = 0U; index < REGION_BUFFER_BYTES; ++index)
    {
        buffer[index] = canary;
    }
    BlockType *cells = buffer + REGION_GUARD + offset;
    WorldRegionContents contents = WorldFillRegion(world, 0, 0, 0, 1, 1, (int32_t)count, cells);
    ProviderExpect(contents == ClassifyBytes(cells, count),
                   "разбор региона разошёлся с побайтным эталоном");
    if (canary != BLOCK_AIR)
    {
        for (uint32_t index = 0U; index < REGION_GUARD + offset; ++index)
        {
            ProviderExpect(buffer[index] == canary, "запись до начала региона");
        }
        for (uint32_t index = REGION_GUARD + offset + count; index < REGION_BUFFER_BYTES; ++index)
        {
            ProviderExpect(buffer[index] == canary, "запись за концом региона");
        }
    }
    (void)useFillRegion;
}

static void RunRegionPatterns(World *world, bool useFillRegion)
{
    static const uint32_t counts[] = {1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,
                                      10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 23U,
                                      24U, 25U, 31U, 32U, 33U, 63U, 64U, 65U};
    static const BlockType solids[] = {(BlockType)0x01U, (BlockType)0x7fU, (BlockType)0x80U,
                                       (BlockType)0xffU};
    for (uint32_t sizeIndex = 0U; sizeIndex < sizeof(counts) / sizeof(counts[0]); ++sizeIndex)
    {
        uint32_t count = counts[sizeIndex];
        for (uint32_t solidIndex = 0U; solidIndex < 4U; ++solidIndex)
        {
            BlockType solid = solids[solidIndex];
            for (uint32_t offset = 0U; offset < 8U; ++offset)
            {
                // Целиком пусто.
                regionPatternCount = count;
                for (uint32_t index = 0U; index < count; ++index)
                {
                    regionPattern[index] = BLOCK_AIR;
                }
                RunRegionCase(world, count, offset, (BlockType)0xabU, useFillRegion);
                RunRegionCase(world, count, offset, BLOCK_AIR, useFillRegion);

                // Целиком непусто.
                for (uint32_t index = 0U; index < count; ++index)
                {
                    regionPattern[index] = solid;
                }
                RunRegionCase(world, count, offset, (BlockType)0xabU, useFillRegion);
                RunRegionCase(world, count, offset, BLOCK_AIR, useFillRegion);

                // Ровно один ноль в каждой позиции: и внутри первого слова, и
                // в хвосте, который слово уже не покрывает.
                for (uint32_t hole = 0U; hole < count; ++hole)
                {
                    for (uint32_t index = 0U; index < count; ++index)
                    {
                        regionPattern[index] = index == hole ? BLOCK_AIR : solid;
                    }
                    RunRegionCase(world, count, offset, (BlockType)0xabU, useFillRegion);
                    RunRegionCase(world, count, offset, BLOCK_AIR, useFillRegion);
                }

                // Ровно одна непустая ячейка в каждой позиции.
                for (uint32_t spot = 0U; spot < count; ++spot)
                {
                    for (uint32_t index = 0U; index < count; ++index)
                    {
                        regionPattern[index] = index == spot ? solid : BLOCK_AIR;
                    }
                    RunRegionCase(world, count, offset, (BlockType)0xabU, useFillRegion);
                    RunRegionCase(world, count, offset, BLOCK_AIR, useFillRegion);
                }
            }
        }
    }
}

static void TestRegionClassification(void)
{
    // Через fillRegion: провайдер сам заполняет весь регион.
    WorldBaseProvider provider = {0};
    provider.context = NULL;
    provider.getBlock = PatternGetBlock;
    provider.fillRegion = PatternFillRegion;
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "мир с провайдером образца создан");
    RunRegionPatterns(world, true);
    WorldDestroy(world);

    // Через getBlock: тот же образец, но регион собирается поячеечно.
    provider.fillRegion = NULL;
    world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "мир с поячеечным провайдером создан");
    RunRegionPatterns(world, false);

    // Разреженные правки поверх провайдера: они кладутся после заполнения и
    // обязаны учитываться разбором.
    regionPatternCount = 16U;
    for (uint32_t index = 0U; index < 16U; ++index)
    {
        regionPattern[index] = (BlockType)0x7fU;
    }
    for (uint32_t hole = 0U; hole < 16U; ++hole)
    {
        WorldSetBlock(world, 0, 0, (int64_t)hole, BLOCK_AIR);
        static BlockType buffer[REGION_BUFFER_BYTES];
        for (uint32_t index = 0U; index < REGION_BUFFER_BYTES; ++index)
        {
            buffer[index] = (BlockType)0xabU;
        }
        BlockType *cells = buffer + REGION_GUARD;
        WorldRegionContents contents = WorldFillRegion(world, 0, 0, 0, 1, 1, 16, cells);
        ProviderExpect(contents == ClassifyBytes(cells, 16U),
                       "разреженная правка не учтена разбором региона");
        ProviderExpect(cells[hole] == BLOCK_AIR, "разреженная правка не попала в регион");
        WorldSetBlock(world, 0, 0, (int64_t)hole, (BlockType)0x7fU);
    }
    WorldDestroy(world);
}

// === Регион поперёк границ чанков ===
//
// Мешеру нужен регион на один блок шире чанка, поэтому он задевает по слою
// от каждого из двадцати шести соседей. Выборка идёт по отсортированным
// дельтам скачками, и ошибиться она может ровно там, где кусок чанка узкий:
// на гранях, рёбрах и углах. Эталон здесь — поячеечный WorldGetBlock,
// который к этой выборке никакого отношения не имеет.

#define HALO_SPAN 4

// Два образца. Дырявый рвёт всякую последовательность локальных индексов, а
// сплошной, наоборот, даёт целые колонны подряд. Выборка региона записывает
// подряд идущие дельты отрезками, и ошибка в длине отрезка видна только на
// втором образце: на первом отрезков просто нет.
static bool haloDense;

static BlockType HaloPattern(int64_t x, int64_t y, int64_t z)
{
    int64_t mixed = x * 7 + y * 13 + z * 31;
    if (haloDense)
    {
        // Колонны с x, не кратным четырём, заполнены целиком, и такие колонны
        // идут подряд: локальные индексы соседних переходят одна в другую без
        // разрыва, и отрезок обязан оборваться на границе колонны сам.
        // Остальные колонны с дырами через одну — на них проверяется, что
        // отрезок не продолжается через пропущенный блок.
        if ((((x % 4) + 4) % 4) != 0)
        {
            return (BlockType)(1U + (uint32_t)((((x + y) % 7) + 7) % 7));
        }
        return ((((z % 2) + 2) % 2) == 0) ? (BlockType)9U : BLOCK_AIR;
    }
    if (((x + y + z) & 3) == 0)
    {
        return BLOCK_AIR;
    }
    return (BlockType)(1U + (uint32_t)(((mixed % 255) + 255) % 255));
}

static void HaloFill(World *world, int64_t low, int64_t high)
{
    for (int64_t x = low; x < high; ++x)
    {
        for (int64_t y = low; y < high; ++y)
        {
            for (int64_t z = low; z < high; ++z)
            {
                BlockType block = HaloPattern(x, y, z);
                if (block != BLOCK_AIR)
                {
                    WorldSetBlock(world, x, y, z, block);
                }
            }
        }
    }
}

// Сверяет каждую ячейку региона с отдельным запросом блока.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void HaloCompare(World *world, int64_t minX, int64_t minY, int64_t minZ, int32_t sizeX,
                        int32_t sizeY, int32_t sizeZ, BlockType *cells)
{
    WorldRegionContents contents =
        WorldFillRegion(world, minX, minY, minZ, sizeX, sizeY, sizeZ, cells);
    bool anyAir = false;
    bool anySolid = false;
    for (int32_t y = 0; y < sizeY; ++y)
    {
        for (int32_t x = 0; x < sizeX; ++x)
        {
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                size_t index =
                    (((size_t)y * (size_t)sizeX) + (size_t)x) * (size_t)sizeZ + (size_t)z;
                BlockType expected = WorldGetBlock(world, minX + x, minY + y, minZ + z);
                ProviderExpect(cells[index] == expected,
                               "ячейка региона разошлась с отдельным запросом блока");
                anyAir |= expected == BLOCK_AIR;
                anySolid |= expected != BLOCK_AIR;
            }
        }
    }
    WorldRegionContents wanted = anyAir && anySolid
                                     ? WORLD_REGION_MIXED
                                     : (anySolid ? WORLD_REGION_ALL_SOLID : WORLD_REGION_ALL_AIR);
    ProviderExpect(contents == wanted, "разбор региона поперёк чанков разошёлся с эталоном");
}

static void TestHaloRegionsWithPattern(void)
{
    static BlockType cells[(CHUNK_SIZE + 2) * 4 * 4];
    World *world = WorldCreate(NULL);
    ProviderExpect(world != NULL, "мир для проверки halo создан");

    // Три чанка подряд по каждой оси, начиная с отрицательных координат:
    // регион ниже будет резать их по граням, рёбрам и углам.
    HaloFill(world, -CHUNK_SIZE, 2 * CHUNK_SIZE);

    // Узкие регионы всех форм: слой, брусок и одиночная ячейка, каждый —
    // на границе чанков и со сдвигом внутрь и наружу.
    static const int64_t offsets[] = {-CHUNK_SIZE - 1, -CHUNK_SIZE,   -1, 0, 1, CHUNK_SIZE - 1,
                                      CHUNK_SIZE,      CHUNK_SIZE + 1};
    static const int32_t spans[] = {1, 2, 3, CHUNK_SIZE + 2};
    for (uint32_t offsetIndex = 0U; offsetIndex < sizeof(offsets) / sizeof(offsets[0]);
         ++offsetIndex)
    {
        int64_t base = offsets[offsetIndex];
        for (uint32_t spanIndex = 0U; spanIndex < sizeof(spans) / sizeof(spans[0]); ++spanIndex)
        {
            int32_t span = spans[spanIndex];
            // Слой толщиной в блок по каждой оси по очереди.
            HaloCompare(world, base, base, base, span, 1, 1, cells);
            HaloCompare(world, base, base, base, 1, span, 1, cells);
            HaloCompare(world, base, base, base, 1, 1, span, cells);
            // Брусок и куб.
            HaloCompare(world, base, base, base, span, 2, 2, cells);
            if (span <= HALO_SPAN)
            {
                HaloCompare(world, base, base, base, span, span, span, cells);
            }
        }
    }

    // Регион вокруг целого чанка: только здесь центральный чанк попадает
    // внутрь целиком, и выборка идёт быстрым путём, который пишет подряд
    // идущие дельты отрезками. На узких регионах этот путь не исполняется
    // вовсе — первая версия проверки его и пропускала.
    {
        static BlockType whole[(CHUNK_SIZE + 2) * (CHUNK_SIZE + 2) * (CHUNK_SIZE + 2)];
        HaloCompare(world, -1, -1, -1, CHUNK_SIZE + 2, CHUNK_SIZE + 2, CHUNK_SIZE + 2, whole);
        HaloCompare(world, CHUNK_SIZE - 1, -1, -1, CHUNK_SIZE + 2, CHUNK_SIZE + 2, CHUNK_SIZE + 2,
                    whole);
        HaloCompare(world, -CHUNK_SIZE - 1, -CHUNK_SIZE - 1, -CHUNK_SIZE - 1, CHUNK_SIZE + 2,
                    CHUNK_SIZE + 2, CHUNK_SIZE + 2, whole);
    }

    // Разреженная правка внутри узкого куска обязана дойти до региона.
    WorldSetBlock(world, -1, 5, 7, (BlockType)200U);
    WorldSetBlock(world, CHUNK_SIZE, 5, 7, (BlockType)201U);
    WorldSetBlock(world, 5, -1, 7, BLOCK_AIR);
    HaloCompare(world, -1, -1, -1, CHUNK_SIZE + 2, 1, 1, cells);
    HaloCompare(world, -1, 5, 7, CHUNK_SIZE + 2, 1, 1, cells);
    HaloCompare(world, 5, -1, 7, 1, CHUNK_SIZE + 2, 1, cells);

    // После переноса начала мира локальные координаты уезжают, а содержимое
    // региона обязано остаться тем же относительно блоков.
    ProviderExpect(WorldRebase(world, CHUNK_SIZE, -CHUNK_SIZE, CHUNK_SIZE), "перенос мира");
    for (uint32_t offsetIndex = 0U; offsetIndex < sizeof(offsets) / sizeof(offsets[0]);
         ++offsetIndex)
    {
        int64_t base = offsets[offsetIndex];
        HaloCompare(world, base, base, base, CHUNK_SIZE + 2, 1, 1, cells);
        HaloCompare(world, base, base, base, 1, CHUNK_SIZE + 2, 1, cells);
        HaloCompare(world, base, base, base, 1, 1, CHUNK_SIZE + 2, cells);
        HaloCompare(world, base, base, base, 3, 3, 3, cells);
    }
    WorldDestroy(world);
}

static void TestRegionCoordinateLimits(void)
{
    static BlockType cells[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];
    const uint32_t localOffsets[] = {0U, 1U, 31U, 62U, 63U};
    const int32_t spans[] = {1, 2, 63, 64, 65, 66};
    for (uint32_t end = 0U; end < 2U; ++end)
    {
        World *world = WorldCreate(NULL);
        ProviderExpect(world != NULL, "coordinate-limit world created");
        int64_t base = end == 0U ? INT64_MIN : INT64_MAX - (CHUNK_SIZE - 1);
        for (uint32_t x = 0U; x < sizeof(localOffsets) / sizeof(localOffsets[0]); ++x)
        {
            for (uint32_t y = 0U; y < sizeof(localOffsets) / sizeof(localOffsets[0]); ++y)
            {
                for (uint32_t z = 0U; z < sizeof(localOffsets) / sizeof(localOffsets[0]); ++z)
                {
                    ProviderExpect(WorldTrySetBlock(world, base + localOffsets[x],
                                                    base + localOffsets[y], base + localOffsets[z],
                                                    (BlockType)(1U + x + y * 5U + z * 25U)),
                                   "coordinate-limit delta inserted");
                }
            }
        }
        // Entire edge chunk takes the direct path, including its first/last delta.
        HaloCompare(world, base, base, base, CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE, cells);
        for (uint32_t axis = 0U; axis < 3U; ++axis)
        {
            for (uint32_t sample = 0U; sample < sizeof(spans) / sizeof(spans[0]); ++sample)
            {
                int64_t minimum[3] = {base, base, base};
                int32_t size[3] = {2, 2, 2};
                size[axis] = spans[sample];
                minimum[axis] = end == 0U ? INT64_MIN : INT64_MAX - ((int64_t)size[axis] - 1);
                HaloCompare(world, minimum[0], minimum[1], minimum[2], size[0], size[1], size[2],
                            cells);
            }
            int64_t invalid[3] = {0, 0, 0};
            invalid[axis] = INT64_MAX;
            BlockType guard = (BlockType)0xa5U;
            ProviderExpect(WorldFillRegion(world, invalid[0], invalid[1], invalid[2], 2, 2, 2,
                                           &guard) == WORLD_REGION_ALL_AIR &&
                               guard == (BlockType)0xa5U,
                           "overflowing region rejected without writing output");
        }
        WorldDestroy(world);
    }
}

static void TestHaloRegions(void)
{
    haloDense = false;
    TestHaloRegionsWithPattern();
    haloDense = true;
    TestHaloRegionsWithPattern();
    haloDense = false;
}

// === Быстрый путь пустого мира ===
//
// WorldGetBlock, пока в таблице нет ни одного чанка, отвечает провайдером,
// не трогая ни блокировку, ни таблицу. Проверка держит главное свойство
// этого пути: первая же правка обязана быть видна следующим запросом, а
// откат к значению провайдера — тоже. Батч публикует чанки отдельной
// веткой, поэтому проверяется и он.

static void TestFastPathAfterEmpty(void)
{
    ProviderContext context = {0};
    WorldBaseProvider provider = {
        .context = &context,
        .getBlock = ProviderGetBlock,
        .fillRegion = ProviderFillRegion,
        .rebase = ProviderRebase,
    };
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "fast-path world was not created");

    // До единой правки ответ даёт провайдер, ревизия ещё нулевая.
    BlockType base = ProviderPattern(&context, 3, 4, 5);
    ProviderExpect(WorldGetBlock(world, 3, 4, 5) == base && WorldGetRevision(world) == 0U,
                   "fast path did not read the provider on an empty world");

    // Первая правка должна быть видна следующим же запросом.
    ProviderExpect(WorldTrySetBlock(world, 3, 4, 5, (BlockType)200U) &&
                       WorldGetBlock(world, 3, 4, 5) == (BlockType)200U,
                   "first edit after the empty fast path was not visible");
    ProviderExpect(WorldGetRevision(world) == 1U, "first edit did not advance the revision");

    // Чанк, в котором правок нет, по-прежнему отдаётся провайдером.
    BlockType other = ProviderPattern(&context, 99, 4, 5);
    ProviderExpect(WorldGetBlock(world, 99, 4, 5) == other,
                   "unedited chunk was not read from the provider");

    // Возврат к значению провайдера обязан быть виден.
    ProviderExpect(WorldTrySetBlock(world, 3, 4, 5, base) &&
                       WorldGetBlock(world, 3, 4, 5) == base,
                   "reverting to the provider value was not visible");
    WorldDestroy(world);

    // Батч публикует чанки своей веткой: на пустом мире он тоже обязан
    // пробить быстрый путь с первого же применения.
    context = (ProviderContext){0};
    world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "fast-path batch world was not created");
    BlockType batchBase = ProviderPattern(&context, 7, 8, 9);
    WorldBlockMutation mutation = {
        .block = {7, 8, 9},
        .expected = batchBase,
        .replacement = (BlockType)123U,
    };
    ProviderExpect(WorldApplyBlockBatch(world, &mutation, 1U) &&
                       WorldGetBlock(world, 7, 8, 9) == (BlockType)123U,
                   "first batch after the empty fast path was not visible");
    WorldDestroy(world);

    // Пустой провайдер: быстрый путь обязан вернуть воздух.
    World *empty = WorldCreate(NULL);
    ProviderExpect(empty != NULL && WorldGetBlock(empty, 1, 2, 3) == BLOCK_AIR,
                   "NULL-provider fast path did not return air");
    WorldDestroy(empty);
}

// === Быстрый путь при гонке читателей и первой правки ===
//
// Быстрый путь читает editedChunkCount без блокировки. Проверяется именно
// публикация: как только правка возвращается и флаг phase выставлен с
// release, любой поток, увидевший phase с acquire, обязан увидеть и правку,
// а не снова уйти на быстрый путь с нулевым счётчиком.
//
// Провайдер здесь без счётчиков: его зовут несколько потоков сразу, а
// ProviderPattern из основного набора инкрементирует общий счётчик вызовов.

typedef struct FastPathRaceState
{
    World *world;
    int64_t block[3];
    BlockType edited;
    volatile uint32_t phase;
    volatile uint32_t stop;
    volatile uint32_t badReads;
} FastPathRaceState;

static BlockType RaceProviderGetBlock(void *rawContext, int64_t x, int64_t y, int64_t z)
{
    (void)rawContext;
    int64_t sum = x + y * 3 + z * 5;
    return sum % 4 == 0 ? BLOCK_AIR : (BlockType)9U;
}

static uint32_t FastPathRaceReader(void *rawContext)
{
    FastPathRaceState *state = (FastPathRaceState *)rawContext;
    while (PlatformAtomicLoadU32Acquire(&state->stop) == 0U)
    {
        if (PlatformAtomicLoadU32Acquire(&state->phase) != 0U)
        {
            if (WorldGetBlock(state->world, state->block[0], state->block[1],
                              state->block[2]) != state->edited)
            {
                PlatformAtomicIncrementU32(&state->badReads);
            }
        }
        else
        {
            (void)WorldGetBlock(state->world, state->block[0], state->block[1],
                                state->block[2]);
        }
    }
    return 0U;
}

static void TestFastPathConcurrentVisibility(void)
{
    const uint32_t readerCount = 4U;
    WorldBaseProvider provider = {0};
    provider.getBlock = RaceProviderGetBlock;
    World *world = WorldCreate(&provider);
    ProviderExpect(world != NULL, "race world was not created");

    FastPathRaceState state;
    for (uint32_t index = 0U; index < sizeof(state); ++index)
    {
        ((uint8_t *)&state)[index] = 0U;
    }
    state.world = world;
    state.block[0] = 5;
    state.block[1] = 6;
    state.block[2] = 7;
    state.edited = (BlockType)200U;
    ProviderExpect(RaceProviderGetBlock(NULL, 5, 6, 7) != state.edited,
                   "the concurrent edit must differ from the provider value");

    PlatformThread readers[4];
    uint32_t started = 0U;
    for (uint32_t index = 0U; index < readerCount; ++index)
    {
        if (PlatformThreadStart(&readers[index], FastPathRaceReader, &state))
        {
            ++started;
        }
    }
    ProviderExpect(started == readerCount, "fast-path readers did not start");

    // Дать читателям поработать на пустом мире (быстрый путь).
    for (volatile uint32_t warm = 0U; warm < 200000U; ++warm)
    {
    }

    ProviderExpect(WorldTrySetBlock(world, state.block[0], state.block[1], state.block[2],
                                    state.edited),
                   "the first concurrent edit failed");
    PlatformAtomicStoreU32Release(&state.phase, 1U);

    for (volatile uint32_t spin = 0U; spin < 400000U; ++spin)
    {
    }

    PlatformAtomicStoreU32Release(&state.stop, 1U);
    for (uint32_t index = 0U; index < readerCount; ++index)
    {
        PlatformThreadJoin(&readers[index]);
    }

    ProviderExpect(PlatformAtomicLoadU32Acquire(&state.badReads) == 0U,
                   "a reader that saw the published edit still read the provider");
    ProviderExpect(WorldGetBlock(world, state.block[0], state.block[1], state.block[2])
                       == state.edited,
                   "the first edit was not visible after the readers joined");
    WorldDestroy(world);
}

LAIUE_TEST_ENTRY(WorldProviderTestEntryPoint)
{
    TestFastPathConcurrentVisibility();
    TestEmptyWorld();
    TestProviderAndMutations();
    TestRebaseAndFormatting();
    TestRegionClassification();
    TestHaloRegions();
    TestRegionCoordinateLimits();
    TestFastPathAfterEmpty();
    LaiueTestRuntimeWrite("World provider tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
