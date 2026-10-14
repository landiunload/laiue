#pragma once

#include "audio/audio_output_service.h"

#include "audio/audio.h"

/* Called only by the module lifecycle entrypoint.  Devices copy the table
 * pointer at creation and keep it until they are destroyed. */
void AudioMixerSetOutputService(const LaiueAudioOutputServiceV1 *service);
bool AudioMixerTryAcquireOutputService(const void *owner,
                                       const LaiueAudioOutputServiceV1 *service);
void AudioMixerReleaseOutputService(const void *owner);

/* Instance-bound creation used by the dynamically loaded audio module.  The
 * legacy AudioDeviceCreate entry point remains available for source and ABI
 * compatibility and uses the process default configured by
 * AudioMixerSetOutputService. */
AudioResult AudioDeviceCreateWithOutputService(
    const AudioDeviceConfiguration *configuration,
    const LaiueAudioOutputServiceV1 *outputService,
    AudioDevice **outDevice);

AudioResult AudioDeviceCreateWithOutputServiceEx(
    const AudioDeviceConfiguration *configuration,
    const LaiueAudioOutputServiceV1 *outputService,
    uint32_t outputServiceSize,
    AudioDevice **outDevice);
