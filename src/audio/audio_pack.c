// Формат `.la` и звукопаки `.lap`. Загрузчик принимает только готовый
// бинарный payload: разбор WAV, MP3 и прочих контейнеров в рантайме не
// выполняется — движок читает свой формат и ничего не декодирует сверх
// него, ровно как текстурпак не декодирует PNG.

#include "audio/audio_pack.h"
#include "content/content_catalog.h"
#include "platform/system.h"

#include <string.h>

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

        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            uint8_t packed = cursor[frame / 2u];
            uint32_t nibble = (frame & 1u) != 0u ? (uint32_t)(packed >> 4) : (uint32_t)(packed & 15u);
            outSamples[frame * channelCount + channel] = AdpcmDecodeNibble(&state, nibble);
        }
        cursor += AdpcmChannelBytes(frameCount);
    }
    return true;
}

// === Разбор `.la` ===

typedef struct DecodedSound
{
    int16_t *samples;
    uint32_t frameCount;
    uint32_t channelCount;
    uint32_t sampleRate;
} DecodedSound;

static AudioPackLoadStatus DecodeSound(const uint8_t *bytes, uint32_t sizeBytes,
                                       DecodedSound *outSound)
{
    if (bytes == NULL || sizeBytes < LA_HEADER_SIZE) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (ReadU32Le(bytes) != LA_MAGIC) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (ReadU16Le(bytes + 4) != LA_VERSION) return AUDIO_PACK_LOAD_INVALID_SOUND;
    if (ReadU16Le(bytes + 6) != LA_HEADER_SIZE) return AUDIO_PACK_LOAD_INVALID_SOUND;

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
    if (payloadBytes > sizeBytes - LA_HEADER_SIZE) return AUDIO_PACK_LOAD_INVALID_SOUND;

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

    size_t sampleBytes = (size_t)frameCount * channelCount * sizeof(int16_t);
    int16_t *samples = PlatformAllocate(sampleBytes, false);
    if (samples == NULL) return AUDIO_PACK_LOAD_OUT_OF_MEMORY;

    const uint8_t *payload = bytes + LA_HEADER_SIZE;
    if (encoding == LA_ENCODING_PCM16)
    {
        // Сэмплы в файле little-endian; собираются побайтово, чтобы
        // формат не зависел от порядка байтов машины.
        for (uint32_t index = 0; index < frameCount * channelCount; ++index)
        {
            samples[index] = (int16_t)ReadU16Le(payload + (size_t)index * 2u);
        }
    }
    else if (!AdpcmDecode(payload, payloadBytes, frameCount, channelCount, samples))
    {
        PlatformFree(samples);
        return AUDIO_PACK_LOAD_INVALID_SOUND;
    }

    outSound->samples = samples;
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
    AudioResult result = AudioClipCreate(device, &description, &clip);
    PlatformFree(sound->samples);
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
    AudioPackLoadStatus status = DecodeSound((const uint8_t *)bytes, sizeBytes, &sound);
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
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (!LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_SOUND_PACK, activeName,
                                          LAIUE_CONTENT_NAME_CAPACITY))
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NO_ACTIVE_PACK;
        return false;
    }
    if (!LaiueContentCatalogBuildPath(catalog, LAIUE_CONTENT_SOUND_PACK, activeName, childName,
                                      destination, capacity))
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_IO_ERROR;
        return false;
    }
    return true;
}

// Имя звука превращается в имя файла добавлением расширения. Само имя
// проверяется как отдельная сущность содержимого, поэтому выйти за
// пределы пака через `..` или разделитель каталогов нельзя.
static bool BuildSoundFileName(const wchar_t *soundName, wchar_t *destination, uint32_t capacity)
{
    if (soundName == NULL || !LaiueContentNameIsSafe(soundName)) return false;

    uint32_t length = 0u;
    while (soundName[length] != L'\0') ++length;
    const wchar_t *extension = LaiueContentFormatGet(LAIUE_CONTENT_SOUND)->extension;
    uint32_t extensionLength = 0u;
    while (extension[extensionLength] != L'\0') ++extensionLength;
    if (length + extensionLength + 1u > capacity) return false;

    memcpy(destination, soundName, (size_t)length * sizeof(wchar_t));
    memcpy(destination + length, extension, (size_t)extensionLength * sizeof(wchar_t));
    destination[length + extensionLength] = L'\0';
    return true;
}

AudioClip *AudioClipLoadFrom(AudioDevice *device, LaiueContentCatalog *catalog,
                             const wchar_t *soundName, AudioPackLoadStatus *outStatus)
{
    if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_NOT_ATTEMPTED;
    if (device == NULL || soundName == NULL)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_INVALID_SOUND;
        return NULL;
    }
    if (catalog == NULL) catalog = LaiueContentCatalogDefault();

    wchar_t fileName[LAIUE_CONTENT_NAME_CAPACITY];
    if (!BuildSoundFileName(soundName, fileName, LAIUE_CONTENT_NAME_CAPACITY))
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_INVALID_SOUND;
        return NULL;
    }

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL)
    {
        if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_OUT_OF_MEMORY;
        return NULL;
    }

    AudioClip *clip = NULL;
    if (BuildActivePackChildPath(catalog, fileName, path, LAIUE_CONTENT_PATH_CAPACITY, outStatus))
    {
        if (!PlatformPathExists(path))
        {
            if (outStatus != NULL) *outStatus = AUDIO_PACK_LOAD_SOUND_NOT_FOUND;
        }
        else
        {
            clip = AudioClipLoadFile(device, path, outStatus);
        }
    }
    PlatformFree(path);
    return clip;
}

// Отбрасывает расширение `.la`: наружу пак отдаёт имена звуков, а не
// имена файлов, поэтому приложение просит звук так же, как назвало его.
static void StripSoundExtension(wchar_t *name)
{
    uint32_t length = 0u;
    while (name[length] != L'\0') ++length;
    const wchar_t *extension = LaiueContentFormatGet(LAIUE_CONTENT_SOUND)->extension;
    uint32_t extensionLength = 0u;
    while (extension[extensionLength] != L'\0') ++extensionLength;
    if (length <= extensionLength) return;

    uint32_t offset = length - extensionLength;
    for (uint32_t index = 0; index < extensionLength; ++index)
    {
        if (name[offset + index] != extension[index]) return;
    }
    name[offset] = L'\0';
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
    if (catalog == NULL || outList == NULL) return false;

    LaiueContentList contentList;
    if (!LaiueContentCatalogEnumerate(catalog, LAIUE_CONTENT_SOUND_PACK, &contentList))
        return false;

    bool copied = CopyContentListInto(&contentList, outList, false);
    LaiueContentListRelease(&contentList);
    return copied;
}

typedef struct DirectoryScratch
{
    PlatformDirectoryIterator iterator;
    PlatformDirectoryEntry entry;
} DirectoryScratch;

bool AudioPackEnumerateSoundsFrom(LaiueContentCatalog *catalog, AudioPackList *outList)
{
    if (outList == NULL) return false;
    outList->entries = NULL;
    outList->count = 0u;
    if (catalog == NULL) catalog = LaiueContentCatalogDefault();

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (path == NULL) return false;
    if (!BuildActivePackChildPath(catalog, NULL, path, LAIUE_CONTENT_PATH_CAPACITY, NULL))
    {
        PlatformFree(path);
        // Активного пака нет — это пустой список, а не ошибка.
        return true;
    }

    // Итератор и запись каталога вместе занимают больше страницы, а в
    // сборке без CRT кадр стека обязан оставаться меньше: за границей
    // 4 КиБ компилятор вставляет вызов __chkstk, которого там нет.
    DirectoryScratch *scratch = PlatformAllocate(sizeof(*scratch), false);
    if (scratch == NULL)
    {
        PlatformFree(path);
        return false;
    }

    if (!PlatformDirectoryOpen(&scratch->iterator, path))
    {
        PlatformFree(scratch);
        PlatformFree(path);
        return true;
    }

    // Два прохода: сначала счёт, потом заполнение. Так список выделяется
    // ровно один раз и не растёт перевыделениями во время обхода.
    uint32_t soundCount = 0u;
    while (PlatformDirectoryNext(&scratch->iterator, &scratch->entry))
    {
        if (scratch->entry.isDirectory || scratch->entry.isSymbolicLink) continue;
        if (LaiueContentNameMatches(LAIUE_CONTENT_SOUND, scratch->entry.name)) ++soundCount;
    }
    PlatformDirectoryClose(&scratch->iterator);

    if (soundCount == 0u)
    {
        PlatformFree(scratch);
        PlatformFree(path);
        return true;
    }

    outList->entries = PlatformAllocate((size_t)soundCount * sizeof(AudioPackEntry), true);
    if (outList->entries == NULL)
    {
        PlatformFree(scratch);
        PlatformFree(path);
        return false;
    }

    if (!PlatformDirectoryOpen(&scratch->iterator, path))
    {
        PlatformFree(outList->entries);
        outList->entries = NULL;
        PlatformFree(scratch);
        PlatformFree(path);
        return false;
    }
    uint32_t written = 0u;
    while (written < soundCount && PlatformDirectoryNext(&scratch->iterator, &scratch->entry))
    {
        if (scratch->entry.isDirectory || scratch->entry.isSymbolicLink) continue;
        if (!LaiueContentNameMatches(LAIUE_CONTENT_SOUND, scratch->entry.name)) continue;

        memcpy(outList->entries[written].name, scratch->entry.name,
               sizeof(outList->entries[written].name));
        outList->entries[written].name[AUDIO_PACK_NAME_MAX - 1u] = (wchar_t)0;
        StripSoundExtension(outList->entries[written].name);
        outList->entries[written].active = false;
        ++written;
    }
    PlatformDirectoryClose(&scratch->iterator);
    outList->count = written;

    PlatformFree(scratch);
    PlatformFree(path);
    return true;
}

bool AudioPackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name)
{
    if (catalog == NULL) return false;
    return LaiueContentCatalogSetActivePack(catalog, LAIUE_CONTENT_SOUND_PACK, name);
}

void AudioPackListRelease(AudioPackList *list)
{
    if (list == NULL) return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0u;
}

bool AudioPackEnumerate(AudioPackList *outList)
{
    return AudioPackEnumerateFrom(LaiueContentCatalogDefault(), outList);
}

bool AudioPackActivate(const wchar_t *name)
{
    return AudioPackActivateIn(LaiueContentCatalogDefault(), name);
}
