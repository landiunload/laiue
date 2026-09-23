#pragma once

#include "audio/audio_output_service.h"

/* Called only by the module lifecycle entrypoint.  Devices copy the table
 * pointer at creation and keep it until they are destroyed. */
void AudioMixerSetOutputService(const LaiueAudioOutputServiceV1 *service);

