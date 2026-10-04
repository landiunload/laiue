// Системный вывод macOS и iOS через выходной AudioUnit. Это общий для обеих
// систем C-интерфейс AudioToolbox: на macOS — DefaultOutput (устройство по
// умолчанию, следует за его сменой), на iOS — RemoteIO. Модель pull
// совпадает с контрактом микшера: система сама вызывает render callback на
// своём realtime-потоке и просит ровно столько кадров, сколько нужно.
//
// Формат входа юнита задаётся чередующимся float-стерео; конвертер внутри
// AudioUnit сам приводит его к формату устройства и частоте.

#include "audio/audio_backend.h"
#include "platform/system.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <string.h>

#define AUDIO_COREAUDIO_DEFAULT_SAMPLE_RATE 48000u
#define AUDIO_COREAUDIO_CHANNELS 2u

typedef struct AudioCoreAudioBackend
{
    AudioBackend base;

    AudioRenderCallback render;
    void *context;

    AudioComponentInstance unit;
    bool initialized;
    bool started;
    // Буфер, в который система не смогла получить весь запрошенный объём,
    // считается опустошением: тишина лучше чтения за его границей.
    volatile int64_t underruns;
} AudioCoreAudioBackend;

static OSStatus CoreAudioRender(void *refCon, AudioUnitRenderActionFlags *actionFlags,
                                const AudioTimeStamp *timeStamp, UInt32 busNumber,
                                UInt32 frameCount, AudioBufferList *data)
{
    (void)timeStamp;
    (void)busNumber;
    AudioCoreAudioBackend *backend = (AudioCoreAudioBackend *)refCon;
    if (data == NULL || data->mNumberBuffers < 1u || data->mBuffers[0].mData == NULL)
        return noErr;
    AudioBuffer *buffer = &data->mBuffers[0];
    const UInt32 capacity =
        buffer->mDataByteSize / (UInt32)(sizeof(float) * AUDIO_COREAUDIO_CHANNELS);
    UInt32 frames = frameCount;
    if (frames > capacity)
    {
        frames = capacity;
        PlatformAtomicIncrementI64(&backend->underruns);
    }
    if (frames != 0u)
        backend->render(backend->context, (float *)buffer->mData, (uint32_t)frames);
    else if (actionFlags != NULL)
        *actionFlags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

static void CoreAudioDestroy(AudioBackend *base)
{
    AudioCoreAudioBackend *backend = (AudioCoreAudioBackend *)base;
    if (backend->unit != NULL)
    {
        // Stop синхронен: после возврата render callback больше не идёт.
        if (backend->started)
            AudioOutputUnitStop(backend->unit);
        if (backend->initialized)
            AudioUnitUninitialize(backend->unit);
        AudioComponentInstanceDispose(backend->unit);
        backend->unit = NULL;
    }
    AudioBackendFree(&backend->base.allocator, backend);
}

static uint64_t CoreAudioUnderrunCount(const AudioBackend *base)
{
    const AudioCoreAudioBackend *backend = (const AudioCoreAudioBackend *)base;
    return (uint64_t)PlatformAtomicLoadI64(&backend->underruns);
}

static const AudioBackendVtable COREAUDIO_VTABLE = {
    .destroy = CoreAudioDestroy,
    .underrunCount = CoreAudioUnderrunCount,
};

bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend)
{
    if (description == NULL || outBackend == NULL || description->render == NULL)
        return false;
    *outBackend = NULL;
    if (!AudioBackendAllocatorIsValid(&description->allocator))
        return false;

    AudioCoreAudioBackend *backend = (AudioCoreAudioBackend *)AudioBackendAllocate(
        &description->allocator, sizeof(*backend), true);
    if (backend == NULL)
        return false;
    backend->base.vtable = &COREAUDIO_VTABLE;
    backend->base.allocator = description->allocator;
    backend->render = description->render;
    backend->context = description->context;

    AudioComponentDescription componentDescription;
    memset(&componentDescription, 0, sizeof(componentDescription));
    componentDescription.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
    componentDescription.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    componentDescription.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
    componentDescription.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(NULL, &componentDescription);
    if (component == NULL || AudioComponentInstanceNew(component, &backend->unit) != noErr)
    {
        backend->unit = NULL;
        CoreAudioDestroy(&backend->base);
        return false;
    }

    const uint32_t sampleRate = description->sampleRate != 0u
                                    ? description->sampleRate
                                    : AUDIO_COREAUDIO_DEFAULT_SAMPLE_RATE;
    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = (Float64)sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1u;
    format.mChannelsPerFrame = AUDIO_COREAUDIO_CHANNELS;
    format.mBitsPerChannel = 32u;
    format.mBytesPerFrame = (UInt32)(sizeof(float) * AUDIO_COREAUDIO_CHANNELS);
    format.mBytesPerPacket = format.mBytesPerFrame;
    AURenderCallbackStruct callback;
    memset(&callback, 0, sizeof(callback));
    callback.inputProc = CoreAudioRender;
    callback.inputProcRefCon = backend;
    if (AudioUnitSetProperty(backend->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr ||
        AudioUnitSetProperty(backend->unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr)
    {
        CoreAudioDestroy(&backend->base);
        return false;
    }
    if (AudioUnitInitialize(backend->unit) != noErr)
    {
        CoreAudioDestroy(&backend->base);
        return false;
    }
    backend->initialized = true;

    UInt32 maximumFrames = 0u;
    UInt32 propertySize = sizeof(maximumFrames);
    if (AudioUnitGetProperty(backend->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maximumFrames, &propertySize) != noErr)
        maximumFrames = 0u;
    backend->base.sampleRate = sampleRate;
    backend->base.channelCount = AUDIO_COREAUDIO_CHANNELS;
    backend->base.bufferFrameCount =
        maximumFrames != 0u ? (uint32_t)maximumFrames : description->frameCountHint;
    if (backend->base.bufferFrameCount == 0u)
        backend->base.bufferFrameCount = 512u;

    if (AudioOutputUnitStart(backend->unit) != noErr)
    {
        CoreAudioDestroy(&backend->base);
        return false;
    }
    backend->started = true;
    *outBackend = &backend->base;
    return true;
}
