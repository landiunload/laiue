#include "core/physical_construct_renderer.h"

#include "render/chunk_geometry.h"
#include "world/world.h"

#include <stddef.h>
#include <string.h>

#define PHYSICAL_CONSTRUCT_CUBE_CATEGORY_COUNT 2U
#define PHYSICAL_CONSTRUCT_LEVER_DIRECTION_COUNT 6U
#define PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT \
    (PHYSICAL_CONSTRUCT_CUBE_CATEGORY_COUNT \
        + PHYSICAL_CONSTRUCT_LEVER_DIRECTION_COUNT)
#define PHYSICAL_CONSTRUCT_LEVER_GRID 16U
#define PHYSICAL_CONSTRUCT_CUBE_QUAD_COUNT 6U
#define PHYSICAL_CONSTRUCT_LEVER_QUAD_COUNT \
    (PHYSICAL_CONSTRUCT_MODEL_PARTS * 6U)
#define PHYSICAL_CONSTRUCT_PRESENTATION_TICK_RATE 60.0
#define PHYSICAL_CONSTRUCT_INTERPOLATION_DELAY_SECONDS \
    (1.0 / PHYSICAL_CONSTRUCT_PRESENTATION_TICK_RATE)
#define PHYSICAL_CONSTRUCT_MAX_EXTRAPOLATION_SECONDS \
    (2.0 / PHYSICAL_CONSTRUCT_PRESENTATION_TICK_RATE)
#define PHYSICAL_CONSTRUCT_RENDER_POSITION_LIMIT 1099511627776.0
#define PHYSICAL_CONSTRUCT_RENDER_VELOCITY_LIMIT 1048576.0

_Static_assert(PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT == 8U,
    "construct renderer category storage must match category count");

static const int8_t g_leverMountNormals[
    PHYSICAL_CONSTRUCT_LEVER_DIRECTION_COUNT][3] = {
        { 1, 0, 0 },
        {-1, 0, 0 },
        { 0, 1, 0 },
        { 0,-1, 0 },
        { 0, 0, 1 },
        { 0, 0,-1 },
};

static void AppendCuboid(ChunkQuad* quads, uint32_t* count,
    uint32_t startX, uint32_t startY, uint32_t startZ,
    uint32_t extentX, uint32_t extentY, uint32_t extentZ,
    BlockType material)
{
    for (uint32_t face = 0; face < 6U; ++face)
    {
        quads[(*count)++] = PackChunkQuad(
            startX, startY, startZ, face, material,
            extentX, extentY, extentZ);
    }
}

static RendererMesh* CreateCubeMesh(
    Renderer* renderer, BlockType material)
{
    ChunkQuad quads[PHYSICAL_CONSTRUCT_CUBE_QUAD_COUNT];
    uint32_t count = 0;
    AppendCuboid(quads, &count, 0, 0, 0, 1, 1, 1, material);
    return RendererCreateMesh(renderer, quads, count);
}

static bool QuantizeLeverBounds(const PhysicalConstructAabb* bounds,
    uint32_t start[3], uint32_t extent[3])
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        double minimum = bounds->minimum[axis];
        double maximum = bounds->maximum[axis];
        if (minimum < 0.0 || maximum > 1.0 || minimum >= maximum)
        {
            return false;
        }

        uint32_t first = (uint32_t)(
            minimum * (double)PHYSICAL_CONSTRUCT_LEVER_GRID + 0.5);
        uint32_t last = (uint32_t)(
            maximum * (double)PHYSICAL_CONSTRUCT_LEVER_GRID + 0.5);
        if (first >= PHYSICAL_CONSTRUCT_LEVER_GRID
            || last > PHYSICAL_CONSTRUCT_LEVER_GRID || last <= first)
        {
            return false;
        }
        start[axis] = first;
        extent[axis] = last - first;
    }
    return true;
}

static RendererMesh* CreateLeverMesh(
    Renderer* renderer, const int8_t mountNormal[3])
{
    PhysicalConstructModelPart parts[PHYSICAL_CONSTRUCT_MODEL_PARTS];
    if (!PhysicalConstructGetLeverModel(mountNormal, parts))
    {
        return NULL;
    }

    ChunkQuad quads[PHYSICAL_CONSTRUCT_LEVER_QUAD_COUNT];
    uint32_t count = 0;
    for (uint32_t part = 0;
        part < PHYSICAL_CONSTRUCT_MODEL_PARTS; ++part)
    {
        uint32_t start[3];
        uint32_t extent[3];
        if (!QuantizeLeverBounds(
                &parts[part].localBounds, start, extent))
        {
            return NULL;
        }
        BlockType material =
            parts[part].hitPart == PHYSICAL_CONSTRUCT_HIT_LEVER_HANDLE
                ? BLOCK_GRASS : BLOCK_EARTH;
        AppendCuboid(quads, &count,
            start[0], start[1], start[2],
            extent[0], extent[1], extent[2], material);
    }
    return RendererCreateMesh(renderer, quads, count);
}

void PhysicalConstructRendererInit(PhysicalConstructRenderer* constructs)
{
    if (constructs != NULL)
    {
        memset(constructs, 0, sizeof(*constructs));
    }
}

bool PhysicalConstructRendererEnsure(
    PhysicalConstructRenderer* constructs, Renderer* renderer)
{
    if (constructs == NULL || renderer == NULL)
    {
        return false;
    }

    if (constructs->categoryMeshes[0] == NULL)
    {
        constructs->categoryMeshes[0] =
            CreateCubeMesh(renderer, BLOCK_EARTH);
    }
    if (constructs->categoryMeshes[1] == NULL)
    {
        constructs->categoryMeshes[1] =
            CreateCubeMesh(renderer, BLOCK_GRASS);
    }
    for (uint32_t direction = 0;
        direction < PHYSICAL_CONSTRUCT_LEVER_DIRECTION_COUNT;
        ++direction)
    {
        uint32_t category =
            PHYSICAL_CONSTRUCT_CUBE_CATEGORY_COUNT + direction;
        if (constructs->categoryMeshes[category] == NULL)
        {
            constructs->categoryMeshes[category] = CreateLeverMesh(
                renderer, g_leverMountNormals[direction]);
        }
    }

    for (uint32_t category = 0;
        category < PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT;
        ++category)
    {
        if (constructs->categoryMeshes[category] == NULL)
        {
            return false;
        }
    }
    return true;
}

void PhysicalConstructRendererShutdown(
    PhysicalConstructRenderer* constructs, Renderer* renderer)
{
    if (constructs == NULL)
    {
        return;
    }
    if (renderer != NULL)
    {
        for (uint32_t category = 0;
            category < PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT;
            ++category)
        {
            RendererDestroyMesh(
                renderer, constructs->categoryMeshes[category]);
        }
    }
    memset(constructs, 0, sizeof(*constructs));
}

static int32_t LeverDirectionIndex(const int8_t normal[3])
{
    for (uint32_t direction = 0;
        direction < PHYSICAL_CONSTRUCT_LEVER_DIRECTION_COUNT;
        ++direction)
    {
        if (normal[0] == g_leverMountNormals[direction][0]
            && normal[1] == g_leverMountNormals[direction][1]
            && normal[2] == g_leverMountNormals[direction][2])
        {
            return (int32_t)direction;
        }
    }
    return -1;
}

static int32_t BlockCategory(const PhysicalConstructBlock* block)
{
    if (block->kind == PHYSICAL_CONSTRUCT_BLOCK_LEVER)
    {
        int32_t direction = LeverDirectionIndex(block->mountNormal);
        return direction < 0 ? -1
            : (int32_t)PHYSICAL_CONSTRUCT_CUBE_CATEGORY_COUNT
                + direction;
    }
    if (block->kind != PHYSICAL_CONSTRUCT_BLOCK_VOXEL)
    {
        return -1;
    }
    if (block->material == BLOCK_EARTH)
    {
        return 0;
    }
    if (block->material == BLOCK_GRASS)
    {
        return 1;
    }
    return -1;
}

static bool PresentationTickAfter(
    uint32_t candidate, uint32_t reference)
{
    uint32_t distance = candidate - reference;
    return distance != 0U && distance < 0x80000000U;
}

static bool RenderFiniteFloat(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

static bool RenderFiniteDouble(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) !=
        UINT64_C(0x7ff0000000000000);
}

static bool MotionSampleValid(
    const double origin[3], const double velocity[3])
{
    for (uint32_t axis = 0U; axis < 3U; ++axis)
    {
        if (!RenderFiniteDouble(origin[axis])
            || origin[axis] < -PHYSICAL_CONSTRUCT_RENDER_POSITION_LIMIT
            || origin[axis] > PHYSICAL_CONSTRUCT_RENDER_POSITION_LIMIT
            || !RenderFiniteDouble(velocity[axis])
            || velocity[axis] < -PHYSICAL_CONSTRUCT_RENDER_VELOCITY_LIMIT
            || velocity[axis] > PHYSICAL_CONSTRUCT_RENDER_VELOCITY_LIMIT)
        {
            return false;
        }
    }
    return true;
}

static PhysicalConstructMotionTrack* FindMotionTrack(
    PhysicalConstructRenderer* constructs, uint64_t bodyId,
    uint64_t topologyRevision)
{
    PhysicalConstructMotionTrack* empty = NULL;
    PhysicalConstructMotionTrack* sameBody = NULL;
    for (uint32_t index = 0U;
        index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
    {
        PhysicalConstructMotionTrack* track =
            &constructs->motionTracks[index];
        if (track->active && track->id == bodyId &&
            track->revision == topologyRevision)
        {
            return track;
        }
        if (track->active && track->id == bodyId)
            sameBody = track;
        if (!track->active && empty == NULL)
            empty = track;
    }
    PhysicalConstructMotionTrack* track =
        sameBody != NULL ? sameBody : empty;
    if (track == NULL)
    {
        track = &constructs->motionTracks[
            (uint32_t)(bodyId % PHYSICAL_CONSTRUCT_MAX_BODIES)];
    }
    memset(track, 0, sizeof(*track));
    track->active = true;
    track->id = bodyId;
    track->revision = topologyRevision;
    return track;
}

void PhysicalConstructRendererPushMotionSample(
    PhysicalConstructRenderer* constructs, uint64_t bodyId,
    uint64_t topologyRevision, uint32_t serverTick,
    const double origin[3], const double velocity[3])
{
    if (constructs == NULL || bodyId == 0U ||
        topologyRevision == 0U || origin == NULL || velocity == NULL ||
        !MotionSampleValid(origin, velocity))
    {
        return;
    }
    PhysicalConstructMotionTrack* track = FindMotionTrack(
        constructs, bodyId, topologyRevision);
    if (track->hasNewest && serverTick == track->newestTick)
    {
        memcpy(track->newestOrigin, origin,
            sizeof(track->newestOrigin));
        memcpy(track->newestVelocity, velocity,
            sizeof(track->newestVelocity));
        return;
    }
    if (track->hasNewest &&
        !PresentationTickAfter(serverTick, track->newestTick))
    {
        return;
    }
    if (track->hasNewest)
    {
        track->previousTick = track->newestTick;
        memcpy(track->previousOrigin, track->newestOrigin,
            sizeof(track->previousOrigin));
        track->hasPrevious = true;
    }
    track->newestTick = serverTick;
    track->hasNewest = true;
    memcpy(track->newestOrigin, origin, sizeof(track->newestOrigin));
    memcpy(track->newestVelocity, velocity,
        sizeof(track->newestVelocity));
    track->secondsSinceNewest = 0.0;
}

static uint32_t CopyPresentationSnapshot(
    PhysicalConstructRenderer* constructs,
    const PhysicalConstructSystem* system)
{
    bool truncated = false;
    uint32_t bodyCount = PhysicalConstructCopyBodyStates(
        system, constructs->bodies, PHYSICAL_CONSTRUCT_MAX_BODIES,
        &truncated);
    if (bodyCount > PHYSICAL_CONSTRUCT_MAX_BODIES)
    {
        bodyCount = PHYSICAL_CONSTRUCT_MAX_BODIES;
    }

    uint32_t blockOffset = 0;
    for (uint32_t body = 0; body < bodyCount; ++body)
    {
        constructs->bodyBlockOffsets[body] = blockOffset;
        constructs->bodyBlockCounts[body] = 0;
        uint32_t remaining = PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES
            - blockOffset;
        uint32_t capacity = remaining < PHYSICAL_CONSTRUCT_MAX_BLOCKS
            ? remaining : PHYSICAL_CONSTRUCT_MAX_BLOCKS;
        uint32_t copied = 0;
        if (capacity == 0
            || constructs->bodies[body].blockCount > capacity
            || !PhysicalConstructCopyBlocks(system,
                constructs->bodies[body].id,
                constructs->blocks + blockOffset,
                capacity, &copied)
            || copied > capacity
            || copied != constructs->bodies[body].blockCount)
        {
            continue;
        }
        constructs->bodyBlockCounts[body] = copied;
        blockOffset += copied;
    }

    (void)truncated;
    return bodyCount;
}

void PhysicalConstructRendererPrepareFrame(
    PhysicalConstructRenderer* constructs,
    const PhysicalConstructSystem* system,
    float deltaSeconds, bool interpolateRemoteMotion)
{
    if (constructs == NULL) return;
    if (system == NULL)
    {
        constructs->preparedBodyCount = 0U;
        return;
    }
    uint32_t bodyCount = CopyPresentationSnapshot(constructs, system);
    if (!RenderFiniteFloat(deltaSeconds) || deltaSeconds < 0.0f)
        deltaSeconds = 0.0f;
    if (deltaSeconds > 0.1f) deltaSeconds = 0.1f;
    for (uint32_t body = 0; body < bodyCount; ++body)
    {
        const PhysicalConstructBodyState* state = &constructs->bodies[body];
        double target[3] = {
            state->origin[0], state->origin[1], state->origin[2]};
        PhysicalConstructMotionTrack* track = NULL;
        if (interpolateRemoteMotion)
        {
            for (uint32_t index = 0U;
                index < PHYSICAL_CONSTRUCT_MAX_BODIES; ++index)
            {
                PhysicalConstructMotionTrack* candidate =
                    &constructs->motionTracks[index];
                if (candidate->active && candidate->id == state->id &&
                    candidate->revision == state->topologyRevision)
                {
                    track = candidate;
                    break;
                }
            }
        }
        if (track != NULL && track->hasNewest)
        {
            track->secondsSinceNewest += (double)deltaSeconds;
            double maximumClock =
                PHYSICAL_CONSTRUCT_INTERPOLATION_DELAY_SECONDS +
                PHYSICAL_CONSTRUCT_MAX_EXTRAPOLATION_SECONDS;
            if (track->secondsSinceNewest > maximumClock)
                track->secondsSinceNewest = maximumClock;
            double offset = track->secondsSinceNewest -
                PHYSICAL_CONSTRUCT_INTERPOLATION_DELAY_SECONDS;
            if (offset < 0.0 && track->hasPrevious)
            {
                uint32_t tickSpan =
                    track->newestTick - track->previousTick;
                double span = tickSpan != 0U
                    ? (double)tickSpan /
                        PHYSICAL_CONSTRUCT_PRESENTATION_TICK_RATE
                    : 0.0;
                double alpha = span > 0.0
                    ? (span + offset) / span : 1.0;
                if (alpha < 0.0) alpha = 0.0;
                if (alpha > 1.0) alpha = 1.0;
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    target[axis] = track->previousOrigin[axis] +
                        (track->newestOrigin[axis] -
                            track->previousOrigin[axis]) * alpha;
                }
            }
            else
            {
                if (offset < 0.0) offset = 0.0;
                if (offset > PHYSICAL_CONSTRUCT_MAX_EXTRAPOLATION_SECONDS)
                    offset = PHYSICAL_CONSTRUCT_MAX_EXTRAPOLATION_SECONDS;
                for (uint32_t axis = 0U; axis < 3U; ++axis)
                {
                    target[axis] = track->newestOrigin[axis] +
                        track->newestVelocity[axis] * offset;
                }
            }
        }
        memcpy(constructs->displayedOrigins[body], target, sizeof(target));
        constructs->displayedBodyIds[body] = state->id;
    }
    constructs->preparedBodyCount = bodyCount;
}

void PhysicalConstructRendererDraw(
    PhysicalConstructRenderer* constructs, Renderer* renderer,
    const int64_t cameraBlockPosition[3])
{
    if (constructs == NULL || renderer == NULL ||
        cameraBlockPosition == NULL)
    {
        return;
    }
    for (uint32_t category = 0;
        category < PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT;
        ++category)
    {
        if (constructs->categoryMeshes[category] == NULL) return;
    }
    uint32_t bodyCount = constructs->preparedBodyCount;
    if (bodyCount > PHYSICAL_CONSTRUCT_MAX_BODIES)
        bodyCount = PHYSICAL_CONSTRUCT_MAX_BODIES;
    for (uint32_t category = 0;
        category < PHYSICAL_CONSTRUCT_RENDER_CATEGORY_COUNT;
        ++category)
    {
        uint32_t instanceCount = 0;
        for (uint32_t body = 0; body < bodyCount; ++body)
        {
            uint32_t first = constructs->bodyBlockOffsets[body];
            uint32_t count = constructs->bodyBlockCounts[body];
            if (first > PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES ||
                count > PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES - first)
            {
                continue;
            }
            for (uint32_t index = 0; index < count; ++index)
            {
                const PhysicalConstructBlock* block =
                    &constructs->blocks[first + index];
                if (BlockCategory(block) != (int32_t)category)
                {
                    continue;
                }
                if (instanceCount
                    >= PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES)
                {
                    break;
                }

                RendererMeshInstance* instance =
                    &constructs->instances[instanceCount++];
                for (uint32_t axis = 0; axis < 3U; ++axis)
                {
                    instance->originRelative[axis] = (float)(
                        constructs->displayedOrigins[body][axis]
                        - (double)cameraBlockPosition[axis]
                        + (double)block->local[axis]);
                }
                instance->scale = category
                        < PHYSICAL_CONSTRUCT_CUBE_CATEGORY_COUNT
                    ? 1.0f
                    : 1.0f / (float)PHYSICAL_CONSTRUCT_LEVER_GRID;
            }
        }

        if (instanceCount != 0)
        {
            RendererDrawMeshInstances(renderer,
                constructs->categoryMeshes[category],
                constructs->instances, instanceCount);
        }
    }
}
