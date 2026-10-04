// Пак моделей `.lop`. Устроен как звукопак: имена моделей принадлежат
// приложению, подпапки допустимы, исходный OBJ читается как есть, а
// разобранный кладётся рядом готовым `.obj.lo` с отпечатком исходника.
// Свой `.lo` стоит последним: он запасной путь, когда исходника нет или
// он не разобрался. Порядок меняется файлом models/formats.txt.
//
// Модели нет — выдаётся куб-заглушка, а причина остаётся в статусе:
// так же текстурпак показывает нейтральный слой, а звукопак — тишину.

#include "model/model_service.h"

#include "content/content_service.h"
#include "media/model.h"
#include "platform/system.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(LaiueModelVertexV1) == sizeof(ModelVertex) &&
                   offsetof(LaiueModelVertexV1, normal) == offsetof(ModelVertex, normal) &&
                   offsetof(LaiueModelVertexV1, uv) == offsetof(ModelVertex, uv) &&
                   offsetof(LaiueModelVertexV1, colorRGBA) == offsetof(ModelVertex, colorRGBA),
               "public model vertex must match the codec record");
_Static_assert(sizeof(LaiueModelBoxV1) == sizeof(ModelBox) &&
                   offsetof(LaiueModelBoxV1, halfExtent) == offsetof(ModelBox, halfExtent) &&
                   offsetof(LaiueModelBoxV1, rotation) == offsetof(ModelBox, rotation),
               "public model box must match the codec record");

/* Installed by the module host between create and start, or by a direct
 * static integrator. Catalog operations fail closed while it is absent;
 * explicit file and memory loads do not need it. */
static const LaiueContentServiceV1 *g_content;

void LaiueModelSetContentService(const struct LaiueContentServiceV1 *content)
{
    g_content = content;
}

struct LaiueModelV1
{
    LaiueModelViewV1 view;
    // Имена материалов живут в записях кодека: публичные части указывают
    // в них, и отдельной таблицы строк не нужно.
    ModelPart *names;
};

static uint64_t AlignUp(uint64_t value)
{
    return (value + 15u) & ~(uint64_t)15u;
}

// Один блок памяти на модель: заголовок и массивы подряд. Освобождение —
// один вызов, и частично собранной модели не бывает.
static LaiueModelV1 *AllocateModel(uint32_t vertexCount, uint32_t indexCount, uint32_t partCount,
                                   uint32_t boxCount)
{
    uint64_t offset = AlignUp(sizeof(LaiueModelV1));
    const uint64_t verticesOffset = offset;
    offset = AlignUp(offset + (uint64_t)vertexCount * sizeof(LaiueModelVertexV1));
    const uint64_t indicesOffset = offset;
    offset = AlignUp(offset + (uint64_t)indexCount * sizeof(uint32_t));
    const uint64_t partsOffset = offset;
    offset = AlignUp(offset + (uint64_t)partCount * sizeof(LaiueModelPartV1));
    const uint64_t namesOffset = offset;
    offset = AlignUp(offset + (uint64_t)partCount * sizeof(ModelPart));
    const uint64_t boxesOffset = offset;
    offset += (uint64_t)boxCount * sizeof(LaiueModelBoxV1);
    if (offset > ((uint64_t)1 << 32))
        return NULL;
    uint8_t *block = (uint8_t *)PlatformAllocate((size_t)offset, true);
    if (block == NULL)
        return NULL;
    LaiueModelV1 *model = (LaiueModelV1 *)block;
    model->view.structSize = sizeof(model->view);
    model->view.vertices = (const LaiueModelVertexV1 *)(block + verticesOffset);
    model->view.vertexCount = vertexCount;
    model->view.indices = (const uint32_t *)(block + indicesOffset);
    model->view.indexCount = indexCount;
    model->view.parts = (const LaiueModelPartV1 *)(block + partsOffset);
    model->view.partCount = partCount;
    model->view.boxes = boxCount != 0u ? (const LaiueModelBoxV1 *)(block + boxesOffset) : NULL;
    model->view.boxCount = boxCount;
    model->names = (ModelPart *)(block + namesOffset);
    return model;
}

static LaiueModelV1 *ModelFromDecoded(const ModelData *decoded, uint32_t extraFlags)
{
    LaiueModelV1 *model = AllocateModel(decoded->vertexCount, decoded->indexCount,
                                        decoded->partCount, decoded->boxCount);
    if (model == NULL)
        return NULL;
    memcpy((void *)model->view.vertices, decoded->vertices,
           (size_t)decoded->vertexCount * sizeof(ModelVertex));
    memcpy((void *)model->view.indices, decoded->indices,
           (size_t)decoded->indexCount * sizeof(uint32_t));
    memcpy(model->names, decoded->parts, (size_t)decoded->partCount * sizeof(ModelPart));
    LaiueModelPartV1 *parts = (LaiueModelPartV1 *)model->view.parts;
    for (uint32_t part = 0u; part < decoded->partCount; ++part)
    {
        parts[part].firstIndex = decoded->parts[part].firstIndex;
        parts[part].indexCount = decoded->parts[part].indexCount;
        parts[part].material = model->names[part].material;
    }
    if (decoded->boxCount != 0u)
        memcpy((void *)model->view.boxes, decoded->boxes,
               (size_t)decoded->boxCount * sizeof(ModelBox));
    memcpy(model->view.boundsMin, decoded->boundsMin, sizeof(model->view.boundsMin));
    memcpy(model->view.boundsMax, decoded->boundsMax, sizeof(model->view.boundsMax));
    model->view.flags = decoded->flags | extraFlags;
    return model;
}

// Куб единичного размера на земле (z от 0 до 1), пурпурный: его видно
// сразу, и никто не примет его за настоящую модель.
static LaiueModelV1 *CreatePlaceholder(void)
{
    LaiueModelV1 *model = AllocateModel(24u, 36u, 1u, 1u);
    if (model == NULL)
        return NULL;
    static const float normals[6][3] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
    };
    LaiueModelVertexV1 *vertices = (LaiueModelVertexV1 *)model->view.vertices;
    uint32_t *indices = (uint32_t *)model->view.indices;
    for (uint32_t face = 0u; face < 6u; ++face)
    {
        const float *normal = normals[face];
        // Два касательных направления грани; их векторное произведение
        // совпадает с нормалью, поэтому обход против часовой снаружи.
        float u[3] = {normal[1] != 0.0f || normal[2] != 0.0f ? 1.0f : 0.0f,
                      normal[0] != 0.0f ? 1.0f : 0.0f, 0.0f};
        float v[3] = {normal[1] * u[2] - normal[2] * u[1], normal[2] * u[0] - normal[0] * u[2],
                      normal[0] * u[1] - normal[1] * u[0]};
        static const float corners[4][2] = {
            {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
        for (uint32_t corner = 0u; corner < 4u; ++corner)
        {
            LaiueModelVertexV1 *vertex = &vertices[face * 4u + corner];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                const float offset = 0.5f * (normal[axis] + corners[corner][0] * u[axis] +
                                             corners[corner][1] * v[axis]);
                vertex->position[axis] = axis == 2u ? offset + 0.5f : offset;
                vertex->normal[axis] = normal[axis];
            }
            vertex->uv[0] = corners[corner][0] * 0.5f + 0.5f;
            vertex->uv[1] = 0.5f - corners[corner][1] * 0.5f;
            vertex->colorRGBA = 0xFFFF00FFu;
        }
        static const uint32_t pattern[6] = {0u, 1u, 2u, 0u, 2u, 3u};
        for (uint32_t index = 0u; index < 6u; ++index)
            indices[face * 6u + index] = face * 4u + pattern[index];
    }
    LaiueModelPartV1 *part = (LaiueModelPartV1 *)model->view.parts;
    part->firstIndex = 0u;
    part->indexCount = 36u;
    part->material = model->names[0].material;
    LaiueModelBoxV1 *box = (LaiueModelBoxV1 *)model->view.boxes;
    box->center[2] = 0.5f;
    box->halfExtent[0] = box->halfExtent[1] = box->halfExtent[2] = 0.5f;
    box->rotation[3] = 1.0f;
    model->view.boundsMin[0] = model->view.boundsMin[1] = -0.5f;
    model->view.boundsMax[0] = model->view.boundsMax[1] = 0.5f;
    model->view.boundsMax[2] = 1.0f;
    model->view.flags = LAIUE_MODEL_FLAG_NORMALS | LAIUE_MODEL_FLAG_UV | LAIUE_MODEL_FLAG_COLORS |
                        LAIUE_MODEL_FLAG_PLACEHOLDER;
    return model;
}

typedef struct DecodeBuffers
{
    ModelData model;
    void *block;
} DecodeBuffers;

static void DecodeBuffersRelease(DecodeBuffers *buffers)
{
    PlatformFree(buffers->block);
    memset(buffers, 0, sizeof(*buffers));
}

static uint32_t StatusFromCodec(ModelStatus status)
{
    switch (status)
    {
    case MODEL_OK:
        return LAIUE_MODEL_LOAD_OK;
    case MODEL_INVALID_ARGUMENT:
    case MODEL_BUFFER_TOO_SMALL:
        return LAIUE_MODEL_LOAD_IO_ERROR;
    default:
        return LAIUE_MODEL_LOAD_INVALID_MODEL;
    }
}

// Разбор во временные массивы ёмкостью из ModelInspect: у OBJ это
// верхние границы, поэтому итоговая модель потом копируется в точный
// по размеру блок, а временный возвращается сразу.
static uint32_t DecodeBytes(const void *bytes, uint32_t sizeBytes, DecodeBuffers *out)
{
    memset(out, 0, sizeof(*out));
    ModelInfo info;
    ModelStatus status = ModelInspect(bytes, sizeBytes, &info);
    if (status != MODEL_OK)
        return StatusFromCodec(status);
    const uint64_t vertexBytes = AlignUp((uint64_t)info.vertexCapacity * sizeof(ModelVertex));
    const uint64_t indexBytes = AlignUp((uint64_t)info.indexCapacity * sizeof(uint32_t));
    const uint64_t partBytes = AlignUp((uint64_t)info.partCapacity * sizeof(ModelPart));
    const uint64_t boxBytes = AlignUp((uint64_t)info.boxCapacity * sizeof(ModelBox));
    const uint64_t total =
        vertexBytes + indexBytes + partBytes + boxBytes + AlignUp(info.scratchBytes);
    if (total > ((uint64_t)1 << 32))
        return LAIUE_MODEL_LOAD_INVALID_MODEL;
    uint8_t *block = (uint8_t *)PlatformAllocate((size_t)total, false);
    if (block == NULL)
        return LAIUE_MODEL_LOAD_OUT_OF_MEMORY;
    out->block = block;
    out->model.vertices = (ModelVertex *)block;
    out->model.indices = (uint32_t *)(block + vertexBytes);
    out->model.parts = (ModelPart *)(block + vertexBytes + indexBytes);
    out->model.boxes =
        info.boxCapacity != 0u ? (ModelBox *)(block + vertexBytes + indexBytes + partBytes) : NULL;
    void *scratch = block + vertexBytes + indexBytes + partBytes + boxBytes;
    status = ModelDecode(bytes, sizeBytes, NULL, &info, &out->model, scratch, info.scratchBytes);
    if (status != MODEL_OK)
    {
        DecodeBuffersRelease(out);
        return StatusFromCodec(status);
    }
    return LAIUE_MODEL_LOAD_OK;
}

static uint32_t DecodeFile(const wchar_t *path, DecodeBuffers *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!PlatformReadEntireFile(path, MODEL_MAX_FILE_BYTES, &bytes, &size))
        return LAIUE_MODEL_LOAD_IO_ERROR;
    const uint32_t status = DecodeBytes(bytes, (uint32_t)size, out);
    PlatformFree(bytes);
    return status;
}

static void SetStatus(uint32_t *outStatus, uint32_t status)
{
    if (outStatus != NULL)
        *outStatus = status;
}

static LaiueModelV1 *LoadMemory(const void *bytes, uint32_t sizeBytes, uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_MODEL_LOAD_NOT_ATTEMPTED);
    if (bytes == NULL)
    {
        SetStatus(outStatus, LAIUE_MODEL_LOAD_INVALID_MODEL);
        return NULL;
    }
    DecodeBuffers buffers;
    const uint32_t status = DecodeBytes(bytes, sizeBytes, &buffers);
    if (status != LAIUE_MODEL_LOAD_OK)
    {
        SetStatus(outStatus, status);
        return NULL;
    }
    LaiueModelV1 *model = ModelFromDecoded(&buffers.model, 0u);
    DecodeBuffersRelease(&buffers);
    SetStatus(outStatus, model != NULL ? LAIUE_MODEL_LOAD_OK : LAIUE_MODEL_LOAD_OUT_OF_MEMORY);
    return model;
}

static LaiueModelV1 *LoadFile(const wchar_t *path, uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_MODEL_LOAD_NOT_ATTEMPTED);
    if (path == NULL)
    {
        SetStatus(outStatus, LAIUE_MODEL_LOAD_INVALID_MODEL);
        return NULL;
    }
    DecodeBuffers buffers;
    const uint32_t status = DecodeFile(path, &buffers);
    if (status != LAIUE_MODEL_LOAD_OK)
    {
        SetStatus(outStatus, status);
        return NULL;
    }
    LaiueModelV1 *model = ModelFromDecoded(&buffers.model, 0u);
    DecodeBuffersRelease(&buffers);
    SetStatus(outStatus, model != NULL ? LAIUE_MODEL_LOAD_OK : LAIUE_MODEL_LOAD_OUT_OF_MEMORY);
    return model;
}

// === Пак ===

static const wchar_t *const g_modelExtensions[] = {L".obj", L".lo"};

static bool ExtensionIs(const wchar_t *extension, const wchar_t *expected)
{
    uint32_t index = 0u;
    while (extension[index] != 0 && extension[index] == expected[index])
        ++index;
    return extension[index] == expected[index];
}

static bool ModelFileUsable(const wchar_t *path, PlatformPathInformation *outInformation)
{
    return PlatformGetPathInformation(path, outInformation) && outInformation->exists &&
           !outInformation->isDirectory && !outInformation->isSymbolicLink &&
           outInformation->size != 0u && outInformation->size <= MODEL_MAX_FILE_BYTES;
}

// `tree.obj` даёт `tree.obj.lo`: видно, из чего собран кэш, и он не
// займёт место `tree.lo`, положенного человеком.
static void BuildCacheExtension(const wchar_t *extension, wchar_t destination[16])
{
    uint32_t length = 0u;
    while (extension[length] != 0 && length + 4u < 16u)
    {
        destination[length] = extension[length];
        ++length;
    }
    destination[length++] = L'.';
    destination[length++] = L'l';
    destination[length++] = L'o';
    destination[length] = 0;
}

// Кэш необязателен: каталог пака бывает доступен только на чтение, и тогда
// исходник просто разбирается при каждой загрузке.
static void WriteCache(const wchar_t *path, ModelData *model, uint64_t modifiedTime,
                       uint32_t sizeBytes)
{
    model->sourceModifiedTime = modifiedTime;
    model->sourceSizeBytes = sizeBytes;
    uint32_t encodedBytes = 0u;
    if (ModelEncodedBytes(model, &encodedBytes) != MODEL_OK)
        return;
    uint8_t *encoded = (uint8_t *)PlatformAllocate(encodedBytes, false);
    if (encoded == NULL)
        return;
    if (ModelEncode(model, encoded, encodedBytes, NULL) == MODEL_OK)
        (void)PlatformWriteFileAtomic(path, encoded, encodedBytes);
    PlatformFree(encoded);
}

static uint32_t PathCapacity(LaiueContentCatalog *catalog, const wchar_t *activeName,
                             const wchar_t *modelName)
{
    if (g_content->buildPath == NULL)
        return LAIUE_CONTENT_PATH_CAPACITY;
    wchar_t *base =
        (wchar_t *)PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (base == NULL)
        return LAIUE_CONTENT_PATH_CAPACITY;
    uint32_t needed = LAIUE_CONTENT_PATH_CAPACITY;
    if (g_content->buildPath(catalog, LAIUE_CONTENT_MODEL_PACK, activeName, NULL, base,
                             LAIUE_CONTENT_PATH_CAPACITY) != 0u)
    {
        uint64_t length = 0u;
        while (base[length] != 0)
            ++length;
        uint64_t nameLength = 0u;
        while (modelName[nameLength] != 0)
            ++nameLength;
        // Корень + разделитель + имя + самое длинное расширение (.obj.lo) + NUL.
        const uint64_t total = length + nameLength + 9u;
        if (total < LAIUE_CONTENT_PATH_CAPACITY)
            needed = (uint32_t)total;
    }
    PlatformFree(base);
    return needed;
}

static LaiueModelV1 *Placeholder(uint32_t status, uint32_t *outStatus)
{
    LaiueModelV1 *model = CreatePlaceholder();
    SetStatus(outStatus, model != NULL ? status : LAIUE_MODEL_LOAD_OUT_OF_MEMORY);
    return model;
}

static LaiueModelV1 *LoadFrom(LaiueContentCatalog *catalog, const wchar_t *modelName,
                              uint32_t *outStatus)
{
    SetStatus(outStatus, LAIUE_MODEL_LOAD_NOT_ATTEMPTED);
    const LaiueContentServiceV1 *content = g_content;
    if (content == NULL)
        return modelName != NULL ? Placeholder(LAIUE_MODEL_LOAD_NO_CATALOG, outStatus) : NULL;
    // Небезопасное имя — ошибка программы, а не отсутствие содержимого.
    if (modelName == NULL || content->pathIsSafe == NULL || content->pathIsSafe(modelName) == 0u)
    {
        SetStatus(outStatus, LAIUE_MODEL_LOAD_INVALID_MODEL);
        return NULL;
    }
    if (catalog == NULL && content->defaultCatalog != NULL)
        catalog = content->defaultCatalog();
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (catalog == NULL || content->getActivePack == NULL ||
        content->getActivePack(catalog, LAIUE_CONTENT_MODEL_PACK, activeName,
                               LAIUE_CONTENT_NAME_CAPACITY) == 0u)
        return Placeholder(LAIUE_MODEL_LOAD_NO_ACTIVE_PACK, outStatus);

    const uint32_t pathCapacity = PathCapacity(catalog, activeName, modelName);
    wchar_t *paths =
        (wchar_t *)PlatformAllocate((size_t)pathCapacity * 2u * sizeof(wchar_t), false);
    if (paths == NULL)
    {
        SetStatus(outStatus, LAIUE_MODEL_LOAD_OUT_OF_MEMORY);
        return NULL;
    }
    wchar_t *path = paths;
    wchar_t *cachePath = paths + pathCapacity;

    const wchar_t *order[LAIUE_CONTENT_FORMAT_ORDER_MAX];
    const uint32_t orderCount =
        content->orderFormats == NULL
            ? 0u
            : content->orderFormats(catalog, LAIUE_CONTENT_MODEL_PACK, g_modelExtensions,
                                    sizeof(g_modelExtensions) / sizeof(g_modelExtensions[0]), order,
                                    LAIUE_CONTENT_FORMAT_ORDER_MAX);
    uint32_t status = LAIUE_MODEL_LOAD_NOT_FOUND;
    DecodeBuffers buffers;
    memset(&buffers, 0, sizeof(buffers));
    bool decoded = false;
    bool writeCache = false;
    PlatformPathInformation source;
    memset(&source, 0, sizeof(source));
    for (uint32_t index = 0u; !decoded && index < orderCount; ++index)
    {
        const wchar_t *extension = order[index];
        if (content->buildResourcePath == NULL ||
            content->buildResourcePath(catalog, LAIUE_CONTENT_MODEL_PACK, activeName, modelName,
                                       extension, path, pathCapacity) == 0u)
        {
            status = LAIUE_MODEL_LOAD_IO_ERROR;
            continue;
        }
        const bool hasSource = ModelFileUsable(path, &source);
        if (ExtensionIs(extension, L".lo"))
        {
            if (!hasSource)
                continue;
            status = DecodeFile(path, &buffers);
            decoded = status == LAIUE_MODEL_LOAD_OK;
            continue;
        }
        wchar_t cacheExtension[16];
        BuildCacheExtension(extension, cacheExtension);
        if (content->buildResourcePath(catalog, LAIUE_CONTENT_MODEL_PACK, activeName, modelName,
                                       cacheExtension, cachePath, pathCapacity) == 0u)
        {
            status = LAIUE_MODEL_LOAD_IO_ERROR;
            continue;
        }
        PlatformPathInformation cache;
        if (ModelFileUsable(cachePath, &cache))
        {
            // Свежесть кэша определяет отпечаток исходника внутри него:
            // сам исходник при этом не читается — в этом и смысл кэша.
            status = DecodeFile(cachePath, &buffers);
            if (status == LAIUE_MODEL_LOAD_OK &&
                (!hasSource || (buffers.model.sourceSizeBytes == (uint32_t)source.size &&
                                buffers.model.sourceModifiedTime == source.modifiedTime)))
            {
                decoded = true;
                break;
            }
            DecodeBuffersRelease(&buffers);
        }
        if (hasSource)
        {
            status = DecodeFile(path, &buffers);
            if (status == LAIUE_MODEL_LOAD_OK)
            {
                decoded = true;
                writeCache = true;
                break;
            }
        }
    }

    LaiueModelV1 *model = NULL;
    if (decoded)
    {
        if (writeCache)
            WriteCache(cachePath, &buffers.model, source.modifiedTime, (uint32_t)source.size);
        model = ModelFromDecoded(&buffers.model, 0u);
        if (model == NULL)
            status = LAIUE_MODEL_LOAD_OUT_OF_MEMORY;
        DecodeBuffersRelease(&buffers);
    }
    else
    {
        model = CreatePlaceholder();
        if (model == NULL)
            status = LAIUE_MODEL_LOAD_OUT_OF_MEMORY;
    }
    PlatformFree(paths);
    SetStatus(outStatus, status);
    return model;
}

static uint32_t GetView(const LaiueModelV1 *model, LaiueModelViewV1 *outView)
{
    if (model == NULL || outView == NULL)
        return 0u;
    *outView = model->view;
    return 1u;
}

static void Release(LaiueModelV1 *model)
{
    PlatformFree(model);
}

// === Перечисление ===

static bool CopyList(const LaiueContentList *source, LaiueModelListV1 *outList)
{
    outList->entries = NULL;
    outList->count = 0u;
    if (source->count == 0u)
        return true;
    outList->entries = (LaiueModelNameV1 *)PlatformAllocate(
        (size_t)source->count * sizeof(LaiueModelNameV1), true);
    if (outList->entries == NULL)
        return false;
    for (uint32_t index = 0u; index < source->count; ++index)
    {
        memcpy(outList->entries[index].name, source->entries[index].name,
               sizeof(outList->entries[index].name));
        outList->entries[index].active = source->entries[index].active ? 1u : 0u;
    }
    outList->count = source->count;
    return true;
}

static uint32_t EnumeratePacks(LaiueContentCatalog *catalog, LaiueModelListV1 *outList)
{
    if (outList == NULL)
        return 0u;
    outList->entries = NULL;
    outList->count = 0u;
    const LaiueContentServiceV1 *content = g_content;
    if (catalog == NULL || content == NULL || content->enumerate == NULL ||
        content->releaseList == NULL)
        return 0u;
    LaiueContentList list;
    if (content->enumerate(catalog, LAIUE_CONTENT_MODEL_PACK, &list) == 0u)
        return 0u;
    const bool copied = CopyList(&list, outList);
    content->releaseList(&list);
    return copied ? 1u : 0u;
}

static uint32_t ActivatePack(LaiueContentCatalog *catalog, const wchar_t *packName)
{
    const LaiueContentServiceV1 *content = g_content;
    return catalog != NULL && content != NULL && content->setActivePack != NULL &&
                   content->setActivePack(catalog, LAIUE_CONTENT_MODEL_PACK, packName) != 0u
               ? 1u
               : 0u;
}

static void ReleaseList(LaiueModelListV1 *list)
{
    if (list == NULL)
        return;
    PlatformFree(list->entries);
    list->entries = NULL;
    list->count = 0u;
}

// Снимает одно расширение модели, если оно есть; у кэша их два.
static bool StripOneExtension(wchar_t *name)
{
    uint32_t length = 0u;
    while (name[length] != 0)
        ++length;
    for (uint32_t index = 0u; index < sizeof(g_modelExtensions) / sizeof(g_modelExtensions[0]);
         ++index)
    {
        const wchar_t *extension = g_modelExtensions[index];
        uint32_t extensionLength = 0u;
        while (extension[extensionLength] != 0)
            ++extensionLength;
        if (length <= extensionLength)
            continue;
        bool matches = true;
        for (uint32_t position = 0u; position < extensionLength && matches; ++position)
        {
            wchar_t character = name[length - extensionLength + position];
            if (character >= L'A' && character <= L'Z')
                character = (wchar_t)(character + (L'a' - L'A'));
            matches = character == extension[position];
        }
        if (matches)
        {
            name[length - extensionLength] = 0;
            return true;
        }
    }
    return false;
}

typedef struct ModelScan
{
    LaiueModelNameV1 *entries; // NULL на первом проходе: тогда идёт счёт
    uint32_t capacity;
    uint32_t count;
} ModelScan;

typedef struct ScanFrame
{
    PlatformDirectoryIterator iterator;
    PlatformDirectoryEntry entry;
    wchar_t childDirectory[LAIUE_CONTENT_PATH_CAPACITY];
    wchar_t childName[LAIUE_CONTENT_NAME_CAPACITY];
} ScanFrame;

static bool Append(wchar_t *destination, uint32_t capacity, const wchar_t *prefix,
                   const wchar_t *name, wchar_t separator)
{
    uint32_t length = 0u;
    while (prefix[length] != 0)
    {
        if (length + 1u >= capacity)
            return false;
        destination[length] = prefix[length];
        ++length;
    }
    if (length != 0u)
    {
        if (length + 1u >= capacity)
            return false;
        destination[length++] = separator;
    }
    for (uint32_t index = 0u; name[index] != 0; ++index)
    {
        if (length + 1u >= capacity)
            return false;
        destination[length++] = name[index];
    }
    destination[length] = 0;
    return true;
}

static void ScanModels(const wchar_t *directory, const wchar_t *prefix, uint32_t depth,
                       ModelScan *scan)
{
    if (depth >= LAIUE_CONTENT_PATH_SEGMENT_MAX || g_content == NULL ||
        g_content->nameIsSafe == NULL)
        return;
    // Состояние уровня живёт в куче: в no-CRT сборке кадр стека больше
    // страницы потребовал бы __chkstk, которого там нет.
    ScanFrame *frame = (ScanFrame *)PlatformAllocate(sizeof(*frame), false);
    if (frame == NULL || !PlatformDirectoryOpen(&frame->iterator, directory))
    {
        PlatformFree(frame);
        return;
    }
    while (PlatformDirectoryNext(&frame->iterator, &frame->entry))
    {
        if (frame->entry.isSymbolicLink || g_content->nameIsSafe(frame->entry.name) == 0u)
            continue;
        if (frame->entry.isDirectory)
        {
            if (Append(frame->childName, LAIUE_CONTENT_NAME_CAPACITY, prefix, frame->entry.name,
                       L'/') &&
                Append(frame->childDirectory, LAIUE_CONTENT_PATH_CAPACITY, directory,
                       frame->entry.name, L'/'))
                ScanModels(frame->childDirectory, frame->childName, depth + 1u, scan);
            continue;
        }
        if (!Append(frame->childName, LAIUE_CONTENT_NAME_CAPACITY, prefix, frame->entry.name,
                    L'/') ||
            !StripOneExtension(frame->childName))
            continue;
        (void)StripOneExtension(frame->childName);
        if (scan->entries != NULL)
        {
            bool duplicate = false;
            for (uint32_t index = 0u; index < scan->count && !duplicate; ++index)
            {
                const wchar_t *existing = scan->entries[index].name;
                uint32_t position = 0u;
                while (existing[position] != 0 && existing[position] == frame->childName[position])
                    ++position;
                duplicate = existing[position] == frame->childName[position];
            }
            if (duplicate)
                continue;
            if (scan->count >= scan->capacity)
                break;
            memcpy(scan->entries[scan->count].name, frame->childName,
                   sizeof(scan->entries[scan->count].name));
        }
        ++scan->count;
    }
    PlatformDirectoryClose(&frame->iterator);
    PlatformFree(frame);
}

static uint32_t EnumerateModels(LaiueContentCatalog *catalog, LaiueModelListV1 *outList)
{
    if (outList == NULL)
        return 0u;
    outList->entries = NULL;
    outList->count = 0u;
    const LaiueContentServiceV1 *content = g_content;
    if (content == NULL || content->getActivePack == NULL || content->buildPath == NULL)
        return 0u;
    if (catalog == NULL && content->defaultCatalog != NULL)
        catalog = content->defaultCatalog();
    if (catalog == NULL)
        return 0u;
    wchar_t activeName[LAIUE_CONTENT_NAME_CAPACITY];
    if (content->getActivePack(catalog, LAIUE_CONTENT_MODEL_PACK, activeName,
                               LAIUE_CONTENT_NAME_CAPACITY) == 0u)
        return 1u; // активного пака нет — пустой список, а не ошибка
    wchar_t *root =
        (wchar_t *)PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    if (root == NULL)
        return 0u;
    if (content->buildPath(catalog, LAIUE_CONTENT_MODEL_PACK, activeName, NULL, root,
                           LAIUE_CONTENT_PATH_CAPACITY) == 0u)
    {
        PlatformFree(root);
        return 0u;
    }
    // Два прохода: счёт, затем заполнение — список выделяется один раз.
    ModelScan count = {NULL, 0u, 0u};
    ScanModels(root, L"", 0u, &count);
    if (count.count != 0u)
    {
        LaiueModelNameV1 *entries = (LaiueModelNameV1 *)PlatformAllocate(
            (size_t)count.count * sizeof(LaiueModelNameV1), true);
        if (entries == NULL)
        {
            PlatformFree(root);
            return 0u;
        }
        ModelScan fill = {entries, count.count, 0u};
        ScanModels(root, L"", 0u, &fill);
        outList->entries = entries;
        outList->count = fill.count;
    }
    PlatformFree(root);
    return 1u;
}

static const LaiueModelServiceV1 g_service = {
    .structSize = sizeof(LaiueModelServiceV1),
    .abiVersion = LAIUE_MODEL_SERVICE_ABI_VERSION_1,
    .enumeratePacks = EnumeratePacks,
    .activatePack = ActivatePack,
    .enumerateModels = EnumerateModels,
    .releaseList = ReleaseList,
    .loadFrom = LoadFrom,
    .loadFile = LoadFile,
    .loadMemory = LoadMemory,
    .getView = GetView,
    .release = Release,
};

const LaiueModelServiceV1 *LaiueModelGetStaticServiceV1(void)
{
    return &g_service;
}
