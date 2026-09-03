// Формат `.la` и звукопаки `.lap`: сборка файла, загрузка через каталог
// содержимого, подмена звука вторым паком и отказ на повреждённых файлах.
//
// ADPCM проверяется круговым проходом: тест сам кодирует известную волну
// и сверяет декодированный результат с исходником. Формат принадлежит
// движку, поэтому важно именно согласие кодировщика с декодером и то,
// что ошибка остаётся малой, а не совпадение с чужой реализацией.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "audio/audio_pack.h"
#include "content/content_catalog.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define TEST_SAMPLE_RATE 48000u
#define TEST_FRAMES 512u
#define LA_HEADER_SIZE 24u
#define LA_ENCODING_PCM16 0u
#define LA_ENCODING_ADPCM 1u

// Панорама равной мощности сохраняет суммарную мощность, а не амплитуду
// канала: голос по центру звучит в каждом канале с усилением 1/sqrt(2).
// Тест обязан это учитывать, иначе он проверяет не декодер, а панораму.
#define CENTRE_GAIN 0.70710678f

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Audio pack check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static bool Join(wchar_t *destination, uint32_t capacity, const wchar_t *base, const wchar_t *part)
{
    uint32_t length = 0u;
    while (base[length] != L'\0')
    {
        if (length + 1u >= capacity) return false;
        destination[length] = base[length];
        ++length;
    }
    if (length > 0u && destination[length - 1u] != L'/' && destination[length - 1u] != L'\\')
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = L'/';
    }
    for (uint32_t index = 0u; part[index] != L'\0'; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = part[index];
    }
    destination[length] = L'\0';
    return true;
}

static void WriteU16Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void WriteU32Le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void WriteHeader(uint8_t *bytes, uint32_t channels, uint32_t encoding, uint32_t sampleRate,
                        uint32_t frameCount, uint32_t payloadBytes)
{
    bytes[0] = 'L';
    bytes[1] = 'A';
    bytes[2] = 'S';
    bytes[3] = '1';
    WriteU16Le(bytes + 4, 1u);
    WriteU16Le(bytes + 6, LA_HEADER_SIZE);
    WriteU16Le(bytes + 8, channels);
    WriteU16Le(bytes + 10, encoding);
    WriteU32Le(bytes + 12, sampleRate);
    WriteU32Le(bytes + 16, frameCount);
    WriteU32Le(bytes + 20, payloadBytes);
}

// === Кодировщик ADPCM ===
// Копия таблиц из декодера намеренная: тест обязан проверять декодер, а
// не переиспользовать его внутренности.

static const int32_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int32_t STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};

typedef struct EncoderState
{
    int32_t predictor;
    int32_t stepIndex;
} EncoderState;

static uint32_t EncodeNibble(EncoderState *state, int32_t sample)
{
    int32_t step = STEP_TABLE[state->stepIndex];
    int32_t difference = sample - state->predictor;

    uint32_t nibble = 0u;
    if (difference < 0)
    {
        nibble = 8u;
        difference = -difference;
    }

    int32_t reconstructed = step >> 3;
    if (difference >= step)
    {
        nibble |= 4u;
        difference -= step;
        reconstructed += step;
    }
    if (difference >= (step >> 1))
    {
        nibble |= 2u;
        difference -= step >> 1;
        reconstructed += step >> 1;
    }
    if (difference >= (step >> 2))
    {
        nibble |= 1u;
        reconstructed += step >> 2;
    }

    int32_t predictor =
        state->predictor + ((nibble & 8u) != 0u ? -reconstructed : reconstructed);
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = predictor;

    int32_t stepIndex = state->stepIndex + INDEX_TABLE[nibble];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    state->stepIndex = stepIndex;
    return nibble;
}

// Начальный шаг выбирается по первому перепаду. Он хранится в файле
// именно затем, чтобы кодировщик мог это сделать: со шага 7 декодер
// догонял бы быстрый сигнал десяток отсчётов, и весь разгон был бы
// слышен как щелчок в начале звука.
static int32_t ChooseInitialStepIndex(const int16_t *samples, uint32_t frameCount)
{
    int32_t largestDelta = 0;
    uint32_t inspected = frameCount < 16u ? frameCount : 16u;
    for (uint32_t index = 1; index < inspected; ++index)
    {
        int32_t delta = (int32_t)samples[index] - (int32_t)samples[index - 1u];
        if (delta < 0) delta = -delta;
        if (delta > largestDelta) largestDelta = delta;
    }
    for (int32_t index = 0; index < 88; ++index)
    {
        // Максимум, представимый одним полубайтом, — 1,875 шага.
        if (STEP_TABLE[index] * 15 / 8 >= largestDelta) return index;
    }
    return 88;
}

static uint32_t EncodeAdpcmMono(const int16_t *samples, uint32_t frameCount, uint8_t *payload)
{
    EncoderState state = {
        .predictor = samples[0],
        .stepIndex = ChooseInitialStepIndex(samples, frameCount),
    };
    WriteU16Le(payload, (uint32_t)(uint16_t)(int16_t)state.predictor);
    WriteU16Le(payload + 2, (uint32_t)state.stepIndex);

    uint8_t *nibbles = payload + 4;
    uint32_t byteCount = (frameCount + 1u) / 2u;
    for (uint32_t index = 0; index < byteCount; ++index) nibbles[index] = 0u;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        uint32_t nibble = EncodeNibble(&state, samples[frame]);
        if ((frame & 1u) != 0u) nibbles[frame / 2u] |= (uint8_t)(nibble << 4);
        else nibbles[frame / 2u] |= (uint8_t)nibble;
    }
    return 4u + byteCount;
}

// Треугольная волна: у неё предсказуемый спектр и она хорошо ловит
// ошибки шага ADPCM, в отличие от постоянного уровня.
static void FillTriangle(int16_t *samples, uint32_t frameCount)
{
    for (uint32_t index = 0; index < frameCount; ++index)
    {
        uint32_t phase = index % 64u;
        int32_t value = phase < 32u ? (int32_t)phase : (int32_t)(64u - phase);
        samples[index] = (int16_t)((value - 16) * 512);
    }
}

typedef struct AudioPackTestPaths
{
    wchar_t executable[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t root[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t sounds[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t basePack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t modPack[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t baseSound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t modSound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t adpcmSound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t brokenSound[LAIUE_PLATFORM_PATH_CAPACITY];
} AudioPackTestPaths;

static int32_t AbsoluteDifference(int32_t left, int32_t right)
{
    int32_t difference = left - right;
    return difference < 0 ? -difference : difference;
}

LAIUE_TEST_ENTRY(AudioPackTestEntryPoint)
{
    // Пути живут в куче: девять буферов по килослову переполнили бы
    // страницу стека, а в сборке без CRT нет __chkstk, чтобы её нарастить.
    AudioPackTestPaths *paths = PlatformAllocate(sizeof(*paths), true);
    Expect(paths != NULL, "path scratch could not be allocated");

    Expect(PlatformExecutableDirectory(paths->executable, LAIUE_PLATFORM_PATH_CAPACITY),
           "executable directory");
    Expect(Join(paths->root, LAIUE_PLATFORM_PATH_CAPACITY, paths->executable,
                L"audio_pack_test_v1") &&
               Join(paths->sounds, LAIUE_PLATFORM_PATH_CAPACITY, paths->root, L"sounds") &&
               Join(paths->basePack, LAIUE_PLATFORM_PATH_CAPACITY, paths->sounds, L"Base.lap") &&
               Join(paths->modPack, LAIUE_PLATFORM_PATH_CAPACITY, paths->sounds, L"Mod.lap") &&
               Join(paths->baseSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->basePack,
                    L"step.la") &&
               Join(paths->adpcmSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->basePack,
                    L"music.la") &&
               Join(paths->brokenSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->basePack,
                    L"broken.la") &&
               Join(paths->modSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->modPack, L"step.la"),
           "path construction");
    Expect(PlatformCreateDirectory(paths->root) && PlatformCreateDirectory(paths->sounds) &&
               PlatformCreateDirectory(paths->basePack) &&
               PlatformCreateDirectory(paths->modPack),
           "directory creation");

    int16_t *reference = PlatformAllocate(TEST_FRAMES * sizeof(int16_t), false);
    Expect(reference != NULL, "reference samples could not be allocated");
    FillTriangle(reference, TEST_FRAMES);

    // === PCM16: базовый звук ===
    uint32_t pcmPayload = TEST_FRAMES * (uint32_t)sizeof(int16_t);
    uint8_t *pcmFile = PlatformAllocate(LA_HEADER_SIZE + pcmPayload, false);
    Expect(pcmFile != NULL, "pcm file buffer could not be allocated");
    WriteHeader(pcmFile, 1u, LA_ENCODING_PCM16, TEST_SAMPLE_RATE, TEST_FRAMES, pcmPayload);
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        WriteU16Le(pcmFile + LA_HEADER_SIZE + index * 2u, (uint32_t)(uint16_t)reference[index]);
    }
    Expect(PlatformWriteEntireFile(paths->baseSound, pcmFile, LA_HEADER_SIZE + pcmPayload),
           "base sound could not be written");

    // === ADPCM: тот же сигнал вчетверо компактнее ===
    uint32_t adpcmCapacity = 4u + (TEST_FRAMES + 1u) / 2u;
    uint8_t *adpcmFile = PlatformAllocate(LA_HEADER_SIZE + adpcmCapacity, true);
    Expect(adpcmFile != NULL, "adpcm file buffer could not be allocated");
    uint32_t adpcmPayload = EncodeAdpcmMono(reference, TEST_FRAMES, adpcmFile + LA_HEADER_SIZE);
    Expect(adpcmPayload == adpcmCapacity, "the encoder must fill the documented payload size");
    WriteHeader(adpcmFile, 1u, LA_ENCODING_ADPCM, TEST_SAMPLE_RATE, TEST_FRAMES, adpcmPayload);
    Expect(PlatformWriteEntireFile(paths->adpcmSound, adpcmFile, LA_HEADER_SIZE + adpcmPayload),
           "adpcm sound could not be written");
    Expect(adpcmPayload * 4u < pcmPayload + 64u, "ADPCM must be about four times smaller");

    // === Повреждённый файл ===
    uint8_t broken[LA_HEADER_SIZE + 8u];
    for (uint32_t index = 0; index < sizeof(broken); ++index) broken[index] = 0u;
    WriteHeader(broken, 1u, LA_ENCODING_PCM16, TEST_SAMPLE_RATE, TEST_FRAMES, pcmPayload);
    // Заголовок обещает больше данных, чем в файле есть.
    Expect(PlatformWriteEntireFile(paths->brokenSound, broken, sizeof(broken)),
           "broken sound could not be written");

    // === Подменяющий пак ===
    int16_t *replacement = PlatformAllocate(TEST_FRAMES * sizeof(int16_t), false);
    Expect(replacement != NULL, "replacement samples could not be allocated");
    for (uint32_t index = 0; index < TEST_FRAMES; ++index) replacement[index] = 4096;
    WriteHeader(pcmFile, 1u, LA_ENCODING_PCM16, TEST_SAMPLE_RATE, TEST_FRAMES, pcmPayload);
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        WriteU16Le(pcmFile + LA_HEADER_SIZE + index * 2u, (uint32_t)(uint16_t)replacement[index]);
    }
    Expect(PlatformWriteEntireFile(paths->modSound, pcmFile, LA_HEADER_SIZE + pcmPayload),
           "replacement sound could not be written");

    // === Каталог и устройство ===
    LaiueContentCatalog *catalog = LaiueContentCatalogCreate(paths->root);
    Expect(catalog != NULL, "catalog could not be created");

    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_OFFSCREEN,
        .sampleRate = TEST_SAMPLE_RATE,
        .frameCountHint = TEST_FRAMES,
        .masterVolume = 1.0f,
    };
    AudioDevice *device = NULL;
    Expect(AudioDeviceCreate(&configuration, &device) == AUDIO_RESULT_OK,
           "offscreen device could not be created");

    AudioPackList packs;
    Expect(AudioPackEnumerateFrom(catalog, &packs), "packs could not be enumerated");
    Expect(packs.count == 2u, "both packs must be listed");
    AudioPackListRelease(&packs);

    // Каталог переживает прогон, поэтому выбор сбрасывается явно: иначе
    // второй запуск начинался бы с пака, выбранного первым.
    Expect(AudioPackActivateIn(catalog, NULL), "the active pack could not be cleared");

    // Без активного пака звук не находится: это не ошибка формата.
    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    Expect(AudioClipLoadFrom(device, catalog, L"step", &status) == NULL,
           "no clip may load without an active pack");
    Expect(status == AUDIO_PACK_LOAD_NO_ACTIVE_PACK, "the status must name the missing pack");

    Expect(AudioPackActivateIn(catalog, L"Base.lap"), "the base pack could not be activated");

    AudioPackList soundList;
    Expect(AudioPackEnumerateSoundsFrom(catalog, &soundList), "sounds could not be enumerated");
    Expect(soundList.count == 3u, "every sound in the pack must be listed");
    AudioPackListRelease(&soundList);

    // === PCM16 загружается точно ===
    AudioClip *clip = AudioClipLoadFrom(device, catalog, L"step", &status);
    Expect(clip != NULL && status == AUDIO_PACK_LOAD_OK, "the base sound could not be loaded");
    Expect(AudioClipDurationSeconds(clip) > 0.0, "the clip must report a duration");

    float *frames = PlatformAllocate(TEST_FRAMES * 2u * sizeof(float), true);
    Expect(frames != NULL, "mix buffer could not be allocated");
    Expect(AudioVoicePlay(device, clip, NULL) != AUDIO_VOICE_NONE, "the clip must be playable");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");

    // Пик исходной волны — 8192 из 32768, то есть четверть шкалы.
    float peak = 0.0f;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        float value = frames[index * 2u] < 0.0f ? -frames[index * 2u] : frames[index * 2u];
        if (value > peak) peak = value;
    }
    // Исходный пик 8192 из 32768 — четверть шкалы, ослабленная панорамой.
    float expectedPeak = 0.25f * CENTRE_GAIN;
    Expect(peak > expectedPeak * 0.9f && peak < expectedPeak * 1.1f,
           "the decoded PCM must reproduce the original amplitude");
    AudioClipDestroy(clip);
    AudioDeviceStopAllVoices(device);
    AudioDeviceRenderFrames(device, frames, TEST_FRAMES);

    // === ADPCM декодируется близко к исходнику ===
    AudioClip *music = AudioClipLoadFrom(device, catalog, L"music", &status);
    Expect(music != NULL && status == AUDIO_PACK_LOAD_OK, "the adpcm sound could not be loaded");
    AudioVoice musicVoice = AudioVoicePlay(device, music, NULL);
    Expect(musicVoice != AUDIO_VOICE_NONE, "the adpcm clip must be playable");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");

    // Сравнение с исходником: ADPCM с потерями, но ошибка обязана
    // оставаться малой долей шкалы, иначе таблицы шага разошлись.
    int32_t worstError = 0;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        int32_t decoded = (int32_t)(frames[index * 2u] / CENTRE_GAIN * 32768.0f);
        int32_t error = AbsoluteDifference(decoded, reference[index]);
        if (error > worstError) worstError = error;
    }
    // На этом сигнале кодек даёт около 70 единиц из 32768. Порог взят с
    // запасом вчетверо: он ловит расхождение таблиц или начального шага,
    // но не срабатывает от разницы округления между компиляторами.
    Expect(worstError < 300, "the ADPCM round trip must stay close to the original signal");
    Expect(worstError > 0, "a lossy codec that reproduces the input exactly is suspicious");

    // Клип рушится, пока его голос ещё активен. Контракт так делать
    // запрещает, но библиотека обязана это пережить: иначе ошибка
    // приложения превращается в чтение освобождённой памяти, которое
    // проявляется редко и не там, где допущено.
    Expect(AudioVoiceIsActive(device, musicVoice), "the adpcm voice must still be active");
    AudioClipDestroy(music);
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    Expect(!AudioVoiceIsActive(device, musicVoice),
           "destroying a clip must stop the voices reading it");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");
    float residual = 0.0f;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        float value = frames[index * 2u] < 0.0f ? -frames[index * 2u] : frames[index * 2u];
        if (value > residual) residual = value;
    }
    Expect(residual == 0.0f, "no sound may remain after the clip is destroyed");

    // === Повреждённый файл отвергается ===
    Expect(AudioClipLoadFrom(device, catalog, L"broken", &status) == NULL,
           "a truncated sound must be rejected");
    Expect(status == AUDIO_PACK_LOAD_INVALID_SOUND, "the status must name the invalid sound");

    Expect(AudioClipLoadFrom(device, catalog, L"missing", &status) == NULL,
           "an absent sound must be reported as missing");
    Expect(status == AUDIO_PACK_LOAD_SOUND_NOT_FOUND, "the status must name the missing sound");

    // Выход за пределы пака запрещён именем, а не проверкой пути.
    Expect(AudioClipLoadFrom(device, catalog, L"../step", &status) == NULL,
           "a traversing name must be refused");

    // === Подмена звука другим паком ===
    Expect(AudioPackActivateIn(catalog, L"Mod.lap"), "the mod pack could not be activated");
    AudioClip *replaced = AudioClipLoadFrom(device, catalog, L"step", &status);
    Expect(replaced != NULL && status == AUDIO_PACK_LOAD_OK,
           "the replacement sound could not be loaded");
    Expect(AudioVoicePlay(device, replaced, NULL) != AUDIO_VOICE_NONE,
           "the replacement must be playable");
    Expect(AudioDeviceRenderFrames(device, frames, TEST_FRAMES), "rendering must succeed");

    // Подменяющий звук — постоянный уровень 4096, то есть одна восьмая.
    float replacedPeak = 0.0f;
    for (uint32_t index = 0; index < TEST_FRAMES; ++index)
    {
        float value = frames[index * 2u] < 0.0f ? -frames[index * 2u] : frames[index * 2u];
        if (value > replacedPeak) replacedPeak = value;
    }
    float expectedReplacedPeak = 0.125f * CENTRE_GAIN;
    Expect(replacedPeak > expectedReplacedPeak * 0.9f &&
               replacedPeak < expectedReplacedPeak * 1.1f,
           "the active pack must decide which sound plays");
    AudioClipDestroy(replaced);

    PlatformFree(frames);
    PlatformFree(pcmFile);
    PlatformFree(adpcmFile);
    PlatformFree(reference);
    PlatformFree(replacement);
    PlatformFree(paths);
    AudioDeviceDestroy(device);
    LaiueContentCatalogDestroy(catalog);

    LaiueTestRuntimeWrite("Audio pack checks passed\n");
    LAIUE_TEST_SUCCESS();
}
