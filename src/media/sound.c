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
