#include "audio/audio_backend.h"
#include "audio/audio_output_service.h"

#include "platform/system.h"

static uint32_t CreateOutput(const LaiueAudioOutputDescription *description,
                             LaiueAudioOutputBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL)
        return 0u;

    AudioBackendDescription internal = {
        .sampleRate = description->sampleRate,
        .frameCountHint = description->frameCountHint,
        .render = description->render,
        .context = description->context,
    };
    AudioBackend *backend = NULL;
    if (!AudioSystemBackendCreate(&internal, &backend))
    {
        *outBackend = NULL;
        return 0u;
    }
    *outBackend = (LaiueAudioOutputBackend *)backend;
    return 1u;
}

static void DestroyOutput(LaiueAudioOutputBackend *backend)
{
    if (backend == NULL) return;
    AudioBackend *internal = (AudioBackend *)backend;
    if (internal->vtable != NULL && internal->vtable->destroy != NULL)
        internal->vtable->destroy(internal);
}

static uint32_t OutputSampleRate(const LaiueAudioOutputBackend *backend)
{
    const AudioBackend *internal = (const AudioBackend *)backend;
    return internal == NULL ? 0u : internal->sampleRate;
}

static uint32_t OutputChannelCount(const LaiueAudioOutputBackend *backend)
{
    const AudioBackend *internal = (const AudioBackend *)backend;
    return internal == NULL ? 0u : internal->channelCount;
}

static uint32_t OutputBufferFrameCount(const LaiueAudioOutputBackend *backend)
{
    const AudioBackend *internal = (const AudioBackend *)backend;
    return internal == NULL ? 0u : internal->bufferFrameCount;
}

static uint64_t OutputUnderrunCount(const LaiueAudioOutputBackend *backend)
{
    const AudioBackend *internal = (const AudioBackend *)backend;
    return internal == NULL || internal->vtable == NULL ||
                   internal->vtable->underrunCount == NULL
               ? 0u
               : internal->vtable->underrunCount(internal);
}

static const LaiueAudioOutputServiceV1 service = {
    .structSize = sizeof(LaiueAudioOutputServiceV1),
    .abiVersion = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
    .create = CreateOutput,
    .destroy = DestroyOutput,
    .sampleRate = OutputSampleRate,
    .channelCount = OutputChannelCount,
    .bufferFrameCount = OutputBufferFrameCount,
    .underrunCount = OutputUnderrunCount,
};

static const char *const provides[] = {LAIUE_AUDIO_OUTPUT_SERVICE_NAME};

typedef struct AudioOutputModuleState
{
    const LaiueModuleHostV1 *host;
} AudioOutputModuleState;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->allocate == NULL || host->free == NULL)
        return 0u;
    AudioOutputModuleState *state =
        (AudioOutputModuleState *)host->allocate(host->context, sizeof(*state));
    if (state == NULL)
        return 0u;
    state->host = host;
    *outContext = state;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    AudioOutputModuleState *state = (AudioOutputModuleState *)context;
    if (state == NULL || state->host == NULL)
        return 0u;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_AUDIO_OUTPUT_SERVICE_NAME,
        .version = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return state->host->publishService(state->host->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    AudioOutputModuleState *state = (AudioOutputModuleState *)context;
    if (state != NULL && state->host != NULL && state->host->unpublishService != NULL)
        (void)state->host->unpublishService(state->host->context,
                                            LAIUE_AUDIO_OUTPUT_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    AudioOutputModuleState *state = (AudioOutputModuleState *)context;
    if (state != NULL)
    {
        const LaiueModuleHostV1 *host = state->host;
        state->host = NULL;
        if (host != NULL && host->free != NULL)
            host->free(host->context, state);
        else
            PlatformFree(state);
    }
}

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.audio.output",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueAudioOutputGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
