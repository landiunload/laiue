#include "character/character_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "voxel/voxel_service.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

enum
{
    WALK_VOXEL_SIZE = 1000,
    WALK_BLOCKS_PER_CELL = LAIUE_CHARACTER_LOCAL_CELL_SIZE / WALK_VOXEL_SIZE,
};

typedef struct WalkVoxelContext
{
    LaiueVoxelProviderV1 sparse;
} WalkVoxelContext;

static int64_t FloorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder < 0)
        --quotient;
    return quotient;
}

static bool AddChecked(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static bool MulAddChecked(int64_t left, int64_t multiplier, int64_t addend, int64_t *out)
{
    if (out == NULL || multiplier < 0 ||
        (left > 0 && left > INT64_MAX / multiplier) ||
        (left < 0 && left < INT64_MIN / multiplier))
        return false;
    return AddChecked(left * multiplier, addend, out);
}

static bool PositionAxisToBlock(int64_t cell, int64_t local, int64_t *out)
{
    const int64_t localBlock = FloorDiv(local, WALK_VOXEL_SIZE);
    return MulAddChecked(cell, WALK_BLOCKS_PER_CELL, localBlock, out);
}

static bool WalkIsSolid(const LaiueVoxelProviderV1 *provider, int64_t x, int64_t y, int32_t z);

static bool NormalizeWalkAxis(int64_t *cell, int64_t *local)
{
    if (cell == NULL || local == NULL)
        return false;
    int64_t quotient = *local / LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    int64_t remainder = *local % LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    if (remainder < 0)
    {
        if (quotient == INT64_MIN)
            return false;
        --quotient;
        remainder += LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    }
    if (!AddChecked(*cell, quotient, cell))
        return false;
    *local = remainder;
    return true;
}

static bool AddWalkAxis(LaiueCharacterPositionV1 *position, uint32_t axis, int64_t delta)
{
    if (position == NULL)
        return false;
    if (axis == 0u)
    {
        if (!AddChecked(position->localX, delta, &position->localX))
            return false;
        return NormalizeWalkAxis(&position->cellX, &position->localX);
    }
    if (axis == 1u)
    {
        if (!AddChecked(position->localY, delta, &position->localY))
            return false;
        return NormalizeWalkAxis(&position->cellY, &position->localY);
    }
    return AddChecked(position->localZ, delta, &position->localZ);
}

static bool WalkAxisBlockRange(const LaiueCharacterPositionV1 *position, uint32_t axis,
                               int64_t halfExtent, int64_t *outMinimum, int64_t *outMaximum)
{
    if (position == NULL || outMinimum == NULL || outMaximum == NULL || halfExtent < 0)
        return false;
    int64_t cell = 0;
    int64_t local = 0;
    if (axis == 0u)
    {
        cell = position->cellX;
        local = position->localX;
    }
    else if (axis == 1u)
    {
        cell = position->cellY;
        local = position->localY;
    }
    else if (axis == 2u)
    {
        local = position->localZ;
    }
    else
        return false;
    int64_t centerBlock = 0;
    if (axis == 2u)
    {
        centerBlock = FloorDiv(local, WALK_VOXEL_SIZE);
    }
    else if (!PositionAxisToBlock(cell, local, &centerBlock))
        return false;
    int64_t withinBlock = local % WALK_VOXEL_SIZE;
    if (withinBlock < 0)
        withinBlock += WALK_VOXEL_SIZE;
    int64_t minimumLocal = 0;
    int64_t maximumLocal = 0;
    if (!AddChecked(withinBlock, -halfExtent, &minimumLocal) ||
        !AddChecked(withinBlock, halfExtent, &maximumLocal))
        return false;
    if (halfExtent != 0 && !AddChecked(maximumLocal, -1, &maximumLocal))
        return false;
    const int64_t minimumOffset = FloorDiv(minimumLocal, WALK_VOXEL_SIZE);
    const int64_t maximumOffset = FloorDiv(maximumLocal, WALK_VOXEL_SIZE);
    return AddChecked(centerBlock, minimumOffset, outMinimum) &&
           AddChecked(centerBlock, maximumOffset, outMaximum);
}

static bool WalkRangeCount(int64_t minimum, int64_t maximum, uint32_t *outCount)
{
    if (outCount == NULL || maximum < minimum)
        return false;
    int64_t distance = 0;
    if (!AddChecked(maximum, -minimum, &distance) || distance > 64)
        return false;
    *outCount = (uint32_t)distance + 1u;
    return true;
}

static bool WalkBlockFacePosition(LaiueCharacterPositionV1 *position, uint32_t axis,
                                  int64_t block, int64_t offset)
{
    if (position == NULL)
        return false;
    if (axis == 2u)
    {
        return MulAddChecked(block, WALK_VOXEL_SIZE, offset, &position->localZ);
    }
    const int64_t blocksPerCell = WALK_BLOCKS_PER_CELL;
    int64_t cell = block / blocksPerCell;
    int64_t remainder = block % blocksPerCell;
    if (remainder < 0)
    {
        --cell;
        remainder += blocksPerCell;
    }
    int64_t local = remainder * WALK_VOXEL_SIZE;
    if (!AddChecked(local, offset, &local))
        return false;
    if (axis == 0u)
    {
        position->cellX = cell;
        position->localX = local;
        return NormalizeWalkAxis(&position->cellX, &position->localX);
    }
    if (axis == 1u)
    {
        position->cellY = cell;
        position->localY = local;
        return NormalizeWalkAxis(&position->cellY, &position->localY);
    }
    return false;
}

static bool WalkAxisCenterBlock(const LaiueCharacterPositionV1 *position, uint32_t axis,
                                int64_t *outBlock)
{
    int64_t minimum = 0;
    int64_t maximum = 0;
    return WalkAxisBlockRange(position, axis, 0, &minimum, &maximum) &&
           minimum == maximum && (*outBlock = minimum, true);
}

static uint32_t WalkSweepAxis(const LaiueVoxelProviderV1 *provider,
                              const LaiueCharacterPositionV1 *position,
                              int64_t halfExtent, uint32_t axis, int64_t delta,
                              LaiueCharacterPositionV1 *outPosition,
                              uint32_t *outCollided)
{
    if (provider == NULL || position == NULL || outPosition == NULL || outCollided == NULL ||
        halfExtent < 0)
        return 0u;
    *outPosition = *position;
    *outCollided = 0u;
    if (!AddWalkAxis(outPosition, axis, delta))
        return 0u;
    if (delta == 0)
        return 1u;

    int64_t startCenterBlock = 0;
    if (!WalkAxisCenterBlock(position, axis, &startCenterBlock))
        return 0u;
    int64_t ranges[3][2];
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
        if (!WalkAxisBlockRange(outPosition, currentAxis, halfExtent,
                                &ranges[currentAxis][0], &ranges[currentAxis][1]))
            return 0u;
    uint32_t counts[3];
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
        if (!WalkRangeCount(ranges[currentAxis][0], ranges[currentAxis][1], &counts[currentAxis]))
            return 0u;

    bool haveCollision = false;
    int64_t bestBlock = 0;
    for (uint32_t ix = 0u; ix < counts[0]; ++ix)
    {
        int64_t blockX = 0;
        if (!AddChecked(ranges[0][0], (int64_t)ix, &blockX))
            return 0u;
        for (uint32_t iy = 0u; iy < counts[1]; ++iy)
        {
            int64_t blockY = 0;
            if (!AddChecked(ranges[1][0], (int64_t)iy, &blockY))
                return 0u;
            for (uint32_t iz = 0u; iz < counts[2]; ++iz)
            {
                int64_t blockZ = 0;
                if (!AddChecked(ranges[2][0], (int64_t)iz, &blockZ))
                    return 0u;
                if (blockZ < INT32_MIN || blockZ > INT32_MAX ||
                    (axis == 0u && delta > 0 && blockX < startCenterBlock) ||
                    (axis == 1u && delta > 0 && blockY < startCenterBlock) ||
                    (axis == 2u && delta > 0 && blockZ < startCenterBlock) ||
                    (axis == 0u && delta < 0 && blockX > startCenterBlock) ||
                    (axis == 1u && delta < 0 && blockY > startCenterBlock) ||
                    (axis == 2u && delta < 0 && blockZ > startCenterBlock) ||
                    !WalkIsSolid(provider, blockX, blockY, (int32_t)blockZ))
                    continue;
                const int64_t candidate = axis == 0u ? blockX : (axis == 1u ? blockY : blockZ);
                if (!haveCollision || (delta > 0 ? candidate < bestBlock : candidate > bestBlock))
                {
                    haveCollision = true;
                    bestBlock = candidate;
                }
            }
        }
    }
    if (!haveCollision)
        return 1u;
    const int64_t offset = delta > 0 ? -halfExtent : halfExtent + WALK_VOXEL_SIZE;
    if (!WalkBlockFacePosition(outPosition, axis, bestBlock, offset))
        return 0u;
    *outCollided = 1u;
    return 1u;
}

static uint32_t BaseGetBlock(const LaiueVoxelCoordV1 *coordinate,
                             LaiueVoxelBlockV1 *outBlock)
{
    if (coordinate == NULL || outBlock == NULL)
        return 0u;
    outBlock->flags = 0u;
    if (coordinate->z > 0)
        outBlock->material = 0u; /* air above the surface */
    else if (coordinate->z == 0)
        outBlock->material = 1u; /* grass */
    else if (coordinate->z >= -3)
        outBlock->material = 2u; /* earth */
    else
        outBlock->material = 3u; /* stone */
    return 1u;
}

static uint32_t WalkGetBlock(const LaiueVoxelProviderV1 *provider,
                             const LaiueVoxelCoordV1 *coordinate,
                             LaiueVoxelBlockV1 *outBlock)
{
    if (provider == NULL || coordinate == NULL || outBlock == NULL)
        return 0u;
    const WalkVoxelContext *context = (const WalkVoxelContext *)provider->context;
    LaiueVoxelBlockV1 overrideBlock = {0u, 0u};
    uint32_t explicitEdit = 0u;
    if (context != NULL && context->sparse.getBlockState != NULL)
    {
        if (context->sparse.getBlockState(&context->sparse, coordinate, &overrideBlock,
                                          &explicitEdit) == 0u)
            return 0u;
    }
    else if (context != NULL && context->sparse.getBlock != NULL)
    {
        if (context->sparse.getBlock(&context->sparse, coordinate, &overrideBlock) == 0u)
            return 0u;
        /* Compatibility providers predating getBlockState have no explicit
         * air bit, so retain their non-air-only override behavior. */
        explicitEdit = overrideBlock.material != 0u || overrideBlock.flags != 0u;
    }
    /* The sparse module is configured with air as its default. An explicit
     * entry, including air, masks the game-owned infinite strata. */
    if (explicitEdit != 0u)
    {
        *outBlock = overrideBlock;
        return 1u;
    }
    return BaseGetBlock(coordinate, outBlock);
}

static bool WalkIsSolid(const LaiueVoxelProviderV1 *provider, int64_t x, int64_t y, int32_t z)
{
    LaiueVoxelCoordV1 coordinate = {x, y, z};
    LaiueVoxelBlockV1 block = {0u, 0u};
    return provider != NULL && provider->getBlock != NULL &&
           provider->getBlock(provider, &coordinate, &block) != 0u && block.material != 0u;
}

static uint32_t WalkSweepAabb(const LaiueCharacterCollisionV1 *collision,
                              const LaiueCharacterPositionV1 *position,
                              int64_t halfExtent, int64_t deltaX, int64_t deltaY,
                              int64_t deltaZ, LaiueCharacterPositionV1 *outPosition,
                              uint32_t *outGrounded)
{
    if (collision == NULL || position == NULL || outPosition == NULL || outGrounded == NULL)
        return 0u;
    const LaiueVoxelProviderV1 *provider = (const LaiueVoxelProviderV1 *)collision->context;
    *outGrounded = 0u;
    if (provider == NULL || halfExtent < 0)
        return 0u;
    LaiueCharacterPositionV1 next = *position;
    uint32_t collided = 0u;
    if (!WalkSweepAxis(provider, &next, halfExtent, 0u, deltaX, &next, &collided) ||
        !WalkSweepAxis(provider, &next, halfExtent, 1u, deltaY, &next, &collided) ||
        !WalkSweepAxis(provider, &next, halfExtent, 2u, deltaZ, &next, &collided))
        return 0u;
    if (deltaZ <= 0)
    {
        int64_t bottom = 0;
        int64_t sample = 0;
        if (!AddChecked(next.localZ, -halfExtent, &bottom) ||
            !AddChecked(bottom, -1, &sample))
            return 0u;
        const int64_t supportBlock = FloorDiv(sample, WALK_VOXEL_SIZE);
        int64_t blockX = 0;
        int64_t blockY = 0;
        if (!PositionAxisToBlock(next.cellX, next.localX, &blockX) ||
            !PositionAxisToBlock(next.cellY, next.localY, &blockY))
            return 0u;
        if (supportBlock >= INT32_MIN && supportBlock <= INT32_MAX &&
            WalkIsSolid(provider, blockX, blockY, (int32_t)supportBlock))
            *outGrounded = 1u;
    }
    *outPosition = next;
    return 1u;
}

#if defined(LAIUE_WALK_DYNAMIC)
static bool JoinPath(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY], const wchar_t *root,
                     const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index] = root[index], ++index;
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}
#endif

static LaiueModuleStatus LoadWalkModules(
    LaiueModuleHost *host, LaiueModuleLoadReportV1 *report,
    LaiueModuleLoadReportEntryV1 *reportEntries,
    LaiueModuleDiagnostic *diagnostic)
{
#if defined(LAIUE_WALK_DYNAMIC)
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t characterPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t voxelPath[LAIUE_PLATFORM_PATH_CAPACITY];
#if defined(_WIN32)
    const wchar_t *characterName = L"laiue_character.dll";
    const wchar_t *voxelName = L"laiue_voxel.dll";
#elif defined(__APPLE__)
    const wchar_t *characterName = L"liblaiue_character.dylib";
    const wchar_t *voxelName = L"liblaiue_voxel.dylib";
#else
    const wchar_t *characterName = L"liblaiue_character.so";
    const wchar_t *voxelName = L"liblaiue_voxel.so";
#endif
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !JoinPath(characterPath, directory, characterName) ||
        !JoinPath(voxelPath, directory, voxelName))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    LaiueModuleBinaryV1 binaries[] = {
        {characterPath, 0u, NULL},
        {voxelPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL},
    };
    LaiueModuleLoadReportInitialize(report, reportEntries,
                                    (uint32_t)(sizeof(binaries) / sizeof(binaries[0])));
    return LaiueModuleHostLoadProfile(
        host, binaries, (uint32_t)(sizeof(binaries) / sizeof(binaries[0])),
        LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, report, diagnostic);
#else
    (void)report;
    (void)reportEntries;
    const LaiueModuleApiV1 *modules[2] = {LaiueCharacterGetStaticModuleApiV1()};
    uint32_t moduleCount = 1u;
#if defined(LAIUE_WALK_STATIC_WITH_VOXEL)
    modules[moduleCount++] = LaiueVoxelGetStaticModuleApiV1();
#endif
    return LaiueModuleHostLoadStatic(host, modules,
                                     moduleCount, diagnostic);
#endif
}

static bool RunWalkExample(void)
{
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        PlatformWriteConsoleUtf8("laiue walk: bootstrap creation failed\n");
        return false;
    }

    static LaiueModuleLoadReportEntryV1 reportEntries[2];
    LaiueModuleLoadReportV1 report;
    LaiueModuleStatus moduleStatus =
        LoadWalkModules(host, &report, reportEntries, &diagnostic);
    if (moduleStatus != LAIUE_MODULE_OK && moduleStatus != LAIUE_MODULE_PARTIAL)
    {
        PlatformWriteConsoleUtf8("laiue walk: module graph failed: ");
        PlatformWriteConsoleUtf8(diagnostic.message);
        PlatformWriteConsoleUtf8("\n");
        LaiueModuleHostDestroy(host);
        return false;
    }

    const LaiueCharacterServiceV1 *character =
        (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
            sizeof(LaiueCharacterServiceV1), NULL, NULL);
    const LaiueVoxelServiceV1 *voxel =
        (const LaiueVoxelServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_VOXEL_SERVICE_NAME, LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
            sizeof(LaiueVoxelServiceV1), NULL, NULL);
    if (character == NULL || character->create == NULL)
    {
        PlatformWriteConsoleUtf8(
            "laiue walk: character provider is unavailable; cannot control player\n");
        LaiueModuleHostUnloadAll(host);
        LaiueModuleHostDestroy(host);
        return false;
    }

    if (voxel == NULL || voxel->create == NULL || voxel->getProvider == NULL)
        PlatformWriteConsoleUtf8(
            "laiue walk: voxel provider unavailable; using base strata only\n");

    LaiueVoxelWorldConfigV1 voxelConfig = {
        .structSize = sizeof(voxelConfig),
        .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        .defaultBlock = {0u, 0u},
    };
    LaiueVoxelWorldV1 *world = NULL;
    LaiueVoxelProviderV1 sparse = {0};
    LaiueCharacterControllerV1 *controller = NULL;
    bool success = true;
    if (voxel != NULL && voxel->create != NULL && voxel->getProvider != NULL)
    {
        success = voxel->create(&voxelConfig, &world) != 0u && world != NULL &&
                  voxel->getProvider(world, &sparse) != 0u;
        if (!success)
        {
            PlatformWriteConsoleUtf8(
                "laiue walk: voxel world could not be created; using base strata only\n");
            if (world != NULL && voxel->destroy != NULL)
                voxel->destroy(world);
            success = true;
            world = NULL;
            sparse = (LaiueVoxelProviderV1){0};
        }
    }
    WalkVoxelContext walkContext = {.sparse = sparse};
    LaiueVoxelProviderV1 walkProvider = {
        .structSize = sizeof(walkProvider),
        .abiVersion = LAIUE_VOXEL_ABI_VERSION_1,
        .context = &walkContext,
        .getBlock = WalkGetBlock,
    };
    LaiueCharacterCollisionV1 collision = {
        .structSize = sizeof(collision),
        .abiVersion = LAIUE_CHARACTER_ABI_VERSION_1,
        .context = &walkProvider,
        .sweepAabb = WalkSweepAabb,
    };
    success = success && character->create(&collision, 400, &controller) != 0u &&
              controller != NULL;
    LaiueCharacterPositionV1 start = {
        .cellX = (int64_t)1 << 40,
        .cellY = -((int64_t)1 << 39),
        .localX = LAIUE_CHARACTER_LOCAL_CELL_SIZE - 100,
        .localY = 0,
        .localZ = 1400,
    };
    success = success && character->setPosition(controller, &start, 1u) != 0u;

    LaiueVoxelBlockV1 grass = {0u, 0u};
    LaiueVoxelCoordV1 grassCoordinate = {0, 0, 0};
    success = success && walkProvider.getBlock(&walkProvider, &grassCoordinate, &grass) != 0u &&
              grass.material == 1u;
    LaiueVoxelBlockV1 air = {0u, 0u};
    LaiueVoxelCoordV1 airCoordinate = {0, 0, 1};
    LaiueVoxelBlockV1 earth = {0u, 0u};
    LaiueVoxelCoordV1 earthCoordinate = {0, 0, -1};
    LaiueVoxelBlockV1 stone = {0u, 0u};
    LaiueVoxelCoordV1 stoneCoordinate = {0, 0, -4};
    success = success && walkProvider.getBlock(&walkProvider, &airCoordinate, &air) != 0u &&
              air.material == 0u &&
              walkProvider.getBlock(&walkProvider, &earthCoordinate, &earth) != 0u &&
              earth.material == 2u &&
              walkProvider.getBlock(&walkProvider, &stoneCoordinate, &stone) != 0u &&
              stone.material == 3u;
    /* One jump takes a little over one second with the fixed-point gravity
     * constants. Run two fixed-step seconds so the smoke test observes both
     * the airborne path and a deterministic landing on z=0. */
    for (uint32_t tick = 0u; success && tick < LAIUE_CHARACTER_TICK_HZ * 2u; ++tick)
    {
        LaiueCharacterInputV1 input = {
            .moveX = 1,
            .moveY = 0,
            .flags = LAIUE_CHARACTER_INPUT_SPRINT |
                     (tick == 0u ? LAIUE_CHARACTER_INPUT_JUMP : 0u),
        };
        success = character->step(controller, &input) != 0u;
    }
    LaiueCharacterPositionV1 end = {0};
    success = success && character->getPosition(controller, &end) != 0u &&
              character->isGrounded(controller) != 0u;
    if (success)
    {
        PlatformWriteConsoleUtf8("laiue walk: SDK character/voxel graph passed\n");
        PlatformWriteConsoleUtf8(end.cellX != start.cellX
                                     ? "laiue walk: infinite-coordinate rebase passed\n"
                                     : "laiue walk: infinite-coordinate rebase failed\n");
        success = end.cellX != start.cellX;
    }

    if (controller != NULL && character->destroy != NULL)
        character->destroy(controller);
    if (world != NULL && voxel != NULL && voxel->destroy != NULL)
        voxel->destroy(world);
    LaiueModuleHostUnloadAll(host);
    LaiueModuleHostDestroy(host);
    return success;
}

#if defined(_WIN32)
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);

void WalkExampleEntryPoint(void)
{
    ExitProcess(RunWalkExample() ? 0u : 1u);
}
#else
int main(void)
{
    return RunWalkExample() ? 0 : 1;
}
#endif
