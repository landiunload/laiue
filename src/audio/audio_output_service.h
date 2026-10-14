#pragma once

/*
 * Optional audio-output provider.  The mixer owns voice state and PCM
 * mixing; this provider owns only a platform stream (WASAPI, ALSA, or a
 * platform-specific implementation) and pulls ready stereo float frames
 * through the callback below.
 *
 * The table is a standalone C ABI.  A build may omit the provider entirely:
 * the mixer still supports its deterministic offscreen backend and reports
 * a backend-initialization error only when system output was requested.
 */

#include "mod/module_api.h"

#include <stddef.h>
#include <stdint.h>

#define LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1 1u
#define LAIUE_AUDIO_OUTPUT_SERVICE_NAME "laiue.audio.output"

typedef struct LaiueAudioOutputBackend LaiueAudioOutputBackend;

typedef void(LAIUE_MODULE_CALL *LaiueAudioOutputRenderFn)(
    void *context, float *frames, uint32_t frameCount);

typedef struct LaiueAudioOutputDescription
{
    uint32_t sampleRate;       /* 0 selects the platform default. */
    uint32_t frameCountHint;   /* 0 selects the platform default. */
    LaiueAudioOutputRenderFn render;
    void *context;
} LaiueAudioOutputDescription;

typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioOutputCreateFn)(
    const LaiueAudioOutputDescription *description, LaiueAudioOutputBackend **outBackend);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioOutputCreateWithContextFn)(
    void *moduleContext, const LaiueAudioOutputDescription *description,
    LaiueAudioOutputBackend **outBackend);
typedef void(LAIUE_MODULE_CALL *LaiueAudioOutputDestroyFn)(LaiueAudioOutputBackend *backend);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioOutputGetSampleRateFn)(
    const LaiueAudioOutputBackend *backend);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioOutputGetChannelCountFn)(
    const LaiueAudioOutputBackend *backend);
typedef uint32_t(LAIUE_MODULE_CALL *LaiueAudioOutputGetBufferFrameCountFn)(
    const LaiueAudioOutputBackend *backend);
typedef uint64_t(LAIUE_MODULE_CALL *LaiueAudioOutputGetUnderrunCountFn)(
    const LaiueAudioOutputBackend *backend);

typedef struct LaiueAudioOutputServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiueAudioOutputCreateFn create;
    LaiueAudioOutputDestroyFn destroy;
    LaiueAudioOutputGetSampleRateFn sampleRate;
    LaiueAudioOutputGetChannelCountFn channelCount;
    LaiueAudioOutputGetBufferFrameCountFn bufferFrameCount;
    LaiueAudioOutputGetUnderrunCountFn underrunCount;
    uintptr_t reserved[8];
    LaiueAudioOutputCreateWithContextFn createWithContext;
    void *context;
} LaiueAudioOutputServiceV1;

#define LAIUE_AUDIO_OUTPUT_SERVICE_V1_LEGACY_SIZE \
    ((uint32_t)offsetof(LaiueAudioOutputServiceV1, createWithContext))
#define LAIUE_AUDIO_OUTPUT_SERVICE_V1_CONTEXT_SIZE \
    ((uint32_t)(offsetof(LaiueAudioOutputServiceV1, context) + sizeof(void *)))

const LaiueModuleApiV1 *LaiueAudioOutputGetStaticModuleApiV1(void);
