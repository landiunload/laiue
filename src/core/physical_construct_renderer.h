#pragma once

#include "construct/physical_construct.h"
#include "render/renderer.h"

#include <stdbool.h>
#include <stdint.h>

#define PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES \
    (PHYSICAL_CONSTRUCT_MAX_BODIES * PHYSICAL_CONSTRUCT_MAX_BLOCKS)

typedef struct PhysicalConstructMotionTrack
{
    uint64_t id;
    uint64_t revision;
    uint32_t previousTick;
    uint32_t newestTick;
    double previousOrigin[3];
    double newestOrigin[3];
    double newestVelocity[3];
    double secondsSinceNewest;
    bool active;
    bool hasNewest;
    bool hasPrevious;
} PhysicalConstructMotionTrack;

typedef struct PhysicalConstructRenderer
{
    // Categories 0..1 are earth/grass cubes; 2..7 are the six lever
    // attachment directions. Every mesh is shared by all construct bodies.
    RendererMesh* categoryMeshes[8];
    PhysicalConstructBodyState bodies[PHYSICAL_CONSTRUCT_MAX_BODIES];
    PhysicalConstructBlock blocks[PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES];
    uint32_t bodyBlockOffsets[PHYSICAL_CONSTRUCT_MAX_BODIES];
    uint32_t bodyBlockCounts[PHYSICAL_CONSTRUCT_MAX_BODIES];
    uint64_t displayedBodyIds[PHYSICAL_CONSTRUCT_MAX_BODIES];
    double displayedOrigins[PHYSICAL_CONSTRUCT_MAX_BODIES][3];
    PhysicalConstructMotionTrack motionTracks[
        PHYSICAL_CONSTRUCT_MAX_BODIES];
    RendererMeshInstance instances[PHYSICAL_CONSTRUCT_RENDER_MAX_INSTANCES];
    uint32_t preparedBodyCount;
} PhysicalConstructRenderer;

void PhysicalConstructRendererInit(PhysicalConstructRenderer* constructs);

// Mesh upload is queued before RendererBeginFrame. Partial allocation is
// retained, so a later presentation frame can retry only missing meshes.
bool PhysicalConstructRendererEnsure(
    PhysicalConstructRenderer* constructs, Renderer* renderer);

void PhysicalConstructRendererShutdown(
    PhysicalConstructRenderer* constructs, Renderer* renderer);

// Adds one authoritative server-tick sample. Stale/reordered samples are
// ignored with wrap-safe uint32 tick comparison.
void PhysicalConstructRendererPushMotionSample(
    PhysicalConstructRenderer* constructs, uint64_t bodyId,
    uint64_t topologyRevision, uint32_t serverTick,
    const double origin[3], const double velocity[3]);

// Copies topology and advances presentation exactly once per application
// frame. Draw may then be called for every panorama pass without advancing
// interpolation more than once.
void PhysicalConstructRendererPrepareFrame(
    PhysicalConstructRenderer* constructs,
    const PhysicalConstructSystem* system,
    float deltaSeconds, bool interpolateRemoteMotion);

// Copies one bounded, internally consistent presentation snapshot and draws
// it using translation-only instances relative to the camera rebase origin.
// Call between RendererBeginScenePass and RendererEndFrame.
void PhysicalConstructRendererDraw(
    PhysicalConstructRenderer* constructs, Renderer* renderer,
    const int64_t cameraBlockPosition[3]);
