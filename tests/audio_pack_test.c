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
#include "mp3_fixtures.h"

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

static uint32_t ReadU32LeAt(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
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
    wchar_t stepsDirectory[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t nestedSound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t waveSound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t mp3Sound[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t waveCache[LAIUE_PLATFORM_PATH_CAPACITY];
    wchar_t mp3Cache[LAIUE_PLATFORM_PATH_CAPACITY];
} AudioPackTestPaths;

static bool NameEquals(const wchar_t *left, const wchar_t *right)
{
    while (*left != 0 && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

// Канонический WAV: только `fmt ` и `data`, PCM16 моно. Полный разбор
// проверяет laiue.tools.soundc; здесь нужен один настоящий файл, чтобы
// увидеть, что пак его берёт.
static void BuildCanonicalWave(uint8_t *file, const int16_t *samples, uint32_t frameCount)
{
    uint32_t dataBytes = frameCount * 2u;
    const char *tags = "RIFFWAVEfmt data";
    for (uint32_t index = 0; index < 4u; ++index)
    {
        file[index] = (uint8_t)tags[index];
        file[8u + index] = (uint8_t)tags[4u + index];
        file[12u + index] = (uint8_t)tags[8u + index];
        file[36u + index] = (uint8_t)tags[12u + index];
    }
    WriteU32Le(file + 4, 36u + dataBytes);
    WriteU32Le(file + 16, 16u);              // размер чанка fmt
    WriteU16Le(file + 20, 1u);               // PCM
    WriteU16Le(file + 22, 1u);               // каналов
    WriteU32Le(file + 24, TEST_SAMPLE_RATE);
    WriteU32Le(file + 28, TEST_SAMPLE_RATE * 2u);
    WriteU16Le(file + 32, 2u);               // выравнивание блока
    WriteU16Le(file + 34, 16u);              // бит на сэмпл
    WriteU32Le(file + 40, dataBytes);

    for (uint32_t index = 0; index < frameCount; ++index)
    {
        WriteU16Le(file + 44u + index * 2u, (uint32_t)(uint16_t)samples[index]);
    }
}

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

    // Без активного пака звук не находится, но выдача не пуста: как
    // текстурпак подставляет нейтральный слой, так звукопак — тишину.
    // Причина при этом названа статусом, а не спрятана.
    AudioPackLoadStatus status = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    AudioClip *silent = AudioClipLoadFrom(device, catalog, L"step", &status);
    Expect(silent != NULL, "a missing pack must still yield the silent default");
    Expect(status == AUDIO_PACK_LOAD_NO_ACTIVE_PACK, "the status must name the missing pack");
    Expect(AudioClipDurationSeconds(silent) < 0.001, "the default sound must be silence");
    AudioClipDestroy(silent);

    Expect(AudioPackActivateIn(catalog, L"Base.lap"), "the base pack could not be activated");

    // Дерево приводится к известному виду до первого перечисления:
    // прерванный прошлый запуск оставил бы в паке лишние файлы, и
    // счёт поехал бы не там, где ошибка.
    if (Join(paths->stepsDirectory, LAIUE_PLATFORM_PATH_CAPACITY, paths->basePack, L"steps"))
    {
        // Кэш от прошлого запуска — тоже файл в паке, и его надо
        // убрать вместе с остальными, иначе счёт звуков поедет.
        static const wchar_t *const leftovers[6] = {
            L"stone.la", L"wood.wav", L"gravel.mp3", L"wood.wav.la", L"gravel.mp3.la", L"wood.la",
        };
        for (uint32_t index = 0; index < 6u; ++index)
        {
            if (Join(paths->nestedSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                     leftovers[index]))
            {
                PlatformDeleteFile(paths->nestedSound);
            }
        }
    }
    // Порядок форматов задаёт файл, и прерванный прогон мог его
    // оставить: следующий начал бы с чужой настройкой.
    if (Join(paths->nestedSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->sounds, L"formats.txt"))
    {
        PlatformDeleteFile(paths->nestedSound);
    }

    AudioPackList soundList;
    Expect(AudioPackEnumerateSoundsFrom(catalog, &soundList), "sounds could not be enumerated");
    Expect(soundList.count == 3u, "every sound in the pack must be listed");
    AudioPackListRelease(&soundList);

    // === Подпапки и исходный WAV ===
    // Звук адресуется именем, а имя вправе быть путём: пак делится на
    // папки, как обычное дерево файлов. Рядом со своим `.la` лежит
    // обычный WAV — его тоже надо уметь взять, иначе «настроить самому»
    // означало бы обязательную пересборку каждого файла.
    Expect(Join(paths->stepsDirectory, LAIUE_PLATFORM_PATH_CAPACITY, paths->basePack, L"steps") &&
               Join(paths->nestedSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                    L"stone.la") &&
               Join(paths->waveSound, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                    L"wood.wav") &&
               Join(paths->mp3Sound, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                    L"gravel.mp3") &&
               Join(paths->waveCache, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                    L"wood.wav.la") &&
               Join(paths->mp3Cache, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory,
                    L"gravel.mp3.la"),
           "nested path construction");
    Expect(PlatformCreateDirectory(paths->stepsDirectory), "nested directory creation");
    Expect(PlatformWriteEntireFile(paths->nestedSound, pcmFile, LA_HEADER_SIZE + pcmPayload),
           "the nested sound could not be written");

    uint32_t waveBytes = 44u + TEST_FRAMES * 2u;
    uint8_t *waveFile = PlatformAllocate(waveBytes, true);
    Expect(waveFile != NULL, "wav buffer could not be allocated");
    BuildCanonicalWave(waveFile, reference, TEST_FRAMES);
    Expect(PlatformWriteEntireFile(paths->waveSound, waveFile, waveBytes),
           "the wav sound could not be written");
    PlatformFree(waveFile);

    Expect(PlatformWriteEntireFile(paths->mp3Sound, MP3_SOUND_FILE, sizeof(MP3_SOUND_FILE)),
           "the mp3 sound could not be written");

    AudioPackList nestedList;
    Expect(AudioPackEnumerateSoundsFrom(catalog, &nestedList), "sounds could not be enumerated");
    Expect(nestedList.count == 6u, "the listing must reach into subdirectories");
    bool sawNested = false;
    bool sawWave = false;
    bool sawMp3 = false;
    for (uint32_t index = 0; index < nestedList.count; ++index)
    {
        sawNested = sawNested || NameEquals(nestedList.entries[index].name, L"steps/stone");
        sawWave = sawWave || NameEquals(nestedList.entries[index].name, L"steps/wood");
        sawMp3 = sawMp3 || NameEquals(nestedList.entries[index].name, L"steps/gravel");
    }
    AudioPackListRelease(&nestedList);
    Expect(sawNested, "a sound in a subdirectory must be listed by its path");
    Expect(sawWave, "a wav sound must be listed under its own name");
    Expect(sawMp3, "an mp3 sound must be listed under its own name");

    AudioClip *nested = AudioClipLoadFrom(device, catalog, L"steps/stone", &status);
    Expect(nested != NULL && status == AUDIO_PACK_LOAD_OK,
           "a sound in a subdirectory must load by its path");
    AudioClipDestroy(nested);

    AudioClip *fromWave = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(fromWave != NULL && status == AUDIO_PACK_LOAD_OK, "a wav sound must load from the pack");
    // Длительность считается по кадрам и частоте: если бы WAV разобрался
    // неправильно, она разъехалась бы вместе с ними.
    double expectedSeconds = (double)TEST_FRAMES / (double)TEST_SAMPLE_RATE;
    double actualSeconds = AudioClipDurationSeconds(fromWave);
    Expect(actualSeconds > expectedSeconds * 0.99 && actualSeconds < expectedSeconds * 1.01,
           "the wav sound must keep its length");
    AudioClipDestroy(fromWave);

    // MP3 берётся из пака так же, как WAV. Длительность здесь — не
    // формальность: она получается только если задержка кодировщика
    // снята, а кадры разобраны все до одного.
    AudioClip *fromMp3 = AudioClipLoadFrom(device, catalog, L"steps/gravel", &status);
    Expect(fromMp3 != NULL && status == AUDIO_PACK_LOAD_OK, "an mp3 sound must load from the pack");
    double mp3Seconds =
        (double)(sizeof(MP3_SOUND_PCM) / sizeof(MP3_SOUND_PCM[0])) /
        ((double)MP3_SOUND_CHANNELS * (double)MP3_SOUND_RATE);
    double actualMp3Seconds = AudioClipDurationSeconds(fromMp3);
    Expect(actualMp3Seconds > mp3Seconds * 0.999 && actualMp3Seconds < mp3Seconds * 1.001,
           "the mp3 sound must keep the length the reference decoder gives");
    AudioClipDestroy(fromMp3);

    // === Кэш рядом с исходником ===
    // Прочитав чужой формат, движок кладёт рядом готовый `.la`. Дальше
    // берётся он: разбирать WAV и MP3 на каждом запуске незачем.
    Expect(PlatformPathExists(paths->waveCache) && PlatformPathExists(paths->mp3Cache),
           "loading a foreign format must leave a prepared sound next to it");

    // === Отпечаток исходника ===
    // Свежесть кэша определяет не «кто новее», а размер и время
    // исходника, записанные в сам кэш: одно обращение к каталогу, без
    // чтения исходника.
    PlatformPathInformation sourceInfo;
    Expect(PlatformGetPathInformation(paths->waveSound, &sourceInfo) && sourceInfo.exists,
           "the wav source could not be inspected");

    uint8_t *cacheBytes = NULL;
    uint64_t cacheSize = 0u;
    Expect(PlatformReadEntireFile(paths->waveCache, 0x1000000u, &cacheBytes, &cacheSize),
           "the cached sound could not be read");
    uint64_t stampTime =
        (uint64_t)ReadU32LeAt(cacheBytes + 24) | ((uint64_t)ReadU32LeAt(cacheBytes + 28) << 32);
    Expect(stampTime == sourceInfo.modifiedTime, "the cache must record the time of its source");
    Expect(ReadU32LeAt(cacheBytes + 32) == (uint32_t)sourceInfo.size,
           "the cache must record the size of its source");

    // Отпечаток портится — кэш обязан быть пересобран, а не принят на
    // веру. Проверка не зависит от разрешения часов файловой системы.
    cacheBytes[32] ^= 0xFFu;
    Expect(PlatformWriteEntireFile(paths->waveCache, cacheBytes, cacheSize),
           "the tampered cache could not be written");
    PlatformFree(cacheBytes);

    AudioClip *restamped = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(restamped != NULL && status == AUDIO_PACK_LOAD_OK,
           "a sound with a tampered cache must still load");
    AudioClipDestroy(restamped);

    cacheBytes = NULL;
    Expect(PlatformReadEntireFile(paths->waveCache, 0x1000000u, &cacheBytes, &cacheSize),
           "the rebuilt cache could not be read");
    Expect(ReadU32LeAt(cacheBytes + 32) == (uint32_t)sourceInfo.size,
           "a cache whose stamp does not match its source must be rebuilt");
    PlatformFree(cacheBytes);

    // Перечисление обязано остаться прежним: кэш — не отдельный звук.
    AudioPackList cachedList;
    Expect(AudioPackEnumerateSoundsFrom(catalog, &cachedList), "sounds could not be enumerated");
    Expect(cachedList.count == 6u, "a cache file must not appear as a sound of its own");
    bool sawCacheName = false;
    for (uint32_t index = 0; index < cachedList.count; ++index)
    {
        sawCacheName = sawCacheName || NameEquals(cachedList.entries[index].name, L"steps/wood.wav");
    }
    AudioPackListRelease(&cachedList);
    Expect(!sawCacheName, "a cache file must not be listed under the name of its source");

    // === Приоритет форматов ===
    // Свой `.la` — запасной путь, а не главный. Пока исходник жив и
    // читается, играет он: иначе `.la`, положенный когда-то, навсегда
    // заслонил бы обновлённый рядом WAV, и «положи новый файл» как
    // способ поменять звук перестал бы работать.
    wchar_t *authoredPath =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *formatsPath =
        PlatformAllocate((size_t)LAIUE_PLATFORM_PATH_CAPACITY * sizeof(wchar_t), false);
    Expect(authoredPath != NULL && formatsPath != NULL, "priority path scratch");
    Expect(Join(authoredPath, LAIUE_PLATFORM_PATH_CAPACITY, paths->stepsDirectory, L"wood.la") &&
               Join(formatsPath, LAIUE_PLATFORM_PATH_CAPACITY, paths->sounds, L"formats.txt"),
           "priority path construction");

    // Длина отличает авторский файл от исходника и от его кэша: это
    // единственное, что тест здесь и различает.
    uint32_t authoredFrames = TEST_FRAMES / 4u;
    uint32_t authoredPayload = authoredFrames * 2u;
    double authoredSeconds = (double)authoredFrames / (double)TEST_SAMPLE_RATE;
    WriteHeader(pcmFile, 1u, LA_ENCODING_PCM16, TEST_SAMPLE_RATE, authoredFrames, authoredPayload);
    for (uint32_t index = 0; index < authoredFrames; ++index)
    {
        WriteU16Le(pcmFile + LA_HEADER_SIZE + index * 2u, (uint32_t)(uint16_t)reference[index]);
    }
    Expect(PlatformWriteEntireFile(authoredPath, pcmFile, LA_HEADER_SIZE + authoredPayload),
           "the authored sound could not be written");

    AudioClip *preferred = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(preferred != NULL && status == AUDIO_PACK_LOAD_OK, "the wav sound must still load");
    double preferredSeconds = AudioClipDurationSeconds(preferred);
    Expect(preferredSeconds > expectedSeconds * 0.99 && preferredSeconds < expectedSeconds * 1.01,
           "a live source must win over an authored .la");
    AudioClipDestroy(preferred);

    // `formats.txt` переставляет порядок. Свой формат первым — играет
    // он, и рядом ничего не появляется: выводить его не из чего.
    Expect(PlatformDeleteFile(paths->waveCache), "the cache could not be removed");
    static const char formatsOwnFirst[] = "# priority\nla\nwav\n";
    Expect(PlatformWriteEntireFile(formatsPath, formatsOwnFirst, sizeof(formatsOwnFirst) - 1u),
           "formats.txt could not be written");

    AudioClip *authored = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(authored != NULL && status == AUDIO_PACK_LOAD_OK, "the authored sound must load");
    double authoredActual = AudioClipDurationSeconds(authored);
    Expect(authoredActual > authoredSeconds * 0.99 && authoredActual < authoredSeconds * 1.01,
           "formats.txt must be able to put the engine format first");
    AudioClipDestroy(authored);
    Expect(!PlatformPathExists(paths->waveCache),
           "nothing may be built next to a source the engine never read");

    // Формат, которого в файле нет, не исчезает — он идёт следом.
    // Иначе одна забытая строка прятала бы содержимое пака.
    static const char formatsOnlyOwn[] = "la\n";
    Expect(PlatformWriteEntireFile(formatsPath, formatsOnlyOwn, sizeof(formatsOnlyOwn) - 1u),
           "formats.txt could not be rewritten");
    Expect(PlatformDeleteFile(authoredPath), "the authored sound could not be removed");
    AudioClip *unlisted = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(unlisted != NULL && status == AUDIO_PACK_LOAD_OK,
           "a format missing from formats.txt must still be reachable");
    double unlistedSeconds = AudioClipDurationSeconds(unlisted);
    Expect(unlistedSeconds > expectedSeconds * 0.99 && unlistedSeconds < expectedSeconds * 1.01,
           "the unlisted wav must be found after the listed formats");
    AudioClipDestroy(unlisted);
    Expect(PlatformDeleteFile(formatsPath), "formats.txt could not be removed");

    // Ради чего свой формат и стоит последним: исходник испортился —
    // играет заранее собранный `.la`, а не тишина.
    Expect(PlatformWriteEntireFile(authoredPath, pcmFile, LA_HEADER_SIZE + authoredPayload),
           "the authored sound could not be restored");
    Expect(PlatformDeleteFile(paths->waveCache), "the rebuilt cache could not be removed");
    static const uint8_t rubbish[64] = {0};
    Expect(PlatformWriteEntireFile(paths->waveSound, rubbish, sizeof(rubbish)),
           "the damaged wav could not be written");

    AudioClip *damaged = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(damaged != NULL && status == AUDIO_PACK_LOAD_OK,
           "a damaged source must fall through to the authored .la");
    double damagedSeconds = AudioClipDurationSeconds(damaged);
    Expect(damagedSeconds > authoredSeconds * 0.99 && damagedSeconds < authoredSeconds * 1.01,
           "the fallback must be the authored sound, not silence");
    AudioClipDestroy(damaged);
    Expect(!PlatformPathExists(paths->waveCache), "a damaged source must not leave a cache");

    // Дальше тест проверяет жизнь кэша, и запасной путь ему помешал бы.
    uint8_t *restoredWave = PlatformAllocate(waveBytes, true);
    Expect(restoredWave != NULL, "wav buffer could not be allocated");
    BuildCanonicalWave(restoredWave, reference, TEST_FRAMES);
    Expect(PlatformWriteEntireFile(paths->waveSound, restoredWave, waveBytes),
           "the wav sound could not be restored");
    PlatformFree(restoredWave);
    Expect(PlatformDeleteFile(authoredPath), "the authored sound could not be removed");
    PlatformFree(authoredPath);
    PlatformFree(formatsPath);

    AudioClip *reread = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(reread != NULL && status == AUDIO_PACK_LOAD_OK, "the restored wav must load");
    AudioClipDestroy(reread);
    Expect(PlatformPathExists(paths->waveCache), "the restored wav must be cached again");

    // Исходник пропал — звук остаётся: собранный `.la` его переживает.
    Expect(PlatformDeleteFile(paths->waveSound) && PlatformDeleteFile(paths->mp3Sound),
           "the source sounds could not be removed");
    AudioClip *survivedWave = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(survivedWave != NULL && status == AUDIO_PACK_LOAD_OK,
           "a sound must survive the loss of its source");
    double survivedSeconds = AudioClipDurationSeconds(survivedWave);
    Expect(survivedSeconds > expectedSeconds * 0.99 && survivedSeconds < expectedSeconds * 1.01,
           "the cached sound must keep the length of its source");
    AudioClipDestroy(survivedWave);

    AudioClip *survivedMp3 = AudioClipLoadFrom(device, catalog, L"steps/gravel", &status);
    Expect(survivedMp3 != NULL && status == AUDIO_PACK_LOAD_OK,
           "an mp3 sound must survive the loss of its source");
    AudioClipDestroy(survivedMp3);

    // Изменился исходник — кэш пересобирается. Судья здесь отпечаток, а
    // не часы: у нового файла другой размер, и этого достаточно.
    uint32_t shortFrames = TEST_FRAMES / 2u;
    uint32_t shortBytes = 44u + shortFrames * 2u;
    uint8_t *shortWave = PlatformAllocate(shortBytes, true);
    Expect(shortWave != NULL, "short wav buffer could not be allocated");
    BuildCanonicalWave(shortWave, reference, shortFrames);
    Expect(PlatformWriteEntireFile(paths->waveSound, shortWave, shortBytes),
           "the shorter wav could not be written");
    PlatformFree(shortWave);

    AudioClip *rebuilt = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(rebuilt != NULL && status == AUDIO_PACK_LOAD_OK, "the changed sound must load");
    double rebuiltSeconds = AudioClipDurationSeconds(rebuilt);
    double shortSeconds = (double)shortFrames / (double)TEST_SAMPLE_RATE;
    Expect(rebuiltSeconds > shortSeconds * 0.99 && rebuiltSeconds < shortSeconds * 1.01,
           "the cache must be rebuilt when the source changes");
    AudioClipDestroy(rebuilt);

    // Нет ни исходника, ни кэша — остаётся тишина.
    Expect(PlatformDeleteFile(paths->waveSound) && PlatformDeleteFile(paths->waveCache),
           "the wav and its cache could not be removed");
    AudioClip *goneWave = AudioClipLoadFrom(device, catalog, L"steps/wood", &status);
    Expect(goneWave != NULL && status == AUDIO_PACK_LOAD_SOUND_NOT_FOUND,
           "a sound without a source and without a cache must fall back to silence");
    Expect(AudioClipDurationSeconds(goneWave) < 0.001, "the default sound must be silence");
    AudioClipDestroy(goneWave);

    // Выйти за пределы пака нельзя и путём.
    Expect(AudioClipLoadFrom(device, catalog, L"steps/../../secret", &status) == NULL,
           "a name that leaves the pack must be refused");
    Expect(status == AUDIO_PACK_LOAD_INVALID_SOUND, "the refusal must name the invalid sound");

    PlatformDeleteFile(paths->nestedSound);
    PlatformDeleteFile(paths->waveSound);
    PlatformDeleteFile(paths->mp3Sound);
    PlatformDeleteFile(paths->waveCache);
    PlatformDeleteFile(paths->mp3Cache);
    PlatformRemoveDirectory(paths->stepsDirectory);

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

    // === Повреждённый и отсутствующий файл дают тишину ===
    // Игра не обязана проверять указатель после каждой загрузки: она
    // получает пригодный клип, а разбираться с причиной может по
    // статусу — ровно как с материалом, которого нет в текстурпаке.
    AudioClip *fromBroken = AudioClipLoadFrom(device, catalog, L"broken", &status);
    Expect(fromBroken != NULL, "a truncated sound must still yield the silent default");
    Expect(status == AUDIO_PACK_LOAD_INVALID_SOUND, "the status must name the invalid sound");
    Expect(AudioClipDurationSeconds(fromBroken) < 0.001, "the default sound must be silence");
    AudioClipDestroy(fromBroken);

    AudioClip *fromMissing = AudioClipLoadFrom(device, catalog, L"missing", &status);
    Expect(fromMissing != NULL, "an absent sound must still yield the silent default");
    Expect(status == AUDIO_PACK_LOAD_SOUND_NOT_FOUND, "the status must name the missing sound");
    Expect(AudioClipDurationSeconds(fromMissing) < 0.001, "the default sound must be silence");
    AudioClipDestroy(fromMissing);

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
