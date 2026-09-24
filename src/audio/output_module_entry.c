#include "audio/audio_backend.h"
#include "audio/audio_output_service.h"

#include "platform/system.h"

typedef struct AudioOutputModuleState AudioOutputModuleState;

struct AudioOutputModuleState
{
    const LaiueModuleHostV1 *host;
    LaiueAudioOutputServiceV1 service;
};

static uint32_t CreateOutputInternal(const LaiueAudioOutputDescription *description,
                                     LaiueAudioOutputBackend **outBackend,
                                     const LaiueModuleHostV1 *host)
{
    if (description == NULL || outBackend == NULL || description->render == NULL)
        return 0u;

    AudioBackendDescription internal = {
        .sampleRate = description->sampleRate,
        .frameCountHint = description->frameCountHint,
        .render = description->render,
        .context = description->context,
    };
    if (host != NULL)
    {
        internal.allocator.context = host->context;
        internal.allocator.allocate = host->allocate;
        internal.allocator.free = host->free;
    }
    if (!AudioBackendAllocatorIsValid(&internal.allocator))
    {
        *outBackend = NULL;
        return 0u;
    }
    AudioBackend *backend = NULL;
    if (!AudioSystemBackendCreate(&internal, &backend))
    {
        *outBackend = NULL;
        return 0u;
    }
    *outBackend = (LaiueAudioOutputBackend *)backend;
    return 1u;
}

static uint32_t CreateOutput(const LaiueAudioOutputDescription *description,
                             LaiueAudioOutputBackend **outBackend)
{
    return CreateOutputInternal(description, outBackend, NULL);
}

static uint32_t CreateOutputWithContext(void *moduleContext,
                                       const LaiueAudioOutputDescription *description,
                                       LaiueAudioOutputBackend **outBackend)
{
    AudioOutputModuleState *state = (AudioOutputModuleState *)moduleContext;
    if (state == NULL || state->host == NULL)
    {
        if (outBackend != NULL) *outBackend = NULL;
        return 0u;
    }
    return CreateOutputInternal(description, outBackend, state->host);
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

static const char *const provides[] = {LAIUE_AUDIO_OUTPUT_SERVICE_NAME};

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
    state->service = (LaiueAudioOutputServiceV1){
        .structSize = sizeof(LaiueAudioOutputServiceV1),
        .abiVersion = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
        .create = CreateOutput,
        .destroy = DestroyOutput,
        .sampleRate = OutputSampleRate,
        .channelCount = OutputChannelCount,
        .bufferFrameCount = OutputBufferFrameCount,
        .underrunCount = OutputUnderrunCount,
        .createWithContext = CreateOutputWithContext,
        .context = state,
    };
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
        .table = &state->service,
        .tableSize = state->service.structSize,
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
