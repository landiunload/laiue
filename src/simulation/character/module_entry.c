#include "character/character_service.h"

#include "platform/system.h"

#include <limits.h>

struct LaiueCharacterControllerV1
{
    LaiueCharacterCollisionV1 collision;
    int64_t halfExtent;
    LaiueCharacterPositionV1 position;
    int64_t verticalVelocity;
    int64_t horizontalRemainderX;
    int64_t horizontalRemainderY;
    int64_t verticalRemainder;
    uint32_t grounded;
};

static bool AddInt64Checked(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL)
        return false;
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static bool MultiplyInt64Checked(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL)
        return false;
    if (left == 0 || right == 0)
    {
        *out = 0;
        return true;
    }
    if (left == -1 && right == INT64_MIN)
        return false;
    if (right == -1 && left == INT64_MIN)
        return false;
    if (left > 0)
    {
        if (right > 0 && left > INT64_MAX / right)
            return false;
        if (right < 0 && right < INT64_MIN / left)
            return false;
    }
    else
    {
        if (right > 0 && left < INT64_MIN / right)
            return false;
        if (right < 0 && left < INT64_MAX / right)
            return false;
    }
    *out = left * right;
    return true;
}

static bool DivideTickChecked(int64_t value, int64_t remainder,
                              int64_t *outDelta, int64_t *outRemainder)
{
    if (outDelta == NULL || outRemainder == NULL ||
        remainder <= -(int64_t)LAIUE_CHARACTER_TICK_HZ ||
        remainder >= (int64_t)LAIUE_CHARACTER_TICK_HZ)
        return false;
    if ((value > 0 && remainder > INT64_MAX - value) ||
        (value < 0 && remainder < INT64_MIN - value))
        return false;
    const int64_t total = value + remainder;
    *outDelta = total / LAIUE_CHARACTER_TICK_HZ;
    *outRemainder = total % LAIUE_CHARACTER_TICK_HZ;
    return true;
}

/* Floor division keeps local coordinates in [0, cellSize), including for
 * negative positions. C's remainder is truncating, so normalize it here. */
static bool NormalizeAxis(int64_t *cell, int64_t *local)
{
    if (cell == NULL || local == NULL)
        return false;
    const int64_t cellSize = LAIUE_CHARACTER_LOCAL_CELL_SIZE;
    int64_t quotient = *local / cellSize;
    int64_t remainder = *local % cellSize;
    if (remainder < 0)
    {
        if (quotient == INT64_MIN)
            return false;
        --quotient;
        remainder += cellSize;
    }
    return AddInt64Checked(*cell, quotient, cell) &&
           (*local = remainder, true);
}

static bool NormalizePosition(LaiueCharacterPositionV1 *position)
{
    return position != NULL && NormalizeAxis(&position->cellX, &position->localX) &&
           NormalizeAxis(&position->cellY, &position->localY);
}

static uint32_t CharacterCreate(
    const LaiueCharacterCollisionV1 *collision,
    int64_t halfExtent,
    LaiueCharacterControllerV1 **outController)
{
    if (outController == NULL || halfExtent < 0)
        return 0u;
    *outController = NULL;
    LaiueCharacterControllerV1 *controller =
        (LaiueCharacterControllerV1 *)PlatformAllocate(sizeof(*controller), true);
    if (controller == NULL)
        return 0u;
    if (collision != NULL)
        controller->collision = *collision;
    controller->collision.structSize = sizeof(controller->collision);
    controller->collision.abiVersion = LAIUE_CHARACTER_ABI_VERSION_1;
    controller->halfExtent = halfExtent;
    controller->horizontalRemainderX = 0;
    controller->horizontalRemainderY = 0;
    controller->verticalRemainder = 0;
    controller->grounded = 0u;
    *outController = controller;
    return 1u;
}

static void CharacterDestroy(LaiueCharacterControllerV1 *controller)
{
    PlatformFree(controller);
}

static uint32_t CharacterGetPosition(
    const LaiueCharacterControllerV1 *controller,
    LaiueCharacterPositionV1 *outPosition)
{
    if (controller == NULL || outPosition == NULL)
        return 0u;
    *outPosition = controller->position;
    return 1u;
}

static uint32_t CharacterSetPosition(
    LaiueCharacterControllerV1 *controller,
    const LaiueCharacterPositionV1 *position,
    uint32_t grounded)
{
    if (controller == NULL || position == NULL)
        return 0u;
    LaiueCharacterPositionV1 normalized = *position;
    if (!NormalizePosition(&normalized))
        return 0u;
    controller->position = normalized;
    controller->grounded = grounded != 0u ? 1u : 0u;
    controller->horizontalRemainderX = 0;
    controller->horizontalRemainderY = 0;
    controller->verticalRemainder = 0;
    if (controller->grounded)
    {
        controller->verticalVelocity = 0;
        controller->verticalRemainder = 0;
    }
    return 1u;
}

static uint32_t CharacterIsGrounded(const LaiueCharacterControllerV1 *controller)
{
    return controller != NULL && controller->grounded != 0u ? 1u : 0u;
}

static uint32_t CharacterStep(
    LaiueCharacterControllerV1 *controller,
    const LaiueCharacterInputV1 *input)
{
    if (controller == NULL || input == NULL)
        return 0u;

    const int64_t speed = (input->flags & LAIUE_CHARACTER_INPUT_SPRINT) != 0u
                              ? LAIUE_CHARACTER_SPRINT_SPEED
                              : LAIUE_CHARACTER_WALK_SPEED;
    int64_t deltaX = 0;
    int64_t deltaY = 0;
    if (!MultiplyInt64Checked(speed, (int64_t)input->moveX, &deltaX) ||
        !MultiplyInt64Checked(speed, (int64_t)input->moveY, &deltaY))
        return 0u;
    int64_t remainderX = 0;
    int64_t remainderY = 0;
    if (!DivideTickChecked(deltaX, controller->horizontalRemainderX,
                           &deltaX, &remainderX) ||
        !DivideTickChecked(deltaY, controller->horizontalRemainderY,
                           &deltaY, &remainderY))
        return 0u;

    uint32_t candidateGrounded = controller->grounded;
    int64_t candidateVelocity = controller->verticalVelocity;
    int64_t candidateVerticalRemainder = controller->verticalRemainder;
    if ((input->flags & LAIUE_CHARACTER_INPUT_JUMP) != 0u && candidateGrounded != 0u)
    {
        candidateVelocity = LAIUE_CHARACTER_JUMP_SPEED;
        candidateVerticalRemainder = 0;
        candidateGrounded = 0u;
    }
    if (candidateVelocity < INT64_MIN + LAIUE_CHARACTER_GRAVITY_PER_TICK)
        return 0u;
    candidateVelocity -= LAIUE_CHARACTER_GRAVITY_PER_TICK;
    int64_t deltaZ = 0;
    int64_t remainderZ = 0;
    if (!DivideTickChecked(candidateVelocity, candidateVerticalRemainder,
                           &deltaZ, &remainderZ))
        return 0u;

    LaiueCharacterPositionV1 next = controller->position;
    uint32_t grounded = 0u;
    if (controller->collision.sweepAabb != NULL)
    {
        if (controller->collision.sweepAabb(
                &controller->collision, &controller->position, controller->halfExtent,
                deltaX, deltaY, deltaZ, &next, &grounded) == 0u)
                return 0u;
    }
    else if (!AddInt64Checked(next.localX, deltaX, &next.localX) ||
             !AddInt64Checked(next.localY, deltaY, &next.localY) ||
             !AddInt64Checked(next.localZ, deltaZ, &next.localZ))
        return 0u;
    if (!NormalizePosition(&next))
        return 0u;
    controller->position = next;
    controller->grounded = grounded != 0u ? 1u : 0u;
    controller->horizontalRemainderX = remainderX;
    controller->horizontalRemainderY = remainderY;
    controller->verticalRemainder = remainderZ;
    controller->verticalVelocity = candidateVelocity;
    if (controller->grounded)
    {
        controller->verticalVelocity = 0;
        controller->verticalRemainder = 0;
    }
    return 1u;
}

static const LaiueCharacterServiceV1 service = {
    .structSize = sizeof(LaiueCharacterServiceV1),
    .abiVersion = LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
    .create = CharacterCreate,
    .destroy = CharacterDestroy,
    .step = CharacterStep,
    .getPosition = CharacterGetPosition,
    .setPosition = CharacterSetPosition,
    .isGrounded = CharacterIsGrounded,
};

typedef struct LaiueCharacterModuleState
{
    const LaiueModuleHostV1 *host;
} LaiueCharacterModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL ||
        host->free == NULL)
        return 0u;
    *outContext = NULL;
    LaiueCharacterModuleState *state =
        (LaiueCharacterModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiueCharacterModuleState *state = (LaiueCharacterModuleState *)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_CHARACTER_SERVICE_NAME,
        .version = LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state != NULL && state->host != NULL &&
                   state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    LaiueCharacterModuleState *state = (LaiueCharacterModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_CHARACTER_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    LaiueCharacterModuleState *state = (LaiueCharacterModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {LAIUE_CHARACTER_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.character",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueCharacterGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
