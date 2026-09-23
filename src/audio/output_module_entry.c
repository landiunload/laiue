#include "audio/audio_backend.h"
#include "audio/audio_output_service.h"

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

static LaiueModuleHostV1 const *moduleHost;

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL)
        return 0u;
    moduleHost = host;
    *outContext = (void *)&moduleHost;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    (void)context;
    LaiueModuleServiceV1 published = {
        .name = LAIUE_AUDIO_OUTPUT_SERVICE_NAME,
        .version = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    return moduleHost != NULL &&
                   moduleHost->publishService(moduleHost->context, &published) == LAIUE_MODULE_OK
               ? 1u
               : 0u;
}

static void ModuleStop(void *context)
{
    (void)context;
    if (moduleHost != NULL && moduleHost->unpublishService != NULL)
        (void)moduleHost->unpublishService(moduleHost->context,
                                            LAIUE_AUDIO_OUTPUT_SERVICE_NAME);
}

static void ModuleDestroy(void *context)
{
    (void)context;
    moduleHost = NULL;
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

