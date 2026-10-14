#include "media/sound.h"
#include "platform/system.h"
#include "test_runtime.h"
#include "mp3_fixtures.h"
#include <string.h>

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("sound stream: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

typedef struct MemorySource
{
    const uint8_t *bytes;
    uint32_t size;
    uint32_t largestRead;
    bool fail;
} MemorySource;

static bool ReadAt(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    MemorySource *source = (MemorySource *)context;
    if (count > source->largestRead)
        source->largestRead = count;
    if (source->fail || offset > source->size || count > source->size - offset)
        return false;
    memcpy(bytes, source->bytes + (size_t)offset, count);
    return true;
}

static void CheckExact(const uint8_t *file, uint32_t size)
{
    MemorySource source = {file, size, 0u, false};
    SoundReader reader = {&source, ReadAt, size};
    SoundInfo full = {0}, incremental = {0};
    Expect(SoundInspect(file, size, &full) == SOUND_OK, "full inspect");
    Expect(SoundStreamInspect(&reader, &incremental) == SOUND_OK, "stream inspect");
    Expect(full.frameCount == incremental.frameCount &&
               full.channelCount == incremental.channelCount &&
               full.sampleRate == incremental.sampleRate,
           "identical metadata");
    int16_t *expected = (int16_t *)PlatformAllocate(full.sampleCount * sizeof(int16_t), false);
    void *fullScratch = PlatformAllocate(full.scratchBytes == 0u ? 1u : full.scratchBytes, true);
    void *scratch = PlatformAllocate(incremental.scratchBytes + 16u, true);
    Expect(expected != NULL && fullScratch != NULL && scratch != NULL, "allocations");
    memset((uint8_t *)scratch + incremental.scratchBytes, 0xA5, 16u);
    Expect(SoundDecodeSamples(file, size, &full, expected, full.sampleCount, fullScratch,
                              full.scratchBytes) == SOUND_OK,
           "full decode");
    SoundStream *stream = NULL;
    Expect(SoundStreamInitialize(&reader, scratch, incremental.scratchBytes - 1u, &stream) ==
                   SOUND_BUFFER_TOO_SMALL &&
               stream == NULL,
           "workspace bound");
    Expect(SoundStreamInitialize(&reader, scratch, incremental.scratchBytes, &stream) == SOUND_OK,
           "stream initialize");
    int16_t block[257u * 2u];
    uint32_t position = 0u;
    while (position < full.frameCount)
    {
        uint32_t capacity = position % 257u + 1u;
        uint32_t read = 0u;
        Expect(SoundStreamRead(stream, block, capacity, &read) == SOUND_OK && read != 0u,
               "incremental decode");
        for (uint32_t index = 0u; index < read * full.channelCount; ++index)
            Expect(block[index] == expected[position * full.channelCount + index],
                   "exact full PCM including reservoir and trim");
        position += read;
    }
    uint32_t read = UINT32_MAX;
    Expect(SoundStreamRead(stream, block, 1u, &read) == SOUND_OK && read == 0u, "EOF");
    uint32_t seeks[] = {full.frameCount / 2u, 0u, full.frameCount - 1u, 17u, full.frameCount};
    for (uint32_t seek = 0u; seek < sizeof(seeks) / sizeof(seeks[0]); ++seek)
    {
        position = seeks[seek];
        Expect(SoundStreamSeek(stream, position) == SOUND_OK, "exact seek");
        Expect(SoundStreamRead(stream, block, 257u, &read) == SOUND_OK, "read after seek");
        for (uint32_t index = 0u; index < read * full.channelCount; ++index)
            Expect(block[index] == expected[position * full.channelCount + index],
                   "seek reproduces full reference PCM");
    }
    Expect(SoundStreamSeek(stream, full.frameCount + 1u) == SOUND_INVALID_ARGUMENT,
           "out-of-range seek");
    Expect(SoundStreamSeek(stream, 0u) == SOUND_OK, "rewind");
    source.fail = true;
    Expect(SoundStreamRead(stream, block, 257u, &read) == SOUND_TRUNCATED, "failed source read");
    for (uint32_t index = 0u; index < 16u; ++index)
        Expect(((uint8_t *)scratch)[incremental.scratchBytes + index] == 0xA5, "workspace guard");
    Expect(source.largestRead <= 16384u, "bounded read request");
    PlatformFree(scratch);
    PlatformFree(fullScratch);
    PlatformFree(expected);
}

static void Put32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (8u * index));
}

static void CheckWave(uint32_t bits, uint32_t channels, bool floating)
{
    uint32_t frames = 1237u;
    uint32_t dataBytes = frames * channels * (bits / 8u);
    uint32_t size = 44u + dataBytes;
    uint8_t *file = (uint8_t *)PlatformAllocate(size, true);
    Expect(file != NULL, "wave allocation");
    memcpy(file, "RIFF", 4u);
    Put32(file + 4u, size - 8u);
    memcpy(file + 8u, "WAVEfmt ", 8u);
    Put32(file + 16u, 16u);
    file[20] = floating ? 3u : 1u;
    file[22] = (uint8_t)channels;
    Put32(file + 24u, 44100u);
    file[34] = (uint8_t)bits;
    memcpy(file + 36u, "data", 4u);
    Put32(file + 40u, dataBytes);
    for (uint32_t index = 0u; index < dataBytes; ++index)
        file[44u + index] = (uint8_t)(index * 47u + index / 17u);
    /* Stable finite IEEE values exercise both conversion paths. */
    if (floating)
        for (uint32_t index = 0u; index < frames * channels; ++index)
        {
            if (bits == 32u)
                Put32(file + 44u + index * 4u, index & 1u ? 0xBF000000u : 0x3E800000u);
            else
            {
                Put32(file + 44u + index * 8u, 0u);
                Put32(file + 48u + index * 8u, index & 1u ? 0xBFE00000u : 0x3FD00000u);
            }
        }
    CheckExact(file, size);
    PlatformFree(file);
}

LAIUE_TEST_ENTRY(AudioStreamDecoderTestEntryPoint)
{
    for (uint32_t channels = 1u; channels <= 2u; ++channels)
    {
        CheckWave(8u, channels, false);
        CheckWave(16u, channels, false);
        CheckWave(24u, channels, false);
        CheckWave(32u, channels, false);
        CheckWave(32u, channels, true);
        CheckWave(64u, channels, true);
    }
    CheckExact(MP3_SOUND_FILE, sizeof(MP3_SOUND_FILE));
    LaiueTestRuntimeWrite("sound stream decode and seek passed\n");
    LAIUE_TEST_SUCCESS();
}
