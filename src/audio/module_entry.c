#include "audio/audio_service.h"
#include "audio/audio_mixer_internal.h"
#include "audio/audio_output_service.h"

static LaiueModuleHostV1 const *moduleHost;
static const LaiueAudioOutputServiceV1 *moduleOutput;

static uint32_t DeviceCreate(const AudioDeviceConfiguration *configuration,
                             AudioDevice **outDevice)
{
    return (uint32_t)AudioDeviceCreate(configuration, outDevice);
}

static void DeviceDestroy(AudioDevice *device)
{
    AudioDeviceDestroy(device);
}

static void DeviceSetMasterVolume(AudioDevice *device, float volume)
{
    AudioDeviceSetMasterVolume(device, volume);
}

static float DeviceGetMasterVolume(const AudioDevice *device)
{
    return AudioDeviceGetMasterVolume(device);
}

static uint32_t DeviceGetStats(const AudioDevice *device, AudioDeviceStats *outStats)
{
    return AudioDeviceGetStats(device, outStats) ? 1u : 0u;
}

static uint32_t ClipCreate(AudioDevice *device, const AudioClipDescription *description,
                           AudioClip **outClip)
{
    return (uint32_t)AudioClipCreate(device, description, outClip);
}

static void ClipDestroy(AudioClip *clip)
{
    AudioClipDestroy(clip);
}

static double ClipDuration(const AudioClip *clip)
{
    return AudioClipDurationSeconds(clip);
}

static uint32_t VoicePlay(AudioDevice *device, const AudioClip *clip,
                          const AudioVoiceParameters *parameters)
{
    return AudioVoicePlay(device, clip, parameters);
}

static uint32_t VoiceSetParameters(AudioDevice *device, AudioVoice voice,
                                   const AudioVoiceParameters *parameters)
{
    return AudioVoiceSetParameters(device, voice, parameters) ? 1u : 0u;
}

static void VoiceStop(AudioDevice *device, AudioVoice voice)
{
    AudioVoiceStop(device, voice);
}

static void StopAll(AudioDevice *device)
{
    AudioDeviceStopAllVoices(device);
}

static uint32_t VoiceIsActive(const AudioDevice *device, AudioVoice voice)
{
    return AudioVoiceIsActive(device, voice) ? 1u : 0u;
}

static uint32_t RenderFrames(AudioDevice *device, float *outFrames, uint32_t frameCount)
{
    return AudioDeviceRenderFrames(device, outFrames, frameCount) ? 1u : 0u;
}

static const LaiueAudioServiceV1 service = {
    .structSize = sizeof(LaiueAudioServiceV1),
    .abiVersion = LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
    .deviceCreate = DeviceCreate,
    .deviceDestroy = DeviceDestroy,
    .deviceSetMasterVolume = DeviceSetMasterVolume,
    .deviceGetMasterVolume = DeviceGetMasterVolume,
    .deviceGetStats = DeviceGetStats,
    .clipCreate = ClipCreate,
    .clipDestroy = ClipDestroy,
    .clipDuration = ClipDuration,
    .voicePlay = VoicePlay,
    .voiceSetParameters = VoiceSetParameters,
    .voiceStop = VoiceStop,
    .stopAll = StopAll,
    .voiceIsActive = VoiceIsActive,
    .renderFrames = RenderFrames,
};

static uint32_t ModuleCreate(const LaiueModuleHostV1 *host, void **outContext)
{
    if (host == NULL || outContext == NULL || host->publishService == NULL ||
        host->unpublishService == NULL || host->queryService == NULL)
        return 0u;
    moduleHost = host;
    moduleOutput = NULL;
    AudioMixerSetOutputService(NULL);
    *outContext = (void *)&moduleHost;
    return 1u;
}

static uint32_t ModuleStart(void *context)
{
    (void)context;
    if (moduleHost == NULL || moduleHost->queryService == NULL) return 0u;
    uint32_t outputVersion = 0u;
    uint32_t outputSize = 0u;
    moduleOutput = (const LaiueAudioOutputServiceV1 *)moduleHost->queryService(
        moduleHost->context, LAIUE_AUDIO_OUTPUT_SERVICE_NAME,
        LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1, sizeof(LaiueAudioOutputServiceV1),
        &outputVersion, &outputSize);
    if (moduleOutput != NULL &&
        (outputVersion < LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1 ||
         outputSize < sizeof(*moduleOutput) || moduleOutput->create == NULL ||
         moduleOutput->destroy == NULL || moduleOutput->sampleRate == NULL ||
         moduleOutput->channelCount == NULL || moduleOutput->bufferFrameCount == NULL ||
         moduleOutput->underrunCount == NULL))
    {
        moduleOutput = NULL;
        return 0u;
    }
    AudioMixerSetOutputService(moduleOutput);
    LaiueModuleServiceV1 published = {
        .name = LAIUE_AUDIO_SERVICE_NAME,
        .version = LAIUE_AUDIO_SERVICE_ABI_VERSION_1,
        .table = &service,
        .tableSize = sizeof(service),
    };
    if (moduleHost->publishService(moduleHost->context, &published) == LAIUE_MODULE_OK)
        return 1u;
    AudioMixerSetOutputService(NULL);
    moduleOutput = NULL;
    return 0u;
}

static void ModuleStop(void *context)
{
    (void)context;
    if (moduleHost != NULL && moduleHost->unpublishService != NULL)
        (void)moduleHost->unpublishService(moduleHost->context, LAIUE_AUDIO_SERVICE_NAME);
    AudioMixerSetOutputService(NULL);
    moduleOutput = NULL;
}

static void ModuleDestroy(void *context)
{
    (void)context;
    AudioMixerSetOutputService(NULL);
    moduleOutput = NULL;
    moduleHost = NULL;
}

static const char *const provides[] = {LAIUE_AUDIO_SERVICE_NAME};
static const LaiueModuleRequirementV1 optionalServices[] = {
    {LAIUE_AUDIO_OUTPUT_SERVICE_NAME, LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1},
};

static const LaiueModuleApiV1 api = {
    .structSize = sizeof(LaiueModuleApiV1),
    .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
    .descriptor = {
        .structSize = sizeof(LaiueModuleDescriptorV1),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .id = "laiue.audio",
        .version = "1.0.0",
        .providesServices = provides,
        .providesCount = 1u,
        .optionalServices = optionalServices,
        .optionalCount = sizeof(optionalServices) / sizeof(optionalServices[0]),
        .optionalMagic = LAIUE_MODULE_DESCRIPTOR_OPTIONAL_MAGIC,
    },
    .create = ModuleCreate,
    .start = ModuleStart,
    .stop = ModuleStop,
    .destroy = ModuleDestroy,
};

const LaiueModuleApiV1 *LaiueAudioGetStaticModuleApiV1(void)
{
    return &api;
}

#if !defined(LAIUE_STATIC)
LAIUE_MODULE_EXPORT const LaiueModuleApiV1 *LAIUE_MODULE_CALL LaiueModuleGetApiV1(void)
{
    return &api;
}
#endif
