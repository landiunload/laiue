#pragma once
#include "audio/audio_stream.h"
#include "media/sound.h"
#include "platform/system.h"

#define AUDIO_STREAM_PACKET_FRAMES 1024u
#define AUDIO_STREAM_PACKET_COUNT 8u

typedef struct AudioStreamPacket
{
    uint32_t epoch;
    uint32_t frameCount;
    int16_t samples[(AUDIO_STREAM_PACKET_FRAMES + 1u) * 2u];
} AudioStreamPacket;

struct AudioStream
{
    AudioDevice *device;
    AudioStream *next;
    AudioStreamDescription source;
    SoundInfo info;
    SoundStream *decoder;
    void *scratch;
    PlatformMutex decoderLock;
    PlatformThread worker;
    bool workerReady;
    bool mutexReady;
    bool sourceOwned;
    bool retired;
    AudioVoice voice;
    uint64_t retireFrame;
    uint64_t retireCommand;
    volatile uint32_t terminate;
    volatile uint32_t read;
    volatile uint32_t write;
    volatile uint32_t result;
    volatile int64_t underruns;
    uint32_t epoch;
    uint32_t producerPosition;
    bool looping;
    bool eof;
    bool carryReady;
    int16_t carry[2];
    AudioStreamPacket packets[AUDIO_STREAM_PACKET_COUNT];
};

AudioResult AudioStreamAllocate(AudioDevice *device, const AudioStreamDescription *description,
                                bool worker, AudioStream **outStream);
AudioResult AudioStreamAllocateFile(AudioDevice *device, const char *path, bool worker,
                                    AudioStream **outStream);
void AudioStreamShutdown(AudioStream *stream);
void AudioStreamFree(AudioStream *stream);
bool AudioStreamPrepare(AudioStream *stream, uint32_t frame, bool looping, uint32_t *outEpoch);
void AudioStreamSetLooping(AudioStream *stream, bool looping);
void AudioStreamPump(AudioStream *stream);
/* Only the output consumer advances read/offset; no locks or callbacks. */
bool AudioStreamSample(AudioStream *stream, uint32_t epoch, double *offset, double step,
                       float *left, float *right);
