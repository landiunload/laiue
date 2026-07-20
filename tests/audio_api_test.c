#include <windows.h>

#include "audio/audio.h"

static wchar_t audioTestPath[MAX_PATH];

static void AudioTestFail(uint32_t code)
{
    if (audioTestPath[0] != L'\0')
    {
        DeleteFileW(audioTestPath);
    }
    ExitProcess(code);
}

typedef struct AudioTestWaveHeader
{
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char format[4];
    uint32_t formatSize;
    uint16_t audioFormat;
    uint16_t channelCount;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
} AudioTestWaveHeader;

static bool AudioTestCreateWave(void)
{
    wchar_t temporaryDirectory[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0
        || GetTempFileNameW(temporaryDirectory, L"lau", 0,
            audioTestPath) == 0)
    {
        return false;
    }

    HANDLE file = CreateFileW(audioTestPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    AudioTestWaveHeader header = {
        .riff = { 'R', 'I', 'F', 'F' },
        .fileSize = 36U + 16000U,
        .wave = { 'W', 'A', 'V', 'E' },
        .format = { 'f', 'm', 't', ' ' },
        .formatSize = 16,
        .audioFormat = 1,
        .channelCount = 1,
        .sampleRate = 8000,
        .byteRate = 8000,
        .blockAlign = 1,
        .bitsPerSample = 8,
        .data = { 'd', 'a', 't', 'a' },
        .dataSize = 16000,
    };

    DWORD written = 0;
    bool succeeded = WriteFile(file, &header, sizeof(header),
        &written, NULL) && written == sizeof(header);

    uint8_t silence[256];
    for (uint32_t index = 0; index < sizeof(silence); ++index)
    {
        silence[index] = 128;
    }
    for (uint32_t offset = 0; succeeded && offset < header.dataSize;
        offset += sizeof(silence))
    {
        uint32_t remaining = header.dataSize - offset;
        DWORD blockSize = remaining < sizeof(silence)
            ? remaining : sizeof(silence);
        succeeded = WriteFile(file, silence, blockSize, &written, NULL)
            && written == blockSize;
    }
    succeeded = CloseHandle(file) && succeeded;
    return succeeded;
}

static bool AudioTestWaitForState(AudioPlayer* player,
    AudioPlaybackState expected, uint32_t timeoutMilliseconds)
{
    uint32_t elapsed = 0;
    while (elapsed < timeoutMilliseconds)
    {
        AudioEvent event;
        while (AudioPlayerPollEvent(player, &event))
        {
            if (event.type == AUDIO_EVENT_ERROR)
            {
                return false;
            }
        }

        AudioPlayerSnapshot snapshot;
        if (AudioPlayerGetSnapshot(player, &snapshot)
            && snapshot.state == expected)
        {
            return true;
        }
        Sleep(10);
        elapsed += 10;
    }
    return false;
}

void AudioApiTestEntryPoint(void)
{
    AudioPlayer* player = NULL;
    AudioResult result = AudioPlayerCreate(NULL, &player);
    if (result == AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED
        || result == AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED)
    {
        // Корректный skip для Windows N/Server без Media Feature Pack.
        ExitProcess(125);
    }
    if (result != AUDIO_RESULT_OK || player == NULL)
    {
        AudioTestFail(1);
    }

    AudioPlayerSnapshot snapshot;
    if (!AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.state != AUDIO_PLAYBACK_EMPTY
        || snapshot.volume != 1.0f
        || snapshot.muted || snapshot.looping || snapshot.hasAudio)
    {
        AudioTestFail(2);
    }

    AudioEvent event;
    if (AudioPlayerPollEvent(player, &event))
    {
        AudioTestFail(3);
    }
    if (AudioPlayerPlay(player) != AUDIO_RESULT_INVALID_STATE
        || AudioPlayerLoadUri(player, L"") != AUDIO_RESULT_INVALID_ARGUMENT
        || AudioPlayerSeek(player, -1.0) != AUDIO_RESULT_INVALID_ARGUMENT)
    {
        AudioTestFail(4);
    }

    if (AudioPlayerSetVolume(player, 0.25f) != AUDIO_RESULT_OK
        || AudioPlayerSetMuted(player, true) != AUDIO_RESULT_OK
        || AudioPlayerSetLooping(player, true) != AUDIO_RESULT_OK
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.volume != 0.25f || !snapshot.muted || !snapshot.looping)
    {
        AudioTestFail(5);
    }

    if (!AudioTestCreateWave()
        || AudioPlayerLoadFile(player, audioTestPath) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_READY, 5000)
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || !snapshot.hasAudio || snapshot.durationSeconds <= 0.0)
    {
        AudioTestFail(6);
    }
    if (AudioPlayerPlay(player) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_PLAYING, 3000)
        || AudioPlayerPause(player) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_PAUSED, 3000))
    {
        AudioTestFail(7);
    }

    if (AudioPlayerClear(player) != AUDIO_RESULT_OK
        || !AudioPlayerPollEvent(player, &event)
        || event.type != AUDIO_EVENT_CLEARED
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.state != AUDIO_PLAYBACK_EMPTY)
    {
        AudioTestFail(8);
    }

    AudioPlayerDestroy(player);
    DeleteFileW(audioTestPath);
    audioTestPath[0] = L'\0';
    ExitProcess(0);
}
