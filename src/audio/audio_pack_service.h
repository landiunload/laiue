#pragma once

#include "audio/audio_pack.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_AUDIO_PACK_SERVICE_ABI_VERSION_1 1u
#define LAIUE_AUDIO_PACK_SERVICE_NAME "laiue.audio.pack"

typedef struct LaiueAudioPackServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t (*enumerate)(LaiueContentCatalog *catalog, AudioPackList *outList);
    uint32_t (*activate)(LaiueContentCatalog *catalog, const wchar_t *name);
    void (*releaseList)(AudioPackList *list);
    uint32_t (*enumerateSounds)(LaiueContentCatalog *catalog, AudioPackList *outList);
    AudioClip *(*loadFrom)(AudioDevice *device, LaiueContentCatalog *catalog,
                           const wchar_t *soundName, AudioPackLoadStatus *outStatus);
    AudioClip *(*loadFile)(AudioDevice *device, const wchar_t *path,
                           AudioPackLoadStatus *outStatus);
    AudioClip *(*loadMemory)(AudioDevice *device, const void *bytes, uint32_t sizeBytes,
                             AudioPackLoadStatus *outStatus);
    uintptr_t reserved[8];
} LaiueAudioPackServiceV1;

LAIUE_AUDIO_API const LaiueModuleApiV1 *LaiueAudioPackGetStaticModuleApiV1(void);
