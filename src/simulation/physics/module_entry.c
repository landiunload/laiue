#include "physics/physics_service.h"

#include "mod/module_api.h"
#include "mod/module_service.h"
#include "numeric/numeric_service.h"
#include "physics/numeric_provider.h"
#include "task/task_service.h"

typedef struct LaiuePhysicsModuleState
{
    const LaiueModuleHostV1 *host;
    const LaiueNumericServiceV1 *numeric;
    const LaiueTaskServiceV1 *jobs;
} LaiuePhysicsModuleState;

static LaiuePhysicsModuleState moduleState;

static uint32_t ThreadIsConfigured(void)
{
    return VoxelPhysicsThreadIsConfigured() ? 1u : 0u;
}

static const LaiuePhysicsServiceV1 service = {
    .structSize = sizeof(LaiuePhysicsServiceV1),
    .abiVersion = LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
    .configureThread = VoxelPhysicsConfigureThread,
    .threadIsConfigured = ThreadIsConfigured,
    .bodyInitialize = VoxelRigidBodyInitialize,
    .bodyRelease = VoxelRigidBodyRelease,
    .bodyWake = VoxelRigidBodyWake,
    .stepScratchBytes = VoxelRigidBodyStepScratchBytes,
    .step = VoxelRigidBodyStep,
    .stepEx = VoxelRigidBodyStepEx,
    .stepCached = VoxelRigidBodyStepCached,
    .stepIndexed = VoxelRigidBodyStepIndexed,
    .compoundScratchBytes = VoxelRigidBodyStepCompoundScratchBytes,
    .stepCompoundEx = VoxelRigidBodyStepCompoundEx,
    .compoundMassProperties = VoxelRigidCompoundMassProperties,
    .compoundMergeBoxes = VoxelRigidCompoundMergeBoxes,
    .contactCacheBytes = VoxelRigidContactCacheBytes,
    .contactCacheInitialize = VoxelRigidContactCacheInitialize,
    .contactCacheReset = VoxelRigidContactCacheReset,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleState.host = host;
    moduleState.numeric = NULL;
    moduleState.jobs = NULL;
    PhysicsSetNumericService(NULL);
    *outContext = &moduleState;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    LaiuePhysicsModuleState *state = (LaiuePhysicsModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    PhysicsSetNumericService(NULL);
    state->numeric = NULL;
    state->jobs = NULL;
    state->numeric = (const LaiueNumericServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_NUMERIC_SERVICE_NAME,
        LAIUE_NUMERIC_SERVICE_ABI_VERSION_1, sizeof(LaiueNumericServiceV1));
    state->jobs = (const LaiueTaskServiceV1 *)LaiueModuleQueryRequiredService(
        state->host, LAIUE_TASK_SERVICE_NAME,
        LAIUE_TASK_SERVICE_ABI_VERSION_1, sizeof(LaiueTaskServiceV1));
    if (state->numeric == NULL || state->jobs == NULL)
    {
        state->numeric = NULL;
        state->jobs = NULL;
        return 0u;
    }
    PhysicsSetNumericService(state->numeric);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_PHYSICS_SERVICE_NAME,
        .version = LAIUE_PHYSICS_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        state->numeric = NULL;
        state->jobs = NULL;
        PhysicsSetNumericService(NULL);
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    LaiuePhysicsModuleState *state = (LaiuePhysicsModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                             LAIUE_PHYSICS_SERVICE_NAME);
    if (state != NULL)
    {
        state->numeric = NULL;
        state->jobs = NULL;
        PhysicsSetNumericService(NULL);
    }
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleState.host = NULL;
    moduleState.numeric = NULL;
    moduleState.jobs = NULL;
    PhysicsSetNumericService(NULL);
}

static const char *const provides[] = {LAIUE_PHYSICS_SERVICE_NAME};
static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_NUMERIC_SERVICE_NAME, LAIUE_NUMERIC_SERVICE_ABI_VERSION_1},
    {LAIUE_TASK_SERVICE_NAME, LAIUE_TASK_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.physics",
        .version = "1.0.0",
        .requiresServices = requiresServices,
        .requiresCount = sizeof(requiresServices) / sizeof(requiresServices[0]),
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiuePhysicsGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
