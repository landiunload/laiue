#include "character/character_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "numeric/numeric_service.h"
#include "voxel/voxel_service.h"
#include "world/world_service.h"
#include "walk_runtime.h"
#if defined(LAIUE_WALK_WINDOWED)
#include "graphics/graphics_device_service.h"
#include "render/graphics_service.h"
#include "input/input_service.h"
#include "platform/window_service.h"
#include "ui/ui_service.h"
#endif

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(LAIUE_WALK_WINDOWED)
#include <wchar.h>
#endif

enum
{
    WALK_VOXEL_SIZE = 1000,
    WALK_BLOCKS_PER_CELL = LAIUE_CHARACTER_LOCAL_CELL_SIZE / WALK_VOXEL_SIZE,
};

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
    /* A range spanning INT64_MIN cannot be represented by the checked
     * subtraction below. It is also far outside the bounded swept-AABB
     * neighbourhood, so reject it before evaluating -minimum. */
    if (minimum == INT64_MIN)
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
    /* The public caller intentionally reuses one position buffer for the
     * three sequential axes.  Keep an immutable start snapshot so aliasing
     * position/outPosition cannot erase the swept interval before it is
     * inspected. */
    const LaiueCharacterPositionV1 startPosition = *position;
    *outPosition = startPosition;
    *outCollided = 0u;
    if (!AddWalkAxis(outPosition, axis, delta))
        return 0u;
    if (delta == 0)
        return 1u;

    int64_t startCenterBlock = 0;
    if (!WalkAxisCenterBlock(&startPosition, axis, &startCenterBlock))
        return 0u;
    int64_t startRanges[3][2];
    int64_t endRanges[3][2];
    int64_t ranges[3][2];
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
        if (!WalkAxisBlockRange(&startPosition, currentAxis, halfExtent,
                                &startRanges[currentAxis][0],
                                &startRanges[currentAxis][1]) ||
            !WalkAxisBlockRange(outPosition, currentAxis, halfExtent,
                                &endRanges[currentAxis][0],
                                &endRanges[currentAxis][1]))
            return 0u;
    for (uint32_t currentAxis = 0u; currentAxis < 3u; ++currentAxis)
    {
        if (currentAxis != axis)
        {
            ranges[currentAxis][0] = endRanges[currentAxis][0];
            ranges[currentAxis][1] = endRanges[currentAxis][1];
            continue;
        }
        /* The old implementation inspected only the final AABB.  A fast
         * fixed-step movement could therefore jump over a one-block wall.
         * Sweep the complete integer interval between the start and end
         * occupied ranges, then choose the nearest hit below. */
        ranges[currentAxis][0] = startRanges[currentAxis][0] < endRanges[currentAxis][0]
                                     ? startRanges[currentAxis][0]
                                     : endRanges[currentAxis][0];
        ranges[currentAxis][1] = startRanges[currentAxis][1] > endRanges[currentAxis][1]
                                     ? startRanges[currentAxis][1]
                                     : endRanges[currentAxis][1];
    }
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
    int64_t offset = 0;
    if (delta > 0)
        offset = -halfExtent;
    else if (!AddChecked(halfExtent, WALK_VOXEL_SIZE, &offset))
        return 0u;
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

LAIUE_WALK_RUNTIME_API uint32_t WalkGetBlock(
    const LaiueVoxelProviderV1 *provider,
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

LAIUE_WALK_RUNTIME_API uint32_t WalkSweepAabb(
    const LaiueCharacterCollisionV1 *collision,
    const LaiueCharacterPositionV1 *position,
    int64_t halfExtent,
    int64_t deltaX,
    int64_t deltaY,
    int64_t deltaZ,
    LaiueCharacterPositionV1 *outPosition,
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

#if defined(LAIUE_WALK_WINDOWED)
typedef struct WalkWindowState
{
    const LaiueWindowServiceV1 *windowService;
    const LaiueInputServiceV1 *inputService;
    const LaiueGraphicsDeviceServiceV1 *graphicsService;
    const LaiueUiServiceV1 *uiService;
    const LaiueCharacterServiceV1 *characterService;
    Window *window;
    Input *input;
    LaiueGraphicsDeviceV1 *device;
    void *uiContext;
    const uint8_t *fontPixels;
    uint32_t fontWidth;
    uint32_t fontHeight;
    LaiueCharacterControllerV1 *controller;
    double lastTime;
    double accumulator;
    bool failed;
} WalkWindowState;

static LaiueUiQuadV1 walkUiQuads[LAIUE_GRAPHICS_UI_MAX_QUADS];

static void WalkRawInput(void *opaque, void *rawInput)
{
    WalkWindowState *state = (WalkWindowState *)opaque;
    if (state != NULL && state->inputService != NULL && state->input != NULL &&
        state->inputService->handleRawInput != NULL)
        state->inputService->handleRawInput(state->input, rawInput);
}

static void WalkWindowFrame(void *opaque)
{
    WalkWindowState *state = (WalkWindowState *)opaque;
    if (state == NULL || state->window == NULL || state->input == NULL ||
        state->device == NULL || state->controller == NULL)
        return;
    if (state->windowService->consumeFocusLoss != NULL &&
        state->windowService->consumeFocusLoss(state->window) != 0 &&
        state->inputService->resetState != NULL)
        state->inputService->resetState(state->input);
    if (state->windowService->consumeResize != NULL &&
        state->windowService->consumeResize(state->window) != 0)
    {
        int32_t width = 0;
        int32_t height = 0;
        state->windowService->getClientSize(state->window, &width, &height);
        if (width > 0 && height > 0)
            state->graphicsService->resize(state->device, width, height);
    }
    if (state->inputService->wasKeyPressed(state->input, INPUT_KEY_ESCAPE))
    {
        (void)state->inputService->consumeKeyPress(state->input, INPUT_KEY_ESCAPE);
        state->windowService->requestClose(state->window);
    }
    const double now = PlatformMonotonicSeconds();
    double elapsed = state->lastTime == 0.0 ? 0.0 : now - state->lastTime;
    state->lastTime = now;
    if (elapsed < 0.0)
        elapsed = 0.0;
    if (elapsed > 0.25)
        elapsed = 0.25;
    state->accumulator += elapsed;
    const double fixedStep = 1.0 / 128.0;
    uint32_t ticks = 0u;
    while (state->accumulator >= fixedStep && ticks < 8u)
    {
        LaiueCharacterInputV1 input = {0};
        input.moveX = (state->inputService->isKeyDown(state->input, INPUT_KEY_D) ? 1 : 0) -
                      (state->inputService->isKeyDown(state->input, INPUT_KEY_A) ? 1 : 0);
        input.moveY = (state->inputService->isKeyDown(state->input, INPUT_KEY_W) ? 1 : 0) -
                      (state->inputService->isKeyDown(state->input, INPUT_KEY_S) ? 1 : 0);
        if (state->inputService->isKeyDown(state->input, INPUT_KEY_SHIFT))
            input.flags |= LAIUE_CHARACTER_INPUT_SPRINT;
        if (state->inputService->wasKeyPressed(state->input, INPUT_KEY_SPACE))
        {
            input.flags |= LAIUE_CHARACTER_INPUT_JUMP;
            (void)state->inputService->consumeKeyPress(state->input, INPUT_KEY_SPACE);
        }
        if (state->characterService->step(state->controller, &input) == 0u)
        {
            state->failed = true;
            state->windowService->requestClose(state->window);
            break;
        }
        state->accumulator -= fixedStep;
        ++ticks;
    }
    if (ticks == 8u && state->accumulator >= fixedStep)
        state->accumulator = 0.0;

    int32_t width = 0;
    int32_t height = 0;
    state->windowService->getClientSize(state->window, &width, &height);
    if (width > 0 && height > 0 && state->graphicsService->createDevice != NULL)
    {
        if (state->device->beginFrame(state->device, (uint32_t)width, (uint32_t)height) == 0u)
            state->failed = true;
        else
        {
            if (state->uiService != NULL && state->uiContext != NULL &&
                state->uiService->begin != NULL && state->uiService->rect != NULL &&
                state->uiService->textUtf8 != NULL &&
                state->uiService->copyDrawList != NULL)
            {
                int32_t mouseX = 0;
                int32_t mouseY = 0;
                state->windowService->getCursorClientPosition(state->window, &mouseX, &mouseY);
                const uint32_t mouseDown =
                    state->inputService->isMouseButtonDown(state->input,
                                                           INPUT_MOUSE_BUTTON_LEFT)
                        ? 1u
                        : 0u;
                const uint32_t mousePressed =
                    state->inputService->wasMouseButtonPressed(state->input,
                                                               INPUT_MOUSE_BUTTON_LEFT)
                        ? 1u
                        : 0u;
                const float wheel = state->windowService->consumeMouseWheelSteps != NULL
                                        ? state->windowService->consumeMouseWheelSteps(
                                              state->window)
                                        : 0.0f;
                if (state->uiService->begin(state->uiContext, width, height,
                                            (float)mouseX, (float)mouseY, mouseDown,
                                            mousePressed, wheel, (float)elapsed) != 0u)
                {
                    if (state->device->setUiFontAtlas != NULL &&
                        state->uiService->getFontAtlas != NULL)
                    {
                        const uint8_t *pixels = NULL;
                        uint32_t atlasWidth = 0u;
                        uint32_t atlasHeight = 0u;
                        if (state->uiService->getFontAtlas(state->uiContext, &pixels,
                                                           &atlasWidth, &atlasHeight) != 0u &&
                            pixels != NULL && atlasWidth != 0u && atlasHeight != 0u &&
                            (pixels != state->fontPixels || atlasWidth != state->fontWidth ||
                             atlasHeight != state->fontHeight))
                        {
                            if (state->device->setUiFontAtlas(state->device, pixels,
                                                               atlasWidth, atlasHeight) != 0u)
                            {
                                state->fontPixels = pixels;
                                state->fontWidth = atlasWidth;
                                state->fontHeight = atlasHeight;
                            }
                        }
                    }
                    state->uiService->rect(state->uiContext, 16.0f, 16.0f, 330.0f, 126.0f,
                                           8.0f, 0xF4221A16u);
                    state->uiService->textUtf8(state->uiContext, 32.0f, 32.0f, 0xFFFFFFFFu,
                                               "LAIUE Walk");
                    state->uiService->textUtf8(state->uiContext, 32.0f, 58.0f, 0xFFE8ECF4u,
                                               "WASD move   Shift sprint   Space jump");
                    state->uiService->textUtf8(state->uiContext, 32.0f, 84.0f, 0xFFB8C8FFu,
                                               "Character: online");
                    state->uiService->textUtf8(state->uiContext, 32.0f, 106.0f, 0xFFB8C8FFu,
                                               "Voxel: optional sparse edits");
                    uint32_t quadCount = 0u;
                    if (state->uiService->copyDrawList(state->uiContext, walkUiQuads,
                                                       LAIUE_GRAPHICS_UI_MAX_QUADS,
                                                       &quadCount) == 0u ||
                        state->device->submitUi == NULL ||
                        state->device->submitUi(state->device, walkUiQuads, quadCount) == 0u)
                        state->failed = true;
                }
            }
            if (state->device->endFrame(state->device) == 0u)
                state->failed = true;
        }
    }
    if (state->inputService->endFrame != NULL)
        state->inputService->endFrame(state->input);
}
#endif

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

#if !defined(LAIUE_WALK_RUNTIME_ONLY)
static LaiueModuleStatus LoadWalkModules(
    LaiueModuleHost *host, LaiueModuleLoadReportV1 *report,
    LaiueModuleLoadReportEntryV1 *reportEntries,
    LaiueModuleDiagnostic *diagnostic)
{
#if defined(LAIUE_WALK_DYNAMIC)
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t characterPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t numericPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t worldPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t voxelPath[LAIUE_PLATFORM_PATH_CAPACITY];
#if defined(LAIUE_WALK_WINDOWED)
    static wchar_t windowPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t inputPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t renderPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t uiPath[LAIUE_PLATFORM_PATH_CAPACITY];
#endif
#if defined(_WIN32)
    const wchar_t *characterName = L"laiue_character.dll";
    const wchar_t *numericName = L"laiue_numeric.dll";
    const wchar_t *worldName = L"laiue_world.dll";
    const wchar_t *voxelName = L"laiue_voxel.dll";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *windowName = L"laiue_window.dll";
    const wchar_t *inputName = L"laiue_input.dll";
    const wchar_t *renderName = L"laiue_render.dll";
    const wchar_t *uiName = L"laiue_ui.dll";
#endif
#elif defined(__APPLE__)
    const wchar_t *characterName = L"liblaiue_character.dylib";
    const wchar_t *numericName = L"liblaiue_numeric.dylib";
    const wchar_t *worldName = L"liblaiue_world.dylib";
    const wchar_t *voxelName = L"liblaiue_voxel.dylib";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *windowName = L"liblaiue_window.dylib";
    const wchar_t *inputName = L"liblaiue_input.dylib";
    const wchar_t *renderName = L"liblaiue_render.dylib";
    const wchar_t *uiName = L"liblaiue_ui.dylib";
#endif
#else
    const wchar_t *characterName = L"liblaiue_character.so";
    const wchar_t *numericName = L"liblaiue_numeric.so";
    const wchar_t *worldName = L"liblaiue_world.so";
    const wchar_t *voxelName = L"liblaiue_voxel.so";
#if defined(LAIUE_WALK_WINDOWED)
    const wchar_t *windowName = L"liblaiue_window.so";
    const wchar_t *inputName = L"liblaiue_input.so";
    const wchar_t *renderName = L"liblaiue_render.so";
    const wchar_t *uiName = L"liblaiue_ui.so";
#endif
#endif
    if (!PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY) ||
        !JoinPath(characterPath, directory, characterName) ||
        !JoinPath(numericPath, directory, numericName) ||
        !JoinPath(worldPath, directory, worldName) ||
        !JoinPath(voxelPath, directory, voxelName))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    LaiueModuleBinaryV1 binaries[12];
    uint32_t binaryCount = 0u;
    binaries[binaryCount++] = (LaiueModuleBinaryV1){characterPath, 0u, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){numericPath,
                                                   LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] = (LaiueModuleBinaryV1){worldPath,
                                                   LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){voxelPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
#if defined(LAIUE_WALK_WINDOWED)
    if (!JoinPath(windowPath, directory, windowName) ||
        !JoinPath(inputPath, directory, inputName) ||
        !JoinPath(renderPath, directory, renderName) ||
        !JoinPath(uiPath, directory, uiName))
        return LAIUE_MODULE_INVALID_ARGUMENT;
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){windowPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){inputPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){renderPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
    binaries[binaryCount++] =
        (LaiueModuleBinaryV1){uiPath, LAIUE_MODULE_BINARY_OPTIONAL, NULL};
#endif
    LaiueModuleLoadReportInitialize(report, reportEntries,
                                    binaryCount);
    return LaiueModuleHostLoadProfile(
        host, binaries, binaryCount,
        LAIUE_MODULE_PROFILE_ALLOW_PARTIAL, report, diagnostic);
#else
    (void)report;
    (void)reportEntries;
    const LaiueModuleApiV1 *modules[12] = {LaiueCharacterGetStaticModuleApiV1()};
    uint32_t moduleCount = 1u;
#if defined(LAIUE_WALK_STATIC_WITH_VOXEL)
    modules[moduleCount++] = LaiueNumericGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueWorldGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueVoxelGetStaticModuleApiV1();
#endif
#if defined(LAIUE_WALK_WINDOWED)
    modules[moduleCount++] = LaiueWindowGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueInputGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueGraphicsGetStaticModuleApiV1();
    modules[moduleCount++] = LaiueUiGetStaticModuleApiV1();
#endif
    return LaiueModuleHostLoadStatic(host, modules,
                                     moduleCount, diagnostic);
#endif
}
#endif /* !LAIUE_WALK_RUNTIME_ONLY */

#if !defined(LAIUE_WALK_RUNTIME_ONLY)
static bool RunWalkExample(bool headless)
{
#if !defined(LAIUE_WALK_WINDOWED)
    (void)headless;
#endif
    LaiueModuleHostConfigV1 config;
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleDiagnostic diagnostic;
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    if (host == NULL)
    {
        PlatformWriteConsoleUtf8("laiue walk: bootstrap creation failed\n");
        return false;
    }

    static LaiueModuleLoadReportEntryV1 reportEntries[8];
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

    if (voxel == NULL || voxel->getProvider == NULL ||
        voxel->createWithContext == NULL)
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
    if (voxel != NULL && voxel->getProvider != NULL &&
        voxel->createWithContext != NULL)
    {
        success = voxel->createWithContext(voxel->context, &voxelConfig, &world) != 0u &&
                  world != NULL &&
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

    bool ranWindow = false;
#if defined(LAIUE_WALK_WINDOWED)
    void *walkUiContext = NULL;
    const LaiueUiServiceV1 *walkUiService = NULL;
    if (!headless)
    {
        const LaiueWindowServiceV1 *windowService =
            (const LaiueWindowServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_WINDOW_SERVICE_NAME, LAIUE_WINDOW_SERVICE_ABI_VERSION_1,
                sizeof(LaiueWindowServiceV1), NULL, NULL);
        const LaiueInputServiceV1 *inputService =
            (const LaiueInputServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_INPUT_SERVICE_NAME, LAIUE_INPUT_SERVICE_ABI_VERSION_1,
                sizeof(LaiueInputServiceV1), NULL, NULL);
        const LaiueGraphicsDeviceServiceV1 *graphicsService =
            (const LaiueGraphicsDeviceServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_GRAPHICS_DEVICE_SERVICE_NAME,
                LAIUE_GRAPHICS_DEVICE_SERVICE_ABI_VERSION_1,
                sizeof(LaiueGraphicsDeviceServiceV1), NULL, NULL);
        walkUiService = (const LaiueUiServiceV1 *)LaiueModuleHostQueryService(
                host, LAIUE_UI_SERVICE_NAME, LAIUE_UI_SERVICE_ABI_VERSION_1,
                sizeof(LaiueUiServiceV1), NULL, NULL);
        if (windowService != NULL && inputService != NULL && graphicsService != NULL &&
            windowService->create != NULL && inputService->create != NULL &&
            graphicsService->createDevice != NULL)
        {
            WindowConfiguration windowConfiguration = {
                .title = L"LAIUE Walk",
                .width = 1280,
                .height = 720,
            };
            Window *window = windowService->create(&windowConfiguration);
            Input *input = window == NULL ? NULL :
                inputService->create(windowService->getNativeHandle(window));
            LaiueGraphicsDeviceV1 *device = NULL;
            if (walkUiService != NULL && walkUiService->contextCreateWithContext != NULL &&
                walkUiService->contextDestroy != NULL && walkUiService->context != NULL)
                (void)walkUiService->contextCreateWithContext(walkUiService->context,
                                                          &walkUiContext);
            if (window != NULL && input != NULL)
            {
                windowService->setRawInputCallback(window, WalkRawInput, NULL);
                if (graphicsService->createDevice(
                        windowService->getNativeHandle(window), 1280, 720,
                        LAIUE_GRAPHICS_BACKEND_AUTO, &device) == 0u)
                    device = NULL;
                /* The callback context is installed after the state is
                 * complete, so the window never observes a half-built input. */
            }
            if (window != NULL && input != NULL && device != NULL)
            {
                WalkWindowState state = {
                    .windowService = windowService,
                    .inputService = inputService,
                    .graphicsService = graphicsService,
                    .uiService = walkUiService,
                    .characterService = character,
                    .window = window,
                    .input = input,
                    .device = device,
                    .uiContext = walkUiContext,
                    .controller = controller,
                    .lastTime = PlatformMonotonicSeconds(),
                };
                windowService->setRawInputCallback(window, WalkRawInput, &state);
                windowService->setMouseLook(window, false);
                PlatformWriteConsoleUtf8(
                    "laiue walk: windowed mode (WASD, Shift, Space, Esc)\n");
                windowService->runLoop(window, WalkWindowFrame, &state);
                success = success && !state.failed;
                ranWindow = true;
                if (!state.failed)
                    PlatformWriteConsoleUtf8("laiue walk: windowed session ended cleanly\n");
                graphicsService->destroyDevice(device);
            }
            else
            {
                PlatformWriteConsoleUtf8(
                    "laiue walk: graphics/window unavailable; using diagnostic headless mode\n");
                if (device != NULL)
                    graphicsService->destroyDevice(device);
            }
            if (input != NULL)
                inputService->destroy(input);
            if (window != NULL)
                windowService->destroy(window);
        }
        else
            PlatformWriteConsoleUtf8(
                "laiue walk: graphics/window modules missing; using diagnostic headless mode\n");
    }
    if (walkUiContext != NULL && walkUiService != NULL &&
        walkUiService->contextDestroy != NULL)
        walkUiService->contextDestroy(walkUiContext);
#endif
    if (!ranWindow)
    {
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
__declspec(dllimport) const wchar_t *__stdcall GetCommandLineW(void);

static bool WalkHeadlessArgument(void)
{
    const wchar_t *commandLine = GetCommandLineW();
    if (commandLine == NULL)
        return false;
    for (uint32_t index = 0u; commandLine[index] != L'\0'; ++index)
        if (commandLine[index] == L'-' && commandLine[index + 1u] == L'-' &&
            commandLine[index + 2u] == L'h' && commandLine[index + 3u] == L'e' &&
            commandLine[index + 4u] == L'a' && commandLine[index + 5u] == L'd' &&
            commandLine[index + 6u] == L'l' && commandLine[index + 7u] == L'e' &&
            commandLine[index + 8u] == L's' && commandLine[index + 9u] == L's')
            return true;
    return false;
}

void WalkExampleEntryPoint(void)
{
    ExitProcess(RunWalkExample(WalkHeadlessArgument()) ? 0u : 1u);
}
#else
static bool WalkHeadlessArgument(int argc, char **argv)
{
    for (int32_t argument = 1; argument < argc; ++argument)
    {
        const char *value = argv[argument];
        static const char expected[] = "--headless";
        uint32_t index = 0u;
        if (value == NULL)
            continue;
        while (value[index] != '\0' && expected[index] != '\0' &&
               value[index] == expected[index])
            ++index;
        if (value[index] == '\0' && expected[index] == '\0')
            return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    return RunWalkExample(WalkHeadlessArgument(argc, argv)) ? 0 : 1;
}
#endif
#endif /* !LAIUE_WALK_RUNTIME_ONLY */
