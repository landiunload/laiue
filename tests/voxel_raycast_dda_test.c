// Адверсариальные и property-проверки DDA-обхода VoxelRaycast.
//
// Изменение выбора оси и восстановления previousBlock не должно сдвинуть ни
// один детерминированный hit. Тест фиксирует точные ожидаемые значения на
// границах (нулевое направление, ровный ноль и ровная единица начала
// координат, tie двух осей, близкий render origin, отрицательная диагональ)
// и проверяет инварианты восстановления previousBlock/normal на разреженном
// мире со множеством лучей.

#include "voxel/raycast.h"
#include "world/numeric_provider.h"
#include "test_runtime.h"
#include "world/world.h"
#include "numeric/numeric_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Эталонный отпечаток property-обхода, снятый с исходной реализации DDA
// (до изменения выбора оси и восстановления previousBlock). Любое расхождение
// hit-данных на представительных и адверсариальных лучах ломает тест.
#define VOXEL_RAYCAST_DDA_REFERENCE UINT64_C(0xef267ba5c6f0cb8b)

static uint32_t raycastDdaChecks;

static BlockType QueryWorldBlock(void *context, int64_t x, int64_t y, int64_t z)
{
    return WorldGetBlock((World *)context, x, y, z);
}

static bool TestVoxelRaycast(World *world, const double origin[3],
                             const float direction[3], float maximumDistance,
                             VoxelRaycastHit *outHit)
{
    return VoxelRaycastWithBlockQuery(world, QueryWorldBlock, origin, direction,
                                      maximumDistance, outHit);
}

static void DdaExpect(bool condition, const char *name)
{
    ++raycastDdaChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Voxel raycast DDA check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool BlockEquals(const int64_t value[3], int64_t x, int64_t y, int64_t z)
{
    return value[0] == x && value[1] == y && value[2] == z;
}

static bool DistanceClose(double value, double expected)
{
    double difference = value - expected;
    if (difference < 0.0) difference = -difference;
    return difference < 1e-5;
}

static uint32_t RunExplicitCases(World *world)
{
    VoxelRaycastHit hit;
    const double origin[] = { 0.5, 0.5, 0.5 };

    // Нулевое направление: шага нет ни по одной оси, луч не попадает никуда.
    {
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        DdaExpect(!TestVoxelRaycast(world, origin, zero, 10.0f, &hit),
            "zero direction must not hit");
    }

    // Ровная граница +1: начало координат x=1.0 и блок на x=2.
    {
        DdaExpect(WorldTrySetBlockExplicit(world, 2, 0, 0, (BlockType)5U),
            "boundary target was not created");
        const double boundary[3] = { 1.0, 0.5, 0.5 };
        const float positiveX[3] = { 1.0f, 0.0f, 0.0f };
        DdaExpect(TestVoxelRaycast(world, boundary, positiveX, 10.0f, &hit) &&
                      BlockEquals(hit.block, 2, 0, 0) &&
                      BlockEquals(hit.previousBlock, 1, 0, 0) &&
                      hit.normal[0] == -1 && hit.distance == 1.0,
            "exact +1 boundary hit is wrong");
    }

    // Ровный ноль на отрицательном направлении: попадание с distance 0.
    {
        DdaExpect(WorldTrySetBlockExplicit(world, -1, 0, 0, (BlockType)5U),
            "negative boundary target was not created");
        const double zeroOrigin[3] = { 0.0, 0.5, 0.5 };
        const float negativeX[3] = { -1.0f, 0.0f, 0.0f };
        DdaExpect(TestVoxelRaycast(world, zeroOrigin, negativeX, 10.0f, &hit) &&
                      BlockEquals(hit.block, -1, 0, 0) &&
                      BlockEquals(hit.previousBlock, 0, 0, 0) &&
                      hit.normal[0] == 1 && hit.distance == 0.0,
            "exact zero boundary hit is wrong");
    }

    // Tie двух осей: побеждает младшая ось, поэтому первым находится (1,0,0).
    {
        DdaExpect(WorldTrySetBlockExplicit(world, 1, 0, 0, (BlockType)5U) &&
                      WorldTrySetBlockExplicit(world, 0, 1, 0, (BlockType)5U),
            "tie targets were not created");
        const double corner[3] = { 0.0, 0.0, 0.0 };
        const float diagonalXY[3] = { 1.0f, 1.0f, 0.0f };
        DdaExpect(TestVoxelRaycast(world, corner, diagonalXY, 10.0f, &hit) &&
                      BlockEquals(hit.block, 1, 0, 0) &&
                      BlockEquals(hit.previousBlock, 0, 0, 0) &&
                      hit.normal[0] == -1 &&
                      DistanceClose(hit.distance, 1.0),
            "tie between axes must pick the lowest axis first");
    }

    WorldDestroy(world);

    // Близкий render origin: большой абсолютной координате соответствует та
    // же локальная арифметика, что и в начале координат.
    {
        World *farWorld = WorldCreate(NULL);
        DdaExpect(farWorld != NULL, "far world was not created");
        DdaExpect(WorldTrySetBlockExplicit(farWorld, 1000000003, 0, 0, (BlockType)5U),
            "far target was not created");
        const double farOrigin[3] = { 1000000000.5, 0.5, 0.5 };
        const float positiveX[3] = { 1.0f, 0.0f, 0.0f };
        DdaExpect(TestVoxelRaycast(farWorld, farOrigin, positiveX, 10.0f, &hit) &&
                      BlockEquals(hit.block, 1000000003, 0, 0) &&
                      BlockEquals(hit.previousBlock, 1000000002, 0, 0) &&
                      hit.normal[0] == -1 && hit.distance == 2.5,
            "near render origin hit is wrong");
        WorldDestroy(farWorld);
    }

    // Отрицательная диагональ с tie по двум осям.
    {
        World *diagonal = WorldCreate(NULL);
        DdaExpect(diagonal != NULL, "diagonal world was not created");
        DdaExpect(WorldTrySetBlockExplicit(diagonal, -2, -2, 0, (BlockType)5U),
            "negative diagonal target was not created");
        const double diagonalOrigin[3] = { -0.5, -0.5, 0.5 };
        const float negativeDiagonal[3] = { -0.70710678f, -0.70710678f, 0.0f };
        DdaExpect(TestVoxelRaycast(diagonal, diagonalOrigin, negativeDiagonal, 10.0f, &hit) &&
                      BlockEquals(hit.block, -2, -2, 0) &&
                      BlockEquals(hit.previousBlock, -2, -1, 0) &&
                      hit.normal[0] == 0 && hit.normal[1] == 1 && hit.normal[2] == 0 &&
                      DistanceClose(hit.distance, 0.70710678),
            "negative diagonal hit is wrong");
        WorldDestroy(diagonal);
    }

    // Граница максимальной дистанции включающая и исключающая.
    {
        World *reach = WorldCreate(NULL);
        DdaExpect(reach != NULL, "reach world was not created");
        DdaExpect(WorldTrySetBlockExplicit(reach, 10, 0, 0, (BlockType)5U),
            "reach target was not created");
        const float positiveX[3] = { 1.0f, 0.0f, 0.0f };
        const float positiveZ[3] = { 0.0f, 0.0f, 1.0f };
        DdaExpect(TestVoxelRaycast(reach, origin, positiveX, 10.0f, &hit) &&
                      BlockEquals(hit.block, 10, 0, 0),
            "target exactly at maximum distance must be reached");
        DdaExpect(WorldTrySetBlockExplicit(reach, 11, 0, 0, (BlockType)5U),
            "beyond target was not created");
        // Первый блок теперь на 10, но проверим отсечение через пустой луч по Z.
        DdaExpect(!TestVoxelRaycast(reach, origin, positiveZ, 5.0f, &hit),
            "empty ray must stop at maximum distance");
        DdaExpect(!TestVoxelRaycast(reach, origin, positiveX,
                       VOXEL_RAYCAST_MAX_DISTANCE + 1.0f, &hit),
            "over-limit maximum distance must be rejected");
        WorldDestroy(reach);
    }

    // Экстремальные начала координат не должны ломать приведение к int64.
    {
        World *extreme = WorldCreate(NULL);
        DdaExpect(extreme != NULL, "extreme world was not created");
        const float positiveX[3] = { 1.0f, 0.0f, 0.0f };
        const double hugePositive[3] = { 9.2e18, 0.5, 0.5 };
        const double hugeNegative[3] = { -9.2e18, 0.5, 0.5 };
        DdaExpect(!TestVoxelRaycast(extreme, hugePositive, positiveX, 4.0f, &hit),
            "huge origin must return a defined miss");
        DdaExpect(!TestVoxelRaycast(extreme, hugeNegative, positiveX, 4.0f, &hit),
            "huge negative origin must return a defined miss");
        WorldDestroy(extreme);
    }

    return raycastDdaChecks;
}

static bool BlockSolid(int64_t x, int64_t y, int64_t z)
{
    return ((x * 31 + y * 17 + z * 7) % 23) == 0;
}

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

static uint64_t HashResult(uint64_t hash, bool hitFound,
    const VoxelRaycastHit *result)
{
    unsigned char flag = hitFound ? 1u : 0u;
    hash = HashBytes(hash, &flag, sizeof(flag));
    if (!hitFound)
    {
        return hash;
    }
    hash = HashBytes(hash, result->block, sizeof(result->block));
    hash = HashBytes(hash, result->previousBlock, sizeof(result->previousBlock));
    hash = HashBytes(hash, result->normal, sizeof(result->normal));
    hash = HashBytes(hash, &result->distance, sizeof(result->distance));
    return hash;
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
    LaiueTestRuntimeWrite(text);
}

// Property-проверка: на каждом hit предыдущий блок отличается ровно по одной
// оси, normal — единичный вектор этой оси, а сам hit воспроизводится повторно.
static void RunPropertySweep(World *world)
{
    static const double fractions[5] = { 0.0, 0.25, 0.5, 0.75, 0.9375 };
    uint32_t hits = 0u;
    uint64_t checksum = UINT64_C(14695981039346656037);

    for (int64_t x = -6; x <= 6; ++x)
    {
        for (int64_t y = -6; y <= 6; ++y)
        {
            for (int64_t z = -6; z <= 6; ++z)
            {
                for (uint32_t fraction = 0; fraction < 5u; ++fraction)
                {
                    double origin[3] = {
                        (double)x + fractions[fraction],
                        (double)y + fractions[(fraction + 1u) % 5u],
                        (double)z + fractions[(fraction + 2u) % 5u],
                    };
                    const float directions[6][3] = {
                        { 1.0f, 1.0f, 1.0f },
                        { -1.0f, 1.0f, -1.0f },
                        { 0.0f, 0.0f, 1.0f },
                        { -1.0f, 0.0f, 0.0f },
                        { 0.5f, -0.25f, 0.75f },
                        { -0.125f, -0.5f, 0.25f },
                    };
                    for (uint32_t direction = 0; direction < 6u; ++direction)
                    {
                        VoxelRaycastHit first;
                        VoxelRaycastHit second;
                        bool hitFound = TestVoxelRaycast(
                            world, origin, directions[direction], 32.0f, &first);
                        bool repeatFound = TestVoxelRaycast(
                            world, origin, directions[direction], 32.0f, &second);
                        DdaExpect(hitFound == repeatFound,
                            "raycast must be deterministic across repeated calls");
                        checksum = HashResult(checksum, hitFound, &first);
                        if (!hitFound)
                        {
                            continue;
                        }
                        ++hits;
                        DdaExpect(BlockSolid(first.block[0], first.block[1], first.block[2]),
                            "a returned hit must be a solid block");
                        DdaExpect(first.distance == second.distance &&
                                      BlockEquals(first.block, second.block[0], second.block[1], second.block[2]) &&
                                      BlockEquals(first.previousBlock, second.previousBlock[0], second.previousBlock[1], second.previousBlock[2]),
                            "repeated raycast hit must be identical");

                        int32_t differing = 0;
                        int32_t axis = -1;
                        for (int32_t component = 0; component < 3; ++component)
                        {
                            if (first.block[component] != first.previousBlock[component])
                            {
                                ++differing;
                                axis = component;
                            }
                        }
                        DdaExpect(differing == 1 && axis >= 0,
                            "block and previousBlock must differ on exactly one axis");
                        if (differing != 1 || axis < 0)
                        {
                            continue;
                        }
                        int64_t step = first.block[axis] - first.previousBlock[axis];
                        DdaExpect(step == 1 || step == -1,
                            "consecutive blocks must differ by exactly one voxel");
                        DdaExpect(first.normal[axis] == (int8_t)-step,
                            "normal must point from the hit back to the previous block");
                        for (int32_t component = 0; component < 3; ++component)
                        {
                            if (component != axis)
                            {
                                DdaExpect(first.normal[component] == 0,
                                    "normal must be a single-axis unit vector");
                            }
                        }
                        DdaExpect(first.distance >= 0.0,
                            "hit distance must not be negative");
                    }
                }
            }
        }
    }

    LaiueTestRuntimeWrite("Voxel raycast property sweep hits: ");
    char digits[16];
    uint32_t length = 0u;
    if (hits == 0u) digits[length++] = '0';
    while (hits != 0u)
    {
        digits[length++] = (char)('0' + (hits % 10u));
        hits /= 10u;
    }
    for (uint32_t index = 0; index < length; ++index)
    {
        char single[2] = { digits[length - index - 1u], '\0' };
        LaiueTestRuntimeWrite(single);
    }
    LaiueTestRuntimeWrite("\r\n");

    LaiueTestRuntimeWrite("Voxel raycast DDA checksum: ");
    WriteHex(checksum);
    LaiueTestRuntimeWrite("\r\n");
    DdaExpect(checksum == VOXEL_RAYCAST_DDA_REFERENCE,
        "property sweep checksum differs from the baseline reference");
}

LAIUE_TEST_ENTRY(VoxelRaycastDdaTestEntryPoint)
{
    WorldSetNumericService(LaiueNumericGetStaticServiceV1());
    World *world = WorldCreate(NULL);
    DdaExpect(world != NULL, "empty world was not created");
    DdaExpect(RunExplicitCases(world) > 0u, "no explicit checks ran");

    World *sweepWorld = WorldCreate(NULL);
    DdaExpect(sweepWorld != NULL, "sweep world was not created");
    for (int64_t x = -10; x <= 10; ++x)
    {
        for (int64_t y = -10; y <= 10; ++y)
        {
            for (int64_t z = -10; z <= 10; ++z)
            {
                if (BlockSolid(x, y, z))
                {
                    DdaExpect(WorldTrySetBlockExplicit(sweepWorld, x, y, z, (BlockType)6U),
                        "sweep block was not created");
                }
            }
        }
    }
    RunPropertySweep(sweepWorld);
    WorldDestroy(sweepWorld);

    LaiueTestRuntimeWrite("Voxel raycast DDA tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
