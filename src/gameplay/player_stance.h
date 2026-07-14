#pragma once

#include <stdbool.h>

#include "physics/voxel_body.h"

typedef struct PlayerStanceConfig
{
    double radius;
    double standingHeight;
    double standingEyeHeight;
    double crouchingHeight;
    double crouchingEyeHeight;
    double collisionEpsilon;
} PlayerStanceConfig;

typedef struct PlayerStance
{
    PlayerStanceConfig config;
    bool crouching;
} PlayerStance;

void PlayerStanceInit(PlayerStance* stance,
    const PlayerStanceConfig* config);
void PlayerStanceReset(PlayerStance* stance, double position[3]);

// Меняет стойку, сохраняя положение ног. Встать под потолком нельзя.
bool PlayerStanceTrySet(PlayerStance* stance,
    const VoxelCollisionSource* collision,
    double position[3], bool crouching);

VoxelBodyShape PlayerStanceGetShape(const PlayerStance* stance);
bool PlayerStanceIsCrouching(const PlayerStance* stance);
