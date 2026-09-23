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

static uint32_t BaseGetBlock(const LaiueVoxelCoordV1 *coordinate,
                             LaiueVoxelBlockV1 *outBlock)
{
    if (coordinate == NULL || outBlock == NULL)
        return 0u;
    outBlock->flags = 0u;
    if (coordinate->z == 0)
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
    if (context != NULL && context->sparse.getBlock != NULL &&
        context->sparse.getBlock(&context->sparse, coordinate, &overrideBlock) == 0u)
        return 0u;
    /* The sparse module is configured with air as its default. A non-air
     * entry is an explicit game edit; otherwise the game-owned base provider
     * supplies the infinite grass/earth/stone strata. */
    if (overrideBlock.material != 0u || overrideBlock.flags != 0u)
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
    *outPosition = *position;
    *outGrounded = 0u;
    if (!AddChecked(position->localX, deltaX, &outPosition->localX) ||
        !AddChecked(position->localY, deltaY, &outPosition->localY) ||
        !AddChecked(position->localZ, deltaZ, &outPosition->localZ))
        return 0u;

    int64_t blockX = 0;
    int64_t blockY = 0;
    if (!PositionAxisToBlock(position->cellX, position->localX, &blockX) ||
        !PositionAxisToBlock(position->cellY, position->localY, &blockY))
        return 0u;

    /* A character is supported by the first solid block below its feet. The
     * sample terrain is infinite and axis-aligned, but the lookup is routed
     * through the public voxel provider so a game can replace it with a
     * streamed/chunked implementation without changing the controller. */
    int64_t targetBottom = 0;
    int64_t currentBottom = 0;
    int64_t sampleBottom = 0;
    if (!AddChecked(outPosition->localZ, -halfExtent, &targetBottom) ||
        !AddChecked(position->localZ, -halfExtent, &currentBottom) ||
        !AddChecked(targetBottom, -1, &sampleBottom))
        return 0u;
    const int64_t targetBlock = FloorDiv(sampleBottom, WALK_VOXEL_SIZE);
    if (deltaZ <= 0 && targetBlock >= INT32_MIN && targetBlock <= INT32_MAX &&
        WalkIsSolid(provider, blockX, blockY, (int32_t)targetBlock))
    {
        const int64_t top = (targetBlock + 1) * WALK_VOXEL_SIZE;
        if (currentBottom >= top || targetBottom <= top)
        {
            outPosition->localZ = top + halfExtent;
            *outGrounded = 1u;
        }
    }
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

static LaiueModuleStatus LoadWalkModules(LaiueModuleHost *host,
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
        {characterPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL},
        {voxelPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL},
    };
    return LaiueModuleHostLoad(host, binaries,
                               (uint32_t)(sizeof(binaries) / sizeof(binaries[0])), diagnostic);
#else
    const LaiueModuleApiV1 *modules[] = {
        LaiueCharacterGetStaticModuleApiV1(),
        LaiueVoxelGetStaticModuleApiV1(),
    };
    return LaiueModuleHostLoadStatic(host, modules,
                                     (uint32_t)(sizeof(modules) / sizeof(modules[0])), diagnostic);
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

    if (LoadWalkModules(host, &diagnostic) != LAIUE_MODULE_OK)
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
    if (character == NULL || voxel == NULL || character->create == NULL || voxel->create == NULL)
    {
        PlatformWriteConsoleUtf8("laiue walk: required SDK services are unavailable\n");
        LaiueModuleHostUnloadAll(host);
        LaiueModuleHostDestroy(host);
        return false;
    }

    LaiueVoxelWorldConfigV1 voxelConfig = {
        .structSize = sizeof(voxelConfig),
        .abiVersion = LAIUE_VOXEL_SERVICE_ABI_VERSION_1,
        .defaultBlock = {0u, 0u},
    };
    LaiueVoxelWorldV1 *world = NULL;
    LaiueVoxelProviderV1 sparse = {0};
    LaiueCharacterControllerV1 *controller = NULL;
    bool success = voxel->create(&voxelConfig, &world) != 0u && world != NULL &&
                   voxel->getProvider(world, &sparse) != 0u;
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
    for (uint32_t tick = 0u; success && tick < LAIUE_CHARACTER_TICK_HZ; ++tick)
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
    if (world != NULL && voxel->destroy != NULL)
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
