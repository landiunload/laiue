#include "audio/audio_stream_internal.h"

#include <string.h>

static bool ReadSource(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    AudioStream *stream = (AudioStream *)context;
    return offset <= stream->source.sizeBytes && count <= stream->source.sizeBytes - offset &&
           stream->source.readAt(stream->source.context, offset, bytes, count) != 0u;
}

static bool ReadFile(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    uint32_t read = 0u;
    return PlatformFileReadAt((PlatformReadFile *)context, offset, bytes, count, &read) &&
           read == count;
}

static uint32_t ReadFileSource(void *context, uint64_t offset, void *bytes, uint32_t count)
{
    return ReadFile(context, offset, bytes, count) ? 1u : 0u;
}

static void CloseFile(void *context)
{
    PlatformFileClose((PlatformReadFile *)context);
    PlatformFree(context);
}

static bool DecodePacket(AudioStream *stream, AudioStreamPacket *packet)
{
    uint32_t channels = stream->info.channelCount;
    uint32_t count = 0u;
    if (stream->carryReady)
    {
        memcpy(packet->samples, stream->carry, channels * sizeof(int16_t));
        count = 1u;
        ++stream->producerPosition;
        stream->carryReady = false;
    }
    while (count < AUDIO_STREAM_PACKET_FRAMES)
    {
        if (stream->producerPosition == stream->info.frameCount)
        {
            if (!stream->looping)
            {
                stream->eof = true;
                break;
            }
            if (SoundStreamSeek(stream->decoder, 0u) != SOUND_OK)
                return false;
            stream->producerPosition = 0u;
        }
        uint32_t needed = AUDIO_STREAM_PACKET_FRAMES - count;
        if (needed > stream->info.frameCount - stream->producerPosition)
            needed = stream->info.frameCount - stream->producerPosition;
        uint32_t read = 0u;
        if (SoundStreamRead(stream->decoder, packet->samples + count * channels, needed, &read) !=
                SOUND_OK ||
            read != needed)
            return false;
        count += read;
        stream->producerPosition += read;
    }
    if (count == 0u)
        return false;
    if (stream->producerPosition == stream->info.frameCount)
    {
        if (stream->looping)
        {
            if (SoundStreamSeek(stream->decoder, 0u) != SOUND_OK)
                return false;
            stream->producerPosition = 0u;
        }
        else
            stream->eof = true;
    }
    if (!stream->eof)
    {
        uint32_t read = 0u;
        if (SoundStreamRead(stream->decoder, stream->carry, 1u, &read) != SOUND_OK || read != 1u)
            return false;
        stream->carryReady = true;
        memcpy(packet->samples + count * channels, stream->carry, channels * sizeof(int16_t));
    }
    else
        memcpy(packet->samples + count * channels, packet->samples + (count - 1u) * channels,
               channels * sizeof(int16_t));
    packet->epoch = stream->epoch;
    packet->frameCount = count;
    return true;
}

static void PumpLocked(AudioStream *stream)
{
    while (!stream->eof && PlatformAtomicLoadU32Acquire(&stream->result) == AUDIO_RESULT_OK)
    {
        uint32_t write = stream->write;
        uint32_t read = PlatformAtomicLoadU32Acquire(&stream->read);
        if (write - read >= AUDIO_STREAM_PACKET_COUNT)
            break;
        AudioStreamPacket *packet = &stream->packets[write % AUDIO_STREAM_PACKET_COUNT];
        if (!DecodePacket(stream, packet))
        {
            PlatformAtomicStoreU32Release(&stream->result, AUDIO_RESULT_SOURCE_REJECTED);
            break;
        }
        PlatformAtomicStoreU32Release(&stream->write, write + 1u);
    }
}

void AudioStreamPump(AudioStream *stream)
{
    if (stream == NULL || stream->retired)
        return;
    PlatformMutexLock(&stream->decoderLock);
    PumpLocked(stream);
    PlatformMutexUnlock(&stream->decoderLock);
}

static uint32_t Worker(void *context)
{
    AudioStream *stream = (AudioStream *)context;
    while (PlatformAtomicLoadU32Acquire(&stream->terminate) == 0u)
    {
        /* retired belongs to the API caller; the worker observes terminate. */
        PlatformMutexLock(&stream->decoderLock);
        PumpLocked(stream);
        PlatformMutexUnlock(&stream->decoderLock);
        PlatformSleepMilliseconds(1u);
    }
    return 0u;
}

AudioResult AudioStreamAllocate(AudioDevice *device, const AudioStreamDescription *description,
                                bool worker, AudioStream **outStream)
{
    if (outStream != NULL)
        *outStream = NULL;
    if (device == NULL || description == NULL || outStream == NULL ||
        description->structSize < sizeof(*description) || description->readAt == NULL)
        return AUDIO_RESULT_INVALID_ARGUMENT;
    AudioStream *stream = (AudioStream *)PlatformAllocate(sizeof(*stream), true);
    if (stream == NULL)
        return AUDIO_RESULT_OUT_OF_MEMORY;
    stream->device = device;
    stream->source = *description;
    SoundReader reader = {stream, ReadSource, description->sizeBytes};
    AudioResult result = AUDIO_RESULT_SOURCE_REJECTED;
    if (SoundStreamInspect(&reader, &stream->info) != SOUND_OK)
        goto failed;
    stream->scratch = PlatformAllocate(stream->info.scratchBytes, true);
    if (stream->scratch == NULL)
    {
        result = AUDIO_RESULT_OUT_OF_MEMORY;
        goto failed;
    }
    if (SoundStreamInitialize(&reader, stream->scratch, stream->info.scratchBytes,
                              &stream->decoder) != SOUND_OK)
        goto failed;
    if (!PlatformMutexInitialize(&stream->decoderLock))
    {
        result = AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED;
        goto failed;
    }
    stream->mutexReady = true;
    stream->epoch = 1u;
    stream->eof = true;
    if (worker && !PlatformThreadStart(&stream->worker, Worker, stream))
    {
        result = AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED;
        goto failed;
    }
    stream->workerReady = worker;
    stream->sourceOwned = true;
    *outStream = stream;
    return AUDIO_RESULT_OK;
failed:
    AudioStreamFree(stream);
    return result;
}

AudioResult AudioStreamAllocateFile(AudioDevice *device, const char *path, bool worker,
                                    AudioStream **outStream)
{
    if (outStream != NULL)
        *outStream = NULL;
    if (path == NULL || device == NULL || outStream == NULL)
        return AUDIO_RESULT_INVALID_ARGUMENT;
    PlatformReadFile *file = (PlatformReadFile *)PlatformAllocate(sizeof(*file), true);
    if (file == NULL)
        return AUDIO_RESULT_OUT_OF_MEMORY;
    uint64_t size = 0u;
    if (!PlatformFileOpenRead(path, file, &size))
    {
        PlatformFree(file);
        return AUDIO_RESULT_SOURCE_REJECTED;
    }
    AudioStreamDescription description = {sizeof(description), file, ReadFileSource, CloseFile,
                                          size};
    AudioResult result = AudioStreamAllocate(device, &description, worker, outStream);
    if (result != AUDIO_RESULT_OK)
        CloseFile(file);
    return result;
}

void AudioStreamShutdown(AudioStream *stream)
{
    PlatformAtomicStoreU32Release(&stream->terminate, 1u);
    if (stream->workerReady)
    {
        PlatformThreadJoin(&stream->worker);
        stream->workerReady = false;
    }
    if (stream->sourceOwned)
    {
        if (stream->source.close != NULL)
            stream->source.close(stream->source.context);
        stream->sourceOwned = false;
    }
}

void AudioStreamFree(AudioStream *stream)
{
    if (stream == NULL)
        return;
    AudioStreamShutdown(stream);
    if (stream->mutexReady)
        PlatformMutexDestroy(&stream->decoderLock);
    PlatformFree(stream->scratch);
    PlatformFree(stream);
}

bool AudioStreamPrepare(AudioStream *stream, uint32_t frame, bool looping, uint32_t *outEpoch)
{
    PlatformMutexLock(&stream->decoderLock);
    if (looping && frame == stream->info.frameCount)
        frame = 0u;
    bool ok = SoundStreamSeek(stream->decoder, frame) == SOUND_OK;
    if (ok)
    {
        ++stream->epoch;
        if (stream->epoch == 0u)
            ++stream->epoch;
        stream->producerPosition = frame;
        stream->looping = looping;
        stream->eof = frame == stream->info.frameCount;
        stream->carryReady = false;
        PlatformAtomicStoreU32Release(&stream->result, AUDIO_RESULT_OK);
        *outEpoch = stream->epoch;
        PumpLocked(stream);
    }
    PlatformMutexUnlock(&stream->decoderLock);
    return ok;
}

void AudioStreamSetLooping(AudioStream *stream, bool looping)
{
    PlatformMutexLock(&stream->decoderLock);
    if (looping && stream->eof && SoundStreamSeek(stream->decoder, 0u) == SOUND_OK)
    {
        stream->producerPosition = 0u;
        stream->carryReady = false;
        stream->eof = false;
    }
    stream->looping = looping;
    PlatformMutexUnlock(&stream->decoderLock);
}

bool AudioStreamSample(AudioStream *stream, uint32_t epoch, double *offset, double step,
                       float *left, float *right)
{
    uint32_t read = stream->read;
    for (;;)
    {
        uint32_t write = PlatformAtomicLoadU32Acquire(&stream->write);
        if (read == write)
            return false;
        AudioStreamPacket *packet = &stream->packets[read % AUDIO_STREAM_PACKET_COUNT];
        if (packet->epoch != epoch)
        {
            if ((int32_t)(packet->epoch - epoch) > 0)
                return false;
            PlatformAtomicStoreU32Release(&stream->read, ++read);
            continue;
        }
        if (*offset >= packet->frameCount)
        {
            *offset -= packet->frameCount;
            PlatformAtomicStoreU32Release(&stream->read, ++read);
            continue;
        }
        uint32_t index = (uint32_t)*offset;
        float fraction = (float)(*offset - index);
        uint32_t channels = stream->info.channelCount;
        float a = packet->samples[index * channels] * (1.0f / 32768.0f);
        float b = packet->samples[(index + 1u) * channels] * (1.0f / 32768.0f);
        *left = a + (b - a) * fraction;
        *right = *left;
        if (channels == 2u)
        {
            a = packet->samples[index * channels + 1u] * (1.0f / 32768.0f);
            b = packet->samples[(index + 1u) * channels + 1u] * (1.0f / 32768.0f);
            *right = a + (b - a) * fraction;
        }
        *offset += step;
        return true;
    }
}

double AudioStreamDurationSeconds(const AudioStream *stream)
{
    return stream == NULL ? 0.0 : (double)stream->info.frameCount / stream->info.sampleRate;
}

bool AudioStreamGetStats(const AudioStream *stream, AudioStreamStats *stats)
{
    if (stream == NULL || stats == NULL || stats->structSize < sizeof(*stats) || stream->retired)
        return false;
    stats->frameCount = stream->info.frameCount;
    stats->sampleRate = stream->info.sampleRate;
    stats->channelCount = stream->info.channelCount;
    uint32_t read = PlatformAtomicLoadU32Acquire(&stream->read);
    uint32_t write = PlatformAtomicLoadU32Acquire(&stream->write);
    /* Conservative upper bound avoids racing consumer/producer packet fields. */
    uint32_t packets = write - read;
    if (packets > AUDIO_STREAM_PACKET_COUNT)
        packets = AUDIO_STREAM_PACKET_COUNT;
    stats->bufferedFrames = packets * AUDIO_STREAM_PACKET_FRAMES;
    stats->memoryBytes = sizeof(*stream) + stream->info.scratchBytes +
                         (stream->source.close == CloseFile ? sizeof(PlatformReadFile) : 0u);
    stats->underruns = (uint64_t)PlatformAtomicLoadI64(&stream->underruns);
    stats->result = (AudioResult)PlatformAtomicLoadU32Acquire(&stream->result);
    return true;
}
