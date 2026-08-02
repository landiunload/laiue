#include "gameplay/player_replication.h"
#include "test_runtime.h"

static uint32_t g_checks;

#define JITTER_INPUT_COUNT 360u
#define JITTER_STATE_CAPACITY 160u

typedef struct DelayedInput
{
    uint32_t arrivalTick;
    uint32_t sequence;
    PlayerControllerCommand command;
} DelayedInput;

typedef struct DelayedAuthoritativeState
{
    uint32_t deliveryTick;
    uint32_t serverTick;
    uint32_t acknowledgedSequence;
    PlayerControllerState state;
} DelayedAuthoritativeState;

static DelayedInput g_delayedInputs[JITTER_INPUT_COUNT];
static DelayedAuthoritativeState g_delayedStates[JITTER_STATE_CAPACITY];
static PlayerPredictionHistory g_jitterHistory;
static PlayerPredictionHistory g_evictionHistory;

static void TestExpect(bool condition, const char *name)
{
    ++g_checks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Проверка не пройдена: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static double AbsoluteDouble(double value)
{
    return value < 0.0 ? -value : value;
}

static float AbsoluteFloat(float value)
{
    return value < 0.0f ? -value : value;
}

static bool NearDouble(double left, double right, double epsilon)
{
    return AbsoluteDouble(left - right) <= epsilon;
}

static void QueryFlatWorld(void *context, int64_t x, int64_t y, int64_t z,
                           VoxelBlockPhysics *outBlock)
{
    (void)context;
    (void)x;
    (void)y;
    outBlock->flags = z <= 0 ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.6f;
}

static void InitPlayer(PlayerController *controller, Camera *camera,
                       PlayerCollisionSource *collision)
{
    PlayerControllerConfig config;
    PlayerControllerGetDefaultConfig(&config);
    PlayerControllerInit(controller, &config);
    camera->position[0] = 0.5;
    camera->position[1] = 0.5;
    camera->position[2] = 1.0 + config.collisionEpsilon + config.standingEyeHeight;
    camera->yaw = 0.0f;
    camera->pitch = 0.0f;
    collision->context = NULL;
    collision->queryBlockPhysics = QueryFlatWorld;
    collision->queryDynamicColliders = NULL;

    PlayerControllerCommand idle = {0};
    PlayerControllerSimulateFixedSteps(controller, collision, camera, &idle, 1u);
}

static bool StateEquals(const PlayerControllerState *left, const PlayerControllerState *right,
                        double epsilon)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!NearDouble(left->position[axis], right->position[axis], epsilon))
        {
            return false;
        }
    }
    return NearDouble(left->locomotionVelocityX, right->locomotionVelocityX, epsilon) &&
           NearDouble(left->locomotionVelocityY, right->locomotionVelocityY, epsilon) &&
           NearDouble(left->verticalVelocity, right->verticalVelocity, epsilon) &&
           NearDouble(left->externalVelocityX, right->externalVelocityX, epsilon) &&
           NearDouble(left->externalVelocityY, right->externalVelocityY, epsilon) &&
           NearDouble(left->jumpBufferRemaining, right->jumpBufferRemaining, epsilon) &&
           NearDouble(left->coyoteTimeRemaining, right->coyoteTimeRemaining, epsilon) &&
           NearDouble(left->colliderCrouchProgress, right->colliderCrouchProgress, epsilon) &&
           NearDouble(left->eyeCrouchProgress, right->eyeCrouchProgress, epsilon) &&
           left->airJumpsRemaining == right->airJumpsRemaining &&
           left->crouchingRequested == right->crouchingRequested &&
           left->grounded == right->grounded;
}

static void TestStateCaptureRestore(void)
{
    PlayerController controller;
    Camera camera;
    PlayerCollisionSource collision;
    InitPlayer(&controller, &camera, &collision);

    PlayerControllerApplyImpulse(&controller, 1.25f, -0.5f, 2.0f);
    PlayerControllerCommand command = {
        .movementX = 1.0,
        .movementY = 0.0,
        .crouchHeld = true,
    };
    PlayerControllerSimulateFixedSteps(&controller, &collision, &camera, &command, 7u);

    PlayerControllerState captured;
    PlayerControllerCaptureState(&controller, &camera, &captured);
    controller.simulationAccumulator = 0.125;

    PlayerControllerCommand other = {
        .movementX = -1.0,
        .jumpPressed = true,
        .jumpHeld = true,
    };
    PlayerControllerSimulateFixedSteps(&controller, &collision, &camera, &other, 20u);
    TestExpect(PlayerControllerRestoreState(&controller, &camera, &captured),
               "valid state restore");
    TestExpect(controller.simulationAccumulator == 0.0, "restore clears accumulator");

    PlayerControllerState restored;
    PlayerControllerCaptureState(&controller, &camera, &restored);
    TestExpect(StateEquals(&captured, &restored, 0.0), "capture restore exact round trip");

    union
    {
        uint64_t bits;
        double value;
    } notFinite = {0x7ff8000000000001ULL};
    PlayerControllerState invalid = captured;
    invalid.position[0] = notFinite.value;
    TestExpect(!PlayerControllerRestoreState(&controller, &camera, &invalid),
               "non-finite state rejected");
    PlayerControllerState afterRejected;
    PlayerControllerCaptureState(&controller, &camera, &afterRejected);
    TestExpect(StateEquals(&restored, &afterRejected, 0.0), "invalid restore is atomic");
}

static void BuildCommand(uint32_t index, PlayerControllerCommand *command)
{
    *command = (PlayerControllerCommand){0};
    switch (index % 4u)
    {
    case 0u:
        command->movementX = 1.0;
        break;
    case 1u:
        command->movementY = 1.0;
        break;
    case 2u:
        command->movementX = -1.0;
        command->sprintHeld = true;
        break;
    default:
        command->movementY = -1.0;
        command->crouchHeld = true;
        break;
    }
}

static void TestExactStepScheduler(void)
{
    PlayerController exact;
    PlayerController framed;
    Camera exactCamera;
    Camera framedCamera;
    PlayerCollisionSource exactCollision;
    PlayerCollisionSource framedCollision;
    InitPlayer(&exact, &exactCamera, &exactCollision);
    InitPlayer(&framed, &framedCamera, &framedCollision);

    float halfServerTick = 1.0f / 120.0f;
    for (uint32_t tick = 0u; tick < 120u; ++tick)
    {
        PlayerControllerCommand command;
        BuildCommand(tick, &command);
        PlayerControllerSimulateFixedSteps(&exact, &exactCollision, &exactCamera, &command, 4u);

        // Два render frames по два physics substeps должны дать тот же
        // результат, что один authoritative command из четырёх substeps.
        PlayerControllerUpdate(&framed, &framedCollision, &framedCamera, &command, halfServerTick);
        command.jumpPressed = false;
        PlayerControllerUpdate(&framed, &framedCollision, &framedCamera, &command, halfServerTick);
    }

    PlayerControllerState exactState;
    PlayerControllerState framedState;
    PlayerControllerCaptureState(&exact, &exactCamera, &exactState);
    PlayerControllerCaptureState(&framed, &framedCamera, &framedState);
    TestExpect(StateEquals(&exactState, &framedState, 0.0),
               "fixed steps independent from render partition");
}

static void TestRenderRateIndependence(void)
{
    enum
    {
        TEST_SECONDS = 10,
        NETWORK_TICKS_PER_SECOND = 60,
        PHYSICS_STEPS_PER_NETWORK_TICK = 4,
    };
    static const uint32_t renderRates[] = {30u, 60u, 144u};

    PlayerController reference;
    Camera referenceCamera;
    PlayerCollisionSource referenceCollision;
    InitPlayer(&reference, &referenceCamera, &referenceCollision);
    PlayerControllerCommand command = {
        .movementX = 0.6,
        .movementY = 0.8,
        .sprintHeld = true,
    };
    for (uint32_t tick = 0u; tick < TEST_SECONDS * NETWORK_TICKS_PER_SECOND; ++tick)
    {
        PlayerControllerSimulateFixedSteps(&reference, &referenceCollision, &referenceCamera,
                                           &command, PHYSICS_STEPS_PER_NETWORK_TICK);
    }
    PlayerControllerState referenceState;
    PlayerControllerCaptureState(&reference, &referenceCamera, &referenceState);

    for (uint32_t rateIndex = 0u; rateIndex < sizeof(renderRates) / sizeof(renderRates[0]);
         ++rateIndex)
    {
        uint32_t renderRate = renderRates[rateIndex];
        PlayerController framed;
        Camera framedCamera;
        PlayerCollisionSource framedCollision;
        InitPlayer(&framed, &framedCamera, &framedCollision);
        double frameSeconds = 1.0 / (double)renderRate;
        for (uint32_t frame = 0u; frame < TEST_SECONDS * renderRate; ++frame)
        {
            PlayerControllerUpdate(&framed, &framedCollision, &framedCamera, &command,
                                   frameSeconds);
        }

        PlayerControllerState framedState;
        PlayerControllerCaptureState(&framed, &framedCamera, &framedState);
        TestExpect(StateEquals(&referenceState, &framedState, 0.0),
                   renderRate == 30u   ? "30 FPS matches authoritative fixed ticks"
                   : renderRate == 60u ? "60 FPS matches authoritative fixed ticks"
                                       : "144 FPS matches authoritative fixed ticks");
    }
}

static void TestNetworkFixedTickClock(void)
{
    enum
    {
        TEST_SECONDS = 10,
        NETWORK_TICKS_PER_SECOND = 60,
        MAXIMUM_CATCH_UP_TICKS = 8,
    };
    static const uint32_t renderRates[] = {30u, 60u, 144u};
    const double fixedTickSeconds = 1.0 / (double)NETWORK_TICKS_PER_SECOND;

    for (uint32_t rateIndex = 0u; rateIndex < sizeof(renderRates) / sizeof(renderRates[0]);
         ++rateIndex)
    {
        PlayerFixedTickClock clock;
        PlayerFixedTickClockInit(&clock);
        uint32_t ticks = 0u;
        uint32_t renderRate = renderRates[rateIndex];
        for (uint32_t frame = 0u; frame < TEST_SECONDS * renderRate; ++frame)
        {
            PlayerFixedTickClockAccumulate(&clock, 1.0 / (double)renderRate, fixedTickSeconds,
                                           MAXIMUM_CATCH_UP_TICKS);
            while (PlayerFixedTickClockConsumeTick(&clock, fixedTickSeconds))
            {
                ++ticks;
            }
        }
        TestExpect(ticks == TEST_SECONDS * NETWORK_TICKS_PER_SECOND,
                   renderRate == 30u   ? "network clock emits 60 Hz at 30 FPS"
                   : renderRate == 60u ? "network clock emits 60 Hz at 60 FPS"
                                       : "network clock emits 60 Hz at 144 FPS");
        TestExpect(NearDouble(PlayerFixedTickClockAlpha(&clock, fixedTickSeconds), 0.0, 1e-9),
                   "network clock has no render-rate drift");
    }

    PlayerFixedTickClock clock;
    PlayerFixedTickClockInit(&clock);
    PlayerFixedTickClockAccumulate(&clock, 10.0, fixedTickSeconds, MAXIMUM_CATCH_UP_TICKS);
    uint32_t catchUpTicks = 0u;
    while (PlayerFixedTickClockConsumeTick(&clock, fixedTickSeconds))
    {
        ++catchUpTicks;
    }
    TestExpect(catchUpTicks == MAXIMUM_CATCH_UP_TICKS, "network clock bounds catch-up work");

    PlayerFixedTickClockAccumulate(&clock, fixedTickSeconds, fixedTickSeconds,
                                   MAXIMUM_CATCH_UP_TICKS);
    TestExpect(PlayerFixedTickClockHasTick(&clock, fixedTickSeconds),
               "unconsumed network tick survives send backpressure");
    PlayerFixedTickClockAccumulate(&clock, 0.0, fixedTickSeconds, MAXIMUM_CATCH_UP_TICKS);
    TestExpect(PlayerFixedTickClockConsumeTick(&clock, fixedTickSeconds) &&
                   !PlayerFixedTickClockHasTick(&clock, fixedTickSeconds),
               "network tick is removed only after explicit consume");
}

static void TestPredictionReconcile(void)
{
    PlayerController predicted;
    PlayerController authoritative;
    PlayerController reference;
    Camera predictedCamera;
    Camera authoritativeCamera;
    Camera referenceCamera;
    PlayerCollisionSource predictedCollision;
    PlayerCollisionSource authoritativeCollision;
    PlayerCollisionSource referenceCollision;
    InitPlayer(&predicted, &predictedCamera, &predictedCollision);
    InitPlayer(&authoritative, &authoritativeCamera, &authoritativeCollision);
    InitPlayer(&reference, &referenceCamera, &referenceCollision);

    // History intentionally lives outside the small no-CRT Windows stack:
    // production также хранит bounded ring в долговечном session state.
    static PlayerPredictionHistory history;
    PlayerPredictionHistoryInit(&history);
    PlayerControllerCommand commands[8];
    uint32_t sequences[8];
    uint32_t sequence = UINT32_MAX - 3u;
    PlayerControllerState authoritativeState = {0};

    for (uint32_t index = 0u; index < 8u; ++index)
    {
        sequences[index] = sequence;
        ++sequence;
        if (sequence == 0u)
            sequence = 1u;
        BuildCommand(index, &commands[index]);
        PlayerControllerSimulateFixedSteps(&predicted, &predictedCollision, &predictedCamera,
                                           &commands[index], 4u);
        PlayerControllerState predictedState;
        PlayerControllerCaptureState(&predicted, &predictedCamera, &predictedState);
        TestExpect(PlayerPredictionHistoryRecord(&history, sequences[index], &commands[index],
                                                 &predictedState),
                   "prediction record");

        if (index < 4u)
        {
            PlayerControllerSimulateFixedSteps(&authoritative, &authoritativeCollision,
                                               &authoritativeCamera, &commands[index], 4u);
            PlayerControllerSimulateFixedSteps(&reference, &referenceCollision, &referenceCamera,
                                               &commands[index], 4u);
        }
    }

    PlayerControllerCaptureState(&authoritative, &authoritativeCamera, &authoritativeState);
    authoritativeState.position[0] -= 0.125;
    TestExpect(PlayerControllerRestoreState(&reference, &referenceCamera, &authoritativeState),
               "reference correction restore");
    for (uint32_t index = 4u; index < 8u; ++index)
    {
        PlayerControllerSimulateFixedSteps(&reference, &referenceCollision, &referenceCamera,
                                           &commands[index], 4u);
    }

    uint32_t replayed = 0u;
    PlayerPredictionReconcileResult result = PlayerPredictionHistoryReconcile(
        &history, 100u, sequences[3], &authoritativeState, &predicted, &predictedCollision,
        &predictedCamera, 4u, &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED, "authoritative correction applied");
    TestExpect(replayed == 4u && PlayerPredictionHistoryCount(&history) == 4u,
               "only unacknowledged commands replayed");

    PlayerControllerState replayedState;
    PlayerControllerState referenceState;
    PlayerControllerCaptureState(&predicted, &predictedCamera, &replayedState);
    PlayerControllerCaptureState(&reference, &referenceCamera, &referenceState);
    TestExpect(StateEquals(&replayedState, &referenceState, 0.0), "reconcile replay exact result");

    result = PlayerPredictionHistoryReconcile(&history, 100u, sequences[3], &authoritativeState,
                                              &predicted, &predictedCollision, &predictedCamera, 4u,
                                              &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_STALE,
               "duplicate authoritative state rejected");

    result = PlayerPredictionHistoryReconcile(&history, 101u, sequences[3], &authoritativeState,
                                              &predicted, &predictedCollision, &predictedCamera, 4u,
                                              &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED && replayed == 4u,
               "newer server tick with unchanged acknowledgement applied");

    static PlayerPredictionHistory rolloverHistory;
    PlayerPredictionHistoryInit(&rolloverHistory);
    TestExpect(PlayerPredictionHistoryRecord(&rolloverHistory, 1u, &commands[4], &replayedState),
               "zero-reserved rollover record");
    result = PlayerPredictionHistoryReconcile(&rolloverHistory, UINT32_MAX, UINT32_MAX,
                                              &authoritativeState, &predicted, &predictedCollision,
                                              &predictedCamera, 4u, &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED && replayed == 1u,
               "zero-reserved rollover replay");
    result = PlayerPredictionHistoryReconcile(&rolloverHistory, 0u, UINT32_MAX, &authoritativeState,
                                              &predicted, &predictedCollision, &predictedCamera, 4u,
                                              &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED && replayed == 1u,
               "authoritative server tick wrap");
}

static void TestPredictionHistoryEviction(void)
{
    PlayerController controller;
    Camera camera;
    PlayerCollisionSource collision;
    InitPlayer(&controller, &camera, &collision);

    PlayerControllerCommand command = {0};
    PlayerControllerState state;
    PlayerControllerCaptureState(&controller, &camera, &state);
    PlayerPredictionHistoryInit(&g_evictionHistory);

    const uint32_t evictedEntries = 4u;
    for (uint32_t sequence = 1u; sequence <= PLAYER_PREDICTION_HISTORY_CAPACITY + evictedEntries;
         ++sequence)
    {
        TestExpect(PlayerPredictionHistoryRecord(&g_evictionHistory, sequence, &command, &state),
                   "prediction eviction record");
    }
    TestExpect(PlayerPredictionHistoryCount(&g_evictionHistory) ==
                   PLAYER_PREDICTION_HISTORY_CAPACITY,
               "prediction history stays bounded at 256 entries");
    TestExpect(g_evictionHistory.entries[g_evictionHistory.start].sequence == evictedEntries + 1u,
               "prediction history evicts the oldest entry");

    uint32_t originalStart = g_evictionHistory.start;
    PlayerControllerState beforeMiss;
    PlayerControllerCaptureState(&controller, &camera, &beforeMiss);
    PlayerControllerState authoritative = beforeMiss;
    authoritative.position[0] += 0.25;
    uint32_t replayed = UINT32_MAX;
    PlayerPredictionReconcileResult result = PlayerPredictionHistoryReconcile(
        &g_evictionHistory, 1u, evictedEntries - 1u, &authoritative, &controller, &collision,
        &camera, 4u, &replayed);
    PlayerControllerState afterMiss;
    PlayerControllerCaptureState(&controller, &camera, &afterMiss);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_HISTORY_MISS && replayed == 0u,
               "ack older than the retained prediction window is a miss");
    TestExpect(g_evictionHistory.start == originalStart &&
                   PlayerPredictionHistoryCount(&g_evictionHistory) ==
                       PLAYER_PREDICTION_HISTORY_CAPACITY &&
                   !g_evictionHistory.hasAuthoritativeState &&
                   StateEquals(&beforeMiss, &afterMiss, 0.0),
               "history miss leaves prediction and controller unchanged");

    result = PlayerPredictionHistoryReconcile(&g_evictionHistory, 1u, evictedEntries, &beforeMiss,
                                              &controller, &collision, &camera, 4u, &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED &&
                   replayed == PLAYER_PREDICTION_HISTORY_CAPACITY,
               "ack immediately before retained history replays safely");

    result = PlayerPredictionHistoryReconcile(
        &g_evictionHistory, 2u, PLAYER_PREDICTION_HISTORY_CAPACITY + evictedEntries + 1u,
        &beforeMiss, &controller, &collision, &camera, 4u, &replayed);
    TestExpect(result == PLAYER_PREDICTION_RECONCILE_HISTORY_MISS,
               "ack newer than local prediction history is a miss");
}

static void BuildJitterCommand(uint32_t tick, PlayerControllerCommand *command)
{
    *command = (PlayerControllerCommand){0};
    if (tick == JITTER_INPUT_COUNT - 1u)
    {
        return;
    }
    switch ((tick / 45u) % 4u)
    {
    case 0u:
        command->movementX = 1.0;
        break;
    case 1u:
        command->movementY = 1.0;
        command->sprintHeld = true;
        break;
    case 2u:
        command->movementX = -1.0;
        command->crouchHeld = true;
        break;
    default:
        command->movementY = -1.0;
        break;
    }
    command->jumpPressed = tick == 30u || tick == 170u;
    command->jumpHeld = (tick >= 30u && tick < 42u) || (tick >= 170u && tick < 182u);
}

static void TestLatencyJitterReconciliation(void)
{
    PlayerController predicted;
    PlayerController authoritative;
    Camera predictedCamera;
    Camera authoritativeCamera;
    PlayerCollisionSource predictedCollision;
    PlayerCollisionSource authoritativeCollision;
    InitPlayer(&predicted, &predictedCamera, &predictedCollision);
    InitPlayer(&authoritative, &authoritativeCamera, &authoritativeCollision);
    PlayerPredictionHistoryInit(&g_jitterHistory);

    uint32_t newestInputArrival = 0u;
    uint32_t availableInputs = 0u;
    uint32_t consumedInputs = 0u;
    uint32_t stateCount = 0u;
    uint32_t deliveredStates = 0u;
    uint32_t newestStateDelivery = 0u;
    uint32_t acknowledgedSequence = 0u;
    uint32_t appliedStates = 0u;
    PlayerControllerCommand serverCommand = {0};

    for (uint32_t wallTick = 0u; wallTick < JITTER_INPUT_COUNT; ++wallTick)
    {
        PlayerControllerCommand command;
        BuildJitterCommand(wallTick, &command);
        uint32_t sequence = wallTick + 1u;
        PlayerControllerSimulateFixedSteps(&predicted, &predictedCollision, &predictedCamera,
                                           &command, 4u);
        PlayerControllerState predictedState;
        PlayerControllerCaptureState(&predicted, &predictedCamera, &predictedState);
        TestExpect(
            PlayerPredictionHistoryRecord(&g_jitterHistory, sequence, &command, &predictedState),
            "jitter prediction history record");

        // QUIC preserves order, while variable delivery delay creates
        // realistic gaps and short bursts at the server.
        uint32_t arrivalTick = wallTick + 5u + ((wallTick * 5u) % 7u);
        if (arrivalTick < newestInputArrival)
            arrivalTick = newestInputArrival;
        newestInputArrival = arrivalTick;
        g_delayedInputs[wallTick] = (DelayedInput){
            .arrivalTick = arrivalTick,
            .sequence = sequence,
            .command = command,
        };

        while (availableInputs <= wallTick &&
               g_delayedInputs[availableInputs].arrivalTick <= wallTick)
        {
            ++availableInputs;
        }
        if (consumedInputs < availableInputs)
        {
            serverCommand = g_delayedInputs[consumedInputs].command;
            acknowledgedSequence = g_delayedInputs[consumedInputs].sequence;
            ++consumedInputs;
        }
        else
        {
            serverCommand.jumpPressed = false;
        }
        if (wallTick == 140u)
        {
            // Server-only gameplay event must arrive through the full
            // authoritative state and survive replay.
            PlayerControllerApplyImpulse(&authoritative, 0.75f, -0.5f, 2.5f);
        }
        PlayerControllerSimulateFixedSteps(&authoritative, &authoritativeCollision,
                                           &authoritativeCamera, &serverCommand, 4u);
        serverCommand.jumpPressed = false;

        if (wallTick % 3u == 0u)
        {
            TestExpect(stateCount < JITTER_STATE_CAPACITY, "jitter authoritative state capacity");
            uint32_t deliveryTick = wallTick + 4u + ((wallTick * 5u) % 6u);
            if (deliveryTick < newestStateDelivery)
                deliveryTick = newestStateDelivery;
            newestStateDelivery = deliveryTick;
            DelayedAuthoritativeState *delayed = &g_delayedStates[stateCount++];
            delayed->deliveryTick = deliveryTick;
            delayed->serverTick = wallTick + 1u;
            delayed->acknowledgedSequence = acknowledgedSequence;
            PlayerControllerCaptureState(&authoritative, &authoritativeCamera, &delayed->state);
        }

        while (deliveredStates < stateCount &&
               g_delayedStates[deliveredStates].deliveryTick <= wallTick)
        {
            const DelayedAuthoritativeState *delayed = &g_delayedStates[deliveredStates++];
            uint32_t replayed = 0u;
            PlayerPredictionReconcileResult result = PlayerPredictionHistoryReconcile(
                &g_jitterHistory, delayed->serverTick, delayed->acknowledgedSequence,
                &delayed->state, &predicted, &predictedCollision, &predictedCamera, 4u, &replayed);
            TestExpect(result == PLAYER_PREDICTION_RECONCILE_APPLIED,
                       "latency/jitter authoritative state applied");
            ++appliedStates;
        }
    }

    uint32_t drainTick = JITTER_INPUT_COUNT;
    while (consumedInputs < JITTER_INPUT_COUNT)
    {
        while (availableInputs < JITTER_INPUT_COUNT &&
               g_delayedInputs[availableInputs].arrivalTick <= drainTick)
        {
            ++availableInputs;
        }
        if (consumedInputs < availableInputs)
        {
            serverCommand = g_delayedInputs[consumedInputs].command;
            acknowledgedSequence = g_delayedInputs[consumedInputs].sequence;
            ++consumedInputs;
        }
        else
        {
            serverCommand.jumpPressed = false;
        }
        PlayerControllerSimulateFixedSteps(&authoritative, &authoritativeCollision,
                                           &authoritativeCamera, &serverCommand, 4u);
        serverCommand.jumpPressed = false;
        ++drainTick;
    }

    PlayerControllerState finalAuthoritative;
    PlayerControllerCaptureState(&authoritative, &authoritativeCamera, &finalAuthoritative);
    uint32_t replayed = UINT32_MAX;
    PlayerPredictionReconcileResult finalResult = PlayerPredictionHistoryReconcile(
        &g_jitterHistory, drainTick, acknowledgedSequence, &finalAuthoritative, &predicted,
        &predictedCollision, &predictedCamera, 4u, &replayed);
    PlayerControllerState finalPredicted;
    PlayerControllerCaptureState(&predicted, &predictedCamera, &finalPredicted);
    TestExpect(appliedStates > 50u, "latency/jitter exercised repeated reconciliation");
    TestExpect(finalResult == PLAYER_PREDICTION_RECONCILE_APPLIED && replayed == 0u &&
                   PlayerPredictionHistoryCount(&g_jitterHistory) == 0u,
               "latency/jitter final acknowledgement drained history");
    TestExpect(StateEquals(&finalPredicted, &finalAuthoritative, 0.0),
               "latency/jitter converged exactly after server impulse");
}

static PlayerInterpolationSnapshot MakeSnapshot(uint32_t tick, double x, float yaw)
{
    PlayerInterpolationSnapshot snapshot = {
        .serverTick = tick,
        .position = {x, 2.0, 3.0},
        .yaw = yaw,
        .pitch = 0.25f,
        .grounded = true,
    };
    return snapshot;
}

static void TestInterpolation(void)
{
    PlayerInterpolationBuffer buffer;
    PlayerInterpolationBufferInit(&buffer);
    for (uint32_t tick = 100u; tick <= 110u; ++tick)
    {
        float yaw = 0.1f * (float)(tick - 100u);
        if (tick == 104u)
            yaw = 3.10f;
        if (tick == 105u)
            yaw = -3.10f;
        PlayerInterpolationSnapshot snapshot = MakeSnapshot(tick, (double)(tick - 100u), yaw);
        TestExpect(PlayerInterpolationBufferPush(&buffer, &snapshot),
                   "ordered interpolation snapshot");
    }

    PlayerInterpolationSnapshot stale = MakeSnapshot(109u, 9.0, 0.0f);
    TestExpect(!PlayerInterpolationBufferPush(&buffer, &stale),
               "stale interpolation snapshot rejected");

    PlayerInterpolatedPose pose;
    TestExpect(PlayerInterpolationBufferSample(&buffer, 0.0, &pose),
               "delayed interpolation sample");
    TestExpect(NearDouble(pose.position[0], 4.0, 1e-12) && !pose.extrapolated,
               "six tick interpolation delay");

    TestExpect(PlayerInterpolationBufferSample(&buffer, 0.5, &pose),
               "wrapped angle interpolation sample");
    TestExpect(AbsoluteFloat(pose.yaw) > 3.0f && NearDouble(pose.position[0], 4.5, 1e-12),
               "yaw follows shortest wrapped arc");
    TestExpect(NearDouble(pose.velocityPerTick[0], 1.0, 1e-12), "interpolation velocity");

    TestExpect(PlayerInterpolationBufferSample(&buffer, 8.0, &pose),
               "bounded extrapolation sample");
    TestExpect(pose.extrapolated && NearDouble(pose.position[0], 12.0, 1e-12),
               "two tick extrapolation");
    TestExpect(PlayerInterpolationBufferSample(&buffer, 100.0, &pose) &&
                   NearDouble(pose.position[0], 12.0, 1e-12),
               "extrapolation freezes after bound");

    PlayerInterpolationSnapshot teleport = MakeSnapshot(111u, 100.0, 0.0f);
    TestExpect(PlayerInterpolationBufferPush(&buffer, &teleport), "teleport snapshot accepted");
    TestExpect(PlayerInterpolationBufferSample(&buffer, 0.0, &pose) &&
                   NearDouble(pose.position[0], 100.0, 0.0),
               "teleport clears interpolation trail");

    PlayerInterpolationBufferInit(&buffer);
    PlayerInterpolationSnapshot beforeWrap = MakeSnapshot(UINT32_MAX, 0.0, 0.0f);
    PlayerInterpolationSnapshot afterWrap = MakeSnapshot(0u, 1.0, 0.1f);
    TestExpect(PlayerInterpolationBufferPush(&buffer, &beforeWrap) &&
                   PlayerInterpolationBufferPush(&buffer, &afterWrap),
               "server tick wrap is ordered");
}

static void TestGroundProbeSnap(void)
{
    PlayerController controller;
    Camera camera;
    PlayerCollisionSource collision;
    PlayerControllerConfig config;
    PlayerControllerGetDefaultConfig(&config);
    PlayerControllerInit(&controller, &config);
    collision.context = NULL;
    collision.queryBlockPhysics = QueryFlatWorld;
    collision.queryDynamicColliders = NULL;
    camera.position[0] = 0.5;
    camera.position[1] = 0.5;
    camera.position[2] = 1.02 + config.standingEyeHeight;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;

    PlayerControllerState falling;
    PlayerControllerCaptureState(&controller, &camera, &falling);
    falling.verticalVelocity = -0.25;
    falling.grounded = false;
    TestExpect(PlayerControllerRestoreState(&controller, &camera, &falling), "falling state setup");

    PlayerControllerCommand idle = {0};
    PlayerControllerSimulateFixedSteps(&controller, &collision, &camera, &idle, 1u);
    double expectedEyeZ = 1.0 + config.collisionEpsilon + config.standingEyeHeight;
    TestExpect(PlayerControllerIsGrounded(&controller), "falling probe confirms ground");
    TestExpect(NearDouble(camera.position[2], expectedEyeZ, 1e-12),
               "ground probe snaps to contact instead of hovering");
    TestExpect(controller.jump.verticalVelocity == 0.0, "landing velocity canonical zero");
}

LAIUE_TEST_ENTRY(PlayerReplicationTestEntryPoint)
{
    TestStateCaptureRestore();
    TestExactStepScheduler();
    TestRenderRateIndependence();
    TestNetworkFixedTickClock();
    TestPredictionReconcile();
    TestPredictionHistoryEviction();
    TestLatencyJitterReconciliation();
    TestInterpolation();
    TestGroundProbeSnap();
    LaiueTestRuntimeWrite("player replication checks: OK\r\n");
    LAIUE_TEST_SUCCESS();
}
