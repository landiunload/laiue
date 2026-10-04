#include "audio/audio_stream.h"
#include "audio/audio_service.h"
#include "audio/audio_output_service.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "mp3_fixtures.h"
#include <string.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("audio stream: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void WriteNumber(uint64_t value)
{
    char digits[32];
    uint32_t used = 0u;
    do
    {
        digits[used++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    char text[32];
    for (uint32_t index = 0u; index < used; ++index)
        text[index] = digits[used - index - 1u];
    text[used] = 0;
    LaiueTestRuntimeWrite(text);
}

typedef struct Source
{
    uint8_t header[44];
    uint64_t size;
    uint32_t frames;
    uint32_t channels;
    uint32_t largestRead;
    uint64_t totalRead;
    volatile uint32_t closes;
    bool mp3;
    bool fail;
} Source;

static void Put32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (8u * index));
}

static int16_t Sample(uint32_t frame, uint32_t channel)
{
    return (int16_t)(1000u + ((frame % 20000u) * 13u + channel * 1000u) % 20000u);
}

static void Wave(Source *source, uint32_t frames, uint32_t channels)
{
    memset(source, 0, sizeof(*source));
    source->frames = frames;
    source->channels = channels;
    source->size = 44u + (uint64_t)frames * channels * 2u;
    memcpy(source->header, "RIFF", 4u);
    Put32(source->header + 4u, (uint32_t)source->size - 8u);
    memcpy(source->header + 8u, "WAVEfmt ", 8u);
    Put32(source->header + 16u, 16u);
    source->header[20] = 1u;
    source->header[22] = (uint8_t)channels;
    Put32(source->header + 24u, 48000u);
    source->header[34] = 16u;
    memcpy(source->header + 36u, "data", 4u);
    Put32(source->header + 40u, frames * channels * 2u);
}

static uint32_t ReadAt(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    Source *source = (Source *)context;
    Expect(PlatformAtomicLoadU32Acquire(&source->closes) == 0u, "read after close");
    if (source->fail || offset > source->size || count > source->size - offset)
        return 0u;
    if (count > source->largestRead)
        source->largestRead = count;
    source->totalRead += count;
    uint8_t *target = (uint8_t *)bytes;
    for (uint32_t index = 0u; index < count; ++index)
    {
        uint64_t position = offset + index;
        if (source->mp3)
        {
            static const uint8_t header[4] = {0xFFu, 0xFBu, 0x90u, 0u};
            uint32_t within = (uint32_t)(position % 417u);
            target[index] = within < 4u ? header[within] : 0u;
        }
        else if (position < 44u)
            target[index] = source->header[position];
        else
        {
            uint32_t sample = (uint32_t)((position - 44u) / 2u);
            uint16_t value = (uint16_t)Sample(sample / source->channels, sample % source->channels);
            target[index] = (uint8_t)(value >> (((position - 44u) & 1u) * 8u));
        }
    }
    return 1u;
}

static void Close(void *context)
{
    Source *source = (Source *)context;
    Expect(PlatformAtomicIncrementU32(&source->closes) == 1u, "exactly one source close");
}

static AudioDevice *Device(uint32_t rate)
{
    AudioDeviceConfiguration configuration = {AUDIO_BACKEND_OFFSCREEN, rate, 256u, 1.0f};
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK, "offscreen create");
    return device;
}

static AudioStream *Stream(AudioDevice *device, Source *source)
{
    AudioStreamDescription description = {sizeof(description), source, ReadAt, Close, source->size};
    AudioStream *stream = NULL;
    Expect(AudioStreamCreate(device, &description, &stream) == AUDIO_RESULT_OK, "stream create");
    return stream;
}

static void Render(AudioDevice *device, float *frames, uint32_t count)
{
    Expect(AudioDeviceRenderFrames(device, frames, count), "render");
}

static double Position(AudioDevice *device, AudioVoice voice)
{
    double seconds = -1.0;
    Expect(AudioVoiceGetPosition(device, voice, &seconds), "position query");
    return seconds;
}

static void CheckWavePlayback(void)
{
    AudioDevice *device = Device(48000u), *other = Device(48000u);
    Source source;
    Wave(&source, 17003u, 2u);
    AudioStream *stream = Stream(device, &source);
    AudioVoiceParameters parameters = {1.0f, -1.0f, 1.0f, false};
    Expect(AudioVoicePlayStream(other, stream, &parameters) == AUDIO_VOICE_NONE,
           "device ownership");
    AudioVoice voice = AudioVoicePlayStream(device, stream, &parameters);
    Expect(voice != AUDIO_VOICE_NONE, "play stream");
    Expect(AudioVoicePlayStream(device, stream, NULL) == AUDIO_VOICE_NONE, "one cursor per stream");
    float *frames = (float *)PlatformAllocate(4099u * 2u * sizeof(float), false);
    Expect(frames != NULL, "output allocation");
    Render(device, frames, 4099u);
    for (uint32_t index = 0u; index < 4099u; ++index)
    {
        if (frames[index * 2u] != Sample(index, 0u) * (1.0f / 32768.0f))
        {
            LaiueTestRuntimeWrite("frame ");
            WriteNumber(index);
            LaiueTestRuntimeWrite(" actual sample ");
            WriteNumber((uint64_t)(frames[index * 2u] * 32768.0f));
            LaiueTestRuntimeWrite(" expected ");
            WriteNumber((uint16_t)Sample(index, 0u));
            LaiueTestRuntimeWrite("\n");
        }
        Expect(frames[index * 2u] == Sample(index, 0u) * (1.0f / 32768.0f) &&
                   frames[index * 2u + 1u] == 0.0f,
               "exact streaming packet boundaries");
    }
    Expect(Position(device, voice) == 4099.0 / 48000.0, "source-time position");
    Expect(AudioVoicePause(device, voice, true), "pause");
    Render(device, frames, 32u);
    for (uint32_t index = 0u; index < 64u; ++index)
        Expect(frames[index] == 0.0f, "paused silence");
    Expect(Position(device, voice) == 4099.0 / 48000.0 && AudioVoiceIsActive(device, voice),
           "pause retains position and voice");
    Expect(AudioVoiceSeek(device, voice, 9001.0 / 48000.0), "seek paused, full ring");
    Expect(AudioVoiceSeek(device, voice, 12003.0 / 48000.0), "seek twice before ACK");
    Render(device, frames, 1u);
    Expect(frames[0] == 0.0f && Position(device, voice) == 12003.0 / 48000.0,
           "paused seek applies");
    Expect(AudioVoicePause(device, voice, false), "resume");
    Render(device, frames, 1u);
    Expect(frames[0] == Sample(12003u, 0u) * (1.0f / 32768.0f),
           "first render after seek is ready exact PCM");
    Expect(AudioVoiceSeek(device, voice, 41.0 / 48000.0), "backwards seek with full ring");
    Render(device, frames, 1u);
    Expect(frames[0] == Sample(41u, 0u) * (1.0f / 32768.0f), "first backwards seek sample");
    parameters.speed = 0.5f;
    parameters.pan = 1.0f;
    parameters.volume = 0.5f;
    Expect(AudioVoiceSetParameters(device, voice, &parameters), "volume pan speed");
    Expect(AudioVoiceSeek(device, voice, 1023.0 / 48000.0), "seek packet edge");
    Render(device, frames, 4u);
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        uint32_t a = 1023u + index / 2u;
        float sample = Sample(a, 1u) * (1.0f / 32768.0f);
        if ((index & 1u) != 0u)
            sample += (Sample(a + 1u, 1u) - Sample(a, 1u)) * (1.0f / 65536.0f);
        Expect(frames[index * 2u] == 0.0f && frames[index * 2u + 1u] == sample * 0.5f,
               "fractional speed interpolation and gains");
    }
    Expect(!AudioVoiceSeek(device, voice, -1.0) &&
               !AudioVoiceSeek(device, voice, AudioStreamDurationSeconds(stream) + 1.0),
           "seek bounds");
    Expect(AudioVoiceSeek(device, voice, AudioStreamDurationSeconds(stream)), "seek EOF");
    Render(device, frames, 1u);
    Expect(frames[0] == 0.0f && !AudioVoiceIsActive(device, voice), "seek EOF finishes");
    AudioVoice replay = AudioVoicePlayStream(device, stream, NULL);
    Expect(replay != AUDIO_VOICE_NONE && replay != voice, "replay from start with fresh handle");
    Expect(!AudioVoicePause(device, voice, true) && !AudioVoiceSeek(device, voice, 0.0),
           "stale controls rejected");
    AudioStreamDestroy(stream);
    Expect(source.closes == 1u, "source closes without waiting for output ACK");
    Render(device, frames, 8u);
    Expect(!AudioVoiceIsActive(device, replay), "destroy cancels pending play");
    PlatformFree(frames);
    AudioDeviceDestroy(other);
    AudioDeviceDestroy(device);
}

static void CheckLoopAndLifecycle(void)
{
    AudioDevice *device = Device(48000u);
    Source source;
    Wave(&source, 7u, 1u);
    AudioStream *stream = Stream(device, &source);
    AudioVoiceParameters parameters = {1.0f, -1.0f, 16.0f, true};
    AudioVoice voice = AudioVoicePlayStream(device, stream, &parameters);
    float frames[64];
    Render(device, frames, 32u);
    for (uint32_t index = 0u; index < 32u; ++index)
        Expect(frames[index * 2u] == Sample((index * 16u) % 7u, 0u) * (1.0f / 32768.0f),
               "loop short source with step longer than track");
    Expect(AudioVoiceSeek(device, voice, AudioStreamDurationSeconds(stream)), "loop seek end");
    Render(device, frames, 1u);
    Expect(frames[0] == Sample(0u, 0u) * (1.0f / 32768.0f), "loop EOF seek wraps ready");
    parameters.looping = false;
    parameters.speed = 1.0f;
    Expect(AudioVoiceSetParameters(device, voice, &parameters), "disable loop");
    Expect(AudioVoiceSeek(device, voice, 0.0), "seek nonloop");
    Render(device, frames, 32u);
    Expect(!AudioVoiceIsActive(device, voice), "natural EOF");
    AudioStreamDestroy(stream);
    /* No output call after destroy: device must release deferred ring. */
    AudioDeviceDestroy(device);
    Expect(source.closes == 1u, "stopped destroy without rendering");
    device = Device(48000u);
    Source pending;
    Wave(&pending, 20000u, 1u);
    stream = Stream(device, &pending);
    Expect(AudioVoicePlayStream(device, stream, NULL) != AUDIO_VOICE_NONE, "pending lifecycle");
    AudioDeviceDestroy(device);
    Expect(pending.closes == 1u, "device owns still-live streams and pending voices");
}

static uint64_t MemoryFor(Source *source)
{
    AudioDevice *device = Device(48000u);
    AudioStream *stream = Stream(device, source);
    AudioStreamStats stats = {.structSize = sizeof(stats)};
    Expect(AudioStreamGetStats(stream, &stats), "stream stats");
    Expect(stats.memoryBytes < 200000u && stats.bufferedFrames <= 8192u,
           "fixed memory upper bound");
    Expect(AudioVoicePlayStream(device, stream, NULL) != AUDIO_VOICE_NONE, "bounded play");
    float frames[32];
    Render(device, frames, 16u);
    Expect(source->largestRead <= 16384u, "bounded source read");
    LaiueTestRuntimeWrite(source->mp3 ? "MP3 stream heap bytes: " : "WAV stream heap bytes: ");
    WriteNumber(stats.memoryBytes);
    LaiueTestRuntimeWrite("\n");
    AudioDeviceDestroy(device);
    Expect(source->closes == 1u, "bounded source closed");
    return stats.memoryBytes;
}

static void CheckBoundedMemory(void)
{
    Source shortWave, longWave;
    Wave(&shortWave, 48000u, 2u);
    Wave(&longWave, 48000u * 300u, 2u);
    Expect(MemoryFor(&shortWave) == MemoryFor(&longWave),
           "five-minute WAV uses identical workspace");
    Expect(longWave.totalRead < 200000u, "WAV open/play does not read full track");
    Source shortMp3 = {0}, longMp3 = {0};
    shortMp3.mp3 = longMp3.mp3 = true;
    shortMp3.size = 417u * 5u;
    longMp3.size = 417u * 11485u; /* five minutes at 44.1 kHz */
    Expect(MemoryFor(&shortMp3) == MemoryFor(&longMp3), "five-minute MP3 uses identical workspace");
    Expect(longMp3.totalRead < longMp3.size / 4u, "MP3 inspect scans headers rather than payload");
    AudioDevice *device = Device(48000u);
    Source failed;
    Wave(&failed, 100u, 1u);
    failed.fail = true;
    AudioStreamDescription description = {sizeof(description), &failed, ReadAt, Close, failed.size};
    AudioStream *stream = (AudioStream *)(uintptr_t)1u;
    Expect(AudioStreamCreate(device, &description, &stream) == AUDIO_RESULT_SOURCE_REJECTED &&
               stream == NULL && failed.closes == 0u,
           "failed create retains caller ownership");
    AudioDeviceDestroy(device);
}

/* System output substitute pulls the same realtime callback without a sound
 * device. Decoder worker is real; source ownership is checked after join. */
struct LaiueAudioOutputBackend
{
    LaiueAudioOutputDescription description;
};
static LaiueAudioOutputBackend mockOutput;
static const LaiueAudioServiceV1 *publishedAudio;

static uint32_t OutputCreate(const LaiueAudioOutputDescription *description,
                             LaiueAudioOutputBackend **output)
{
    mockOutput.description = *description;
    *output = &mockOutput;
    return 1u;
}
static void OutputDestroy(LaiueAudioOutputBackend *output)
{
    (void)output;
}
static uint32_t OutputRate(const LaiueAudioOutputBackend *output)
{
    (void)output;
    return 48000u;
}
static uint32_t OutputChannels(const LaiueAudioOutputBackend *output)
{
    (void)output;
    return 2u;
}
static uint32_t OutputFrames(const LaiueAudioOutputBackend *output)
{
    (void)output;
    return 256u;
}
static uint64_t OutputUnderruns(const LaiueAudioOutputBackend *output)
{
    (void)output;
    return 0u;
}
static void *HostAllocate(void *context, uint64_t size)
{
    (void)context;
    return PlatformAllocate((size_t)size, true);
}
static void HostFree(void *context, void *memory)
{
    (void)context;
    PlatformFree(memory);
}
static LaiueModuleStatus Publish(void *context, const LaiueModuleServiceV1 *service)
{
    (void)context;
    publishedAudio = (const LaiueAudioServiceV1 *)service->table;
    return LAIUE_MODULE_OK;
}
static LaiueModuleStatus Unpublish(void *context, const char *name)
{
    (void)context;
    (void)name;
    publishedAudio = NULL;
    return LAIUE_MODULE_OK;
}
static const void *QueryOutput(void *context, const char *name, uint32_t version, uint32_t size,
                               uint32_t *outVersion, uint32_t *outSize)
{
    (void)name;
    (void)version;
    (void)size;
    *outVersion = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1;
    *outSize = sizeof(LaiueAudioOutputServiceV1);
    return context;
}

static void CheckWorker(void)
{
    LaiueAudioOutputServiceV1 output = {
        .structSize = sizeof(output),
        .abiVersion = LAIUE_AUDIO_OUTPUT_SERVICE_ABI_VERSION_1,
        .create = OutputCreate,
        .destroy = OutputDestroy,
        .sampleRate = OutputRate,
        .channelCount = OutputChannels,
        .bufferFrameCount = OutputFrames,
        .underrunCount = OutputUnderruns,
    };
    LaiueModuleHostV1 host = {
        .structSize = sizeof(host),
        .abiVersion = LAIUE_MODULE_ABI_VERSION_1,
        .context = &output,
        .allocate = HostAllocate,
        .free = HostFree,
        .queryService = QueryOutput,
        .publishService = Publish,
        .unpublishService = Unpublish,
    };
    const LaiueModuleApiV1 *module = LaiueAudioGetStaticModuleApiV1();
    void *context = NULL;
    Expect(module->create(&host, &context) != 0u && module->start(context) != 0u,
           "instance module start");
    Expect(LaiueAudioServiceV1HasStreaming(publishedAudio, publishedAudio->structSize),
           "service publishes complete optional tail");
    AudioDeviceConfiguration configuration = {AUDIO_BACKEND_SYSTEM, 48000u, 256u, 1.0f};
    AudioDevice *device = NULL;
    Expect(publishedAudio->deviceCreateWithContext(publishedAudio->context, &configuration,
                                                   &device) == AUDIO_RESULT_OK,
           "fake system output device");
    Source source;
    Wave(&source, 50000u, 2u);
    AudioStream *stream = Stream(device, &source);
    AudioVoiceParameters parameters = {1.0f, -1.0f, 1.0f, true};
    AudioVoice voice = publishedAudio->voicePlayStream(device, stream, &parameters);
    Expect(voice != AUDIO_VOICE_NONE, "worker voice");
    float frames[256];
    for (uint32_t pass = 0u; pass < 180u; ++pass)
    {
        mockOutput.description.render(mockOutput.description.context, frames, 128u);
        PlatformSleepMilliseconds(3u);
    }
    Expect(Position(device, voice) > 8192.0 / 48000.0,
           "worker refills beyond initial ring without offscreen pump");
    Expect(publishedAudio->voicePause(device, voice, 1u) != 0u, "worker pause");
    mockOutput.description.render(mockOutput.description.context, frames, 1u);
    AudioStreamDestroy(stream);
    Expect(source.closes == 1u, "worker joins and closes without output callback");
    mockOutput.description.render(mockOutput.description.context, frames, 1u);
    Expect(!AudioVoiceIsActive(device, voice), "paused worker stream retired");
    AudioDeviceDestroy(device);
    Expect(source.closes == 1u, "worker device closes once");
    /* A second worker survives until device teardown, with a pending START. */
    Expect(publishedAudio->deviceCreateWithContext(publishedAudio->context, &configuration,
                                                   &device) == AUDIO_RESULT_OK,
           "second worker device");
    Source pending;
    Wave(&pending, 50000u, 1u);
    stream = Stream(device, &pending);
    Expect(AudioVoicePlayStream(device, stream, NULL) != AUDIO_VOICE_NONE, "pending worker play");
    AudioDeviceDestroy(device);
    Expect(pending.closes == 1u, "pending worker join at device destruction");
    module->stop(context);
    module->destroy(context);
}

static void CheckQueuePressure(void)
{
    AudioDevice *device = Device(48000u);
    Source source;
    Wave(&source, 20000u, 1u);
    AudioStream *stream = Stream(device, &source);
    AudioVoice voice = AudioVoicePlayStream(device, stream, NULL);
    float frames[2];
    Render(device, frames, 1u);
    Expect(AudioVoicePause(device, voice, true), "pause before queue pressure");
    Render(device, frames, 1u);
    AudioVoiceParameters parameters = {1.0f, -1.0f, 1.0f, false};
    uint32_t accepted = 0u;
    while (AudioVoiceSetParameters(device, voice, &parameters))
        ++accepted;
    Expect(accepted == 255u, "command ring full");
    double position = Position(device, voice);
    Expect(!AudioVoiceSeek(device, voice, 123.0 / 48000.0),
           "seek rejected before decoder mutation");
    Expect(Position(device, voice) == position, "rejected seek retains position");
    AudioStreamDestroy(stream);
    Expect(source.closes == 1u, "close despite full command ring");
    Render(device, frames, 1u);
    Expect(!AudioVoiceIsActive(device, voice) && frames[0] == 0.0f,
           "destroy cancels paused stream even when STOP is dropped");
    AudioDeviceDestroy(device);
}

static void CheckExtremeRate(void)
{
    AudioDevice *device = Device(UINT32_MAX);
    Source source;
    Wave(&source, 1000u, 1u);
    Put32(source.header + 24u, 1000u);
    AudioStream *stream = Stream(device, &source);
    union
    {
        uint32_t bits;
        float value;
    } nanSpeed;
    nanSpeed.bits = 0x7FC00000u;
    AudioVoiceParameters parameters = {1.0f, -1.0f, nanSpeed.value, true};
    AudioVoice voice = AudioVoicePlayStream(device, stream, &parameters);
    Expect(voice != AUDIO_VOICE_NONE, "extreme device rate and NaN speed sanitized");
    float frames[512];
    Render(device, frames, 256u);
    for (uint32_t index = 0u; index < 256u; ++index)
        Expect(frames[index * 2u] == frames[index * 2u] && frames[index * 2u] > 0.0f,
               "small step stays finite without overflowing integer prefetch count");
    AudioDeviceDestroy(device);
}

static void CheckFileAndAbi(void)
{
    LaiueAudioServiceV1 service = {0};
    service.structSize = LAIUE_AUDIO_SERVICE_V1_CONTEXT_SIZE;
    service.abiVersion = LAIUE_AUDIO_SERVICE_ABI_VERSION_1;
    Expect(!LaiueAudioServiceV1HasStreaming(&service, sizeof(service)), "old table has no tail");
    service.structSize = sizeof(service);
    Expect(!LaiueAudioServiceV1HasStreaming(&service, LAIUE_AUDIO_SERVICE_V1_CONTEXT_SIZE),
           "host size prevents out-of-bounds tail access");
    const char *path = "audio-stream-test.mp3";
    const wchar_t *widePath = L"audio-stream-test.mp3";
    Expect(PlatformWriteEntireFile(widePath, MP3_SOUND_FILE, sizeof(MP3_SOUND_FILE)),
           "file fixture");
    AudioDevice *device = Device(MP3_SOUND_RATE);
    AudioStream *stream = NULL;
    Expect(AudioStreamOpenFile(device, path, &stream) == AUDIO_RESULT_OK, "UTF-8 file stream open");
    AudioVoiceParameters parameters = {1.0f, -1.0f, 1.0f, false};
    AudioVoice voice = AudioVoicePlayStream(device, stream, &parameters);
    Expect(voice != AUDIO_VOICE_NONE, "file stream play");
    float frames[128];
    Render(device, frames, 64u);
    bool nonzero = false;
    for (uint32_t index = 0u; index < 64u; ++index)
        nonzero |= frames[index * 2u] != 0.0f;
    Expect(nonzero, "real MP3 produces audio");
    Expect(AudioVoiceSeek(device, voice, 17.0 / MP3_SOUND_RATE), "file seek");
    Render(device, frames, 32u);
    AudioStreamDestroy(stream);
    AudioDeviceDestroy(device);
    Expect(PlatformDeleteFile(widePath), "stream releases file handle");
}

LAIUE_TEST_ENTRY(AudioStreamTestEntryPoint)
{
    CheckWavePlayback();
    CheckLoopAndLifecycle();
    CheckBoundedMemory();
    CheckWorker();
    CheckQueuePressure();
    CheckExtremeRate();
    CheckFileAndAbi();
    LaiueTestRuntimeWrite("audio streaming passed\n");
    LAIUE_TEST_SUCCESS();
}
