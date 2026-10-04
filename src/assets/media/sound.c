#include "media/sound.h"

#include "media/mp3_decode.h"
#include "media/wave_decode.h"

#include <stddef.h>

static SoundStatus SoundFromWave(WaveStatus status)
{
    switch (status)
    {
    case WAVE_OK: return SOUND_OK;
    case WAVE_NOT_RIFF: return SOUND_NOT_RECOGNISED;
    case WAVE_TRUNCATED: return SOUND_TRUNCATED;
    case WAVE_MISSING_FORMAT:
    case WAVE_MISSING_DATA: return SOUND_CORRUPT;
    case WAVE_UNSUPPORTED_FORMAT:
    case WAVE_UNSUPPORTED_DEPTH:
    case WAVE_UNSUPPORTED_CHANNELS:
    case WAVE_UNSUPPORTED_RATE: return SOUND_UNSUPPORTED_FEATURE;
    case WAVE_TOO_LARGE: return SOUND_TOO_LARGE;
    case WAVE_INVALID_ARGUMENT: return SOUND_INVALID_ARGUMENT;
    }
    return SOUND_CORRUPT;
}

static SoundStatus SoundFromMp3(Mp3Status status)
{
    switch (status)
    {
    case MP3_OK: return SOUND_OK;
    case MP3_INVALID_ARGUMENT: return SOUND_INVALID_ARGUMENT;
    case MP3_NOT_RECOGNISED: return SOUND_NOT_RECOGNISED;
    case MP3_TRUNCATED: return SOUND_TRUNCATED;
    case MP3_CORRUPT: return SOUND_CORRUPT;
    case MP3_UNSUPPORTED_FEATURE: return SOUND_UNSUPPORTED_FEATURE;
    case MP3_TOO_LARGE: return SOUND_TOO_LARGE;
    case MP3_BUFFER_TOO_SMALL: return SOUND_BUFFER_TOO_SMALL;
    }
    return SOUND_CORRUPT;
}

SoundFormat SoundProbe(const void *bytes, uint32_t sizeBytes)
{
    if (bytes == NULL) return SOUND_FORMAT_UNKNOWN;
    const uint8_t *file = (const uint8_t *)bytes;
    if (sizeBytes >= 12u && file[0] == 'R' && file[1] == 'I' && file[2] == 'F' && file[3] == 'F' &&
        file[8] == 'W' && file[9] == 'A' && file[10] == 'V' && file[11] == 'E')
    {
        return SOUND_FORMAT_WAVE;
    }
    // MP3 проверяется последним: у него нет сигнатуры, только тег ID3
    // или синхрослово кадра, и оба встречаются в чужих данных чаще,
    // чем полноценная сигнатура.
    if (Mp3Matches(bytes, sizeBytes)) return SOUND_FORMAT_MP3;
    return SOUND_FORMAT_UNKNOWN;
}

SoundStatus SoundInspect(const void *bytes, uint32_t sizeBytes, SoundInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return SOUND_INVALID_ARGUMENT;

    switch (SoundProbe(bytes, sizeBytes))
    {
    case SOUND_FORMAT_WAVE:
    {
        WaveInfo wave;
        SoundStatus status = SoundFromWave(WaveInspect(bytes, sizeBytes, &wave));
        if (status != SOUND_OK) return status;
        outInfo->frameCount = wave.frameCount;
        outInfo->channelCount = wave.channelCount;
        outInfo->sampleRate = wave.sampleRate;
        outInfo->sampleCount = wave.frameCount * wave.channelCount;
        outInfo->scratchBytes = 0u;
        return SOUND_OK;
    }
    case SOUND_FORMAT_MP3:
    {
        Mp3Info mp3;
        SoundStatus status = SoundFromMp3(Mp3Inspect(bytes, sizeBytes, &mp3));
        if (status != SOUND_OK) return status;
        outInfo->frameCount = mp3.frameCount;
        outInfo->channelCount = mp3.channelCount;
        outInfo->sampleRate = mp3.sampleRate;
        outInfo->sampleCount = mp3.frameCount * mp3.channelCount;
        outInfo->scratchBytes = mp3.scratchBytes;
        return SOUND_OK;
    }
    case SOUND_FORMAT_UNKNOWN: break;
    }
    return SOUND_NOT_RECOGNISED;
}

SoundStatus SoundDecodeSamples(const void *bytes, uint32_t sizeBytes, const SoundInfo *info,
                               int16_t *outSamples, uint32_t sampleCapacity, void *scratch,
                               uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || outSamples == NULL) return SOUND_INVALID_ARGUMENT;
    if (sampleCapacity < info->sampleCount) return SOUND_BUFFER_TOO_SMALL;

    switch (SoundProbe(bytes, sizeBytes))
    {
    case SOUND_FORMAT_WAVE:
    {
        WaveInfo wave;
        SoundStatus status = SoundFromWave(WaveInspect(bytes, sizeBytes, &wave));
        if (status != SOUND_OK) return status;
        if (wave.frameCount != info->frameCount || wave.channelCount != info->channelCount)
        {
            return SOUND_CORRUPT;
        }
        return SoundFromWave(
            WaveDecodeSamples(bytes, sizeBytes, &wave, outSamples, sampleCapacity));
    }
    case SOUND_FORMAT_MP3:
    {
        // Разбор заголовков повторяется, а не передаётся через SoundInfo:
        // проход по кадрам дешёвый, а формат не протекает наружу.
        Mp3Info mp3;
        SoundStatus status = SoundFromMp3(Mp3Inspect(bytes, sizeBytes, &mp3));
        if (status != SOUND_OK) return status;
        if (mp3.frameCount != info->frameCount || mp3.channelCount != info->channelCount)
        {
            return SOUND_CORRUPT;
        }
        return SoundFromMp3(Mp3DecodeSamples(bytes, sizeBytes, &mp3, outSamples, sampleCapacity,
                                             scratch, scratchBytes));
    }
    case SOUND_FORMAT_UNKNOWN: break;
    }
    return SOUND_NOT_RECOGNISED;
}

const char *SoundStatusText(SoundStatus status)
{
    switch (status)
    {
    case SOUND_OK: return "ok";
    case SOUND_INVALID_ARGUMENT: return "invalid argument";
    case SOUND_NOT_RECOGNISED: return "the file is not a sound this tool reads";
    case SOUND_TRUNCATED: return "the file ends before the sound does";
    case SOUND_CORRUPT: return "the sound data is damaged";
    case SOUND_UNSUPPORTED_FEATURE: return "the sound uses a feature this decoder does not read";
    case SOUND_TOO_LARGE: return "the sound is longer than one clip may be";
    case SOUND_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}

const char *SoundFormatName(SoundFormat format)
{
    switch (format)
    {
    case SOUND_FORMAT_WAVE: return "WAV";
    case SOUND_FORMAT_MP3: return "MP3";
    case SOUND_FORMAT_UNKNOWN: break;
    }
    return "unknown";
}

struct SoundStream
{
    SoundReader reader;
    SoundInfo info;
    SoundFormat format;
    WaveInfo wave;
    uint32_t position;
    uint8_t waveBlock[16384];
    /* The following payload is pointer-aligned, independently of WAV state. */
    uintptr_t decoder[];
};

SoundStatus SoundStreamInspect(const SoundReader *reader, SoundInfo *outInfo)
{
    if (reader == NULL || reader->readAt == NULL || outInfo == NULL)
        return SOUND_INVALID_ARGUMENT;
    uint8_t prefix[12];
    uint32_t prefixBytes =
        reader->sizeBytes < sizeof(prefix) ? (uint32_t)reader->sizeBytes : sizeof(prefix);
    if (!reader->readAt(reader->context, 0u, prefix, prefixBytes))
        return SOUND_TRUNCATED;
    SoundInfo info = {0};
    switch (SoundProbe(prefix, prefixBytes))
    {
    case SOUND_FORMAT_WAVE:
    {
        WaveInfo wave = {0};
        SoundStatus status = SoundFromWave(WaveInspectReader(reader, &wave));
        if (status != SOUND_OK)
            return status;
        info.frameCount = wave.frameCount;
        info.channelCount = wave.channelCount;
        info.sampleRate = wave.sampleRate;
        info.scratchBytes = sizeof(SoundStream);
        break;
    }
    case SOUND_FORMAT_MP3:
    {
        Mp3Info mp3 = {0};
        SoundStatus status = SoundFromMp3(Mp3InspectReader(reader, &mp3));
        if (status != SOUND_OK)
            return status;
        info.frameCount = mp3.frameCount;
        info.channelCount = mp3.channelCount;
        info.sampleRate = mp3.sampleRate;
        info.scratchBytes = sizeof(SoundStream) + Mp3StreamScratchBytes();
        break;
    }
    default:
        return SOUND_NOT_RECOGNISED;
    }
    info.sampleCount = info.frameCount * info.channelCount;
    *outInfo = info;
    return SOUND_OK;
}

SoundStatus SoundStreamInitialize(const SoundReader *reader, void *scratch, uint32_t scratchBytes,
                                  SoundStream **outStream)
{
    if (outStream != NULL)
        *outStream = NULL;
    if (scratch == NULL || outStream == NULL)
        return SOUND_INVALID_ARGUMENT;
    SoundInfo info = {0};
    SoundStatus status = SoundStreamInspect(reader, &info);
    if (status != SOUND_OK)
        return status;
    if (scratchBytes < info.scratchBytes)
        return SOUND_BUFFER_TOO_SMALL;
    SoundStream *stream = (SoundStream *)scratch;
    uint8_t prefix[12];
    uint32_t count =
        reader->sizeBytes < sizeof(prefix) ? (uint32_t)reader->sizeBytes : sizeof(prefix);
    if (!reader->readAt(reader->context, 0u, prefix, count))
        return SOUND_TRUNCATED;
    stream->reader = *reader;
    stream->info = info;
    stream->format = SoundProbe(prefix, count);
    stream->position = 0u;
    if (stream->format == SOUND_FORMAT_WAVE)
        status = SoundFromWave(WaveInspectReader(reader, &stream->wave));
    else
    {
        Mp3Info mp3 = {0};
        status = SoundFromMp3(Mp3InspectReader(reader, &mp3));
        if (status == SOUND_OK)
            status = SoundFromMp3(
                Mp3StreamInitialize(reader, &mp3, stream->decoder, scratchBytes - sizeof(*stream)));
    }
    if (status == SOUND_OK)
        *outStream = stream;
    return status;
}

SoundStatus SoundStreamRead(SoundStream *stream, int16_t *samples, uint32_t frameCapacity,
                            uint32_t *outFrames)
{
    if (outFrames != NULL)
        *outFrames = 0u;
    if (stream == NULL || samples == NULL || outFrames == NULL)
        return SOUND_INVALID_ARGUMENT;
    if (stream->format == SOUND_FORMAT_MP3)
        return SoundFromMp3(Mp3StreamRead(stream->decoder, samples, frameCapacity, outFrames));
    uint32_t count = stream->info.frameCount - stream->position;
    if (count > frameCapacity)
        count = frameCapacity;
    uint32_t frameBytes = stream->wave.channelCount * (stream->wave.bitsPerSample / 8u);
    uint8_t *block = stream->waveBlock;
    while (*outFrames < count)
    {
        uint32_t frames = count - *outFrames;
        if (frames > sizeof(stream->waveBlock) / frameBytes)
            frames = sizeof(stream->waveBlock) / frameBytes;
        uint32_t bytes = frames * frameBytes;
        uint64_t offset = stream->wave.dataOffset + (uint64_t)stream->position * frameBytes;
        if (!stream->reader.readAt(stream->reader.context, offset, block, bytes))
            return SOUND_TRUNCATED;
        WaveInfo wave = stream->wave;
        wave.dataOffset = 0u;
        wave.dataBytes = bytes;
        wave.frameCount = frames;
        SoundStatus status = SoundFromWave(
            WaveDecodeSamples(block, bytes, &wave, samples + *outFrames * wave.channelCount,
                              frames * wave.channelCount));
        if (status != SOUND_OK)
            return status;
        stream->position += frames;
        *outFrames += frames;
    }
    return SOUND_OK;
}

SoundStatus SoundStreamSeek(SoundStream *stream, uint32_t frame)
{
    if (stream == NULL || frame > stream->info.frameCount)
        return SOUND_INVALID_ARGUMENT;
    if (stream->format == SOUND_FORMAT_MP3)
        return SoundFromMp3(Mp3StreamSeek(stream->decoder, frame));
    stream->position = frame;
    return SOUND_OK;
}
