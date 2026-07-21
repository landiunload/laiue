#include <windows.h>
#include <mmeapi.h>

#include "audio/audio.h"

static wchar_t audioTestPath[MAX_PATH];

// Тест собирается без CRT, поэтому вывод — прямая запись в stdout.
// Без него падение на CI выглядит как «Failed» и ни строчки причины.
static void AudioTestWrite(const char* text)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
    {
        return;
    }

    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }

    DWORD written = 0;
    WriteFile(output, text, length, &written, NULL);
}

static void AudioTestWriteHex(uint32_t value)
{
    static const char hexDigits[] = "0123456789abcdef";
    char text[11];

    text[0] = '0';
    text[1] = 'x';
    for (uint32_t index = 0; index < 8; ++index)
    {
        text[2 + index] = hexDigits[(value >> ((7 - index) * 4)) & 0xFu];
    }
    text[10] = '\0';

    AudioTestWrite(text);
}

static void AudioTestWriteNumber(uint32_t value)
{
    char text[11];
    uint32_t position = sizeof(text) - 1;

    text[position] = '\0';
    do
    {
        text[--position] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (value != 0 && position != 0);

    AudioTestWrite(&text[position]);
}

static void AudioTestCleanup(void)
{
    if (audioTestPath[0] != L'\0')
    {
        DeleteFileW(audioTestPath);
        audioTestPath[0] = L'\0';
    }
}

static void AudioTestFail(uint32_t code, const char* reason)
{
    AudioTestWrite("Проверка ");
    AudioTestWriteNumber(code);
    AudioTestWrite(" не пройдена: ");
    AudioTestWrite(reason);
    AudioTestWrite("\r\n");

    AudioTestCleanup();
    ExitProcess(code);
}

// 125 — код пропуска, объявленный в tests/CMakeLists.txt через SKIP_RETURN_CODE.
static void AudioTestSkip(const char* reason)
{
    AudioTestWrite("Пропуск: ");
    AudioTestWrite(reason);
    AudioTestWrite("\r\n");

    AudioTestCleanup();
    ExitProcess(125);
}

// Единственное обращение к winmm, без COM: ноль устройств вывода означает,
// что воспроизводить некуда. Так выглядит агент CI и сервер без звуковой
// карты — проверки контракта API там осмысленны, а проигрывание нет.
static bool AudioTestHasOutputDevice(void)
{
    return waveOutGetNumDevs() != 0;
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

// Последняя ошибка проигрывателя: нужна и для диагностики, и чтобы отличить
// «нет устройства вывода» от настоящей поломки.
static AudioEvent audioTestLastError;
static bool audioTestSawError;

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
                audioTestLastError = event;
                audioTestSawError = true;
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

// MF_E_NO_AUDIO_PLAYBACK_DEVICE: устройство вывода исчезло между проверкой
// waveOutGetNumDevs и загрузкой источника — тоже повод пропустить, а не падать.
#define AUDIO_TEST_NO_PLAYBACK_DEVICE ((int32_t)0xC00D36FA)

static void AudioTestFailPlayback(uint32_t code, const char* reason)
{
    if (audioTestSawError)
    {
        if (audioTestLastError.platformCode == AUDIO_TEST_NO_PLAYBACK_DEVICE)
        {
            AudioTestSkip("Media Foundation не нашла устройство вывода");
        }

        AudioTestWrite("Ошибка проигрывателя: поток ");
        AudioTestWriteNumber((uint32_t)audioTestLastError.streamError);
        AudioTestWrite(", HRESULT ");
        AudioTestWriteHex((uint32_t)audioTestLastError.platformCode);
        AudioTestWrite("\r\n");
    }

    AudioTestFail(code, reason);
}

void AudioApiTestEntryPoint(void)
{
    AudioPlayer* player = NULL;
    AudioResult result = AudioPlayerCreate(NULL, &player);
    if (result == AUDIO_RESULT_PLATFORM_INITIALIZATION_FAILED
        || result == AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED)
    {
        // Корректный skip для Windows N/Server без Media Feature Pack.
        AudioTestSkip("Media Foundation недоступна");
    }
    if (result != AUDIO_RESULT_OK || player == NULL)
    {
        AudioTestFail(1, "AudioPlayerCreate вернул неожиданный результат");
    }

    AudioPlayerSnapshot snapshot;
    if (!AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.state != AUDIO_PLAYBACK_EMPTY
        || snapshot.volume != 1.0f
        || snapshot.muted || snapshot.looping || snapshot.hasAudio)
    {
        AudioTestFail(2, "состояние нового проигрывателя не пустое");
    }

    AudioEvent event;
    if (AudioPlayerPollEvent(player, &event))
    {
        AudioTestFail(3, "очередь событий нового проигрывателя не пуста");
    }
    if (AudioPlayerPlay(player) != AUDIO_RESULT_INVALID_STATE
        || AudioPlayerLoadUri(player, L"") != AUDIO_RESULT_INVALID_ARGUMENT
        || AudioPlayerSeek(player, -1.0) != AUDIO_RESULT_INVALID_ARGUMENT)
    {
        AudioTestFail(4, "негодные вызовы не отклонены");
    }

    if (AudioPlayerSetVolume(player, 0.25f) != AUDIO_RESULT_OK
        || AudioPlayerSetMuted(player, true) != AUDIO_RESULT_OK
        || AudioPlayerSetLooping(player, true) != AUDIO_RESULT_OK
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.volume != 0.25f || !snapshot.muted || !snapshot.looping)
    {
        AudioTestFail(5, "громкость, приглушение или повтор не применились");
    }

    // Всё выше проверяет контракт API и проходит даже на машине без звука.
    // Всё ниже требует настоящего устройства вывода.
    if (!AudioTestHasOutputDevice())
    {
        AudioPlayerDestroy(player);
        AudioTestSkip("в системе нет устройств вывода звука");
    }

    if (!AudioTestCreateWave())
    {
        AudioTestFail(6, "не удалось создать временный WAV");
    }
    if (AudioPlayerLoadFile(player, audioTestPath) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_READY, 5000)
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || !snapshot.hasAudio || snapshot.durationSeconds <= 0.0)
    {
        AudioTestFailPlayback(6, "источник не дошёл до состояния READY");
    }
    if (AudioPlayerPlay(player) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_PLAYING, 3000)
        || AudioPlayerPause(player) != AUDIO_RESULT_OK
        || !AudioTestWaitForState(player, AUDIO_PLAYBACK_PAUSED, 3000))
    {
        AudioTestFailPlayback(7, "воспроизведение или пауза не сработали");
    }

    if (AudioPlayerClear(player) != AUDIO_RESULT_OK
        || !AudioPlayerPollEvent(player, &event)
        || event.type != AUDIO_EVENT_CLEARED
        || !AudioPlayerGetSnapshot(player, &snapshot)
        || snapshot.state != AUDIO_PLAYBACK_EMPTY)
    {
        AudioTestFail(8, "очистка не вернула проигрыватель в пустое состояние");
    }

    AudioPlayerDestroy(player);
    AudioTestCleanup();
    ExitProcess(0);
}
