#pragma once

#include "walk_runtime.h"
#include "walk_physics.h"
#include "walk_humanoid.h"
#include "scene/scene_service.h"
#include "scene/math_service.h"

#include <stdbool.h>
#include <stdint.h>

/* Caller-owned, stable-address session state. Borrowed service tables outlive
 * Release. Operations run on the owner thread; snapshots expire on mutation. */
typedef struct WalkGameplayServices
{
    const LaiuePhysicsServiceV1 *physics;
    uint32_t physicsSize;
    const LaiueCharacterServiceV1 *character;
    uint32_t characterSize;
    const LaiueVoxelServiceV1 *voxel;
    uint32_t voxelSize;
    const LaiueSceneServiceV1 *scene;
    uint32_t sceneSize;
    const LaiueSceneMathServiceV1 *sceneMath;
    uint32_t sceneMathSize;
} WalkGameplayServices;

/* Flag values are private, not published module ABI. */
enum
{
    WALK_GAMEPLAY_CAP_RAGDOLL = 1u << 0,
    WALK_GAMEPLAY_CAP_CHARACTER = 1u << 1,
    WALK_GAMEPLAY_CAP_SPARSE_WORLD = 1u << 2,
    WALK_GAMEPLAY_CAP_EDIT = 1u << 3,
    WALK_GAMEPLAY_CAP_CAMERA = 1u << 4,
    WALK_GAMEPLAY_CAP_REBASE = 1u << 5
};
typedef enum WalkGameplayActor
{
    WALK_GAMEPLAY_ACTOR_STATIC,
    WALK_GAMEPLAY_ACTOR_RAGDOLL,
    WALK_GAMEPLAY_ACTOR_CHARACTER
} WalkGameplayActor;
typedef enum WalkGameplayStatus
{
    WALK_GAMEPLAY_OK,
    WALK_GAMEPLAY_UNSUPPORTED,
    WALK_GAMEPLAY_INVALID_ARGUMENT,
    WALK_GAMEPLAY_INVALID_STATE,
    WALK_GAMEPLAY_STALE_SNAPSHOT,
    WALK_GAMEPLAY_PROVIDER_FAILURE,
    WALK_GAMEPLAY_INCONSISTENT_STATE
} WalkGameplayStatus;

typedef struct WalkGameplayConfig
{
    LaiueCharacterPositionV1 initialPosition;
    double ragdollSpawnHeight;   /* definition origin Z, currently 1.02 m */
    int64_t characterHalfExtent; /* current fixed-point half extent */
    uint64_t stableIdBase;       /* currently 0x57414C4B52414744 */
    uint32_t requiredCapabilities;
    float initialYaw, initialPitch;
    float fovRadians, nearPlane, farPlane;
    bool firstPerson;
    uint8_t selectedMaterial; /* validate against current walk materials */
} WalkGameplayConfig;

enum
{
    WALK_GAMEPLAY_PULSE_JUMP = 1u << 0,
    WALK_GAMEPLAY_PULSE_BREAK = 1u << 1,
    WALK_GAMEPLAY_PULSE_PLACE = 1u << 2
};
typedef struct WalkGameplayInput
{
    double strafe, forward; /* camera-relative unit disk, magnitude preserved */
    bool sprint;
    bool jumpHeld;   /* release required before repeat on landing */
    uint32_t pulses; /* caller clears automatically on accepted Submit */
} WalkGameplayInput;

typedef struct WalkGameplay
{
    WalkGameplayServices services;
    WalkPhysicsContext physicsContext;
    WalkGameplayConfig config;
    uint32_t capabilities;
    WalkGameplayActor actor;
    bool initialized, failed;
    WalkGameplayStatus failure;
    uint64_t tick, revision;

    LaiueVoxelWorldV1 *world; /* owned */
    WalkVoxelContext walkContext;
    LaiueVoxelProviderV1 walkProvider; /* adapter; context points to walkContext */
    LaiueCharacterCollisionV1 characterCollision;
    LaiueCharacterControllerV1 *controller; /* owned if character actor */
    LaiueCharacterPositionV1 lastPosition;
    VoxelRagdoll ragdoll; /* owned if ragdoll initialized */
    VoxelCollisionSource ragdollCollision;
    VoxelRigidStepSettings ragdollRigidSettings;
    VoxelRagdollSettings ragdollSettings;
    void *ragdollScratch; /* owned Platform allocation */
    uint32_t ragdollScratchBytes;
    bool ragdollReady, grounded;
    WalkHumanoidControllerState humanoidController;
    double facingYaw, gaitPhase;
    int64_t ragdollBlockOrigin[3];  /* provider-frame block + body local origin */
    int64_t providerFrameOrigin[3]; /* absolute origin of current provider frame */

    Camera camera;
    bool firstPerson;
    uint8_t selectedMaterial;
    double lastSafeCameraEye[3]; /* local ragdoll coordinates, TPP only */
    bool lastSafeCameraEyeValid;
    WalkGameplayInput input; /* continuous state; pulses field kept zero */
    uint32_t pendingPulses;
    bool previousJumpHeld; /* handles character fallback held jump */
    bool jumpArmed;        /* after pause, require observed release before held edge */
} WalkGameplay;

enum
{
    WALK_GAMEPLAY_SPATIAL_ACTOR_VALID = 1u << 0,
    WALK_GAMEPLAY_SPATIAL_FORWARD_VALID = 1u << 1,
    WALK_GAMEPLAY_SPATIAL_CAMERA_VALID = 1u << 2
};
typedef struct WalkGameplaySpatialSnapshot
{
    const WalkGameplay *owner;
    uint64_t tick, revision;
    uint32_t valid;
    WalkGameplayActor actor;
    bool grounded, firstPerson;
    uint8_t selectedMaterial;
    int64_t providerFrameOrigin[3];
    int64_t centerBlock[3];        /* provider-frame floor(root) */
    double rootFraction[3];        /* [0,1), never huge absolute doubles */
    double rootLocal[3];           /* meaningful for ragdoll actor only */
    int64_t renderOriginBlock[3];  /* provider-frame chunk origin, all 3 axes */
    double ragdollRenderOrigin[3]; /* body local coordinates, all 3 axes */
    double cameraEyeLocal[3];      /* coordinate frame selected by actor */
    float cameraRelativeEye[3];    /* bounded relative to matching render origin */
    float forward[3];
    float viewProjection[16];
} WalkGameplaySpatialSnapshot;

typedef struct WalkGameplayRebaseResult
{
    bool changed;
    int64_t localBlockShift[3];
    int64_t providerFrameShift[3]; /* zero for ragdoll-only rebase */
} WalkGameplayRebaseResult;
typedef struct WalkGameplayStepResult
{
    uint64_t tick, revision;
    WalkHumanoidStepFailure humanoidFailure;
    WalkGameplayRebaseResult rebase;
} WalkGameplayStepResult;
typedef struct WalkGameplayEditItem
{
    bool place, changed;
    WalkGameplayStatus status;
    LaiueVoxelCoordV1 coordinate; /* provider frame; valid iff changed */
} WalkGameplayEditItem;
typedef struct WalkGameplayEditResult
{
    uint32_t count; /* zero to two, BREAK then PLACE */
    WalkGameplayEditItem items[2];
    uint64_t revision;
} WalkGameplayEditResult;
typedef enum WalkGameplayOrientationMode
{
    WALK_GAMEPLAY_ORIENT_ABSOLUTE,
    WALK_GAMEPLAY_ORIENT_RELATIVE
} WalkGameplayOrientationMode;

void WalkGameplayConfigDefault(WalkGameplayConfig *out);
WalkGameplayStatus WalkGameplayInit(WalkGameplay *game, const WalkGameplayServices *services,
                                    const WalkGameplayConfig *config);
void WalkGameplayRelease(WalkGameplay *game);
WalkGameplayStatus WalkGameplayHealth(const WalkGameplay *game);
WalkGameplayStatus WalkGameplaySubmitInput(WalkGameplay *game, WalkGameplayInput *input);
void WalkGameplayResetInput(WalkGameplay *game);
WalkGameplayStatus WalkGameplayStep(
    WalkGameplay *game, WalkGameplayStepResult *out); /* exactly LAIUE_CHARACTER_TICK_HZ = 128 Hz */
WalkGameplayStatus WalkGameplayOrient(WalkGameplay *game, float yaw, float pitch,
                                      WalkGameplayOrientationMode mode);
WalkGameplayStatus WalkGameplaySetView(WalkGameplay *game, bool firstPerson,
                                       uint8_t selectedMaterial);
WalkGameplayStatus WalkGameplaySnapshot(const WalkGameplay *game, WalkGameplaySpatialSnapshot *out);
WalkGameplayStatus WalkGameplayCamera(WalkGameplay *game, uint32_t width, uint32_t height,
                                      WalkGameplaySpatialSnapshot *out);
WalkGameplayStatus WalkGameplayEdit(WalkGameplay *game,
                                    const WalkGameplaySpatialSnapshot *cameraSnapshot,
                                    WalkGameplayEditResult *out);
WalkGameplayStatus WalkGameplayRebase(WalkGameplay *game, const int64_t *explicitBlockShiftOrNull,
                                      WalkGameplayRebaseResult *out);

/* Generated terrain and sparse overrides share the same provider frame. */
uint8_t WalkGameplayReadBlock(const WalkGameplay *game, int64_t x, int64_t y, int64_t z);
bool WalkGameplaySpatialDelta(const WalkGameplaySpatialSnapshot *from,
                              const WalkGameplaySpatialSnapshot *to, double outDelta[3]);
