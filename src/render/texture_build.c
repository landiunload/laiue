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
    // Владелец памяти `albedo`: либо сам `albedo` (PNG/JPEG/GIF), либо
    // прочитанный целиком файл `.lt`, внутрь которого смотрит `albedo`.
    void *albedoOwner;
    // Владелец памяти `normal`. У встроенной в `.lt` карты нормалей он
    // остаётся NULL: её кадры лежат в том же буфере, что и albedo.
    void *normalOwner;
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    // Сколько кадров у карты нормалей: отдельный файл вправе нести один
    // кадр на всю анимацию.
    uint32_t normalFrameCount;
    // Длительность каждого кадра: в GIF они вправе различаться.
    uint16_t frameMilliseconds[IMAGE_MAX_FRAMES];
    // Отпечаток исходника, записанный в `.lt`. Нулевой размер означает,
    // что файл ни из чего не выведен.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
    bool found;
} MaterialSource;

// Сведения о ресурсе без его пикселей: ровно то, что нужно первому
// проходу сборки, чтобы посчитать геометрию пака. Материал, найденный и
// в `.lt`, и в исходнике, отдаёт здесь то же, что дал бы полный разбор.
typedef struct ResourceMeta
{
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    bool found;
    // Карта нормалей пришла внутри самого `.lt` (формат RGBA8_NORMALS).
    bool hasNormals;
} ResourceMeta;

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
    // У `.lt` пиксели смотрят внутрь прочитанного файла, и освобождать
    // нужно именно его один раз; у остальных форматов владелец — сам
    // `albedo`.
    if (source->albedoOwner != NULL)
        PlatformFree(source->albedoOwner);
    else
        PlatformFree(source->albedo);
    PlatformFree(source->normalOwner);
    source->albedo = NULL;
    source->normal = NULL;
    source->albedoOwner = NULL;
    source->normalOwner = NULL;
    source->normalFrameCount = 0u;
}

// === Свой формат одной текстуры ===

// Разобранный заголовок своего формата: геометрия, расписание и
// отпечаток исходника без единого пикселя. Первый проход сборки читает
// только его, поэтому все раскодированные текстуры в ОЗУ не копятся.
typedef struct SingleTextureHeader
{
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    // Версия 1 несла одну длительность на всю анимацию.
    uint32_t frameMilliseconds;
    // Ноль у версии 1: таблицы длительностей на кадр нет.
    uint32_t durationTableOffset;
    uint32_t payloadOffset;
    uint32_t payloadBytes;
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
    bool withNormals;
} SingleTextureHeader;

// Проверяет заголовок целиком, включая расписание, но пиксели не
// трогает. Расписание читается из уже прочитанного файла и стоит
// копейки; так полный разбор строится поверх той же проверки, а не
// повторяет её.
static bool ParseSingleTextureHeader(const uint8_t *file, uint32_t availableBytes,
                                     uint64_t fileBytes, SingleTextureHeader *outHeader)
{
    if (availableBytes < LT_HEADER_BYTES_V1)
        return false;
    if (ReadU32Le(file) != LT_MAGIC)
        return false;

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
    if (availableBytes < headerSize) return false;

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

    uint64_t sourceModifiedTime = 0u;
    uint32_t sourceSizeBytes = 0u;
    if (version != 1u)
    {
        sourceModifiedTime =
            (uint64_t)ReadU32Le(file + 24) | ((uint64_t)ReadU32Le(file + 28) << 32);
        sourceSizeBytes = ReadU32Le(file + 32);
    }

    uint64_t frameBytes = (uint64_t)width * height * 4u;
    uint64_t albedoBytes = frameBytes * frameCount;
    uint64_t expected = withNormals ? albedoBytes * 2u : albedoBytes;
    if (expected > 0xFFFFFFFFull) return false;
    if (payloadBytes != (uint32_t)expected) return false;

    uint32_t tableBytes = version == 1u ? 0u : frameCount * 2u;
    if (availableBytes < headerSize + tableBytes) return false;
    if ((uint64_t)fileBytes != (uint64_t)headerSize + tableBytes + payloadBytes) return false;

    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        uint32_t duration =
            version == 1u ? frameMilliseconds : ReadU16Le(file + headerSize + frame * 2u);
        if (frameCount > 1u && duration == 0u) return false;
    }

    outHeader->width = width;
    outHeader->height = height;
    outHeader->frameCount = frameCount;
    outHeader->frameMilliseconds = frameMilliseconds;
    outHeader->durationTableOffset = version == 1u ? 0u : headerSize;
    outHeader->payloadOffset = headerSize + tableBytes;
    outHeader->payloadBytes = payloadBytes;
    outHeader->sourceModifiedTime = sourceModifiedTime;
    outHeader->sourceSizeBytes = sourceSizeBytes;
    outHeader->withNormals = withNormals;
    return true;
}

// Разбирает `.lt`, забирая буфер файла себе. Пиксели не копируются: они
// смотрят внутрь этого буфера, и освобождает его ReleaseSource вместе с
// источником. Так на большой текстуре не появляется второй буфер размером
// с payload. При отказе буфер освобождается здесь же.
static bool ParseSingleTexture(uint8_t *file, uint32_t fileBytes, MaterialSource *outSource,
                               bool wantNormals)
{
    SingleTextureHeader header;
    if (!ParseSingleTextureHeader(file, fileBytes, fileBytes, &header))
    {
        PlatformFree(file);
        return false;
    }

    for (uint32_t frame = 0; frame < header.frameCount; ++frame)
    {
        uint32_t duration = header.durationTableOffset == 0u
                                ? header.frameMilliseconds
                                : ReadU16Le(file + header.durationTableOffset + frame * 2u);
        outSource->frameMilliseconds[frame] = (uint16_t)(header.frameCount > 1u ? duration : 0u);
    }

    // Заголовок уже проверил, что payload целиком лежит в файле и
    // помещается в uint32.
    uint8_t *albedo = file + header.payloadOffset;
    uint8_t *normal = NULL;
    if (header.withNormals && wantNormals)
    {
        normal = albedo + (size_t)header.width * header.height * 4u * header.frameCount;
    }

    outSource->albedo = albedo;
    outSource->albedoOwner = file;
    outSource->normal = normal;
    outSource->normalOwner = NULL;
    outSource->normalFrameCount = normal != NULL ? header.frameCount : 0u;
    outSource->width = header.width;
    outSource->height = header.height;
    outSource->frameCount = header.frameCount;
    outSource->sourceModifiedTime = header.sourceModifiedTime;
    outSource->sourceSizeBytes = header.sourceSizeBytes;
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

// Заголовок обычного изображения без декодирования: размеры и число
// кадров нужны первому проходу, а сами пиксели — нет.
static bool InspectImageMeta(const uint8_t *file, uint32_t fileBytes, ResourceMeta *outMeta)
{
    ImageInfo info = {0};
    if (ImageInspect(file, fileBytes, &info) != IMAGE_OK) return false;
    if (info.frameCount == 0u || info.frameCount > TEXTURE_MAX_FRAMES) return false;

    outMeta->width = info.width;
    outMeta->height = info.height;
    outMeta->frameCount = info.frameCount;
    outMeta->found = true;
    outMeta->hasNormals = false;
    return true;
}

typedef struct LoadScratch
{
    wchar_t path[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t cachePath[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t resource[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t packName[LAIUE_CONTENT_NAME_CAPACITY];
    // Текущий материал второго прохода. Лежит здесь, а не на стеке:
    // frameMilliseconds[IMAGE_MAX_FRAMES] вместе с локалями вызывающего,
    // в который LTO вклеивает всю сборку пака, переваливает предел кадра
    // 4 КиБ и требует __chkstk, которого без CRT нет.
    MaterialSource source;
    // Заголовок .lt и максимальная таблица длительностей без payload.
    // Здесь, на heap, чтобы LTO не увеличивал кадр стека renderer.
    uint8_t headerPrefix[LT_HEADER_BYTES + TEXTURE_MAX_FRAMES * 2u];
    // Порядок форматов вычисляется один раз на сборку: файл
    // `formats.txt` читается и разбирается, и делать это заново на каждый
    // ресурс обоих проходов незачем.
    const wchar_t *order[LAIUE_CONTENT_FORMAT_ORDER_MAX];
    uint32_t orderCount;
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

    // Порядок форматов посчитан один раз на сборку.
    const wchar_t *const *order = scratch->order;
    uint32_t orderCount = scratch->orderCount;

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
            // Разбор забирает буфер: он же становится владельцем пикселей.
            if (ParseSingleTexture(bytes, (uint32_t)size, outSource, wantNormals)) return;
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

static void MetadataFromSingleTexture(const SingleTextureHeader *header, ResourceMeta *outMeta)
{
    outMeta->width = header->width;
    outMeta->height = header->height;
    outMeta->frameCount = header->frameCount;
    outMeta->found = true;
    outMeta->hasNormals = header->withNormals;
}

static bool ReadSingleTextureHeader(const wchar_t *path, LoadScratch *scratch,
                                    SingleTextureHeader *outHeader)
{
    uint32_t prefixBytes = 0;
    uint64_t fileBytes = 0;
    if (!PlatformReadFilePrefix(path, TEXTURE_MAX_FILE_BYTES, scratch->headerPrefix,
                                (uint32_t)sizeof(scratch->headerPrefix), &prefixBytes, &fileBytes))
    {
        return false;
    }
    // Полный размер взят из того же handle: усечённый payload по-прежнему
    // отвергается, хотя ради метаданных сами пиксели больше не читаются.
    return ParseSingleTextureHeader(scratch->headerPrefix, prefixBytes, fileBytes, outHeader);
}

// Разбор ресурса до пикселей: тот же перебор форматов и кэшей, что и в
// LoadResource, но файл читается ради заголовка и сразу освобождается.
// Итог обязан совпасть с тем, что даст полный разбор на том же
// состоянии папки.
static void LoadResourceMeta(LaiueContentCatalog *catalog, LoadScratch *scratch,
                             const wchar_t *resourcePath, ResourceMeta *outMeta)
{
    memset(outMeta, 0, sizeof(*outMeta));

    uint8_t *bytes = NULL;
    uint64_t size = 0u;

    // Порядок форматов посчитан один раз на сборку.
    const wchar_t *const *order = scratch->order;
    uint32_t orderCount = scratch->orderCount;

    for (uint32_t index = 0; index < orderCount; ++index)
    {
        const wchar_t *extension = order[index];
        if (!BuildPath(catalog, scratch, resourcePath, extension, scratch->path))
            continue;

        PlatformPathInformation source;
        bool hasSource = UsableFile(scratch->path, &source);

        if (ExtensionIs(extension, L".lt"))
        {
            SingleTextureHeader header;
            if (hasSource && ReadSingleTextureHeader(scratch->path, scratch, &header))
            {
                MetadataFromSingleTexture(&header, outMeta);
                return;
            }
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
        if (!hasSource && !hasCache)
            continue;

        if (hasCache)
        {
            SingleTextureHeader header;
            bool parsed = ReadSingleTextureHeader(scratch->cachePath, scratch, &header);
            bool fresh =
                parsed && (!hasSource || (header.sourceSizeBytes == (uint32_t)source.size &&
                                          header.sourceModifiedTime == source.modifiedTime));
            if (fresh)
            {
                MetadataFromSingleTexture(&header, outMeta);
                return;
            }
            memset(outMeta, 0, sizeof(*outMeta));
        }

        if (hasSource &&
            PlatformReadEntireFile(scratch->path, TEXTURE_MAX_FILE_BYTES, &bytes, &size))
        {
            bool parsed = InspectImageMeta(bytes, (uint32_t)size, outMeta);
            PlatformFree(bytes);
            if (parsed) return;
            memset(outMeta, 0, sizeof(*outMeta));
        }
    }
}

// Первый проход по материалу: находятся ли albedo и карта нормалей и
// какова их геометрия. Пиксели не читаются.
static void LoadMaterialMeta(LaiueContentCatalog *catalog, LoadScratch *scratch,
                             const wchar_t *name, ResourceMeta *outMeta)
{
    memset(outMeta, 0, sizeof(*outMeta));
    if (name == NULL || !LaiueContentPathIsSafe(name)) return;

    ResourceMeta albedo;
    LoadResourceMeta(catalog, scratch, name, &albedo);
    if (!albedo.found) return;
    *outMeta = albedo;
    if (albedo.hasNormals) return;   // `.lt` уже принёс карту нормалей

    if (!BuildNormalResource(name, scratch->resource, LAIUE_CONTENT_PATH_CAPACITY)) return;
    ResourceMeta normal;
    LoadResourceMeta(catalog, scratch, scratch->resource, &normal);
    if (!normal.found) return;

    // Карта нормалей обязана совпадать по геометрии: либо один кадр на
    // всю анимацию, либо столько же, сколько у albedo.
    outMeta->hasNormals = normal.width == albedo.width && normal.height == albedo.height &&
                          (normal.frameCount == 1u || normal.frameCount == albedo.frameCount);
}

static void LoadMaterial(LaiueContentCatalog *catalog, LoadScratch *scratch, const wchar_t *name,
                         MaterialSource *outSource)
{
    memset(outSource, 0, sizeof(*outSource));
    if (name == NULL || !LaiueContentPathIsSafe(name)) return;

    LoadResource(catalog, scratch, name, true, outSource);
    if (!outSource->found) return;
    if (outSource->normal != NULL)
    {
        // `.lt` уже принёс карту нормалей: её кадров столько же, сколько
        // у albedo.
        outSource->normalFrameCount = outSource->frameCount;
        return;
    }

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

    // Один кадр повторяется на все кадры albedo без копии: читатель
    // берёт нулевой кадр по индексу. Раньше под это выделялся буфер во
    // всю анимацию и заполнялся копиями одного и того же кадра.
    outSource->normal = normalSource.albedo;
    outSource->normalOwner = normalSource.albedoOwner != NULL ? normalSource.albedoOwner
                                                              : (void *)normalSource.albedo;
    outSource->normalFrameCount = normalSource.frameCount;
    normalSource.albedo = NULL;
    normalSource.albedoOwner = NULL;
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
    ResourceMeta *metas = PlatformAllocate(sizeof(*metas) * materialCount, true);
    if (scratch == NULL || metas == NULL)
    {
        PlatformFree(scratch);
        PlatformFree(metas);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    if (!LaiueContentCatalogGetActivePack(catalog, LAIUE_CONTENT_TEXTURE_PACK, scratch->packName,
                                          LAIUE_CONTENT_NAME_CAPACITY))
    {
        PlatformFree(scratch);
        PlatformFree(metas);
        return TEXTURE_PACK_LOAD_NO_ACTIVE_PACK;
    }

    // Порядок форматов один на всю сборку: файл `formats.txt` читается
    // здесь, а не заново на каждый ресурс каждого прохода.
    scratch->orderCount = LaiueContentCatalogOrderFormats(
        catalog, LAIUE_CONTENT_TEXTURE_PACK, g_textureExtensions,
        sizeof(g_textureExtensions) / sizeof(g_textureExtensions[0]), scratch->order,
        LAIUE_CONTENT_FORMAT_ORDER_MAX);

    // === Проход 1: только заголовки. ===
    // Раскодированные пиксели здесь не задерживаются: файл читается ради
    // размеров и расписания и сразу освобождается. В ОЗУ живёт лишь
    // таблица метаданных на 64 материала.
    uint32_t largest = 1u;
    uint32_t sliceCount = 0u;
    uint32_t missing = 0u;
    bool anyNormal = false;
    for (uint32_t material = 0; material < materialCount; ++material)
    {
        LoadMaterialMeta(catalog, scratch, materialNames[material], &metas[material]);
        const ResourceMeta *meta = &metas[material];
        if (!meta->found)
        {
            ++missing;
            ++sliceCount;
            continue;
        }
        if (meta->width > largest) largest = meta->width;
        if (meta->height > largest) largest = meta->height;
        if (meta->hasNormals) anyNormal = true;
        if (sliceCount > TEXTURE_PACK_MAX_SLICES - meta->frameCount)
        {
            PlatformFree(scratch);
            PlatformFree(metas);
            return TEXTURE_PACK_LOAD_INVALID;
        }
        sliceCount += meta->frameCount;
    }

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
        PlatformFree(scratch);
        PlatformFree(metas);
        return TEXTURE_PACK_LOAD_IO_ERROR;
    }

    // === Проход 2: по одному материалу. ===
    // Пак уже выделен целиком, поэтому в памяти одновременно он и один
    // раскодированный материал, а не все. Источник освобождается сразу
    // после записи своей цепочки мипов.
    uint8_t *albedoCursor = pixels;
    uint8_t *normalCursor = anyNormal ? pixels + albedoBytes : NULL;
    uint32_t firstSlice = 0u;
    memset(outPack, 0, sizeof(*outPack));

    for (uint32_t material = 0; material < materialCount; ++material)
    {
        MaterialSource *source = &scratch->source;
        LoadMaterial(catalog, scratch, materialNames[material], source);
        const ResourceMeta *meta = &metas[material];
        uint32_t frames = meta->found ? meta->frameCount : 1u;

        // Источник карты нормалей предыдущего кадра. У карты с одним кадром на
        // всю анимацию он повторяется кадр в кадр, и цепочку мипов достаточно
        // посчитать один раз: одинаковый вход даёт побайтово одинаковый выход,
        // поэтому следующий слой копируется, а не пересчитывается заново.
        const uint8_t *previousNormal = NULL;
        bool havePreviousNormal = false;

        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            bool hasPixels = source->found && frame < source->frameCount;
            const uint8_t *normalSource = NULL;
            if (hasPixels)
            {
                uint32_t frameBytes = source->width * source->height * 4u;
                WriteSliceChain(source->albedo + (size_t)frame * frameBytes, source->width,
                                source->height, size, albedoCursor);
                if (source->normal != NULL)
                {
                    // Один кадр карты нормалей может обслуживать всю
                    // анимацию: тогда он берётся по нулевому индексу.
                    uint32_t normalFrame = frame < source->normalFrameCount ? frame : 0u;
                    normalSource = source->normal + (size_t)normalFrame * frameBytes;
                }
            }
            else
            {
                WriteConstantChain(g_missingTexel, size, albedoCursor);
            }

            if (normalCursor != NULL)
            {
                if (normalSource != NULL)
                {
                    if (havePreviousNormal && normalSource == previousNormal)
                    {
                        memcpy(normalCursor, normalCursor - chainBytes, chainBytes);
                    }
                    else
                    {
                        WriteSliceChain(normalSource, source->width, source->height, size,
                                        normalCursor);
                    }
                }
                else if (havePreviousNormal && previousNormal == NULL)
                {
                    // Нейтральная нормаль одинакова байт в байт на всех кадрах.
                    memcpy(normalCursor, normalCursor - chainBytes, chainBytes);
                }
                else
                {
                    WriteConstantChain(g_flatNormalTexel, size, normalCursor);
                }
                previousNormal = normalSource;
                havePreviousNormal = true;
            }

            albedoCursor += chainBytes;
            if (normalCursor != NULL) normalCursor += chainBytes;
        }

        outPack->animation[material].firstSlice = (uint16_t)firstSlice;
        outPack->animation[material].frameCount = (uint16_t)frames;
        uint32_t cycle = 0u;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            uint16_t duration = source->found && frames > 1u && frame < source->frameCount
                                    ? source->frameMilliseconds[frame]
                                    : 0u;
            outPack->sliceMilliseconds[firstSlice + frame] = duration;
            cycle += duration;
        }
        outPack->animation[material].cycleMilliseconds = cycle;
        firstSlice += frames;
        ReleaseSource(source);
    }
    PlatformFree(scratch);
    PlatformFree(metas);

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
