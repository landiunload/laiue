#include "audio/audio_pack_runtime.h"

#include <string.h>

static AudioPackRuntime runtime;

void AudioPackRuntimeSet(const AudioPackRuntime *value)
{
    if (value == NULL)
        memset(&runtime, 0, sizeof(runtime));
    else
        runtime = *value;
}

const AudioPackRuntime *AudioPackRuntimeGet(void)
{
    return &runtime;
}
