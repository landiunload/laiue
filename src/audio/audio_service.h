#pragma once

/*
 * Public, versioned audio service.  The implementation lives in the audio
 * module; consumers only see opaque objects and call through this table.
 * Keeping the table separate from audio.h lets the module be loaded by the
 * bootstrap without importing any other LAIUE DLL.
 */

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "mod/module_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_AUDIO_SERVICE_ABI_VERSION_1 1u
#define LAIUE_AUDIO_SERVICE_NAME "laiue.audio"

typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioDeviceCreateFn)(
    const AudioDeviceConfiguration *configuration, AudioDevice **outDevice);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioDeviceCreateWithContextFn)(
    void *moduleContext, const AudioDeviceConfiguration *configuration,
    AudioDevice **outDevice);
typedef void(LAIUE_MODULE_CALL *LaiueAudioDeviceDestroyFn)(AudioDevice *device);
typedef void(LAIUE_MODULE_CALL *LaiueAudioDeviceSetMasterVolumeFn)(AudioDevice *device,
                                                                    float volume);
typedef float(LAIUE_MODULE_CALL *LaiueAudioDeviceGetMasterVolumeFn)(const AudioDevice *device);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioDeviceGetStatsFn)(
    const AudioDevice *device, AudioDeviceStats *outStats);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioClipCreateFn)(
    AudioDevice *device, const AudioClipDescription *description, AudioClip **outClip);
typedef void(LAIUE_MODULE_CALL *LaiueAudioClipDestroyFn)(AudioClip *clip);
typedef double(LAIUE_MODULE_CALL *LaiueAudioClipDurationFn)(const AudioClip *clip);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioVoicePlayFn)(
    AudioDevice *device, const AudioClip *clip, const AudioVoiceParameters *parameters);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioVoiceSetParametersFn)(
    AudioDevice *device, AudioVoice voice, const AudioVoiceParameters *parameters);
typedef void(LAIUE_MODULE_CALL *LaiueAudioVoiceStopFn)(AudioDevice *device, AudioVoice voice);
typedef void(LAIUE_MODULE_CALL *LaiueAudioStopAllFn)(AudioDevice *device);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioVoiceIsActiveFn)(
    const AudioDevice *device, AudioVoice voice);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioRenderFramesFn)(
    AudioDevice *device, float *outFrames, uint32_t frameCount);

typedef struct LaiueAudioServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueAudioDeviceCreateFn deviceCreate;
    LaiueAudioDeviceDestroyFn deviceDestroy;
    LaiueAudioDeviceSetMasterVolumeFn deviceSetMasterVolume;
    LaiueAudioDeviceGetMasterVolumeFn deviceGetMasterVolume;
    LaiueAudioDeviceGetStatsFn deviceGetStats;
    LaiueAudioClipCreateFn clipCreate;
    LaiueAudioClipDestroyFn clipDestroy;
    LaiueAudioClipDurationFn clipDuration;
    LaiueAudioVoicePlayFn voicePlay;
    LaiueAudioVoiceSetParametersFn voiceSetParameters;
    LaiueAudioVoiceStopFn voiceStop;
    LaiueAudioStopAllFn stopAll;
    LaiueAudioVoiceIsActiveFn voiceIsActive;
    LaiueAudioRenderFramesFn renderFrames;
    uintptr_t reserved[8];
    LaiueAudioDeviceCreateWithContextFn deviceCreateWithContext;
    void *context;
} LaiueAudioServiceV1;

#define LAIUE_AUDIO_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueAudioServiceV1, deviceCreateWithContext))
#define LAIUE_AUDIO_SERVICE_V1_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueAudioServiceV1, context) + sizeof(void *)))

LAIUE_AUDIO_API const LaiueModuleApiV1 *LaiueAudioGetStaticModuleApiV1(void);
