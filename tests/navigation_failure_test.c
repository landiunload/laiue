/* Через эти обёртки выделяют память только скопированные объекты навигации;
 * сам тест и платформа пользуются обычным аллокатором. Каждое выделение
 * заливки по очереди отказывает, и каждый отказ оставляет сетку прежней. */
#include "navigation/navigation.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define AREA_X (-8)
#define AREA_Y (-8)
#define AREA_WIDTH 272
#define AREA_HEIGHT 208

static uint32_t allocateCalls, failAllocateCall, liveAllocations;
static uint8_t before[AREA_HEIGHT][AREA_WIDTH];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Navigation failure check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

void *NavigationTestAllocate(size_t size, bool zero)
{
    if (++allocateCalls == failAllocateCall)
        return NULL;
    void *memory = PlatformAllocate(size, zero);
    if (memory != NULL)
        ++liveAllocations;
    return memory;
}

void *NavigationTestReallocate(void *memory, size_t size, bool zeroNew)
{
    if (++allocateCalls == failAllocateCall)
        return NULL;
    void *grown = PlatformReallocate(memory, size, zeroNew);
    if (memory == NULL && grown != NULL)
        ++liveAllocations;
    return grown;
}

void NavigationTestFree(void *memory)
{
    if (memory != NULL)
        --liveAllocations;
    PlatformFree(memory);
}

/* 32 чанка заполняют таблицу до порога роста. В чанке (0, 0) хранятся
 * клетки, чанк (1, 1) однородный, ещё тридцать лежат далеко. */
static LaiueNavGrid *CreateGrid(void)
{
    const LaiueNavGridDesc desc = {.structSize = sizeof(desc), .defaultCost = 1u};
    LaiueNavGrid *grid = NULL;
    Expect(LaiueNavGridCreate(&desc, &grid) == LAIUE_NAV_OK, "grid create");
    Expect(LaiueNavGridSetCell(grid, 5, 5, 7u) == LAIUE_NAV_OK, "materialized chunk");
    const LaiueNavRect uniform = {64, 64, 64u, 64u};
    Expect(LaiueNavGridFillRect(grid, &uniform, 4u) == LAIUE_NAV_OK, "uniform chunk");
    for (int64_t index = 0; index < 30; ++index)
        Expect(LaiueNavGridSetCell(grid, 64 * (100 + index), 0, 2u) == LAIUE_NAV_OK,
               "distant chunk");
    return grid;
}

static bool Inside(const LaiueNavRect *rect, int64_t x, int64_t y)
{
    return x >= rect->x && x < rect->x + (int64_t)rect->width && y >= rect->y &&
           y < rect->y + (int64_t)rect->height;
}

LAIUE_TEST_ENTRY(NavigationFailureTestEntryPoint)
{
    /* Частичные чанки со всех сторон, целые (1, 1) и (2, 1) внутри. */
    const LaiueNavRect fill = {10, 10, 192u, 128u};
    const uint8_t cost = 9u;
    uint32_t failures = 0u;
    bool filled = false;
    for (uint32_t failAt = 1u; failAt < 64u && !filled; ++failAt)
    {
        LaiueNavGrid *grid = CreateGrid();
        for (int64_t y = 0; y < AREA_HEIGHT; ++y)
            for (int64_t x = 0; x < AREA_WIDTH; ++x)
                before[y][x] = LaiueNavGridGetCell(grid, AREA_X + x, AREA_Y + y);
        const uint64_t revision = LaiueNavGridRevision(grid);

        allocateCalls = 0u;
        failAllocateCall = failAt;
        const LaiueNavStatus status = LaiueNavGridFillRect(grid, &fill, cost);
        failAllocateCall = 0u;

        filled = status == LAIUE_NAV_OK;
        Expect(filled || status == LAIUE_NAV_OUT_OF_MEMORY, "fill status");
        if (!filled)
            ++failures;
        Expect(filled == (LaiueNavGridRevision(grid) != revision),
               "the revision changes exactly when the fill succeeds");
        for (int64_t y = 0; y < AREA_HEIGHT; ++y)
            for (int64_t x = 0; x < AREA_WIDTH; ++x)
            {
                const uint8_t expected =
                    filled && Inside(&fill, AREA_X + x, AREA_Y + y) ? cost : before[y][x];
                Expect(LaiueNavGridGetCell(grid, AREA_X + x, AREA_Y + y) == expected,
                       filled ? "a successful fill writes the rectangle only"
                              : "a failed fill leaves every cell unchanged");
            }
        LaiueNavGridDestroy(grid);
        Expect(liveAllocations == 0u, "the grid frees everything it allocated");
    }
    Expect(filled, "the fill succeeds once allocations stop failing");
    Expect(failures != 0u, "the fill allocates, so failures were injected");

    LaiueTestRuntimeWrite("navigation failure: passed\n");
    LAIUE_TEST_SUCCESS();
}
