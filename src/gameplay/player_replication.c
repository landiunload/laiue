#include "gameplay/player_replication.h"

#include <stddef.h>

#define PLAYER_REPLICATION_POSITION_LIMIT 1099511627776.0
#define PLAYER_ANGLE_PI 3.14159265358979323846f
#define PLAYER_ANGLE_TWO_PI 6.28318530717958647692f
#define PLAYER_PITCH_LIMIT 1.57079632679489661923f

static bool SequenceAfter(uint32_t left, uint32_t right)
{
    uint32_t difference = left - right;
    return difference != 0u && difference < 0x80000000u;
}

static bool IsImmediateSequenceSuccessor(uint32_t sequence, uint32_t previous)
{
    return sequence - previous == 1u
           // Production wire IDs reserve zero, while the generic ring also
           // supports callers that use the complete uint32_t range.
           || (previous == UINT32_MAX && sequence == 1u);
}

static int64_t TickOffset(uint32_t tick, uint32_t reference)
{
    uint32_t forward = tick - reference;
    if (forward < 0x80000000u)
    {
        return (int64_t)forward;
    }
    return -(int64_t)(reference - tick);
}

static bool IsFiniteBoundedDouble(double value, double limit)
{
    return value == value && value >= -limit && value <= limit;
}

static bool IsFiniteBoundedFloat(float value, float limit)
{
    return value == value && value >= -limit && value <= limit;
}

static float WrapAngle(float angle)
{
    if (angle > PLAYER_ANGLE_PI)
    {
        angle -= PLAYER_ANGLE_TWO_PI;
    }
    else if (angle < -PLAYER_ANGLE_PI)
    {
        angle += PLAYER_ANGLE_TWO_PI;
    }
    return angle;
}

static float ShortestAngleDifference(float from, float to)
{
    return WrapAngle(to - from);
}

static bool ValidFixedTickSeconds(double value)
{
    // The practical upper bound also rejects positive infinity without
    // depending on the platform CRT's isfinite implementation.
    return value > 0.0 && value <= 3600.0;
}

static double FixedTickEpsilon(double fixedTickSeconds)
{
    return fixedTickSeconds * 1e-10;
}

void PlayerFixedTickClockInit(PlayerFixedTickClock *clock)
{
    if (clock != NULL)
    {
        clock->accumulatorSeconds = 0.0;
    }
}

void PlayerFixedTickClockAccumulate(PlayerFixedTickClock *clock, double elapsedSeconds,
                                    double fixedTickSeconds, uint32_t maximumTicks)
{
    if (clock == NULL || !(elapsedSeconds >= 0.0) || !ValidFixedTickSeconds(fixedTickSeconds) ||
        maximumTicks == 0u)
    {
        return;
    }

    double maximumSeconds = fixedTickSeconds * (double)maximumTicks;
    if (elapsedSeconds >= maximumSeconds ||
        clock->accumulatorSeconds >= maximumSeconds - elapsedSeconds)
    {
        clock->accumulatorSeconds = maximumSeconds;
        return;
    }
    clock->accumulatorSeconds += elapsedSeconds;
}

bool PlayerFixedTickClockHasTick(const PlayerFixedTickClock *clock, double fixedTickSeconds)
{
    return clock != NULL && ValidFixedTickSeconds(fixedTickSeconds) &&
           clock->accumulatorSeconds + FixedTickEpsilon(fixedTickSeconds) >= fixedTickSeconds;
}

bool PlayerFixedTickClockConsumeTick(PlayerFixedTickClock *clock, double fixedTickSeconds)
{
    if (!PlayerFixedTickClockHasTick(clock, fixedTickSeconds))
    {
        return false;
    }
    if (clock->accumulatorSeconds <= fixedTickSeconds)
    {
        clock->accumulatorSeconds = 0.0;
    }
    else
    {
        clock->accumulatorSeconds -= fixedTickSeconds;
    }
    return true;
}

double PlayerFixedTickClockAlpha(const PlayerFixedTickClock *clock, double fixedTickSeconds)
{
    if (clock == NULL || !ValidFixedTickSeconds(fixedTickSeconds))
    {
        return 0.0;
    }
    double alpha = clock->accumulatorSeconds / fixedTickSeconds;
    if (alpha < 0.0)
    {
        return 0.0;
    }
    return alpha > 1.0 ? 1.0 : alpha;
}

static uint32_t PredictionPhysicalIndex(const PlayerPredictionHistory *history,
                                        uint32_t logicalIndex)
{
    return (history->start + logicalIndex) % PLAYER_PREDICTION_HISTORY_CAPACITY;
}

static uint32_t InterpolationPhysicalIndex(const PlayerInterpolationBuffer *buffer,
                                           uint32_t logicalIndex)
{
    return (buffer->start + logicalIndex) % PLAYER_INTERPOLATION_BUFFER_CAPACITY;
}

void PlayerPredictionHistoryInit(PlayerPredictionHistory *history)
{
    history->start = 0u;
    history->count = 0u;
    history->lastAuthoritativeSequence = 0u;
    history->lastAuthoritativeServerTick = 0u;
    history->hasAuthoritativeState = false;
}

bool PlayerPredictionHistoryRecord(PlayerPredictionHistory *history, uint32_t sequence,
                                   const PlayerControllerCommand *command,
                                   const PlayerControllerState *predictedState)
{
    if (history->hasAuthoritativeState &&
        !SequenceAfter(sequence, history->lastAuthoritativeSequence))
    {
        return false;
    }
    if (history->count != 0u)
    {
        const PlayerPredictionEntry *newest =
            &history->entries[PredictionPhysicalIndex(history, history->count - 1u)];
        if (!SequenceAfter(sequence, newest->sequence))
        {
            return false;
        }
    }

    if (history->count == PLAYER_PREDICTION_HISTORY_CAPACITY)
    {
        history->start = (history->start + 1u) % PLAYER_PREDICTION_HISTORY_CAPACITY;
        --history->count;
    }
    uint32_t index = PredictionPhysicalIndex(history, history->count);
    history->entries[index].sequence = sequence;
    history->entries[index].command = *command;
    history->entries[index].predictedState = *predictedState;
    ++history->count;
    return true;
}

PlayerPredictionReconcileResult PlayerPredictionHistoryReconcile(
    PlayerPredictionHistory *history, uint32_t authoritativeServerTick,
    uint32_t acknowledgedSequence, const PlayerControllerState *authoritativeState,
    PlayerController *controller, const PlayerCollisionSource *collision, Camera *camera,
    uint32_t fixedStepsPerCommand, uint32_t *outReplayedCommands)
{
    if (outReplayedCommands != NULL)
    {
        *outReplayedCommands = 0u;
    }
    if (fixedStepsPerCommand == 0u)
    {
        return PLAYER_PREDICTION_RECONCILE_INVALID_STATE;
    }
    if (history->hasAuthoritativeState &&
        !SequenceAfter(authoritativeServerTick, history->lastAuthoritativeServerTick))
    {
        return PLAYER_PREDICTION_RECONCILE_STALE;
    }
    if (history->hasAuthoritativeState &&
        acknowledgedSequence != history->lastAuthoritativeSequence &&
        !SequenceAfter(acknowledgedSequence, history->lastAuthoritativeSequence))
    {
        return PLAYER_PREDICTION_RECONCILE_STALE;
    }

    uint32_t removeCount = 0u;
    if (history->count != 0u)
    {
        const PlayerPredictionEntry *oldest = &history->entries[history->start];
        const PlayerPredictionEntry *newest =
            &history->entries[PredictionPhysicalIndex(history, history->count - 1u)];
        if (SequenceAfter(acknowledgedSequence, newest->sequence))
        {
            return PLAYER_PREDICTION_RECONCILE_HISTORY_MISS;
        }

        if (SequenceAfter(oldest->sequence, acknowledgedSequence))
        {
            // Replay возможен, если authoritative state расположен ровно
            // перед самым старым сохранённым input. Более старое состояние
            // требует уже вытесненных команд и потому небезопасно.
            if (!IsImmediateSequenceSuccessor(oldest->sequence, acknowledgedSequence))
            {
                return PLAYER_PREDICTION_RECONCILE_HISTORY_MISS;
            }
        }
        else
        {
            while (removeCount < history->count)
            {
                const PlayerPredictionEntry *entry =
                    &history->entries[PredictionPhysicalIndex(history, removeCount)];
                if (SequenceAfter(entry->sequence, acknowledgedSequence))
                {
                    break;
                }
                ++removeCount;
            }
        }
    }

    // Restore валидирует snapshot до единственной записи. Поэтому при
    // ошибке history/controller остаются неизменными.
    if (!PlayerControllerRestoreState(controller, camera, authoritativeState))
    {
        return PLAYER_PREDICTION_RECONCILE_INVALID_STATE;
    }

    history->start = PredictionPhysicalIndex(history, removeCount);
    history->count -= removeCount;
    for (uint32_t index = 0u; index < history->count; ++index)
    {
        PlayerPredictionEntry *entry = &history->entries[PredictionPhysicalIndex(history, index)];
        PlayerControllerSimulateFixedSteps(controller, collision, camera, &entry->command,
                                           fixedStepsPerCommand);
        PlayerControllerCaptureState(controller, camera, &entry->predictedState);
    }
    history->lastAuthoritativeSequence = acknowledgedSequence;
    history->lastAuthoritativeServerTick = authoritativeServerTick;
    history->hasAuthoritativeState = true;
    if (outReplayedCommands != NULL)
    {
        *outReplayedCommands = history->count;
    }
    return PLAYER_PREDICTION_RECONCILE_APPLIED;
}

uint32_t PlayerPredictionHistoryCount(const PlayerPredictionHistory *history)
{
    return history->count;
}

void PlayerInterpolationBufferInit(PlayerInterpolationBuffer *buffer)
{
    buffer->start = 0u;
    buffer->count = 0u;
}

static bool ValidateInterpolationSnapshot(const PlayerInterpolationSnapshot *snapshot)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!IsFiniteBoundedDouble(snapshot->position[axis], PLAYER_REPLICATION_POSITION_LIMIT))
        {
            return false;
        }
    }
    return IsFiniteBoundedFloat(snapshot->yaw, PLAYER_ANGLE_PI) &&
           IsFiniteBoundedFloat(snapshot->pitch, PLAYER_PITCH_LIMIT);
}

static double SnapshotDistanceSquared(const PlayerInterpolationSnapshot *left,
                                      const PlayerInterpolationSnapshot *right)
{
    double result = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double difference = right->position[axis] - left->position[axis];
        result += difference * difference;
    }
    return result;
}

bool PlayerInterpolationBufferPush(PlayerInterpolationBuffer *buffer,
                                   const PlayerInterpolationSnapshot *snapshot)
{
    if (!ValidateInterpolationSnapshot(snapshot))
    {
        return false;
    }
    if (buffer->count != 0u)
    {
        const PlayerInterpolationSnapshot *newest =
            &buffer->snapshots[InterpolationPhysicalIndex(buffer, buffer->count - 1u)];
        if (!SequenceAfter(snapshot->serverTick, newest->serverTick))
        {
            return false;
        }
        double teleportDistanceSquared =
            PLAYER_INTERPOLATION_TELEPORT_DISTANCE * PLAYER_INTERPOLATION_TELEPORT_DISTANCE;
        if (SnapshotDistanceSquared(newest, snapshot) > teleportDistanceSquared)
        {
            buffer->start = 0u;
            buffer->count = 0u;
        }
    }

    if (buffer->count == PLAYER_INTERPOLATION_BUFFER_CAPACITY)
    {
        buffer->start = (buffer->start + 1u) % PLAYER_INTERPOLATION_BUFFER_CAPACITY;
        --buffer->count;
    }
    uint32_t index = InterpolationPhysicalIndex(buffer, buffer->count);
    buffer->snapshots[index] = *snapshot;
    ++buffer->count;
    return true;
}

static void SegmentVelocity(const PlayerInterpolationSnapshot *left,
                            const PlayerInterpolationSnapshot *right, double outVelocity[3],
                            float *outYawVelocity)
{
    int64_t tickDistance = TickOffset(right->serverTick, left->serverTick);
    if (tickDistance <= 0)
    {
        outVelocity[0] = 0.0;
        outVelocity[1] = 0.0;
        outVelocity[2] = 0.0;
        *outYawVelocity = 0.0f;
        return;
    }
    double inverseTicks = 1.0 / (double)tickDistance;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        outVelocity[axis] = (right->position[axis] - left->position[axis]) * inverseTicks;
    }
    *outYawVelocity = ShortestAngleDifference(left->yaw, right->yaw) / (float)tickDistance;
}

static void PoseFromSnapshot(const PlayerInterpolationSnapshot *snapshot,
                             PlayerInterpolatedPose *pose)
{
    pose->position[0] = snapshot->position[0];
    pose->position[1] = snapshot->position[1];
    pose->position[2] = snapshot->position[2];
    pose->velocityPerTick[0] = 0.0;
    pose->velocityPerTick[1] = 0.0;
    pose->velocityPerTick[2] = 0.0;
    pose->yaw = snapshot->yaw;
    pose->pitch = snapshot->pitch;
    pose->yawVelocityPerTick = 0.0f;
    pose->grounded = snapshot->grounded;
    pose->extrapolated = false;
}

bool PlayerInterpolationBufferSample(const PlayerInterpolationBuffer *buffer,
                                     double ticksSinceNewest, PlayerInterpolatedPose *outPose)
{
    if (buffer->count == 0u || !IsFiniteBoundedDouble(ticksSinceNewest, 1048576.0))
    {
        return false;
    }
    if (ticksSinceNewest < 0.0)
    {
        ticksSinceNewest = 0.0;
    }

    const PlayerInterpolationSnapshot *newest =
        &buffer->snapshots[InterpolationPhysicalIndex(buffer, buffer->count - 1u)];
    const PlayerInterpolationSnapshot *oldest = &buffer->snapshots[buffer->start];
    double targetOffset = ticksSinceNewest - (double)PLAYER_INTERPOLATION_DELAY_TICKS;
    double oldestOffset = (double)TickOffset(oldest->serverTick, newest->serverTick);

    PlayerInterpolatedPose pose;
    if (targetOffset <= oldestOffset || buffer->count == 1u)
    {
        PoseFromSnapshot(oldest, &pose);
        if (buffer->count > 1u)
        {
            const PlayerInterpolationSnapshot *next =
                &buffer->snapshots[InterpolationPhysicalIndex(buffer, 1u)];
            SegmentVelocity(oldest, next, pose.velocityPerTick, &pose.yawVelocityPerTick);
        }
        *outPose = pose;
        return true;
    }

    for (uint32_t index = 1u; index < buffer->count; ++index)
    {
        const PlayerInterpolationSnapshot *right =
            &buffer->snapshots[InterpolationPhysicalIndex(buffer, index)];
        double rightOffset = (double)TickOffset(right->serverTick, newest->serverTick);
        if (targetOffset > rightOffset)
        {
            continue;
        }

        const PlayerInterpolationSnapshot *left =
            &buffer->snapshots[InterpolationPhysicalIndex(buffer, index - 1u)];
        double leftOffset = (double)TickOffset(left->serverTick, newest->serverTick);
        double span = rightOffset - leftOffset;
        double alpha = span > 0.0 ? (targetOffset - leftOffset) / span : 0.0;
        SegmentVelocity(left, right, pose.velocityPerTick, &pose.yawVelocityPerTick);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            pose.position[axis] =
                left->position[axis] + (right->position[axis] - left->position[axis]) * alpha;
        }
        pose.yaw =
            WrapAngle(left->yaw + ShortestAngleDifference(left->yaw, right->yaw) * (float)alpha);
        pose.pitch = left->pitch + (right->pitch - left->pitch) * (float)alpha;
        pose.grounded = alpha < 0.5 ? left->grounded : right->grounded;
        pose.extrapolated = false;
        *outPose = pose;
        return true;
    }

    PoseFromSnapshot(newest, &pose);
    if (buffer->count > 1u)
    {
        const PlayerInterpolationSnapshot *previous =
            &buffer->snapshots[InterpolationPhysicalIndex(buffer, buffer->count - 2u)];
        SegmentVelocity(previous, newest, pose.velocityPerTick, &pose.yawVelocityPerTick);
    }
    double extrapolation = targetOffset;
    if (extrapolation < 0.0)
        extrapolation = 0.0;
    if (extrapolation > (double)PLAYER_INTERPOLATION_MAX_EXTRAPOLATION_TICKS)
    {
        extrapolation = (double)PLAYER_INTERPOLATION_MAX_EXTRAPOLATION_TICKS;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        pose.position[axis] += pose.velocityPerTick[axis] * extrapolation;
    }
    pose.yaw = WrapAngle(pose.yaw + pose.yawVelocityPerTick * (float)extrapolation);
    pose.extrapolated = extrapolation > 0.0;
    *outPose = pose;
    return true;
}
