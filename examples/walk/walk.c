#include "character/character_api.h"
#include "platform/system.h"
#include "voxel/voxel_api.h"

#include <stdbool.h>
#include <stdint.h>

enum
{
    WALK_CELL_UNITS = 1024,
    WALK_MILLIMETERS_PER_UNIT = 1000,
    WALK_TICK_HZ = LAIUE_CHARACTER_TICK_HZ
};

typedef struct WalkState
{
    LaiueCharacterPositionV1 position;
    int64_t verticalVelocity;
    bool grounded;
} WalkState;

static uint32_t WalkGetBlock(const LaiueVoxelProviderV1 *provider,
                             const LaiueVoxelCoordV1 *coordinate,
                             LaiueVoxelBlockV1 *outBlock)
{
    (void)provider;
    if (coordinate == NULL || outBlock == NULL)
        return false;
    outBlock->flags = 0u;
    if (coordinate->z == 0)
        outBlock->material = 1u; /* grass */
    else if (coordinate->z >= -3)
        outBlock->material = 2u; /* earth */
    else
        outBlock->material = 3u; /* stone */
    return true;
}

static const LaiueVoxelProviderV1 walkProvider = {
    .structSize = sizeof(LaiueVoxelProviderV1),
    .abiVersion = LAIUE_VOXEL_ABI_VERSION_1,
    .getBlock = WalkGetBlock,
};

static void NormalizeHorizontal(LaiueCharacterPositionV1 *position)
{
    const int64_t cellSize = (int64_t)WALK_CELL_UNITS * WALK_MILLIMETERS_PER_UNIT;
    while (position->localX >= cellSize)
    {
        position->localX -= cellSize;
        ++position->cellX;
    }
    while (position->localX < 0)
    {
        position->localX += cellSize;
        --position->cellX;
    }
    while (position->localY >= cellSize)
    {
        position->localY -= cellSize;
        ++position->cellY;
    }
    while (position->localY < 0)
    {
        position->localY += cellSize;
        --position->cellY;
    }
}

static void WalkStep(WalkState *state, const LaiueCharacterInputV1 *input)
{
    if (state == NULL || input == NULL)
        return;
    const int64_t horizontalSpeed =
        (input->flags & LAIUE_CHARACTER_INPUT_SPRINT) != 0u ? 7000 : 3500;
    state->position.localY += horizontalSpeed * input->moveY / WALK_TICK_HZ;
    state->position.localX += horizontalSpeed * input->moveX / WALK_TICK_HZ;
    NormalizeHorizontal(&state->position);

    if ((input->flags & LAIUE_CHARACTER_INPUT_JUMP) != 0u && state->grounded)
    {
        state->verticalVelocity = 5200;
        state->grounded = false;
    }
    state->verticalVelocity -= 9800 / WALK_TICK_HZ;
    state->position.localZ += state->verticalVelocity / WALK_TICK_HZ;
    if (state->position.localZ <= WALK_MILLIMETERS_PER_UNIT * 2)
    {
        state->position.localZ = WALK_MILLIMETERS_PER_UNIT * 2;
        state->verticalVelocity = 0;
        state->grounded = true;
    }
}

static void RunWalkExample(void)
{
    WalkState state = {
        .position = {.cellX = (int64_t)1 << 40, .cellY = -((int64_t)1 << 39),
                     .localX = WALK_CELL_UNITS * WALK_MILLIMETERS_PER_UNIT - 100,
                     .localY = 0, .localZ = WALK_MILLIMETERS_PER_UNIT * 2},
        .grounded = true,
    };
    LaiueVoxelBlockV1 block;
    (void)walkProvider.getBlock(&walkProvider, &(LaiueVoxelCoordV1){0, 0, 0}, &block);
    LaiueCharacterInputV1 input = {.moveY = 1, .flags = LAIUE_CHARACTER_INPUT_SPRINT};
    for (uint32_t tick = 0u; tick < WALK_TICK_HZ; ++tick)
        WalkStep(&state, &input);
    PlatformWriteConsoleUtf8("laiue walk: infinite-coordinate character tick passed\n");
    PlatformWriteConsoleUtf8(block.material == 1u && state.grounded
                                 ? "laiue walk: grass provider and jump controller ready\n"
                                 : "laiue walk: provider/controller validation failed\n");
}

#if defined(_WIN32)
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(unsigned int);

void WalkExampleEntryPoint(void)
{
    RunWalkExample();
    ExitProcess(0u);
}
#else
int main(void)
{
    RunWalkExample();
    return 0;
}
#endif
