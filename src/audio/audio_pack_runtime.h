#pragma once

#include "audio/audio_service.h"
#include "content/content_service.h"

/* Internal indirection used by the pack component.  The standalone module
 * receives these tables from the bootstrap, so the pack DLL has no import
 * dependency on either the mixer or the content DLL. */
typedef struct AudioPackRuntime
{
    const LaiueAudioServiceV1 *audio;
    const LaiueContentServiceV1 *content;
} AudioPackRuntime;

void AudioPackRuntimeSet(const AudioPackRuntime *runtime);
const AudioPackRuntime *AudioPackRuntimeGet(void);
