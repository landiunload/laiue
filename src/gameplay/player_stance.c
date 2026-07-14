#include "gameplay/player_stance.h"

static double ClampUnit(double value)
{
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double SmootherStep(double value)
{
    value = ClampUnit(value);
    return value * value * value
        * (value * (value * 6.0 - 15.0) + 10.0);
}

static double Interpolate(double standing, double crouching, double progress)
{
    return standing + (crouching - standing) * SmootherStep(progress);
}

static double MinimumHeadClearance(const PlayerStance* stance)
{
    double standingClearance = stance->config.standingHeight
        - stance->config.standingEyeHeight;
    double crouchingClearance = stance->config.crouchingHeight
        - stance->config.crouchingEyeHeight;
    double clearance = standingClearance < crouchingClearance
        ? standingClearance : crouchingClearance;
    return clearance > 0.0 ? clearance : 0.0;
}

static double EyeHeightForProgress(const PlayerStance* stance,
    double colliderProgress, double eyeProgress)
{
    double height = Interpolate(stance->config.standingHeight,
        stance->config.crouchingHeight, colliderProgress);
    double eyeHeight = Interpolate(stance->config.standingEyeHeight,
        stance->config.crouchingEyeHeight, eyeProgress);
    double maximumEyeHeight = height - MinimumHeadClearance(stance);
    return eyeHeight < maximumEyeHeight ? eyeHeight : maximumEyeHeight;
}

static double ActiveHeight(const PlayerStance* stance)
{
    return Interpolate(stance->config.standingHeight,
        stance->config.crouchingHeight,
        stance->colliderCrouchProgress);
}

static double ActiveEyeHeight(const PlayerStance* stance)
{
    return EyeHeightForProgress(stance,
        stance->colliderCrouchProgress, stance->eyeCrouchProgress);
}

static double AdvanceProgress(double progress, bool crouching,
    double duration, double stepSeconds)
{
    if (duration <= 0.0)
    {
        return crouching ? 1.0 : 0.0;
    }
    double delta = stepSeconds / duration;
    return ClampUnit(progress + (crouching ? delta : -delta));
}

static VoxelBodyShape ShapeForProgress(const PlayerStance* stance,
    double colliderProgress, double eyeProgress)
{
    VoxelBodyShape shape = {
        .radius = stance->config.radius,
        .height = Interpolate(stance->config.standingHeight,
            stance->config.crouchingHeight, colliderProgress),
        .eyeHeight = EyeHeightForProgress(
            stance, colliderProgress, eyeProgress),
        .collisionEpsilon = stance->config.collisionEpsilon,
    };
    return shape;
}

void PlayerStanceInit(PlayerStance* stance,
    const PlayerStanceConfig* config)
{
    stance->config = *config;
    stance->colliderCrouchProgress = 0.0;
    stance->eyeCrouchProgress = 0.0;
    stance->crouchingRequested = false;
}

void PlayerStanceReset(PlayerStance* stance, double position[3])
{
    double feet = position[2] - ActiveEyeHeight(stance);
    stance->colliderCrouchProgress = 0.0;
    stance->eyeCrouchProgress = 0.0;
    stance->crouchingRequested = false;
    position[2] = feet + ActiveEyeHeight(stance);
}

bool PlayerStanceSetCrouching(PlayerStance* stance, bool crouching)
{
    bool wasCrouching = PlayerStanceIsCrouching(stance);
    if (stance->crouchingRequested == crouching)
    {
        return false;
    }
    stance->crouchingRequested = crouching;
    return wasCrouching != PlayerStanceIsCrouching(stance);
}

bool PlayerStanceStep(PlayerStance* stance,
    const VoxelCollisionSource* collision,
    double position[3], double stepSeconds)
{
    bool wasCrouching = PlayerStanceIsCrouching(stance);
    if (stepSeconds <= 0.0)
    {
        return false;
    }

    double previousEyeHeight = ActiveEyeHeight(stance);
    double feet = position[2] - previousEyeHeight;
    bool crouching = stance->crouchingRequested;

    double colliderDuration = crouching
        ? stance->config.crouchColliderDuration
        : stance->config.standColliderDuration;
    double eyeDuration = crouching
        ? stance->config.crouchEyeDuration
        : stance->config.standEyeDuration;
    double nextCollider = AdvanceProgress(
        stance->colliderCrouchProgress,
        crouching, colliderDuration, stepSeconds);
    double nextEye = AdvanceProgress(
        stance->eyeCrouchProgress,
        crouching, eyeDuration, stepSeconds);

    // При штатных длительностях камера опускается раньше коллайдера и
    // поднимается позже него. EyeHeightForProgress дополнительно сохраняет
    // физический зазор до головы для конфигураций с другими диапазонами.
    if (nextEye < nextCollider)
    {
        nextEye = nextCollider;
    }

    if (!crouching
        && nextCollider < stance->colliderCrouchProgress)
    {
        VoxelBodyShape candidateShape = ShapeForProgress(
            stance, nextCollider, nextEye);
        double candidatePosition[3] = {
            position[0], position[1], feet + candidateShape.eyeHeight,
        };
        if (VoxelBodyCollides(
                collision, candidatePosition, &candidateShape))
        {
            nextCollider = stance->colliderCrouchProgress;
            if (nextEye < nextCollider)
            {
                nextEye = nextCollider;
            }
        }
    }

    stance->colliderCrouchProgress = nextCollider;
    stance->eyeCrouchProgress = nextEye;
    position[2] = feet + ActiveEyeHeight(stance);
    return wasCrouching != PlayerStanceIsCrouching(stance);
}

VoxelBodyShape PlayerStanceGetShape(const PlayerStance* stance)
{
    VoxelBodyShape shape = {
        .radius = stance->config.radius,
        .height = ActiveHeight(stance),
        .eyeHeight = ActiveEyeHeight(stance),
        .collisionEpsilon = stance->config.collisionEpsilon,
    };
    return shape;
}

bool PlayerStanceIsCrouching(const PlayerStance* stance)
{
    return stance->crouchingRequested
        || stance->colliderCrouchProgress > 1e-12
        || stance->eyeCrouchProgress > 1e-12;
}

double PlayerStanceGetCrouchAmount(const PlayerStance* stance)
{
    return SmootherStep(stance->colliderCrouchProgress);
}
