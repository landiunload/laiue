#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "api.h"
#include "gameplay/player_controller.h"

#define PLAYER_PREDICTION_HISTORY_CAPACITY 256u
#define PLAYER_INTERPOLATION_BUFFER_CAPACITY 32u
#define PLAYER_INTERPOLATION_DELAY_TICKS 6u
#define PLAYER_INTERPOLATION_MAX_EXTRAPOLATION_TICKS 2u
#define PLAYER_INTERPOLATION_TELEPORT_DISTANCE 8.0

// Frame-independent fixed-tick scheduler used by the network prediction
// path. A tick is consumed only after the caller has successfully completed
// it, so temporary send backpressure cannot silently discard simulation
// time.
typedef struct PlayerFixedTickClock
{
    double accumulatorSeconds;
} PlayerFixedTickClock;

LAIUE_GAMEPLAY_API void PlayerFixedTickClockInit(PlayerFixedTickClock *clock);

// Adds elapsed frame time and bounds catch-up work to maximumTicks. Invalid
// negative/NaN time is ignored; positive overflow is safely clamped.
LAIUE_GAMEPLAY_API void PlayerFixedTickClockAccumulate(PlayerFixedTickClock *clock,
                                                       double elapsedSeconds,
                                                       double fixedTickSeconds,
                                                       uint32_t maximumTicks);

LAIUE_GAMEPLAY_API bool PlayerFixedTickClockHasTick(const PlayerFixedTickClock *clock,
                                                    double fixedTickSeconds);

LAIUE_GAMEPLAY_API bool PlayerFixedTickClockConsumeTick(PlayerFixedTickClock *clock,
                                                        double fixedTickSeconds);

// Presentation phase in [0, 1]. This is deliberately independent from
// authoritative simulation and may only be used for interpolation.
LAIUE_GAMEPLAY_API double PlayerFixedTickClockAlpha(const PlayerFixedTickClock *clock,
                                                    double fixedTickSeconds);

typedef struct PlayerPredictionEntry
{
    uint32_t sequence;
    PlayerControllerCommand command;
    PlayerControllerState predictedState;
} PlayerPredictionEntry;

typedef struct PlayerPredictionHistory
{
    PlayerPredictionEntry entries[PLAYER_PREDICTION_HISTORY_CAPACITY];
    uint32_t start;
    uint32_t count;
    uint32_t lastAuthoritativeSequence;
    uint32_t lastAuthoritativeServerTick;
    bool hasAuthoritativeState;
} PlayerPredictionHistory;

typedef enum PlayerPredictionReconcileResult
{
    PLAYER_PREDICTION_RECONCILE_APPLIED = 0,
    PLAYER_PREDICTION_RECONCILE_STALE,
    PLAYER_PREDICTION_RECONCILE_HISTORY_MISS,
    PLAYER_PREDICTION_RECONCILE_INVALID_STATE,
} PlayerPredictionReconcileResult;

LAIUE_GAMEPLAY_API void PlayerPredictionHistoryInit(PlayerPredictionHistory *history);

// Записывает каноническую команду и состояние после её локального
// исполнения. Sequence сравниваются modulo 2^32 при условии, что активное
// окно всегда меньше 2^31; переполнение кольца удаляет самый старый entry.
LAIUE_GAMEPLAY_API bool PlayerPredictionHistoryRecord(PlayerPredictionHistory *history,
                                                      uint32_t sequence,
                                                      const PlayerControllerCommand *command,
                                                      const PlayerControllerState *predictedState);

// Восстанавливает authoritative state на acknowledgedSequence и повторяет
// все более новые команды ровно fixedStepsPerCommand раз. Свежесть state
// определяется wrap-safe server tick, поэтому новый tick с тем же ack
// (сервер продолжил гравитацию без нового input) не теряется. При успехе
// predictedState записей обновляется, подтверждённый префикс удаляется.
LAIUE_GAMEPLAY_API PlayerPredictionReconcileResult PlayerPredictionHistoryReconcile(
    PlayerPredictionHistory *history, uint32_t authoritativeServerTick,
    uint32_t acknowledgedSequence, const PlayerControllerState *authoritativeState,
    PlayerController *controller, const PlayerCollisionSource *collision, Camera *camera,
    uint32_t fixedStepsPerCommand, uint32_t *outReplayedCommands);

LAIUE_GAMEPLAY_API uint32_t PlayerPredictionHistoryCount(const PlayerPredictionHistory *history);

typedef struct PlayerInterpolationSnapshot
{
    uint32_t serverTick;
    double position[3];
    float yaw;
    float pitch;
    bool grounded;
} PlayerInterpolationSnapshot;

typedef struct PlayerInterpolatedPose
{
    double position[3];
    // Производная измеряется в blocks/server-tick, чтобы слой не зависел от
    // конкретной частоты сети. Presentation умножает её на свой tick rate.
    double velocityPerTick[3];
    float yaw;
    float pitch;
    float yawVelocityPerTick;
    bool grounded;
    bool extrapolated;
} PlayerInterpolatedPose;

typedef struct PlayerInterpolationBuffer
{
    PlayerInterpolationSnapshot snapshots[PLAYER_INTERPOLATION_BUFFER_CAPACITY];
    uint32_t start;
    uint32_t count;
} PlayerInterpolationBuffer;

LAIUE_GAMEPLAY_API void PlayerInterpolationBufferInit(PlayerInterpolationBuffer *buffer);

// Принимаются только строго более новые wrap-safe ticks. Телепорт дальше
// PLAYER_INTERPOLATION_TELEPORT_DISTANCE атомарно начинает новую историю.
LAIUE_GAMEPLAY_API bool PlayerInterpolationBufferPush(PlayerInterpolationBuffer *buffer,
                                                      const PlayerInterpolationSnapshot *snapshot);

// Семплирует newestTick - 6 + ticksSinceNewest. При сетевой паузе движение
// продолжается не более двух server ticks, затем pose замораживается.
LAIUE_GAMEPLAY_API bool PlayerInterpolationBufferSample(const PlayerInterpolationBuffer *buffer,
                                                        double ticksSinceNewest,
                                                        PlayerInterpolatedPose *outPose);
