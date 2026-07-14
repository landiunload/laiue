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
    double crouchEyeDuration;
    double crouchColliderDuration;
    double standColliderDuration;
    double standEyeDuration;
} PlayerStanceConfig;

typedef struct PlayerStance
{
    PlayerStanceConfig config;
    double colliderCrouchProgress;
    double eyeCrouchProgress;
    bool crouchingRequested;
} PlayerStance;

void PlayerStanceInit(PlayerStance* stance,
    const PlayerStanceConfig* config);
void PlayerStanceReset(PlayerStance* stance, double position[3]);

// Запрашивает стойку. Сам переход выполняется только fixed-step функцией ниже.
bool PlayerStanceSetCrouching(PlayerStance* stance, bool crouching);

// Анимирует стойку за точное время, сохраняя положение ног. При низком
// потолке расширение коллайдера останавливается и продолжится после выхода.
bool PlayerStanceStep(PlayerStance* stance,
    const VoxelCollisionSource* collision,
    double position[3], double stepSeconds);

VoxelBodyShape PlayerStanceGetShape(const PlayerStance* stance);
bool PlayerStanceIsCrouching(const PlayerStance* stance);
double PlayerStanceGetCrouchAmount(const PlayerStance* stance);
