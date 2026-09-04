// Сборка текстурного массива из каталога `.ltp`.
//
// Пак — папка, а не один файл: имена материалов задаёт приложение, файлы
// внутри вправе лежать по подпапкам, и заменить одну текстуру значит
// положить рядом файл с тем же именем. Читается и свой `.lt`, и обычные
// PNG и GIF: декодеры собственные, без сторонних зависимостей, и лежат
// во внутренней media_support, которой пользуется и офлайн-инструмент.
//
// Анимация приходит из GIF или из заголовка `.lt` и попадает в то же
// расписание, что и раньше: материал занимает по слою на кадр.

#include "render/texture_pack_internal.h"

#include "content/content_catalog.h"
#include "media/image.h"
#include "media/lt_encode.h"
#include "platform/system.h"

#include <string.h>

#define LT_MAGIC 0x3153544Cu   // L, T, S, 1 little-endian
#define LT_FORMAT_RGBA8 1u
#define LT_FORMAT_RGBA8_NORMALS 2u

#define TEXTURE_MAX_DIMENSION 4096u
#define TEXTURE_MAX_FRAMES 256u

// Нейтральный слой для материала, которого в паке нет. Молча показать
// чужую текстуру было бы хуже: серый квадрат сразу виден.
static const uint8_t g_missingTexel[4] = {160u, 160u, 160u, 255u};
static const uint8_t g_flatNormalTexel[4] = {128u, 128u, 255u, 255u};

typedef struct MaterialSource
{
    uint8_t *albedo;    // frameCount кадров width*height*4
    uint8_t *normal;    // NULL, если карты нормалей нет
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    // Длительность каждого кадра: в GIF они вправе различаться.
    uint16_t frameMilliseconds[IMAGE_MAX_FRAMES];
    // Отпечаток исходника, записанный в `.lt`. Нулевой размер означает,
    // что файл ни из чего не выведен.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
    bool found;
} MaterialSource;

static uint16_t ReadU16Le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32Le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool IsPowerOfTwo(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static uint32_t RoundUpToPowerOfTwo(uint32_t value)
{
    uint32_t result = 1u;
    while (result < value && result < TEXTURE_MAX_DIMENSION) result <<= 1;
    return result;
}

static uint32_t FullMipCount(uint32_t size)
{
    uint32_t count = 1u;
    while (size > 1u)
    {
        size >>= 1;
        ++count;
    }
    return count;
}

static uint32_t MipChainBytes(uint32_t size)
{
    uint32_t total = 0u;
    for (uint32_t level = size;; level >>= 1)
    {
        total += level * level * 4u;
        if (level == 1u) break;
    }
    return total;
}

static void ReleaseSource(MaterialSource *source)
{
    PlatformFree(source->albedo);
    PlatformFree(source->normal);
    source->albedo = NULL;
    source->normal = NULL;
}

// === Свой формат одной текстуры ===

static bool ParseSingleTexture(const uint8_t *file, uint32_t fileBytes, MaterialSource *outSource,
                               bool wantNormals)
{
    if (fileBytes < LT_HEADER_BYTES_V1) return false;
    if (ReadU32Le(file) != LT_MAGIC) return false;

    uint32_t version = ReadU16Le(file + 4);
    uint32_t headerSize = ReadU16Le(file + 6);
    // Версия 1 несла одну длительность на всю анимацию и не знала об
    // отпечатке. Она читается по-прежнему: файл, собранный прежней
    // сборкой, не обязан устаревать вместе с форматом.
    if (version == 1u)
    {
        if (headerSize != LT_HEADER_BYTES_V1) return false;
    }
    else if (version == LT_VERSION)
    {
        if (headerSize != LT_HEADER_BYTES) return false;
    }
    else
    {
        return false;
    }
    if (fileBytes < headerSize) return false;

    uint32_t width = ReadU16Le(file + 8);
    uint32_t height = ReadU16Le(file + 10);
    uint32_t frameCount = ReadU16Le(file + 12);
    uint32_t frameMilliseconds = ReadU16Le(file + 14);
    uint32_t format = ReadU16Le(file + 16);
    uint32_t reserved = ReadU16Le(file + 18);
    uint32_t payloadBytes = ReadU32Le(file + 20);

    bool withNormals = format == LT_FORMAT_RGBA8_NORMALS;
    if (!withNormals && format != LT_FORMAT_RGBA8) return false;
    if (reserved != 0u) return false;
    if (width == 0u || height == 0u) return false;
    if (width > TEXTURE_MAX_DIMENSION || height > TEXTURE_MAX_DIMENSION) return false;
    if (frameCount == 0u || frameCount > TEXTURE_MAX_FRAMES) return false;
    if (frameCount > 1u && frameMilliseconds == 0u) return false;

    if (version != 1u)
    {
        outSource->sourceModifiedTime =
            (uint64_t)ReadU32Le(file + 24) | ((uint64_t)ReadU32Le(file + 28) << 32);
        outSource->sourceSizeBytes = ReadU32Le(file + 32);
    }

    uint64_t frameBytes = (uint64_t)width * height * 4u;
    uint64_t albedoBytes = frameBytes * frameCount;
    uint64_t expected = withNormals ? albedoBytes * 2u : albedoBytes;
    if (expected > 0xFFFFFFFFull) return false;
    if (payloadBytes != (uint32_t)expected) return false;

    uint32_t tableBytes = version == 1u ? 0u : frameCount * 2u;
    if ((uint64_t)fileBytes != (uint64_t)headerSize + tableBytes + payloadBytes) return false;

    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        uint32_t duration =
            version == 1u ? frameMilliseconds : ReadU16Le(file + headerSize + frame * 2u);
        if (frameCount > 1u && duration == 0u) return false;
        outSource->frameMilliseconds[frame] = (uint16_t)(frameCount > 1u ? duration : 0u);
    }

    const uint8_t *payload = file + headerSize + tableBytes;
    uint8_t *albedo = PlatformAllocate((size_t)albedoBytes, false);
    if (albedo == NULL) return false;
    memcpy(albedo, payload, (size_t)albedoBytes);

    uint8_t *normal = NULL;
    if (withNormals && wantNormals)
    {
        normal = PlatformAllocate((size_t)albedoBytes, false);
        if (normal == NULL)
        {
            PlatformFree(albedo);
            return false;
        }
        memcpy(normal, payload + albedoBytes, (size_t)albedoBytes);
    }

    outSource->albedo = albedo;
    outSource->normal = normal;
    outSource->width = width;
    outSource->height = height;
    outSource->frameCount = frameCount;
    outSource->found = true;
    return true;
}

// === Обычные изображения ===

static bool DecodeImageFile(const uint8_t *file, uint32_t fileBytes, MaterialSource *outSource)
{
    ImageInfo info = {0};
    ImageStatus status = ImageInspect(file, fileBytes, &info);
    if (status != IMAGE_OK) return false;
    if (info.frameCount > TEXTURE_MAX_FRAMES) return false;

    uint8_t *pixels = PlatformAllocate(info.pixelBytes, false);
    void *scratch = info.scratchBytes != 0u ? PlatformAllocate(info.scratchBytes, false) : NULL;
    if (pixels == NULL || (info.scratchBytes != 0u && scratch == NULL))
    {
        PlatformFree(pixels);
        PlatformFree(scratch);
        return false;
    }

    status = ImageDecode(file, fileBytes, &info, pixels, info.pixelBytes, scratch,
                         info.scratchBytes);
    PlatformFree(scratch);
    if (status != IMAGE_OK)
    {
        PlatformFree(pixels);
        return false;
    }

    outSource->albedo = pixels;
    outSource->width = info.width;
    outSource->height = info.height;
    outSource->frameCount = info.frameCount;
    for (uint32_t frame = 0; frame < info.frameCount && frame < IMAGE_MAX_FRAMES; ++frame)
    {
        outSource->frameMilliseconds[frame] = info.frameMilliseconds[frame];
    }
    outSource->found = true;
    return true;
}

typedef struct LoadScratch
{
    wchar_t path[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t cachePath[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t resource[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t packName[LAIUE_CONTENT_NAME_CAPACITY];
} LoadScratch;

// Порядок по умолчанию: сначала исходники, свой `.lt` последним. Он
// здесь запасной путь — берётся, когда исходника нет, когда тот
// повреждён или когда его не удалось разобрать. Рядом с каждым
// найденным исходником движок держит разобранный `stone.png.lt` и
// дальше берёт уже его: декодировать PNG на каждом запуске незачем.
//
// Порядок меняется файлом `textures/formats.txt`. Поставив в нём `lt`
// первым, приложение возвращается к прежнему поведению: берётся готовый
// `stone.lt`, и ничего рядом не создаётся.
static const wchar_t *const g_textureExtensions[] = {L".png", L".gif", L".jpg", L".jpeg", L".lt"};

static bool ExtensionIs(const wchar_t *extension, const wchar_t *expected)
{
    uint32_t index = 0u;
    while (extension[index] != 0 && extension[index] == expected[index]) ++index;
    return extension[index] == expected[index];
}

#define TEXTURE_MAX_FILE_BYTES 0x10000000u

typedef struct ResourceRead
{
    uint8_t *bytes;
    uint64_t size;
    // Байты уже в своём формате: разбирать их дешевле.
    bool isSingleTexture;
    // Байты пришли из чужого формата, и рядом надо положить `.lt`.
    bool cacheStale;
} ResourceRead;

static bool UsableFile(const wchar_t *path, PlatformPathInformation *outInformation)
{
    return PlatformGetPathInformation(path, outInformation) && outInformation->exists &&
           !outInformation->isDirectory && !outInformation->isSymbolicLink &&
           outInformation->size != 0u && outInformation->size <= TEXTURE_MAX_FILE_BYTES;
}

static bool BuildPath(LaiueContentCatalog *catalog, LoadScratch *scratch,
                      const wchar_t *resourcePath, const wchar_t *extension, wchar_t *destination)
{
    return LaiueContentCatalogBuildResourcePath(catalog, LAIUE_CONTENT_TEXTURE_PACK,
                                                scratch->packName, resourcePath, extension,
                                                destination, LAIUE_CONTENT_PATH_CAPACITY);
}

// `stone.png` даёт `stone.png.lt`. Имя кэша сохраняет расширение
// исходника целиком: так видно, из чего он собран, и он никогда не
// займёт место `stone.lt`, положенного человеком.
static void BuildCacheExtension(const wchar_t *extension, wchar_t *destination, uint32_t capacity)
{
    uint32_t length = 0u;
    while (extension[length] != 0 && length + 4u < capacity)
    {
        destination[length] = extension[length];
        ++length;
    }
    static const wchar_t suffix[] = L".lt";
    for (uint32_t index = 0; suffix[index] != 0; ++index) destination[length++] = suffix[index];
    destination[length] = 0;
}

// Кладёт разобранную текстуру рядом с исходником. Неудача здесь не
// ошибка загрузки: каталог пака бывает доступен только на чтение, и
// тогда движок просто разберёт исходник заново в следующий раз.
static void WriteTextureCache(const wchar_t *path, const MaterialSource *source,
                              uint64_t sourceModifiedTime, uint32_t sourceSizeBytes)
{
    uint32_t encodedBytes = 0u;
    if (LtEncodedBytes(source->width, source->height, source->frameCount, source->normal != NULL,
                       &encodedBytes) != LT_OK)
    {
        return;
    }
    uint8_t *encoded = PlatformAllocate(encodedBytes, false);
    if (encoded == NULL) return;
    LtTexture texture = {
        .albedoFrames = source->albedo,
        .normalFrames = source->normal,
        .width = source->width,
        .height = source->height,
        .frameCount = source->frameCount,
        .frameMilliseconds = source->frameMilliseconds,
        .sourceModifiedTime = sourceModifiedTime,
        .sourceSizeBytes = sourceSizeBytes,
    };
    if (LtEncode(&texture, encoded, encodedBytes, NULL) == LT_OK)
    {
        // Запись атомарна: прерванный запуск не оставит рядом с
        // текстурой полуфайл, который следующий примет за кэш.
        PlatformWriteFileAtomic(path, encoded, encodedBytes);
    }
    PlatformFree(encoded);
}

// Форматы перебираются в порядке приоритета, и на каждом шагу это либо
// свой `.lt` (берётся как есть), либо исходник со своим кэшем.
//
// Свежесть кэша определяет отпечаток исходника, записанный в него при
// сборке: размер и время изменения. Достаточно одного обращения к
// каталогу, сам исходник не читается — в этом и смысл кэша. Сравнение
// «кэш новее исходника» было бы дешевле на один `u64`, но не заметило
// бы отката файла из старой копии.
//
// Кэш переживает исчезновение исходника: собранная текстура остаётся,
// даже если PNG удалили. И наоборот, кэш, который не разобрался, не
// заслоняет живой исходник — он просто перечитывается заново.
static void LoadResource(LaiueContentCatalog *catalog, LoadScratch *scratch,
                         const wchar_t *resourcePath, bool wantNormals, MaterialSource *outSource)
{
    memset(outSource, 0, sizeof(*outSource));

    uint8_t *bytes = NULL;
    uint64_t size = 0u;

    const wchar_t *order[LAIUE_CONTENT_FORMAT_ORDER_MAX];
    uint32_t orderCount = LaiueContentCatalogOrderFormats(
        catalog, LAIUE_CONTENT_TEXTURE_PACK, g_textureExtensions,
        sizeof(g_textureExtensions) / sizeof(g_textureExtensions[0]), order,
        LAIUE_CONTENT_FORMAT_ORDER_MAX);

    for (uint32_t index = 0; index < orderCount; ++index)
    {
        const wchar_t *extension = order[index];
        if (!BuildPath(catalog, scratch, resourcePath, extension, scratch->path)) continue;

        PlatformPathInformation source;
        bool hasSource = UsableFile(scratch->path, &source);

        // Свой формат берётся как есть: выводить его не из чего, и кэш
        // рядом с ним не появляется.
        if (ExtensionIs(extension, L".lt"))
        {
            if (!hasSource ||
                !PlatformReadEntireFile(scratch->path, TEXTURE_MAX_FILE_BYTES, &bytes, &size))
            {
                continue;
            }
            bool parsed = ParseSingleTexture(bytes, (uint32_t)size, outSource, wantNormals);
            PlatformFree(bytes);
            if (parsed) return;
            ReleaseSource(outSource);
            memset(outSource, 0, sizeof(*outSource));
            continue;
        }

        wchar_t cacheExtension[16];
        BuildCacheExtension(extension, cacheExtension, 16u);
        if (!BuildPath(catalog, scratch, resourcePath, cacheExtension, scratch->cachePath))
        {
            continue;
        }

        PlatformPathInformation cache;
        bool hasCache = UsableFile(scratch->cachePath, &cache);
        if (!hasSource && !hasCache) continue;

        if (hasCache && PlatformReadEntireFile(scratch->cachePath, TEXTURE_MAX_FILE_BYTES, &bytes,
                                               &size))
        {
            bool parsed = ParseSingleTexture(bytes, (uint32_t)size, outSource, wantNormals);
            PlatformFree(bytes);
            bool fresh = parsed && (!hasSource ||
                                    (outSource->sourceSizeBytes == (uint32_t)source.size &&
                                     outSource->sourceModifiedTime == source.modifiedTime));
            if (fresh) return;
            ReleaseSource(outSource);
            memset(outSource, 0, sizeof(*outSource));
        }

        if (hasSource &&
            PlatformReadEntireFile(scratch->path, TEXTURE_MAX_FILE_BYTES, &bytes, &size))
        {
            bool parsed = DecodeImageFile(bytes, (uint32_t)size, outSource);
            PlatformFree(bytes);
            if (parsed)
            {
                WriteTextureCache(scratch->cachePath, outSource, source.modifiedTime,
                                  (uint32_t)source.size);
                return;
            }
            ReleaseSource(outSource);
            memset(outSource, 0, sizeof(*outSource));
        }
    }
}

// Дописывает к имени материала суффикс карты нормалей. Суффикс — часть
// последнего сегмента, а не отдельная папка: так карта лежит рядом с
// текстурой и видна глазом.
static bool BuildNormalResource(const wchar_t *name, wchar_t *destination, uint32_t capacity)
{
    uint32_t length = 0u;
    while (name[length] != 0)
    {
        if (length + 1u >= capacity) return false;
        destination[length] = name[length];
        ++length;
    }
    static const wchar_t suffix[] = L".normal";
    for (uint32_t index = 0; suffix[index] != 0; ++index)
    {
        if (length + 1u >= capacity) return false;
        destination[length++] = suffix[index];
    }
    destination[length] = 0;
    return true;
}

static void LoadMaterial(LaiueContentCatalog *catalog, LoadScratch *scratch, const wchar_t *name,
                         MaterialSource *outSource)
{
    memset(outSource, 0, sizeof(*outSource));
    if (name == NULL || !LaiueContentPathIsSafe(name)) return;

    LoadResource(catalog, scratch, name, true, outSource);
    if (!outSource->found) return;
    if (outSource->normal != NULL) return;   // `.lt` уже принёс карту нормалей

    // Карта нормалей отдельным файлом: её отсутствие — норма, а не
    // ошибка. У неё свой исходник и свой кэш рядом с ним.
    if (!BuildNormalResource(name, scratch->resource, LAIUE_CONTENT_PATH_CAPACITY)) return;
    MaterialSource normalSource;
    LoadResource(catalog, scratch, scratch->resource, false, &normalSource);
    if (!normalSource.found) return;

    // Карта нормалей обязана совпадать по геометрии: либо один кадр на
    // всю анимацию, либо столько же, сколько у albedo.
    bool usable = normalSource.width == outSource->width &&
                  normalSource.height == outSource->height &&
                  (normalSource.frameCount == 1u ||
                   normalSource.frameCount == outSource->frameCount);
    if (!usable)
    {
        ReleaseSource(&normalSource);
        return;
    }

    uint32_t frameBytes = outSource->width * outSource->height * 4u;
    if (normalSource.frameCount == outSource->frameCount)
    {
        outSource->normal = normalSource.albedo;
        normalSource.albedo = NULL;
    }
    else
    {
        // Один кадр карты повторяется на все кадры albedo.
        uint8_t *expanded = PlatformAllocate((size_t)frameBytes * outSource->frameCount, false);
        if (expanded != NULL)
        {
            for (uint32_t frame = 0; frame < outSource->frameCount; ++frame)
            {
                memcpy(expanded + (size_t)frame * frameBytes, normalSource.albedo, frameBytes);
            }
            outSource->normal = expanded;
        }
    }
    ReleaseSource(&normalSource);
}

// Заполняет цепочку mip одного слоя: уровень 0 приводится к общему
// размеру, остальные считаются из предыдущего.
static void WriteSliceChain(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
                            uint32_t size, uint8_t *cursor)
{
    ImageResample(source, sourceWidth, sourceHeight, cursor, size, size);
    uint8_t *previous = cursor;
    uint32_t previousSize = size;
    cursor += size * size * 4u;

    while (previousSize > 1u)
    {
        uint32_t nextSize = previousSize >> 1;
        ImageResample(previous, previousSize, previousSize, cursor, nextSize, nextSize);
        previous = cursor;
        previousSize = nextSize;
        cursor += nextSize * nextSize * 4u;
    }
}

static void WriteConstantChain(const uint8_t texel[4], uint32_t size, uint8_t *cursor)
{
    uint32_t total = MipChainBytes(size) / 4u;
    for (uint32_t index = 0; index < total; ++index)
    {
        cursor[index * 4u + 0u] = texel[0];
        cursor[index * 4u + 1u] = texel[1];
        cursor[index * 4u + 2u] = texel[2];
        cursor[index * 4u + 3u] = texel[3];
    }
}

TexturePackLoadStatus TexturePackBuildFrom(LaiueContentCatalog *catalog,
                                           const wchar_t *const *materialNames,
                                           uint32_t materialCount, TexturePackData *outPack)
{
    if (catalog == NULL || outPack == NULL || materialNames == NULL) return TEXTURE_PACK_LOAD_IO_ERROR;
    if (materialCount == 0u || materialCount > TEXTURE_PACK_MAX_LAYERS)
        return TEXTURE_PACK_LOAD_INVALID;

    LoadScratch *scratch = PlatformAllocate(sizeof(*scratch), true);
    MaterialSource *sources = PlatformAllocate(sizeof(*sources) * materialCount, true);
    if (scratch == NULL || sources == NULL)
    {
        PlatformFree(scratch);
        PlatformFree(sources);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    if (!LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, scratch->packName,
                                          LAIUE_CONTENT_NAME_CAPACITY))
    {
        PlatformFree(scratch);
        PlatformFree(sources);
        return TEXTURE_PACK_LOAD_NO_ACTIVE_PACK;
    }

    uint32_t largest = 1u;
    uint32_t sliceCount = 0u;
    uint32_t missing = 0u;
    bool anyNormal = false;
    for (uint32_t material = 0; material < materialCount; ++material)
    {
        LoadMaterial(catalog, scratch, materialNames[material], &sources[material]);
        MaterialSource *source = &sources[material];
        if (!source->found)
        {
            ++missing;
            ++sliceCount;
            continue;
        }
        if (source->width > largest) largest = source->width;
        if (source->height > largest) largest = source->height;
        if (source->normal != NULL) anyNormal = true;
        if (sliceCount > TEXTURE_PACK_MAX_SLICES - source->frameCount)
        {
            for (uint32_t index = 0; index <= material; ++index) ReleaseSource(&sources[index]);
            PlatformFree(scratch);
            PlatformFree(sources);
            return TEXTURE_PACK_LOAD_INVALID;
        }
        sliceCount += source->frameCount;
    }
    PlatformFree(scratch);

    // Общий размер массива: наибольшая сторона, округлённая вверх до
    // степени двойки. Остальные слои приводятся к нему усреднением —
    // размеры в папке, собранной руками, совпадают далеко не всегда.
    uint32_t size = IsPowerOfTwo(largest) ? largest : RoundUpToPowerOfTwo(largest);
    if (size > TEXTURE_MAX_DIMENSION) size = TEXTURE_MAX_DIMENSION;

    uint32_t chainBytes = MipChainBytes(size);
    uint64_t albedoBytes = (uint64_t)chainBytes * sliceCount;
    uint64_t totalBytes = anyNormal ? albedoBytes * 2u : albedoBytes;
    uint8_t *pixels = totalBytes <= 0xFFFFFFFFull
                          ? PlatformAllocate((size_t)totalBytes, false)
                          : NULL;
    if (pixels == NULL)
    {
        for (uint32_t index = 0; index < materialCount; ++index) ReleaseSource(&sources[index]);
        PlatformFree(sources);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    uint8_t *albedoCursor = pixels;
    uint8_t *normalCursor = anyNormal ? pixels + albedoBytes : NULL;
    uint32_t firstSlice = 0u;
    memset(outPack, 0, sizeof(*outPack));

    for (uint32_t material = 0; material < materialCount; ++material)
    {
        MaterialSource *source = &sources[material];
        uint32_t frames = source->found ? source->frameCount : 1u;

        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            if (source->found)
            {
                uint32_t frameBytes = source->width * source->height * 4u;
                WriteSliceChain(source->albedo + (size_t)frame * frameBytes, source->width,
                                source->height, size, albedoCursor);
                if (normalCursor != NULL)
                {
                    if (source->normal != NULL)
                    {
                        WriteSliceChain(source->normal + (size_t)frame * frameBytes, source->width,
                                        source->height, size, normalCursor);
                    }
                    else
                    {
                        WriteConstantChain(g_flatNormalTexel, size, normalCursor);
                    }
                }
            }
            else
            {
                WriteConstantChain(g_missingTexel, size, albedoCursor);
                if (normalCursor != NULL) WriteConstantChain(g_flatNormalTexel, size, normalCursor);
            }
            albedoCursor += chainBytes;
            if (normalCursor != NULL) normalCursor += chainBytes;
        }

        outPack->animation[material].firstSlice = (uint16_t)firstSlice;
        outPack->animation[material].frameCount = (uint16_t)frames;
        uint32_t cycle = 0u;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            uint16_t duration =
                source->found && frames > 1u ? source->frameMilliseconds[frame] : 0u;
            outPack->sliceMilliseconds[firstSlice + frame] = duration;
            cycle += duration;
        }
        outPack->animation[material].cycleMilliseconds = cycle;
        firstSlice += frames;
        ReleaseSource(source);
    }
    PlatformFree(sources);

    outPack->width = (uint16_t)size;
    outPack->height = (uint16_t)size;
    outPack->sliceCount = (uint16_t)sliceCount;
    outPack->mipCount = (uint16_t)FullMipCount(size);
    outPack->materialCount = (uint16_t)materialCount;
    outPack->pixels = pixels;
    outPack->pixelBytes = (uint32_t)albedoBytes;
    outPack->normalPixels = anyNormal ? pixels + albedoBytes : NULL;
    outPack->allocation = pixels;
    return missing == 0u ? TEXTURE_PACK_LOAD_OK : TEXTURE_PACK_LOAD_INCOMPLETE;
}
