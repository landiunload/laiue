// Формат `.la` и звукопаки `.lap`. Загрузчик читает и свой формат, и
// обычный WAV: формат определяется по сигнатуре, а не по расширению,
// поэтому приложение вправе доставлять содержимое как ему удобно.
// Разбор чужого контейнера берётся из общей `media_support` — той же,
// что читает PNG для текстурпака, — и одна реализация служит и
// движку, и офлайн-конвертеру.

#include "audio/audio_pack.h"
#include "audio/audio_pack_runtime.h"
#include "content/content_catalog.h"
#include "media/la_encode.h"
#include "media/sound.h"
#include "platform/system.h"

#include <stdint.h>
#include <string.h>

/* The decoder is usable without the mixer or catalog DLL being linked into
 * this image.  The bootstrap installs these service tables before a pack
 * module is started.  Keep the lookup in one place so every public operation
 * has the same fail-closed behaviour when a dependency is absent. */
static const AudioPackRuntime *PackRuntime(void)
{
    return AudioPackRuntimeGet();
}

static const LaiueAudioServiceV1 *PackAudio(void)
{
    const AudioPackRuntime *runtime = PackRuntime();
    return runtime == NULL ? NULL : runtime->audio;
}

static const LaiueContentServiceV1 *PackContent(void)
{
    const AudioPackRuntime *runtime = PackRuntime();
    return runtime == NULL ? NULL : runtime->content;
}

#define LA_MAGIC 0x3153414Cu   // 'L','A','S','1' little-endian
#define LA_VERSION 1u
#define LA_HEADER_SIZE 24u

#define LA_ENCODING_PCM16 0u
#define LA_ENCODING_ADPCM 1u

// Предел на один звук: 256 МиБ распакованных сэмплов. Значение не про
// вкус, а про арифметику — оно гарантирует, что произведения кадров,
// каналов и размера сэмпла не переполняют 32 бита.
#define LA_MAX_FRAMES 0x04000000u
#define LA_MAX_FILE_BYTES 0x20000000u

static uint16_t ReadU16Le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

// Порядок байтов машины. На little-endian сэмплы PCM16 из файла уже лежат
// как int16, и собирать их побайтово незачем; на big-endian остаётся
// переносимый путь через ReadU16Le.
static bool HostUsesLittleEndian(void)
{
    const uint16_t probe = 1u;
    uint8_t first = 0u;
    memcpy(&first, &probe, sizeof(first));
    return first == 1u;
}

// === IMA ADPCM ===
//
// Кодирование блочное только по названию: клип распаковывается целиком
// при загрузке, поэтому точек синхронизации не нужно и поток идёт одним
// куском на канал. Каналы лежат последовательно, а не чередуются:
// декодировать так проще, а размер тот же.

static const int32_t ADPCM_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int32_t ADPCM_STEP_TABLE[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,
    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,
    73,    80,    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,
    230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};

typedef struct AdpcmState
{
    int32_t predictor;
    int32_t stepIndex;
} AdpcmState;

static int16_t AdpcmDecodeNibble(AdpcmState *state, uint32_t nibble)
{
    int32_t step = ADPCM_STEP_TABLE[state->stepIndex];

    int32_t difference = step >> 3;
    if (nibble & 4u) difference += step;
    if (nibble & 2u) difference += step >> 1;
    if (nibble & 1u) difference += step >> 2;
    if (nibble & 8u) difference = -difference;

    int32_t predictor = state->predictor + difference;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = predictor;

    int32_t stepIndex = state->stepIndex + ADPCM_INDEX_TABLE[nibble & 15u];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    state->stepIndex = stepIndex;

    return (int16_t)predictor;
}

static uint32_t AdpcmChannelBytes(uint32_t frameCount)
{
    return (frameCount + 1u) / 2u;
}

// Полный размер payload ADPCM: по 4 байта состояния на канал плюс
// полубайт на кадр каждого канала.
static bool AdpcmPayloadBytes(uint32_t frameCount, uint32_t channelCount, uint32_t *outBytes)
{
    if (channelCount == 0u || frameCount == 0u) return false;
    uint64_t total = (uint64_t)channelCount * (4u + (uint64_t)AdpcmChannelBytes(frameCount));
    if (total > LA_MAX_FILE_BYTES) return false;
    *outBytes = (uint32_t)total;
    return true;
}

static bool AdpcmDecode(const uint8_t *payload, uint32_t payloadBytes, uint32_t frameCount,
                        uint32_t channelCount, int16_t *outSamples)
{
    uint32_t expected = 0u;
    if (!AdpcmPayloadBytes(frameCount, channelCount, &expected) || payloadBytes < expected)
        return false;

    const uint8_t *cursor = payload;
    for (uint32_t channel = 0; channel < channelCount; ++channel)
    {
        AdpcmState state = {
            .predictor = (int16_t)ReadU16Le(cursor),
            .stepIndex = (int16_t)ReadU16Le(cursor + 2),
        };
        if (state.stepIndex < 0 || state.stepIndex > 88) return false;
        cursor += 4;

        // По два отсчёта на байт: младший полубайт идёт первым. Парный
        // проход снимает деление и проверку чётности с каждого кадра и
        // читает упакованные байты последовательно. Нечётный последний
        // кадр берёт младший полубайт последнего байта; порядок отсчётов
        // и обновление состояния декодера при этом не меняются.
        const uint8_t *packed = cursor;
        int16_t *out = outSamples + channel;
        uint32_t remaining = frameCount;
        while (remaining >= 2u)
        {
            uint8_t byte = *packed++;
            *out = AdpcmDecodeNibble(&state, (uint32_t)(byte & 15u));
            out += channelCount;
            *out = AdpcmDecodeNibble(&state, (uint32_t)(byte >> 4));
            out += channelCount;
            remaining -= 2u;
        }
        if (remaining != 0u)
        {
            *out = AdpcmDecodeNibble(&state, (uint32_t)(*packed & 15u));
        }
        cursor += AdpcmChannelBytes(frameCount);
    }
    return true;
}

// === Разбор `.la` ===

typedef struct DecodedSound
{
    int16_t *samples;
    // Буфер, которым владеет разобранный звук и который надо вернуть после
    // создания клипа. У обычного пути совпадает с `samples`; у вида на
    // PCM16-payload это исходный файл, из которого сэмплы взяты как есть;
    // NULL означает, что буфер принадлежит вызывающему.
    void *allocation;
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
    // Отпечаток исходника, записанный в `.la`. Нулевой размер означает,
    // что файл ни из чего не выведен.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
} DecodedSound;

// `bytesOwned` означает, что вызывающий передаёт владение `bytes`:
// тогда после создания клипа буфер будет возвращён. Если false, `bytes`
// принадлежит вызывающему и только читается.
static AudioPackLoadStatus DecodeSound(const uint8_t *bytes, uint32_t sizeBytes, bool bytesOwned,
                                       DecodedSound *outSound)
{
    if (bytes == NULL || sizeBytes < SOUND_LA_HEADER_BYTES_V1) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (ReadU32Le(bytes) != LA_MAGIC) return AUDIO_PACK_LOAD_INVALID_SOUND;

    uint32_t version = ReadU16Le(bytes + 4);
    uint32_t headerSize = ReadU16Le(bytes + 6);
    // Версия 1 не знала об отпечатке исходника и читается по-прежнему:
    // файл, собранный прежней сборкой, не обязан устаревать вместе с
    // форматом.
    if (version == 1u)
    {
        if (headerSize != SOUND_LA_HEADER_BYTES_V1) return AUDIO_PACK_LOAD_INVALID_SOUND;
    }
    else if (version == SOUND_LA_VERSION)
    {
        if (headerSize != SOUND_LA_HEADER_BYTES) return AUDIO_PACK_LOAD_INVALID_SOUND;
    }
    else
    {
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    }
    if (sizeBytes < headerSize) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (version != 1u)
    {
        outSound->sourceModifiedTime =
            (uint64_t)ReadU32Le(bytes + 24) | ((uint64_t)ReadU32Le(bytes + 28) << 32);
        outSound->sourceSizeBytes = ReadU32Le(bytes + 32);
    }

    uint32_t channelCount = ReadU16Le(bytes + 8);
    uint32_t encoding = ReadU16Le(bytes + 10);
    uint32_t sampleRate = ReadU32Le(bytes + 12);
    uint32_t frameCount = ReadU32Le(bytes + 16);
    uint32_t payloadBytes = ReadU32Le(bytes + 20);

    if (channelCount != 1u && channelCount != 2u) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (encoding != LA_ENCODING_PCM16 && encoding != LA_ENCODING_ADPCM)
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (sampleRate < 1000u || sampleRate > 384000u) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (frameCount == 0u || frameCount > LA_MAX_FRAMES) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (payloadBytes > sizeBytes - headerSize) return AUDIO_PACK_LOAD_INVALID_SOUND;

    uint32_t expectedPayload = 0u;
    if (encoding == LA_ENCODING_PCM16)
    {
        expectedPayload = frameCount * channelCount * (uint32_t)sizeof(int16_t);
        if (expectedPayload / channelCount / (uint32_t)sizeof(int16_t) != frameCount)
            return AUDIO_PACK_LOAD_INVALID_SOUND;
    }
    else if (!AdpcmPayloadBytes(frameCount, channelCount, &expectedPayload))
    {
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    }
    if (payloadBytes != expectedPayload) return AUDIO_PACK_LOAD_INVALID_SOUND;

    const uint8_t *payload = bytes + headerSize;
    // На little-endian выровненный PCM16-payload можно передать как int16
    // без промежуточной копии. Невыровненные данные идут через побайтовый
    // декодер ниже: простое приведение указателя нарушило бы требования C.
    if (encoding == LA_ENCODING_PCM16 && HostUsesLittleEndian() &&
        (uintptr_t)(const void *)payload % _Alignof(int16_t) == 0u)
    {
        outSound->samples = (int16_t *)(void *)payload;
        outSound->allocation = bytesOwned ? (void *)bytes : NULL;
        outSound->frameCount = frameCount;
        outSound->channelCount = channelCount;
        outSound->sampleRate = sampleRate;
        return AUDIO_PACK_LOAD_OK;
    }

    size_t sampleBytes = (size_t)frameCount * channelCount * sizeof(int16_t);
    int16_t *samples = PlatformAllocate(sampleBytes, false);
    if (samples == NULL) return AUDIO_PACK_LOAD_OUT_OF_MEMORY;

    if (encoding == LA_ENCODING_PCM16)
    {
        // Сэмплы в файле little-endian. На совпадающей по порядку байтов
        // машине раскладка int16 уже такая же, и перенос идёт одним memcpy;
        // на big-endian остаётся побайтовый путь, иначе формат зависел бы
        // от машины.
        if (HostUsesLittleEndian())
        {
            memcpy(samples, payload, sampleBytes);
        }
        else
        {
            for (uint32_t index = 0; index < frameCount * channelCount; ++index)
            {
                samples[index] = (int16_t)ReadU16Le(payload + (size_t)index * 2u);
            }
        }
    }
    else if (!AdpcmDecode(payload, payloadBytes, frameCount, channelCount, samples))
    {
        PlatformFree(samples);
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    }

    outSound->samples = samples;
    outSound->allocation = samples;
    outSound->frameCount = frameCount;
    outSound->channelCount = channelCount;
    outSound->sampleRate = sampleRate;
    return AUDIO_PACK_LOAD_OK;
}

static AudioClip *CreateClipFromDecoded(AudioDevice *device, DecodedSound *sound,
                                        AudioPackLoadStatus *outStatus)
{
    AudioClipDescription description = {
        .samples = sound->samples,
        .frameCount = sound->frameCount,
        .channelCount = sound->channelCount,
        .sampleRate = sound->sampleRate,
    };
    AudioClip *clip = NULL;
    const LaiueAudioServiceV1 *audio = PackAudio();
    AudioResult result = audio == NULL || audio->clipCreate == NULL
                             ? AUDIO_RESULT_INVALID_STATE
                             : (AudioResult)audio->clipCreate(device, &description, &clip);
    PlatformFree(sound->allocation);
    sound->allocation = NULL;
    sound->samples = NULL;

    if (result != AUDIO_RESULT_OK)
    {
        if (outStatus != NULL)
        {
            *outStatus = result == AUDIO_RESULT_OUT_OF_MEMORY ? AUDIO_PACK_LOAD_OUT_OF_MEMORY
                                                              : AUDIO_PACK_LOAD_INVALID_SOUND;
        }
        return NULL;
    }
    if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_OK;
    return clip;
}

// Разбор чужих контейнеров живёт в общей media_support: тот же код
// читает исходники в офлайн-инструменте, и набор форматов перечислен
// там, а не здесь. Движок принимает их затем, чтобы звук в паке можно
// было заменить, не собирая `.la`; готовый `.la` при этом остаётся
// быстрее — он уже в нужном виде.
static AudioPackLoadStatus DecodeForeign(const uint8_t *bytes, uint32_t sizeBytes,
                                         DecodedSound *outSound)
{
    SoundInfo info;
    if (SoundInspect(bytes, sizeBytes, &info) != SOUND_OK) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (info.frameCount == 0u || info.frameCount > LA_MAX_FRAMES)
        return AUDIO_PACK_LOAD_INVALID_SOUND;

    int16_t *samples = PlatformAllocate((size_t)info.sampleCount * sizeof(int16_t), false);
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    if (samples == NULL || (info.scratchBytes != 0u && scratch == NULL))
    {
        PlatformFree(samples);
        PlatformFree(scratch);
        return AUDIO_PACK_LOAD_OUT_OF_MEMORY;
    }

    SoundStatus status = SoundDecodeSamples(bytes, sizeBytes, &info, samples, info.sampleCount,
                                            scratch, info.scratchBytes);
    PlatformFree(scratch);
    if (status != SOUND_OK)
    {
        PlatformFree(samples);
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    }

    outSound->samples = samples;
    outSound->allocation = samples;
    outSound->frameCount = info.frameCount;
    outSound->channelCount = info.channelCount;
    outSound->sampleRate = info.sampleRate;
    return AUDIO_PACK_LOAD_OK;
}

AudioClip *AudioClipLoadMemory(AudioDevice *device, const void *bytes, uint32_t sizeBytes,
                               AudioPackLoadStatus *outStatus)
{
    if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    if (device == NULL || bytes == NULL)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_INVALID_SOUND;
        return NULL;
    }

    DecodedSound sound;
    memset(&sound, 0, sizeof(sound));
    // Формат определяется по сигнатуре, а не по расширению: приложение
    // вправе доставлять содержимое своим способом, и имени файла может
    // не быть вовсе.
    AudioPackLoadStatus status =
        SoundProbe(bytes, sizeBytes) != SOUND_FORMAT_UNKNOWN
            ? DecodeForeign((const uint8_t *)bytes, sizeBytes, &sound)
            : DecodeSound((const uint8_t *)bytes, sizeBytes, false, &sound);
    if (status != AUDIO_PACK_LOAD_OK)
    {
        if (outStatus != NULL) *outStatus = status;
        return NULL;
    }
    return CreateClipFromDecoded(device, &sound, outStatus);
}

AudioClip *AudioClipLoadFile(AudioDevice *device, const wchar_t *path,
                             AudioPackLoadStatus *outStatus)
{
    if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    if (device == NULL || path == NULL)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_INVALID_SOUND;
        return NULL;
    }

    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0u;
    if (!PlatformReadEntireFile(path, LA_MAX_FILE_BYTES, &fileBytes, &fileSize))
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_IO_ERROR;
        return NULL;
    }

    AudioClip *clip = AudioClipLoadMemory(device, fileBytes, (uint32_t)fileSize, outStatus);
    PlatformFree(fileBytes);
    return clip;
}

// === Каталог ===

static bool BuildActivePackChildPath(LaiueContentCatalog *catalog, const wchar_t *childName,
                                     wchar_t *destination, uint32_t capacity,
                                     AudioPackLoadStatus *outStatus)
{
    const LaiueContentServiceV1 *content = PackContent();
    if (content == NULL || content->getActivePack == NULL || content->buildPath == NULL)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_IO_ERROR;
        return false;
    }
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (content->getActivePack(catalog, LAIUE_CONTENT_SOUND_PACK, activeName,
                               LAIUE_CONTENT_NAME_CAPACITY) == 0u)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NO_ACTIVE_PACK;
        return false;
    }
    if (content->buildPath(catalog, LAIUE_CONTENT_SOUND_PACK, activeName, childName,
                           destination, capacity) == 0u)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_IO_ERROR;
        return false;
    }
    return true;
}

// Порядок по умолчанию: сначала исходники, свой `.la` последним. Он
// здесь запасной путь — играет, когда исходника нет, когда тот
// повреждён или когда его не удалось разобрать. Рядом с каждым
// найденным исходником движок держит собранный `stone.wav.la` и дальше
// берёт уже его.
//
// Порядок меняется файлом `sounds/formats.txt`. Поставив в нём `la`
// первым, приложение возвращается к прежнему поведению: играет готовый
// `stone.la`, и ничего рядом не создаётся.
static const wchar_t *const g_soundExtensions[] = {L".wav", L".mp3", L".la"};

static bool ExtensionIs(const wchar_t *extension, const wchar_t *expected)
{
    uint32_t index = 0u;
    while (extension[index] != 0 && extension[index] == expected[index]) ++index;
    return extension[index] == expected[index];
}

static bool SoundFileUsable(const wchar_t *path, PlatformPathInformation *outInformation)
{
    return PlatformGetPathInformation(path, outInformation) && outInformation->exists &&
           !outInformation->isDirectory && !outInformation->isSymbolicLink &&
           outInformation->size != 0u && outInformation->size <= LA_MAX_FILE_BYTES;
}

// `stone.wav` даёт `stone.wav.la`. Расширение исходника сохраняется
// целиком: видно, из чего собран кэш, и он не займёт место `stone.la`,
// положенного человеком.
static void BuildSoundCacheExtension(const wchar_t *extension, wchar_t *destination,
                                     uint32_t capacity)
{
    uint32_t length = 0u;
    while (extension[length] != 0 && length + 4u < capacity)
    {
        destination[length] = extension[length];
        ++length;
    }
    static const wchar_t suffix[] = L".la";
    for (uint32_t index = 0; suffix[index] != 0; ++index) destination[length++] = suffix[index];
    destination[length] = 0;
}

static AudioPackLoadStatus DecodeSoundFile(const wchar_t *path, DecodedSound *outSound)
{
    uint8_t *fileBytes = NULL;
    uint64_t fileSize = 0u;
    if (!PlatformReadEntireFile(path, LA_MAX_FILE_BYTES, &fileBytes, &fileSize))
    {
        return AUDIO_PACK_LOAD_IO_ERROR;
    }
    AudioPackLoadStatus status =
        SoundProbe(fileBytes, (uint32_t)fileSize) != SOUND_FORMAT_UNKNOWN
            ? DecodeForeign(fileBytes, (uint32_t)fileSize, outSound)
            : DecodeSound(fileBytes, (uint32_t)fileSize, true, outSound);
    // PCM16-вид передаёт владение файловым буфером разобранному звуку: он
    // возвращается после создания клипа. В остальных случаях буфер наш.
    if (outSound->allocation != (void *)fileBytes) PlatformFree(fileBytes);
    return status;
}

// Кладёт разобранный звук рядом с исходником. Неудача здесь не ошибка
// загрузки: каталог пака бывает доступен только на чтение, и тогда
// движок просто разберёт исходник заново в следующий раз.
//
// Пишется PCM16, а не ADPCM: кэш существует ради скорости загрузки, а
// не ради размера, и терять качество там, где его не просили, нельзя.
// Сжатие остаётся осознанным решением — `soundc --adpcm`.
static void WriteSoundCache(const wchar_t *path, const DecodedSound *sound,
                            uint64_t sourceModifiedTime, uint32_t sourceSizeBytes)
{
    uint32_t encodedBytes = 0u;
    if (SoundEncodedBytes(SOUND_ENCODING_PCM16, sound->frameCount, sound->channelCount,
                          &encodedBytes) != SOUND_OK)
    {
        return;
    }
    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL) return;
    SoundClip clip = {
        .samples = sound->samples,
        .frameCount = sound->frameCount,
        .channelCount = sound->channelCount,
        .sampleRate = sound->sampleRate,
        .encoding = SOUND_ENCODING_PCM16,
        .sourceModifiedTime = sourceModifiedTime,
        .sourceSizeBytes = sourceSizeBytes,
    };
    if (SoundEncode(&clip, encoded, encodedBytes, NULL) == SOUND_OK)
    {
        PlatformWriteFileAtomic(path, encoded, encodedBytes);
    }
    PlatformFree(encoded);
}

// Звук по умолчанию — тишина. Материал, которого нет в текстурпаке,
// показывается нейтральным слоем; звук, которого нет в звукопаке,
// звучит ничем. И там и там причина остаётся в статусе, а приложение
// получает пригодный объект и не обязано проверять указатель после
// каждой загрузки.
static AudioClip *CreateSilentClip(AudioDevice *device)
{
    static const int16_t silence[1] = {0};
    AudioClipDescription description = {
        .samples = silence,
        .frameCount = 1u,
        .channelCount = 1u,
        .sampleRate = 44100u,
    };
    AudioClip *clip = NULL;
    const LaiueAudioServiceV1 *audio = PackAudio();
    if (audio == NULL || audio->clipCreate == NULL) return NULL;
    return audio->clipCreate(device, &description, &clip) == AUDIO_RESULT_OK ? clip : NULL;
}

// Путь рассчитывается по строкам, полученным через provider API: обычный
// lookup не выделяет два максимальных 32K-символьных буфера.
static uint32_t SoundResourcePathCapacity(LaiueContentCatalog *catalog, const wchar_t *activeName,
                                          const wchar_t *soundName)
{
    const LaiueContentServiceV1 *content = PackContent();
    if (content == NULL || content->buildPath == NULL) return LAIUE_CONTENT_PATH_CAPACITY;
    wchar_t base[LAIUE_CONTENT_NAME_CAPACITY];
    if (content->buildPath(catalog, LAIUE_CONTENT_SOUND_PACK, activeName, NULL,
                           base, LAIUE_CONTENT_NAME_CAPACITY) == 0u)
        return LAIUE_CONTENT_PATH_CAPACITY;
    uint32_t baseLength = 0u;
    while (base[baseLength] != 0) ++baseLength;
    uint32_t soundLength = 0u;
    while (soundName[soundLength] != 0) ++soundLength;

    // Provider prefix + separator + resource + longest cache extension (.mp3.la) + NUL.
    uint64_t needed = (uint64_t)baseLength + soundLength + 9u;
    if (needed >= LAIUE_CONTENT_PATH_CAPACITY) return LAIUE_CONTENT_PATH_CAPACITY;
    return (uint32_t)needed;
}

AudioClip *AudioClipLoadFrom(AudioDevice *device, LaiueContentCatalog *catalog,
                             const wchar_t *soundName, AudioPackLoadStatus *outStatus)
{
    if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    const LaiueContentServiceV1 *content = PackContent();
    // Небезопасное имя — ошибка приложения, а не отсутствие содержимого:
    // здесь подставлять тишину значило бы прятать опечатку в коде.
    if (device == NULL || soundName == NULL || content == NULL || content->pathIsSafe == NULL ||
        content->pathIsSafe(soundName) == 0u)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_INVALID_SOUND;
        return NULL;
    }
    if (catalog == NULL && content->defaultCatalog != NULL)
        catalog = content->defaultCatalog();
    if (catalog == NULL)
    {
        AudioClip *silent = CreateSilentClip(device);
        if (outStatus != NULL)
            *outStatus = silent == NULL ? AUDIO_PACK_LOAD_OUT_OF_MEMORY
                                        : AUDIO_PACK_LOAD_NO_ACTIVE_PACK;
        return silent;
    }

    AudioPackLoadStatus status = AUDIO_PACK_LOAD_SOUND_NOT_FOUND;
    DecodedSound sound;
    memset(&sound, 0, sizeof(sound));
    bool decoded = false;
    uint64_t staleModifiedTime = 0u;
    uint32_t staleSizeBytes = 0u;
    wchar_t *pathScratch = NULL;
    wchar_t *path = NULL;
    wchar_t *cachePath = NULL;

    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (content->getActivePack == NULL ||
        content->getActivePack(catalog, LAIUE_CONTENT_SOUND_PACK, activeName,
                               LAIUE_CONTENT_NAME_CAPACITY) == 0u)
    {
        status = AUDIO_PACK_LOAD_NO_ACTIVE_PACK;
    }
    else
    {
        // Буферы путей берутся по фактической длине корня и имени, а не по
        // потолку пути: поиск платит байты, а не 128 КиБ. При отсутствии
        // активного пака не выделяется вовсе.
        uint32_t pathCapacity = SoundResourcePathCapacity(catalog, activeName, soundName);
        pathScratch = PlatformAllocate((size_t)pathCapacity * 2u * sizeof(wchar_t), false);
        if (pathScratch == NULL)
        {
            if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_OUT_OF_MEMORY;
            return NULL;
        }
        path = pathScratch;
        cachePath = pathScratch + pathCapacity;

        const wchar_t *order[LAIUE_CONTENT_FORMAT_ORDER_MAX];
        uint32_t orderCount = content->orderFormats == NULL
                                  ? 0u
                                  : content->orderFormats(
                                        catalog, LAIUE_CONTENT_SOUND_PACK, g_soundExtensions,
                                        sizeof(g_soundExtensions) / sizeof(g_soundExtensions[0]), order,
                                        LAIUE_CONTENT_FORMAT_ORDER_MAX);

        for (uint32_t index = 0; !decoded && index < orderCount; ++index)
        {
            const wchar_t *extension = order[index];
            if (content->buildResourcePath == NULL ||
                content->buildResourcePath(catalog, LAIUE_CONTENT_SOUND_PACK, activeName,
                                           soundName, extension, path, pathCapacity) == 0u)
            {
                status = AUDIO_PACK_LOAD_IO_ERROR;
                continue;
            }

            PlatformPathInformation source;
            bool hasSource = SoundFileUsable(path, &source);

            // Свой формат играется как есть: выводить его не из чего, и
            // кэш рядом с ним не появляется.
            if (ExtensionIs(extension, L".la"))
            {
                if (!hasSource) continue;
                status = DecodeSoundFile(path, &sound);
                decoded = status == AUDIO_PACK_LOAD_OK;
                if (!decoded)
                {
                    PlatformFree(sound.allocation);
                    memset(&sound, 0, sizeof(sound));
                }
                continue;
            }

            wchar_t cacheExtension[16];
            BuildSoundCacheExtension(extension, cacheExtension, 16u);
            if (content->buildResourcePath == NULL ||
                content->buildResourcePath(catalog, LAIUE_CONTENT_SOUND_PACK, activeName,
                                           soundName, cacheExtension, cachePath,
                                           pathCapacity) == 0u)
            {
                status = AUDIO_PACK_LOAD_IO_ERROR;
                continue;
            }

            PlatformPathInformation cache;
            bool hasCache = SoundFileUsable(cachePath, &cache);

            // Свежесть определяет отпечаток исходника, записанный в
            // кэш при сборке: размер и время изменения. Достаточно
            // одного обращения к каталогу, сам исходник не читается — в
            // этом и смысл кэша. Кэш переживает исчезновение исходника:
            // собранный звук остаётся, даже если WAV удалили.
            (void)cache;
            if (hasCache)
            {
                status = DecodeSoundFile(cachePath, &sound);
                bool fresh = status == AUDIO_PACK_LOAD_OK &&
                             (!hasSource || (sound.sourceSizeBytes == (uint32_t)source.size &&
                                             sound.sourceModifiedTime == source.modifiedTime));
                if (fresh)
                {
                    decoded = true;
                    break;
                }
                PlatformFree(sound.allocation);
                memset(&sound, 0, sizeof(sound));
            }
            if (hasSource)
            {
                status = DecodeSoundFile(path, &sound);
                if (status == AUDIO_PACK_LOAD_OK)
                {
                    decoded = true;
                    staleModifiedTime = source.modifiedTime;
                    staleSizeBytes = (uint32_t)source.size;
                    break;
                }
                // Исходник не разобрался — очередь следующего формата, а
                // за ними своего `.la`. Он для того и последний.
                PlatformFree(sound.allocation);
                memset(&sound, 0, sizeof(sound));
            }
        }
    }

    AudioClip *clip = NULL;
    if (decoded)
    {
        if (staleSizeBytes != 0u)
        {
            WriteSoundCache(cachePath, &sound, staleModifiedTime, staleSizeBytes);
        }
        clip = CreateClipFromDecoded(device, &sound, &status);
    }
    else
    {
        clip = CreateSilentClip(device);
        if (clip == NULL) status = AUDIO_PACK_LOAD_OUT_OF_MEMORY;
    }

    PlatformFree(pathScratch);
    if (outStatus != NULL) *outStatus = status;
    return clip;
}

// Отбрасывает расширение: наружу пак отдаёт имена звуков, а не имена
// файлов, поэтому приложение просит звук так же, как назвало его.
// У кэша расширений два — `stone.wav.la`, — и снимаются оба: иначе он
// попал бы в список отдельным звуком с именем `stone.wav`.
static void StripOneSoundExtension(wchar_t *name)
{
    uint32_t length = 0u;
    while (name[length] != 0) ++length;
    for (uint32_t index = 0; index < sizeof(g_soundExtensions) / sizeof(g_soundExtensions[0]);
         ++index)
    {
        const wchar_t *extension = g_soundExtensions[index];
        uint32_t extensionLength = 0u;
        while (extension[extensionLength] != 0) ++extensionLength;
        if (length <= extensionLength) continue;

        uint32_t offset = length - extensionLength;
        bool matches = true;
        for (uint32_t position = 0; position < extensionLength && matches; ++position)
        {
            wchar_t left = name[offset + position];
            if (left >= L'A' && left <= L'Z') left = (wchar_t)(left + (L'a' - L'A'));
            matches = left == extension[position];
        }
        if (matches)
        {
            name[offset] = 0;
            return;
        }
    }
}

static void StripSoundExtension(wchar_t *name)
{
    for (uint32_t pass = 0; pass < 2u; ++pass) StripOneSoundExtension(name);
}

static bool CopyContentListInto(const LaiueContentList *source, AudioPackList *outList,
                                bool stripExtension)
{
    outList->entries = NULL;
    outList->count = 0u;
    if (source->count == 0u) return true;

    outList->entries = PlatformAllocate((size_t)source->count * sizeof(AudioPackEntry), false);
    if (outList->entries == NULL) return false;

    for (uint32_t index = 0; index < source->count; ++index)
    {
        memcpy(outList->entries[index].name, source->entries[index].name,
               sizeof(outList->entries[index].name));
        outList->entries[index].active = source->entries[index].active;
        if (stripExtension) StripSoundExtension(outList->entries[index].name);
    }
    outList->count = source->count;
    return true;
}

bool AudioPackEnumerateFrom(LaiueContentCatalog *catalog, AudioPackList *outList)
{
    const LaiueContentServiceV1 *content = PackContent();
    if (catalog == NULL || outList == NULL || content == NULL || content->enumerate == NULL ||
        content->releaseList == NULL)
        return false;

    LaiueContentList contentList;
    if (content->enumerate(catalog, LAIUE_CONTENT_SOUND_PACK, &contentList) == 0u)
        return false;

    bool copied = CopyContentListInto(&contentList, outList, false);
    content->releaseList(&contentList);
    return copied;
}

typedef struct DirectoryScratch
{
    PlatformDirectoryIterator iterator;
    PlatformDirectoryEntry entry;
} DirectoryScratch;

// Обход пака вглубь: имя звука — это путь от корня пака, поэтому
// подпапки обязаны попадать в список. Иначе приложение видело бы только
// верхний уровень и решило бы, что остальных звуков нет.
typedef struct SoundScan
{
    AudioPackEntry *entries;   // NULL на первом проходе: тогда идёт счёт
    uint32_t capacity;
    uint32_t count;
} SoundScan;

static bool AppendSegment(wchar_t *destination, uint32_t capacity, const wchar_t *prefix,
                          const wchar_t *name)
{
    uint32_t length = 0u;
    while (prefix[length] != 0)
    {
        if (length + 1u >= capacity) return false;
        destination[length] = prefix[length];
        ++length;
    }
    if (length != 0u)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = L'/';
    }
    for (uint32_t index = 0; name[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = name[index];
    }
    destination[length] = 0;
    return true;
}

// Файл звука — свой `.la` либо исходный `.wav`. Перечисление обязано
// показывать оба под одним именем: приложение просит звук по имени и не
// должно знать, в каком виде он лежит.
static bool HasSoundExtension(const wchar_t *name)
{
    uint32_t length = 0u;
    while (name[length] != 0) ++length;
    for (uint32_t index = 0; index < sizeof(g_soundExtensions) / sizeof(g_soundExtensions[0]);
         ++index)
    {
        const wchar_t *extension = g_soundExtensions[index];
        uint32_t extensionLength = 0u;
        while (extension[extensionLength] != 0) ++extensionLength;
        if (length <= extensionLength) continue;

        bool matches = true;
        for (uint32_t position = 0; position < extensionLength && matches; ++position)
        {
            wchar_t left = name[length - extensionLength + position];
            wchar_t right = extension[position];
            if (left >= L'A' && left <= L'Z') left = (wchar_t)(left + (L'a' - L'A'));
            matches = left == right;
        }
        if (matches) return true;
    }
    return false;
}

static void StripSoundExtension(wchar_t *name);

static void ScanSounds(const wchar_t *directory, const wchar_t *prefix, uint32_t depth,
                       SoundScan *scan)
{
    if (depth >= LAIUE_CONTENT_PATH_SEGMENT_MAX) return;

    // Итератор и запись каталога вместе занимают больше страницы, а в
    // сборке без CRT кадр стека обязан оставаться меньше: за границей
    // 4 КиБ компилятор вставляет вызов __chkstk, которого там нет.
    // Поэтому каждый уровень обхода берёт своё состояние из кучи.
    DirectoryScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    wchar_t *childDirectory =
        PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    wchar_t *childName =
        PlatformAllocate((size_t)LAIUE_CONTENT_NAME_CAPACITY * sizeof(wchar_t), false);
    if (scratch == NULL || childDirectory == NULL || childName == NULL ||
        !PlatformDirectoryOpen(&scratch->iterator, directory))
    {
        PlatformFree(scratch);
        PlatformFree(childDirectory);
        PlatformFree(childName);
        return;
    }

    while (PlatformDirectoryNext(&scratch->iterator, &scratch->entry))
    {
        if (scratch->entry.isSymbolicLink) continue;
        const LaiueContentServiceV1 *content = PackContent();
        if (content == NULL || content->nameIsSafe == NULL ||
            content->nameIsSafe(scratch->entry.name) == 0u)
            continue;

        if (scratch->entry.isDirectory)
        {
            if (AppendSegment(childName, LAIUE_CONTENT_NAME_CAPACITY, prefix,
                              scratch->entry.name) &&
                AppendSegment(childDirectory, LAIUE_CONTENT_PATH_CAPACITY, directory,
                              scratch->entry.name))
            {
                ScanSounds(childDirectory, childName, depth + 1u, scan);
            }
            continue;
        }

        if (!HasSoundExtension(scratch->entry.name)) continue;
        if (!AppendSegment(childName, LAIUE_CONTENT_NAME_CAPACITY, prefix, scratch->entry.name))
        {
            continue;
        }
        StripSoundExtension(childName);

        if (scan->entries != NULL)
        {
            if (scan->count >= scan->capacity) break;
            // Один и тот же звук может лежать и как `.la`, и как `.wav`:
            // в списке он обязан быть один раз, под своим именем.
            bool duplicate = false;
            for (uint32_t index = 0; index < scan->count && !duplicate; ++index)
            {
                const wchar_t *existing = scan->entries[index].name;
                uint32_t position = 0u;
                while (existing[position] != 0 && existing[position] == childName[position])
                {
                    ++position;
                }
                duplicate = existing[position] == 0 && childName[position] == 0;
            }
            if (duplicate) continue;

            for (uint32_t index = 0; index < AUDIO_PACK_NAME_MAX; ++index)
            {
                scan->entries[scan->count].name[index] = childName[index];
                if (childName[index] == 0) break;
            }
            scan->entries[scan->count].active = false;
        }
        ++scan->count;
    }

    PlatformDirectoryClose(&scratch->iterator);
    PlatformFree(scratch);
    PlatformFree(childDirectory);
    PlatformFree(childName);
}

bool AudioPackEnumerateSoundsFrom(LaiueContentCatalog *catalog, AudioPackList *outList)
{
    const LaiueContentServiceV1 *content = PackContent();
    if (outList == NULL) return false;
    outList->entries = NULL;
    outList->count = 0u;
    if (content == NULL) return false;
    if (catalog == NULL)
    {
        if (content->defaultCatalog == NULL) return false;
        catalog = content->defaultCatalog();
    }

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL) return false;
    if (!BuildActivePackChildPath(catalog, NULL, path, LAIUE_CONTENT_PATH_CAPACITY, NULL))
    {
        PlatformFree(path);
        // Активного пака нет — это пустой список, а не ошибка.
        return true;
    }

    // Два прохода: сначала счёт, потом заполнение. Так список выделяется
    // ровно один раз и не растёт перевыделениями во время обхода.
    SoundScan scan;
    scan.entries = NULL;
    scan.capacity = 0u;
    scan.count = 0u;
    ScanSounds(path, L"", 0u, &scan);

    if (scan.count == 0u)
    {
        PlatformFree(path);
        return true;
    }

    AudioPackEntry *entries = PlatformAllocate((size_t)scan.count * sizeof(AudioPackEntry), true);
    if (entries == NULL)
    {
        PlatformFree(path);
        return false;
    }

    SoundScan fill;
    fill.entries = entries;
    fill.capacity = scan.count;
    fill.count = 0u;
    ScanSounds(path, L"", 0u, &fill);

    PlatformFree(path);
    outList->entries = entries;
    outList->count = fill.count;
    return true;
}

bool AudioPackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name)
{
    const LaiueContentServiceV1 *content = PackContent();
    if (catalog == NULL || content == NULL || content->setActivePack == NULL) return false;
    return content->setActivePack(catalog, LAIUE_CONTENT_SOUND_PACK, name) != 0u;
}

void AudioPackListRelease(AudioPackList *list)
{
    if (list == NULL) return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0u;
}
