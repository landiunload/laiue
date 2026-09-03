// Бэкенд без вывода. Собственного потока у него нет: кадры готовит тот,
// кто вызывает AudioDeviceRenderFrames. Это делает микшер проверяемым на
// машине без звуковой подсистемы и в контейнере CI.

#include "audio/audio_backend.h"
#include "platform/system.h"

#include <string.h>

#define AUDIO_OFFSCREEN_DEFAULT_SAMPLE_RATE 48000u
#define AUDIO_OFFSCREEN_DEFAULT_FRAMES 480u

typedef struct AudioOffscreenBackend
{
    AudioBackend base;
} AudioOffscreenBackend;

static void OffscreenDestroy(AudioBackend *backend)
{
    PlatformFree(backend);
}

static uint64_t OffscreenUnderrunCount(const AudioBackend *backend)
{
    // Опоздать некуда: кадры готовятся по запросу, а не к сроку.
    (void)backend;
    return 0u;
}

static const AudioBackendVtable OFFSCREEN_VTABLE = {
    .destroy = OffscreenDestroy,
    .underrunCount = OffscreenUnderrunCount,
};

bool AudioOffscreenBackendCreate(const AudioBackendDescription *description,
                                 AudioBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL) return false;
    *outBackend = NULL;

    AudioOffscreenBackend *backend = PlatformAllocate(sizeof(*backend), true);
    if (backend == NULL) return false;

    backend->base.vtable = &OFFSCREEN_VTABLE;
    backend->base.sampleRate = description->sampleRate != 0u
                                   ? description->sampleRate
                                   : AUDIO_OFFSCREEN_DEFAULT_SAMPLE_RATE;
    backend->base.channelCount = 2u;
    backend->base.bufferFrameCount = description->frameCountHint != 0u
                                         ? description->frameCountHint
                                         : AUDIO_OFFSCREEN_DEFAULT_FRAMES;

    *outBackend = &backend->base;
    return true;
}
