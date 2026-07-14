#pragma once

#include <stdbool.h>

typedef struct PlayerLocomotionConfig
{
    double groundAcceleration;
    double groundDeceleration;
    double airAcceleration;
    double sprintJumpSpeed;
} PlayerLocomotionConfig;

typedef struct PlayerLocomotion
{
    PlayerLocomotionConfig config;
    double velocityX;
    double velocityY;
} PlayerLocomotion;

void PlayerLocomotionInit(PlayerLocomotion* locomotion,
    const PlayerLocomotionConfig* config);
void PlayerLocomotionReset(PlayerLocomotion* locomotion);

// Одноразовый горизонтальный импульс при sprint-jump. Обычный прыжок
// эту функцию не вызывает и поэтому не получает дополнительной скорости.
void PlayerLocomotionApplySprintJumpImpulse(
    PlayerLocomotion* locomotion, double directionX, double directionY,
    double minimumRunningSpeed);

// Интегрирует скорость постоянным ускорением и возвращает перемещение
// методом трапеций. friction=1 задаёт эталонные времена из config.
void PlayerLocomotionStep(PlayerLocomotion* locomotion,
    double directionX, double directionY, double targetSpeed,
    bool grounded, double friction, double stepSeconds,
    double* outDistanceX, double* outDistanceY);

void PlayerLocomotionStopAxis(PlayerLocomotion* locomotion, int axis);
