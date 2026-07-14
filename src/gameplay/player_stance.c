#include "gameplay/player_stance.h"

static double ActiveHeight(const PlayerStance* stance)
{
    return stance->crouching
        ? stance->config.crouchingHeight
        : stance->config.standingHeight;
}

static double ActiveEyeHeight(const PlayerStance* stance)
{
    return stance->crouching
        ? stance->config.crouchingEyeHeight
        : stance->config.standingEyeHeight;
}

void PlayerStanceInit(PlayerStance* stance,
    const PlayerStanceConfig* config)
{
    stance->config = *config;
    stance->crouching = false;
}

void PlayerStanceReset(PlayerStance* stance, double position[3])
{
    if (stance->crouching)
    {
        double feet = position[2] - stance->config.crouchingEyeHeight;
        position[2] = feet + stance->config.standingEyeHeight;
    }
    stance->crouching = false;
}

bool PlayerStanceTrySet(PlayerStance* stance,
    const VoxelCollisionSource* collision,
    double position[3], bool crouching)
{
    if (stance->crouching == crouching)
    {
        return false;
    }

    bool previous = stance->crouching;
    double previousEyeHeight = ActiveEyeHeight(stance);
    double feet = position[2] - previousEyeHeight;

    stance->crouching = crouching;
    position[2] = feet + ActiveEyeHeight(stance);

    VoxelBodyShape shape = PlayerStanceGetShape(stance);
    if (!crouching && VoxelBodyCollides(collision, position, &shape))
    {
        stance->crouching = previous;
        position[2] = feet + previousEyeHeight;
        return false;
    }
    return true;
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
    return stance->crouching;
}
