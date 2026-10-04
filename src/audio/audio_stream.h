#pragma once

#include "audio/audio.h"
#include <stddef.h>

typedef struct AudioStream AudioStream;
typedef uint32_t (*AudioStreamReadAtFn)(void *context, uint64_t offset, void *bytes,
                                        uint32_t count);
typedef void (*AudioStreamCloseFn)(void *context);

/* Source callbacks run on a decoder worker or the API/offscreen caller.
 * They never run on system output. Ownership transfers only on successful
 * creation: close is called exactly once after all reads have stopped. */
typedef struct AudioStreamDescription
{
    uint32_t structSize;
    void *context;
    AudioStreamReadAtFn readAt;
    AudioStreamCloseFn close;
    uint64_t sizeBytes;
} AudioStreamDescription;

typedef struct AudioStreamStats
{
    uint32_t structSize;
    uint32_t frameCount;
    uint32_t sampleRate;
    uint32_t channelCount;
    /* Conservative upper bound (packet count * 1024), at most 8192. */
    uint32_t bufferedFrames;
    /* Owned descriptor/ring/decoder/file-handle heap; excludes device,
     * allocator overhead, caller context and OS worker stack. */
    uint64_t memoryBytes;
    uint64_t underruns;
    AudioResult result;
} AudioStreamStats;

LAIUE_AUDIO_API AudioResult AudioStreamCreate(AudioDevice *device,
                                              const AudioStreamDescription *description,
                                              AudioStream **outStream);
/* UTF-8 path; holds one open regular file until destroy/device teardown. */
LAIUE_AUDIO_API AudioResult AudioStreamOpenFile(AudioDevice *device, const char *path,
                                                AudioStream **outStream);
LAIUE_AUDIO_API void AudioStreamDestroy(AudioStream *stream);
LAIUE_AUDIO_API double AudioStreamDurationSeconds(const AudioStream *stream);
LAIUE_AUDIO_API bool AudioStreamGetStats(const AudioStream *stream, AudioStreamStats *stats);
/* A stream supports one active voice. Open another stream for an independent
 * simultaneous cursor. All streams are invalid after device destruction. */
LAIUE_AUDIO_API AudioVoice AudioVoicePlayStream(AudioDevice *device, AudioStream *stream,
                                                const AudioVoiceParameters *parameters);
LAIUE_AUDIO_API bool AudioVoicePause(AudioDevice *device, AudioVoice voice, bool paused);
/* Queue an exact source-time seek; accepts duration (EOF), rejects nonfinite
 * and out-of-range times. Position changes when output applies the command.
 * MP3 decoder history is restored synchronously on the caller thread. */
LAIUE_AUDIO_API bool AudioVoiceSeek(AudioDevice *device, AudioVoice voice, double seconds);
LAIUE_AUDIO_API bool AudioVoiceGetPosition(const AudioDevice *device, AudioVoice voice,
                                           double *outSeconds);
