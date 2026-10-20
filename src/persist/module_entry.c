#include "persist/persist_service.h"

static const LaiuePersistServiceV1 service = {
    .structSize = sizeof(LaiuePersistServiceV1),
    .abiVersion = LAIUE_PERSIST_SERVICE_ABI_VERSION_1,
    .documentCreate = LaiuePersistDocumentCreate,
    .documentDestroy = LaiuePersistDocumentDestroy,
    .documentBeginSection = LaiuePersistDocumentBeginSection,
    .documentAddBorrowed = LaiuePersistDocumentAddBorrowed,
    .documentAddProduced = LaiuePersistDocumentAddProduced,
    .documentCopySection = LaiuePersistDocumentCopySection,
    .documentCopyRemaining = LaiuePersistDocumentCopyRemaining,
    .sectionWrite = LaiuePersistSectionWrite,
    .sectionSize = LaiuePersistSectionSize,
    .sectionEnd = LaiuePersistSectionEnd,
    .save = LaiuePersistSave,
    .saveStart = LaiuePersistSaveStart,
    .open = LaiuePersistOpen,
    .close = LaiuePersistClose,
    .readerGetInfo = LaiuePersistReaderGetInfo,
    .readerGetSection = LaiuePersistReaderGetSection,
    .readerFind = LaiuePersistReaderFind,
    .readerRead = LaiuePersistReaderRead,
    .readerReadUpgraded = LaiuePersistReaderReadUpgraded,
    .blobRelease = LaiuePersistBlobRelease,
    .loadStart = LaiuePersistLoadStart,
    .jobPoll = LaiuePersistJobPoll,
    .jobCancel = LaiuePersistJobCancel,
    .jobWait = LaiuePersistJobWait,
    .jobDestroy = LaiuePersistJobDestroy,
    .statusName = LaiuePersistStatusName,
};

typedef struct PersistModuleState
{
    const LaiueModuleHostV1 *host;
} PersistModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    PersistModuleState *state = host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    PersistModuleState *state = context;
    const LaiueModuleServiceV1 published = {
        .name = LAIUE_PERSIST_SERVICE_NAME,
        .version = LAIUE_PERSIST_SERVICE_ABI_VERSION_1,
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
    PersistModuleState *state = context;
    if (state != NULL && state->host != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_PERSIST_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    PersistModuleState *state = context;
    if (state == NULL)
        return;
    const LaiueModuleHostV1 *host = state->host;
    state->host = NULL;
    if (host != NULL)
        host->free(host->context, state);
}

static const char *const provides[] = {LAIUE_PERSIST_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor =
        {
            .structSize = sizeof(LaiueModuleDescriptorV1),
            .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
            .id = "laiue.persist",
            .version = "1.0.0",
            .providesServices = provides,
            .providesCount = 1u,
        },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiuePersistServiceV1 *LaiuePersistGetStaticServiceV1(void)
{
    return &service;
}

const LaiueModuleApiV1 *LaiuePersistGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
