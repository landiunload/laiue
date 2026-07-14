#include "gameplay/player_locomotion.h"

#include <emmintrin.h>

static double PositiveSquareRoot(double value)
{
    if (value <= 0.0)
    {
        return 0.0;
    }
    __m128d input = _mm_set_sd(value);
    return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), input));
}

static double VectorLengthSquared(double x, double y)
{
    return x * x + y * y;
}

static double ClampFriction(double friction)
{
    if (friction < 0.0) return 0.0;
    if (friction > 1.0) return 1.0;
    return friction;
}

static void NormalizeDirection(double* x, double* y)
{
    double lengthSquared = VectorLengthSquared(*x, *y);
    if (lengthSquared <= 1e-24)
    {
        *x = 0.0;
        *y = 0.0;
        return;
    }

    // Направление команды уже нормализовано mapper-ом. Небольшой допуск
    // покрывает погрешность float sin/cos и убирает sqrt из hot path.
    if (lengthSquared >= 0.999999 && lengthSquared <= 1.000001)
    {
        return;
    }

    double length = PositiveSquareRoot(lengthSquared);
    *x /= length;
    *y /= length;
}

static void MoveVelocityToward(PlayerLocomotion* locomotion,
    double targetX, double targetY, double maximumChange)
{
    double deltaX = targetX - locomotion->velocityX;
    double deltaY = targetY - locomotion->velocityY;
    double distanceSquared = VectorLengthSquared(deltaX, deltaY);
    if (distanceSquared <= 1e-24
        || (maximumChange >= 0.0
            && distanceSquared <= maximumChange * maximumChange))
    {
        locomotion->velocityX = targetX;
        locomotion->velocityY = targetY;
        return;
    }

    double distance = PositiveSquareRoot(distanceSquared);
    double scale = maximumChange / distance;
    locomotion->velocityX += deltaX * scale;
    locomotion->velocityY += deltaY * scale;
}

static bool IsAtGroundTarget(const PlayerLocomotion* locomotion,
    double directionX, double directionY, double targetSpeed)
{
    if (targetSpeed <= 0.0)
    {
        return false;
    }

    double directionLengthSquared = VectorLengthSquared(
        directionX, directionY);
    if (directionLengthSquared <= 1e-24)
    {
        return false;
    }

    double velocityLengthSquared = VectorLengthSquared(
        locomotion->velocityX, locomotion->velocityY);
    double targetSpeedSquared = targetSpeed * targetSpeed;
    double speedSquaredDelta =
        velocityLengthSquared - targetSpeedSquared;
    double cross = locomotion->velocityX * directionY
        - locomotion->velocityY * directionX;
    double dot = locomotion->velocityX * directionX
        + locomotion->velocityY * directionY;

    // После достижения цели velocity уже содержит нормализованное
    // направление предыдущего шага. Проверки ниже ограничивают расхождение
    // тем же epsilon=1e-12, который MoveVelocityToward считает достигнутой
    // целью, и позволяют не нормализовать повторно почти единичный ввод.
    const double halfEpsilonSquared = 2.5e-25;
    return dot > 0.0
        && speedSquaredDelta * speedSquaredDelta
            <= halfEpsilonSquared
                * (velocityLengthSquared + targetSpeedSquared)
        && cross * cross
            <= halfEpsilonSquared * directionLengthSquared;
}

static void ApplyAirControl(PlayerLocomotion* locomotion,
    double directionX, double directionY, double targetSpeed,
    double maximumChange)
{
    if (directionX == 0.0 && directionY == 0.0)
    {
        return;
    }

    double previousSpeedSquared = VectorLengthSquared(
        locomotion->velocityX, locomotion->velocityY);
    double projectedSpeed = locomotion->velocityX * directionX
        + locomotion->velocityY * directionY;
    double available = targetSpeed - projectedSpeed;
    if (available <= 0.0)
    {
        return;
    }

    double change = available < maximumChange ? available : maximumChange;
    locomotion->velocityX += directionX * change;
    locomotion->velocityY += directionY * change;

    // Поворот в воздухе не создаёт энергию: управление меняет направление,
    // но не увеличивает уже имеющуюся скорость выше старой/целевой.
    double targetSpeedSquared = targetSpeed * targetSpeed;
    double maximumSpeedSquared = previousSpeedSquared > targetSpeedSquared
        ? previousSpeedSquared
        : targetSpeedSquared;
    double newSpeedSquared = VectorLengthSquared(
        locomotion->velocityX, locomotion->velocityY);
    if (newSpeedSquared > maximumSpeedSquared
        && newSpeedSquared > 1e-24)
    {
        double scale = PositiveSquareRoot(
            maximumSpeedSquared / newSpeedSquared);
        locomotion->velocityX *= scale;
        locomotion->velocityY *= scale;
    }
}

void PlayerLocomotionInit(PlayerLocomotion* locomotion,
    const PlayerLocomotionConfig* config)
{
    locomotion->config = *config;
    PlayerLocomotionReset(locomotion);
}

void PlayerLocomotionReset(PlayerLocomotion* locomotion)
{
    locomotion->velocityX = 0.0;
    locomotion->velocityY = 0.0;
}

void PlayerLocomotionApplySprintJumpImpulse(
    PlayerLocomotion* locomotion, double directionX, double directionY,
    double minimumRunningSpeed)
{
    NormalizeDirection(&directionX, &directionY);
    if (directionX == 0.0 && directionY == 0.0)
    {
        return;
    }

    double targetSpeed = locomotion->config.sprintJumpSpeed;
    double projectedSpeed = locomotion->velocityX * directionX
        + locomotion->velocityY * directionY;
    if (projectedSpeed + 1e-9 < minimumRunningSpeed)
    {
        return;
    }
    if (projectedSpeed < targetSpeed)
    {
        double impulse = targetSpeed - projectedSpeed;
        locomotion->velocityX += directionX * impulse;
        locomotion->velocityY += directionY * impulse;
    }

    double speedSquared = VectorLengthSquared(
        locomotion->velocityX, locomotion->velocityY);
    double targetSpeedSquared = targetSpeed * targetSpeed;
    if (speedSquared > targetSpeedSquared
        && speedSquared > 1e-24)
    {
        double scale = PositiveSquareRoot(
            targetSpeedSquared / speedSquared);
        locomotion->velocityX *= scale;
        locomotion->velocityY *= scale;
    }
}

void PlayerLocomotionStep(PlayerLocomotion* locomotion,
    double directionX, double directionY, double targetSpeed,
    bool grounded, double friction, double stepSeconds,
    double* outDistanceX, double* outDistanceY)
{
    double oldVelocityX = locomotion->velocityX;
    double oldVelocityY = locomotion->velocityY;

    if (grounded
        && IsAtGroundTarget(
            locomotion, directionX, directionY, targetSpeed))
    {
        *outDistanceX = oldVelocityX * stepSeconds;
        *outDistanceY = oldVelocityY * stepSeconds;
        return;
    }

    NormalizeDirection(&directionX, &directionY);
    bool hasInput = directionX != 0.0 || directionY != 0.0;
    if (grounded)
    {
        double traction = ClampFriction(friction);
        if (hasInput)
        {
            MoveVelocityToward(locomotion,
                directionX * targetSpeed, directionY * targetSpeed,
                locomotion->config.groundAcceleration
                    * traction * stepSeconds);
        }
        else
        {
            MoveVelocityToward(locomotion, 0.0, 0.0,
                locomotion->config.groundDeceleration
                    * traction * stepSeconds);
        }
    }
    else if (hasInput)
    {
        ApplyAirControl(locomotion, directionX, directionY, targetSpeed,
            locomotion->config.airAcceleration * stepSeconds);
    }

    *outDistanceX = 0.5
        * (oldVelocityX + locomotion->velocityX) * stepSeconds;
    *outDistanceY = 0.5
        * (oldVelocityY + locomotion->velocityY) * stepSeconds;
}

void PlayerLocomotionStopAxis(PlayerLocomotion* locomotion, int axis)
{
    if (axis == 0)
    {
        locomotion->velocityX = 0.0;
    }
    else if (axis == 1)
    {
        locomotion->velocityY = 0.0;
    }
}
