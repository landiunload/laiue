#include "audio/audio_pack_service.h"
#include "audio/audio_pack_runtime.h"
#include "audio/audio_service.h"
#include "content/content_service.h"
#include "platform/system.h"

typedef struct AudioPackModuleState
{
    const LaiueModuleHostV1 *host;
    uint32_t runtimeInstalled;
} AudioPackModuleState;

static uint32_t Enumerate(LaiueContentCatalog *catalog, AudioPackList *outList)
{
    return AudioPackEnumerateFrom(catalog, outList) ? 1u : 0u;
}

static uint32_t Activate(LaiueContentCatalog *catalog, const wchar_t *name)
{
    return AudioPackActivateIn(catalog, name) ? 1u : 0u;
}

static uint32_t EnumerateSounds(LaiueContentCatalog *catalog, AudioPackList *outList)
{
    return AudioPackEnumerateSoundsFrom(catalog, outList) ? 1u : 0u;
}

static const LaiueAudioPackServiceV1 service = {
    .structSize = sizeof(LaiueAudioPackServiceV1),
    .abiVersion = LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1,
    .enumerate = Enumerate,
    .activate = Activate,
    .releaseList = AudioPackListRelease,
    .enumerateSounds = EnumerateSounds,
    .loadFrom = AudioClipLoadFrom,
    .loadFile = AudioClipLoadFile,
    .loadMemory = AudioClipLoadMemory,
};

static const LaiueModuleRequirementV1 requiresServices[] = {
    {LAIUE_AUDIO_SERVICE_NAME, LAIUE_AUDIO_SERVICE_ABI_VERSION_1},
    {LAIUE_CONTENT_SERVICE_NAME, LAIUE_CONTENT_SERVICE_ABI_VERSION_1},
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL ||
        host->allocate == NULL || host->free == NULL)
        return 0u;
    *outContext = NULL;
    AudioPackModuleState *state =
        (AudioPackModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    state->runtimeInstalled = 0u;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    AudioPackModuleState *state = (AudioPackModuleState *)context;
    if (state == NULL || state->host == NULL || state->host->queryService == NULL)
        return 0u;

    uint32_t audioVersion = 0u;
    uint32_t audioSize = 0u;
    const LaiueAudioServiceV1 *audio =
        (const LaiueAudioServiceV1 *)state->host->queryService(
            state->host->context, LAIUE_AUDIO_SERVICE_NAME, LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
            LAIUE_AUDIO_SERVICE_V1_LEGACY_SIZE, &audioVersion, &audioSize);
    uint32_t contentVersion = 0u;
    uint32_t contentSize = 0u;
    const LaiueContentServiceV1 *content =
        (const LaiueContentServiceV1 *)state->host->queryService(
            state->host->context, LAIUE_CONTENT_SERVICE_NAME,
            LAIUE_CONTENT_SERVICE_ABI_VERSION_1, sizeof(LaiueContentServiceV1), &contentVersion,
            &contentSize);
    if (audio == NULL || content == NULL ||
        audioSize < LAIUE_AUDIO_SERVICE_V1_LEGACY_SIZE ||
        contentSize < sizeof(*content) || audioVersion < LAIUE_AUDIO_SERVICE_ABI_VERSION_1 ||
        contentVersion < LAIUE_CONTENT_SERVICE_ABI_VERSION_1 || audio->clipCreate == NULL ||
        content->enumerate == NULL || content->releaseList == NULL)
        return 0u;

    AudioPackRuntime runtime = {audio, content};
    AudioPackRuntimeSet(&runtime);
    state->runtimeInstalled = 1u;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_AUDIO_PACK_SERVICE_NAME,
        .version = LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (state->host->publishService(state->host->context, &published) != LAIUE_MODULE_OK)
    {
        AudioPackRuntimeSet(NULL);
        state->runtimeInstalled = 0u;
        return 0u;
    }
    return 1u;
}

static void ModuleStop(void *context)
{
    AudioPackModuleState *state = (AudioPackModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context, LAIUE_AUDIO_PACK_SERVICE_NAME);
    if (state != NULL && state->runtimeInstalled != 0u)
    {
        AudioPackRuntimeSet(NULL);
        state->runtimeInstalled = 0u;
    }
}

static void ModuleDestroy(void *context)
{
    AudioPackModuleState *state = (AudioPackModuleState *)context;
    if (state != NULL && state->runtimeInstalled != 0u)
        AudioPackRuntimeSet(NULL);
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        state->runtimeInstalled = 0u;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
    }
}

static const char *const provides[] = {LAIUE_AUDIO_PACK_SERVICE_NAME};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.audio.pack",
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

const LaiueModuleApiV1 *LaiueAudioPackGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
