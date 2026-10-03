// Модели: разбор `.lo` и Wavefront OBJ, проверка и запись `.lo`.
//
// Ни одного выделения памяти и ни одного вызова CRT, кроме memcpy/memset:
// библиотека собирается и в no-CRT профиле Windows, и в офлайн-утилитах.
// Каждое число из файла проверяется до того, как станет размером,
// смещением или индексом.

#include "media/model.h"

#include <float.h>
#include <string.h>

_Static_assert(sizeof(ModelVertex) == 36u, "ModelVertex must match the VERT section record");

#define LO_MAGIC 0x31444D4Cu // 'L','M','D','1' little-endian
#define LO_TAG(a, b, c, d)                                                                         \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) |     \
     ((uint32_t)(uint8_t)(d) << 24))
#define LO_TAG_VERT LO_TAG('V', 'E', 'R', 'T')
#define LO_TAG_INDX LO_TAG('I', 'N', 'D', 'X')
#define LO_TAG_PART LO_TAG('P', 'A', 'R', 'T')
#define LO_TAG_STRS LO_TAG('S', 'T', 'R', 'S')
#define LO_TAG_BNDS LO_TAG('B', 'N', 'D', 'S')
#define LO_TAG_COLL LO_TAG('C', 'O', 'L', 'L')
#define LO_MAX_SECTIONS 16u
#define LO_SECTION_ENTRY_BYTES 16u
#define LO_VERTEX_BYTES 36u
#define LO_PART_BYTES 16u
#define LO_BOX_BYTES 40u
#define LO_BOUNDS_BYTES 24u
#define LO_MAX_STRING_BYTES 65536u
#define LO_KNOWN_FLAGS (MODEL_FLAG_NORMALS | MODEL_FLAG_UV | MODEL_FLAG_COLORS)

#define OBJ_MAX_LINE_BYTES 65536u
#define OBJ_NONE UINT32_MAX
#define MODEL_WHITE 0xFFFFFFFFu

// === Числа и байты ===

static uint16_t ReadU16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t ReadU32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t ReadU64(const uint8_t *bytes)
{
    return (uint64_t)ReadU32(bytes) | ((uint64_t)ReadU32(bytes + 4) << 32);
}

static float ReadF32(const uint8_t *bytes)
{
    const uint32_t bits = ReadU32(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void WriteU16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void WriteU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void WriteU64(uint8_t *bytes, uint64_t value)
{
    WriteU32(bytes, (uint32_t)value);
    WriteU32(bytes + 4, (uint32_t)(value >> 32));
}

static void WriteF32(uint8_t *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    WriteU32(bytes, bits);
}

static bool Finite(float value)
{
    return value == value && value <= FLT_MAX && value >= -FLT_MAX;
}

static bool FiniteArray(const float *values, uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index)
        if (!Finite(values[index]))
            return false;
    return true;
}

// Корень без math.h: media не тянет зависимостей ради одной нормализации.
// Метод Ньютона от оценки по степени двойки сходится за несколько шагов.
static double SquareRoot(double value)
{
    if (!(value > 0.0))
        return 0.0;
    double estimate = value >= 1.0 ? value : 1.0;
    while (estimate * estimate > value * 4.0)
        estimate *= 0.5;
    for (uint32_t step = 0u; step < 64u; ++step)
    {
        const double next = 0.5 * (estimate + value / estimate);
        if (next == estimate)
            break;
        estimate = next;
    }
    return estimate;
}

static void NormalizeOrUp(float vector[3])
{
    const double length = SquareRoot((double)vector[0] * vector[0] + (double)vector[1] * vector[1] +
                                     (double)vector[2] * vector[2]);
    if (!(length > 1e-20))
    {
        vector[0] = 0.0f;
        vector[1] = 0.0f;
        vector[2] = 1.0f;
        return;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        vector[axis] = (float)((double)vector[axis] / length);
}

static bool SameName(const char *left, const char *right)
{
    uint32_t index = 0u;
    while (left[index] != '\0' && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

static bool QuaternionIsUnit(const float rotation[4])
{
    const double lengthSquared =
        (double)rotation[0] * rotation[0] + (double)rotation[1] * rotation[1] +
        (double)rotation[2] * rotation[2] + (double)rotation[3] * rotation[3];
    return lengthSquared > 0.998 && lengthSquared < 1.002;
}

// === Проверка модели ===

static bool MaterialNameValid(const char *name, uint32_t *outLength)
{
    uint32_t length = 0u;
    while (length < MODEL_MATERIAL_NAME_CAPACITY && name[length] != '\0')
    {
        if ((uint8_t)name[length] < 0x20u || name[length] == 0x7F)
            return false;
        ++length;
    }
    if (length >= MODEL_MATERIAL_NAME_CAPACITY)
        return false;
    if (outLength != NULL)
        *outLength = length;
    return true;
}

ModelStatus ModelValidate(const ModelData *model)
{
    if (model == NULL || model->vertices == NULL || model->indices == NULL ||
        model->parts == NULL || (model->boxCount != 0u && model->boxes == NULL))
        return MODEL_INVALID_ARGUMENT;
    if (model->vertexCount == 0u || model->indexCount < 3u || model->indexCount % 3u != 0u ||
        model->partCount == 0u)
        return MODEL_CORRUPT;
    if (model->vertexCount > MODEL_MAX_VERTICES || model->indexCount > MODEL_MAX_INDICES ||
        model->partCount > MODEL_MAX_PARTS || model->boxCount > MODEL_MAX_BOXES)
        return MODEL_TOO_LARGE;
    if ((model->flags & ~LO_KNOWN_FLAGS) != 0u || !FiniteArray(model->boundsMin, 3u) ||
        !FiniteArray(model->boundsMax, 3u))
        return MODEL_CORRUPT;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (model->boundsMin[axis] > model->boundsMax[axis])
            return MODEL_CORRUPT;

    // Габарит обязан содержать все вершины: по нему режется видимость и
    // строится широкая фаза столкновений, и ложь в нём не видна до кадра,
    // в котором объект пропадёт.
    float tolerance[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        tolerance[axis] = (model->boundsMax[axis] - model->boundsMin[axis]) * 1e-4f + 1e-5f;
    for (uint32_t index = 0u; index < model->vertexCount; ++index)
    {
        const ModelVertex *vertex = &model->vertices[index];
        if (!FiniteArray(vertex->position, 3u) || !FiniteArray(vertex->normal, 3u) ||
            !FiniteArray(vertex->uv, 2u))
            return MODEL_CORRUPT;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (vertex->position[axis] < model->boundsMin[axis] - tolerance[axis] ||
                vertex->position[axis] > model->boundsMax[axis] + tolerance[axis])
                return MODEL_CORRUPT;
    }
    for (uint32_t index = 0u; index < model->indexCount; ++index)
        if (model->indices[index] >= model->vertexCount)
            return MODEL_CORRUPT;
    for (uint32_t part = 0u; part < model->partCount; ++part)
    {
        const ModelPart *entry = &model->parts[part];
        if (entry->indexCount == 0u || entry->indexCount % 3u != 0u ||
            entry->firstIndex % 3u != 0u || entry->firstIndex > model->indexCount ||
            entry->indexCount > model->indexCount - entry->firstIndex ||
            !MaterialNameValid(entry->material, NULL))
            return MODEL_CORRUPT;
    }
    for (uint32_t box = 0u; box < model->boxCount; ++box)
    {
        const ModelBox *entry = &model->boxes[box];
        if (!FiniteArray(entry->center, 3u) || !FiniteArray(entry->halfExtent, 3u) ||
            !FiniteArray(entry->rotation, 4u) || !QuaternionIsUnit(entry->rotation))
            return MODEL_CORRUPT;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!(entry->halfExtent[axis] > 0.0f))
                return MODEL_CORRUPT;
    }
    return MODEL_OK;
}

// === Раскладка `.lo` ===

typedef struct LoLayout
{
    const uint8_t *vertices;
    uint32_t vertexCount;
    const uint8_t *indices;
    uint32_t indexCount;
    const uint8_t *parts;
    uint32_t partCount;
    const uint8_t *strings;
    uint32_t stringBytes;
    const uint8_t *bounds;
    const uint8_t *boxes;
    uint32_t boxCount;
    uint32_t flags;
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
} LoLayout;

static ModelStatus LoParseLayout(const uint8_t *bytes, uint32_t sizeBytes, LoLayout *out)
{
    memset(out, 0, sizeof(*out));
    if (sizeBytes < 4u)
        return MODEL_TRUNCATED;
    if (ReadU32(bytes) != LO_MAGIC)
        return MODEL_NOT_RECOGNISED;
    if (sizeBytes < MODEL_LO_HEADER_BYTES)
        return MODEL_TRUNCATED;
    if (ReadU16(bytes + 4) != MODEL_LO_VERSION)
        return MODEL_UNSUPPORTED_FEATURE;
    if (ReadU16(bytes + 6) != MODEL_LO_HEADER_BYTES)
        return MODEL_CORRUPT;
    const uint32_t sectionCount = ReadU32(bytes + 8);
    const uint32_t fileBytes = ReadU32(bytes + 12);
    if (fileBytes > MODEL_MAX_FILE_BYTES)
        return MODEL_TOO_LARGE;
    if (fileBytes > sizeBytes)
        return MODEL_TRUNCATED;
    if (fileBytes != sizeBytes || sectionCount == 0u || sectionCount > LO_MAX_SECTIONS)
        return MODEL_CORRUPT;
    out->sourceModifiedTime = ReadU64(bytes + 16);
    out->sourceSizeBytes = ReadU32(bytes + 24);
    out->flags = ReadU32(bytes + 28);
    if ((out->flags & ~LO_KNOWN_FLAGS) != 0u || ReadU32(bytes + 32) != 0u ||
        ReadU32(bytes + 36) != 0u)
        return MODEL_CORRUPT;

    const uint64_t tableEnd =
        (uint64_t)MODEL_LO_HEADER_BYTES + (uint64_t)sectionCount * LO_SECTION_ENTRY_BYTES;
    if (tableEnd > sizeBytes)
        return MODEL_TRUNCATED;

    // Секции идут по возрастанию смещений и не перекрываются: у файла
    // одна каноническая раскладка, и каждый байт принадлежит не более
    // чем одной секции. Неизвестные теги пропускаются — так новая версия
    // кодировщика может добавить, например, скелет, не ломая старый
    // загрузчик.
    uint64_t previousEnd = tableEnd;
    for (uint32_t section = 0u; section < sectionCount; ++section)
    {
        const uint8_t *entry = bytes + MODEL_LO_HEADER_BYTES + section * LO_SECTION_ENTRY_BYTES;
        const uint32_t tag = ReadU32(entry);
        const uint32_t offset = ReadU32(entry + 4);
        const uint32_t length = ReadU32(entry + 8);
        const uint32_t count = ReadU32(entry + 12);
        if (offset % 4u != 0u || offset < previousEnd)
            return MODEL_CORRUPT;
        const uint64_t end = (uint64_t)offset + length;
        if (end > sizeBytes)
            return MODEL_TRUNCATED;
        previousEnd = end;
        const uint8_t *payload = bytes + offset;
        switch (tag)
        {
        case LO_TAG_VERT:
            if (out->vertices != NULL)
                return MODEL_CORRUPT;
            if (count > MODEL_MAX_VERTICES)
                return MODEL_TOO_LARGE;
            if (count == 0u || (uint64_t)count * LO_VERTEX_BYTES != length)
                return MODEL_CORRUPT;
            out->vertices = payload;
            out->vertexCount = count;
            break;
        case LO_TAG_INDX:
            if (out->indices != NULL)
                return MODEL_CORRUPT;
            if (count > MODEL_MAX_INDICES)
                return MODEL_TOO_LARGE;
            if (count < 3u || count % 3u != 0u || (uint64_t)count * 4u != length)
                return MODEL_CORRUPT;
            out->indices = payload;
            out->indexCount = count;
            break;
        case LO_TAG_PART:
            if (out->parts != NULL)
                return MODEL_CORRUPT;
            if (count > MODEL_MAX_PARTS)
                return MODEL_TOO_LARGE;
            if (count == 0u || (uint64_t)count * LO_PART_BYTES != length)
                return MODEL_CORRUPT;
            out->parts = payload;
            out->partCount = count;
            break;
        case LO_TAG_STRS:
            if (out->strings != NULL || count != 0u || length > LO_MAX_STRING_BYTES)
                return MODEL_CORRUPT;
            out->strings = payload;
            out->stringBytes = length;
            break;
        case LO_TAG_BNDS:
            if (out->bounds != NULL || count != 1u || length != LO_BOUNDS_BYTES)
                return MODEL_CORRUPT;
            out->bounds = payload;
            break;
        case LO_TAG_COLL:
            if (out->boxes != NULL)
                return MODEL_CORRUPT;
            if (count > MODEL_MAX_BOXES)
                return MODEL_TOO_LARGE;
            if ((uint64_t)count * LO_BOX_BYTES != length)
                return MODEL_CORRUPT;
            out->boxes = payload;
            out->boxCount = count;
            break;
        default:
            break;
        }
    }
    if (out->vertices == NULL || out->indices == NULL || out->parts == NULL || out->bounds == NULL)
        return MODEL_CORRUPT;
    return MODEL_OK;
}

static ModelStatus LoDecode(const uint8_t *bytes, uint32_t sizeBytes, const ModelInfo *info,
                            ModelData *model)
{
    LoLayout layout;
    ModelStatus status = LoParseLayout(bytes, sizeBytes, &layout);
    if (status != MODEL_OK)
        return status;
    if (layout.vertexCount > info->vertexCapacity || layout.indexCount > info->indexCapacity ||
        layout.partCount > info->partCapacity || layout.boxCount > info->boxCapacity)
        return MODEL_BUFFER_TOO_SMALL;

    for (uint32_t index = 0u; index < layout.vertexCount; ++index)
    {
        const uint8_t *record = layout.vertices + (size_t)index * LO_VERTEX_BYTES;
        ModelVertex *vertex = &model->vertices[index];
        for (uint32_t component = 0u; component < 3u; ++component)
        {
            vertex->position[component] = ReadF32(record + component * 4u);
            vertex->normal[component] = ReadF32(record + 12u + component * 4u);
        }
        vertex->uv[0] = ReadF32(record + 24u);
        vertex->uv[1] = ReadF32(record + 28u);
        vertex->colorRGBA = ReadU32(record + 32u);
    }
    for (uint32_t index = 0u; index < layout.indexCount; ++index)
        model->indices[index] = ReadU32(layout.indices + (size_t)index * 4u);
    for (uint32_t part = 0u; part < layout.partCount; ++part)
    {
        const uint8_t *record = layout.parts + (size_t)part * LO_PART_BYTES;
        ModelPart *entry = &model->parts[part];
        entry->firstIndex = ReadU32(record);
        entry->indexCount = ReadU32(record + 4);
        const uint32_t nameOffset = ReadU32(record + 8);
        const uint32_t nameBytes = ReadU32(record + 12);
        if (nameBytes >= MODEL_MATERIAL_NAME_CAPACITY ||
            (nameBytes != 0u && (layout.strings == NULL || nameOffset > layout.stringBytes ||
                                 nameBytes > layout.stringBytes - nameOffset)))
            return MODEL_CORRUPT;
        if (nameBytes != 0u)
            memcpy(entry->material, layout.strings + nameOffset, nameBytes);
        entry->material[nameBytes] = '\0';
        // Встроенный NUL сократил бы имя и склеил разные материалы.
        uint32_t length = 0u;
        if (!MaterialNameValid(entry->material, &length) || length != nameBytes)
            return MODEL_CORRUPT;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        model->boundsMin[axis] = ReadF32(layout.bounds + axis * 4u);
        model->boundsMax[axis] = ReadF32(layout.bounds + 12u + axis * 4u);
    }
    for (uint32_t box = 0u; box < layout.boxCount; ++box)
    {
        const uint8_t *record = layout.boxes + (size_t)box * LO_BOX_BYTES;
        ModelBox *entry = &model->boxes[box];
        for (uint32_t component = 0u; component < 3u; ++component)
        {
            entry->center[component] = ReadF32(record + component * 4u);
            entry->halfExtent[component] = ReadF32(record + 12u + component * 4u);
        }
        for (uint32_t component = 0u; component < 4u; ++component)
            entry->rotation[component] = ReadF32(record + 24u + component * 4u);
    }
    model->vertexCount = layout.vertexCount;
    model->indexCount = layout.indexCount;
    model->partCount = layout.partCount;
    model->boxCount = layout.boxCount;
    model->flags = layout.flags;
    model->sourceModifiedTime = layout.sourceModifiedTime;
    model->sourceSizeBytes = layout.sourceSizeBytes;
    return ModelValidate(model);
}

// === Wavefront OBJ ===

typedef struct ObjLine
{
    const uint8_t *text;
    uint32_t length;
} ObjLine;

typedef struct ObjReader
{
    const uint8_t *bytes;
    uint32_t sizeBytes;
    uint32_t position;
} ObjReader;

// Строка без перевода строки, без '\r' и без комментария. false — конец
// файла; слишком длинная строка отмечается length == UINT32_MAX.
static bool ObjNextLine(ObjReader *reader, ObjLine *out)
{
    if (reader->position >= reader->sizeBytes)
        return false;
    const uint32_t start = reader->position;
    uint32_t end = start;
    while (end < reader->sizeBytes && reader->bytes[end] != '\n')
        ++end;
    reader->position = end < reader->sizeBytes ? end + 1u : end;
    uint32_t length = end - start;
    if (length > OBJ_MAX_LINE_BYTES)
    {
        out->text = reader->bytes + start;
        out->length = UINT32_MAX;
        return true;
    }
    for (uint32_t index = 0u; index < length; ++index)
        if (reader->bytes[start + index] == '#')
        {
            length = index;
            break;
        }
    while (length != 0u && (reader->bytes[start + length - 1u] == '\r' ||
                            reader->bytes[start + length - 1u] == ' ' ||
                            reader->bytes[start + length - 1u] == '\t'))
        --length;
    out->text = reader->bytes + start;
    out->length = length;
    return true;
}

static bool ObjIsSpace(uint8_t character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\f' ||
           character == '\v';
}

typedef struct ObjCursor
{
    const uint8_t *at;
    const uint8_t *end;
} ObjCursor;

static void ObjSkipSpace(ObjCursor *cursor)
{
    while (cursor->at < cursor->end && ObjIsSpace(*cursor->at))
        ++cursor->at;
}

static bool ObjToken(ObjCursor *cursor, const uint8_t **outStart, uint32_t *outLength)
{
    ObjSkipSpace(cursor);
    if (cursor->at >= cursor->end)
        return false;
    const uint8_t *start = cursor->at;
    while (cursor->at < cursor->end && !ObjIsSpace(*cursor->at))
        ++cursor->at;
    *outStart = start;
    *outLength = (uint32_t)(cursor->at - start);
    return true;
}

static bool ObjTokenIs(const uint8_t *token, uint32_t length, const char *keyword)
{
    uint32_t index = 0u;
    while (keyword[index] != '\0')
    {
        if (index >= length || token[index] != (uint8_t)keyword[index])
            return false;
        ++index;
    }
    return index == length;
}

static const double OBJ_POWERS_OF_TEN[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// Десятичное число OBJ: знак, цифры, точка, показатель. Без strtod — его
// нет в no-CRT профиле. Точности double с таблицей степеней хватает для
// float с запасом; inf, nan и шестнадцатеричная запись отвергаются.
static bool ObjParseNumber(const uint8_t *text, uint32_t length, float *out)
{
    uint32_t index = 0u;
    bool negative = false;
    if (index < length && (text[index] == '+' || text[index] == '-'))
        negative = text[index++] == '-';
    uint64_t mantissa = 0u;
    int32_t exponent = 0;
    uint32_t digits = 0u;
    while (index < length && text[index] >= '0' && text[index] <= '9')
    {
        if (mantissa < UINT64_C(100000000000000000))
            mantissa = mantissa * 10u + (uint64_t)(text[index] - '0');
        else
            ++exponent;
        ++digits;
        ++index;
    }
    if (index < length && text[index] == '.')
    {
        ++index;
        while (index < length && text[index] >= '0' && text[index] <= '9')
        {
            if (mantissa < UINT64_C(100000000000000000))
            {
                mantissa = mantissa * 10u + (uint64_t)(text[index] - '0');
                --exponent;
            }
            ++digits;
            ++index;
        }
    }
    if (digits == 0u)
        return false;
    if (index < length && (text[index] == 'e' || text[index] == 'E'))
    {
        ++index;
        bool exponentNegative = false;
        if (index < length && (text[index] == '+' || text[index] == '-'))
            exponentNegative = text[index++] == '-';
        uint32_t exponentDigits = 0u;
        int32_t written = 0;
        while (index < length && text[index] >= '0' && text[index] <= '9')
        {
            if (written < 100000)
                written = written * 10 + (int32_t)(text[index] - '0');
            ++exponentDigits;
            ++index;
        }
        if (exponentDigits == 0u)
            return false;
        exponent += exponentNegative ? -written : written;
    }
    if (index != length)
        return false;
    double value = (double)mantissa;
    if (mantissa != 0u)
    {
        if (exponent > 60 || exponent < -80)
            return exponent < 0 ? (*out = negative ? -0.0f : 0.0f, true) : false;
        int32_t remaining = exponent;
        while (remaining > 22)
        {
            value *= 1e22;
            remaining -= 22;
        }
        while (remaining < -22)
        {
            value /= 1e22;
            remaining += 22;
        }
        value = remaining >= 0 ? value * OBJ_POWERS_OF_TEN[remaining]
                               : value / OBJ_POWERS_OF_TEN[-remaining];
    }
    if (value > (double)FLT_MAX)
        return false;
    *out = negative ? (float)-value : (float)value;
    return true;
}

static uint32_t ObjNumbers(ObjCursor *cursor, float *values, uint32_t capacity, bool *outValid)
{
    uint32_t count = 0u;
    const uint8_t *token = NULL;
    uint32_t length = 0u;
    *outValid = true;
    while (ObjToken(cursor, &token, &length))
    {
        float value = 0.0f;
        if (!ObjParseNumber(token, length, &value))
        {
            *outValid = false;
            return count;
        }
        if (count < capacity)
            values[count] = value;
        ++count;
    }
    return count;
}

// Индекс OBJ: положительный считается от 1, отрицательный — назад от
// последнего уже объявленного элемента.
static bool ObjResolveIndex(const uint8_t *text, uint32_t length, uint32_t definedSoFar,
                            uint32_t total, uint32_t *out)
{
    if (length == 0u)
        return false;
    uint32_t index = 0u;
    bool negative = false;
    if (text[0] == '-')
    {
        negative = true;
        index = 1u;
    }
    if (index >= length)
        return false;
    uint64_t value = 0u;
    for (; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
            return false;
        value = value * 10u + (uint64_t)(text[index] - '0');
        if (value > UINT32_MAX)
            return false;
    }
    if (value == 0u)
        return false;
    if (negative)
    {
        if (value > definedSoFar)
            return false;
        *out = definedSoFar - (uint32_t)value;
    }
    else
    {
        if (value > total)
            return false;
        *out = (uint32_t)(value - 1u);
    }
    return true;
}

static bool ObjNameIsCollision(const uint8_t *name, uint32_t length)
{
    if (length < 4u)
        return false;
    uint8_t prefix[4];
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        uint8_t character = name[index];
        if (character >= 'a' && character <= 'z')
            character = (uint8_t)(character - 'a' + 'A');
        prefix[index] = character;
    }
    return (prefix[0] == 'C' && prefix[1] == 'O' && prefix[2] == 'L' && prefix[3] == '_') ||
           (prefix[0] == 'U' && prefix[1] == 'C' && prefix[2] == 'X' && prefix[3] == '_');
}

// Остаток строки после ключевого слова: имя материала или группы.
static void ObjRestOfLine(ObjCursor *cursor, const uint8_t **outStart, uint32_t *outLength)
{
    ObjSkipSpace(cursor);
    *outStart = cursor->at;
    *outLength = (uint32_t)(cursor->end - cursor->at);
}

typedef struct ObjCounts
{
    uint32_t positions;
    uint32_t uvs;
    uint32_t normals;
    uint32_t corners;
    uint32_t materials;
    uint32_t collisionGroups;
    bool colors;
} ObjCounts;

static ModelStatus ObjCount(const uint8_t *bytes, uint32_t sizeBytes, ObjCounts *out)
{
    memset(out, 0, sizeof(*out));
    ObjReader reader = {bytes, sizeBytes, 0u};
    ObjLine line;
    while (ObjNextLine(&reader, &line))
    {
        if (line.length == UINT32_MAX)
            return MODEL_TOO_LARGE;
        ObjCursor cursor = {line.text, line.text + line.length};
        const uint8_t *keyword = NULL;
        uint32_t keywordLength = 0u;
        if (!ObjToken(&cursor, &keyword, &keywordLength))
            continue;
        if (ObjTokenIs(keyword, keywordLength, "v"))
        {
            if (++out->positions > MODEL_MAX_VERTICES)
                return MODEL_TOO_LARGE;
            float values[7];
            bool valid = false;
            const uint32_t count = ObjNumbers(&cursor, values, 7u, &valid);
            if (!valid || count < 3u || count == 5u || count > 7u)
                return MODEL_CORRUPT;
            out->colors = out->colors || count >= 6u;
        }
        else if (ObjTokenIs(keyword, keywordLength, "vt"))
        {
            if (++out->uvs > MODEL_MAX_VERTICES)
                return MODEL_TOO_LARGE;
        }
        else if (ObjTokenIs(keyword, keywordLength, "vn"))
        {
            if (++out->normals > MODEL_MAX_VERTICES)
                return MODEL_TOO_LARGE;
        }
        else if (ObjTokenIs(keyword, keywordLength, "f"))
        {
            uint32_t corners = 0u;
            const uint8_t *token = NULL;
            uint32_t length = 0u;
            while (ObjToken(&cursor, &token, &length))
                ++corners;
            if (corners < 3u)
                return MODEL_CORRUPT;
            const uint64_t total = (uint64_t)out->corners + 3u * (uint64_t)(corners - 2u);
            if (total > MODEL_MAX_INDICES)
                return MODEL_TOO_LARGE;
            out->corners = (uint32_t)total;
        }
        else if (ObjTokenIs(keyword, keywordLength, "usemtl"))
        {
            if (++out->materials >= MODEL_MAX_PARTS)
                return MODEL_TOO_LARGE;
        }
        else if (ObjTokenIs(keyword, keywordLength, "o") || ObjTokenIs(keyword, keywordLength, "g"))
        {
            const uint8_t *name = NULL;
            uint32_t nameLength = 0u;
            ObjRestOfLine(&cursor, &name, &nameLength);
            if (ObjNameIsCollision(name, nameLength) && ++out->collisionGroups > MODEL_MAX_BOXES)
                return MODEL_TOO_LARGE;
        }
        // Остальное — s, mtllib, l, p и кривые — на треугольную сетку не
        // влияет и пропускается.
    }
    if (out->positions == 0u || out->corners < 3u)
        return MODEL_CORRUPT;
    return MODEL_OK;
}

static bool AddBytes(uint64_t *total, uint64_t count, uint64_t elementBytes)
{
    *total += count * elementBytes;
    // Выравнивание каждого массива под указатель.
    *total = (*total + 7u) & ~(uint64_t)7u;
    return *total <= UINT32_MAX;
}

static ModelStatus ObjInfoFromCounts(const ObjCounts *countsIn, ModelInfo *info)
{
    const ObjCounts counts = *countsIn;
    memset(info, 0, sizeof(*info));
    info->format = MODEL_FORMAT_OBJ;
    info->vertexCapacity =
        counts.corners < MODEL_MAX_VERTICES ? counts.corners : MODEL_MAX_VERTICES;
    info->indexCapacity = counts.corners;
    info->partCapacity = counts.materials + 1u;
    info->boxCapacity = counts.collisionGroups;
    uint64_t scratch = 0u;
    if (!AddBytes(&scratch, counts.positions, 12u) || // позиции
        !AddBytes(&scratch, counts.positions, 4u) ||  // голова цепочки вершин
        !AddBytes(&scratch, counts.colors ? counts.positions : 0u, 4u) ||
        !AddBytes(&scratch, counts.uvs, 8u) || !AddBytes(&scratch, counts.normals, 12u) ||
        !AddBytes(&scratch, info->vertexCapacity, 12u) || // ключ и связь вершины
        !AddBytes(&scratch, counts.collisionGroups, 24u)) // габариты групп
        return MODEL_TOO_LARGE;
    info->scratchBytes = (uint32_t)scratch;
    return MODEL_OK;
}

static ModelStatus ObjInspect(const uint8_t *bytes, uint32_t sizeBytes, ModelInfo *info)
{
    ObjCounts counts;
    const ModelStatus status = ObjCount(bytes, sizeBytes, &counts);
    return status == MODEL_OK ? ObjInfoFromCounts(&counts, info) : status;
}

typedef struct ObjVertexKey
{
    uint32_t uv;
    uint32_t normal;
    uint32_t next;
} ObjVertexKey;

typedef struct ObjScratch
{
    float *positions;
    uint32_t *chainHead;
    uint32_t *colors;
    float *uvs;
    float *normals;
    ObjVertexKey *keys;
    float *groupBounds; // min[3], max[3] на группу столкновений
} ObjScratch;

static void *ScratchTake(uint8_t **cursor, uint64_t count, uint64_t elementBytes)
{
    void *start = *cursor;
    uint64_t bytes = (count * elementBytes + 7u) & ~(uint64_t)7u;
    *cursor += bytes;
    return start;
}

typedef struct ObjBuild
{
    const ModelInfo *info;
    ModelData *model;
    ObjScratch scratch;
    ObjCounts counts;
    bool swapUp;
    bool flipV;
    bool usedUv;
    uint32_t partStart;
    char partName[MODEL_MATERIAL_NAME_CAPACITY];
    bool collision;
    uint32_t box; // текущая группа столкновений
} ObjBuild;

static void ObjClosePart(ObjBuild *build)
{
    ModelData *model = build->model;
    if (model->indexCount == build->partStart)
        return;
    // Соседние части с одним материалом склеиваются: так переключение
    // usemtl туда-обратно не дробит отрисовку без нужды.
    if (model->partCount != 0u)
    {
        ModelPart *last = &model->parts[model->partCount - 1u];
        if (SameName(last->material, build->partName) &&
            last->firstIndex + last->indexCount == build->partStart)
        {
            last->indexCount = model->indexCount - last->firstIndex;
            build->partStart = model->indexCount;
            return;
        }
    }
    ModelPart *part = &model->parts[model->partCount++];
    part->firstIndex = build->partStart;
    part->indexCount = model->indexCount - build->partStart;
    memcpy(part->material, build->partName, sizeof(part->material));
    build->partStart = model->indexCount;
}

static uint32_t ObjVertexFor(ObjBuild *build, uint32_t position, uint32_t uv, uint32_t normal,
                             ModelStatus *status)
{
    ModelData *model = build->model;
    ObjScratch *scratch = &build->scratch;
    for (uint32_t vertex = scratch->chainHead[position]; vertex != OBJ_NONE;
         vertex = scratch->keys[vertex].next)
        if (scratch->keys[vertex].uv == uv && scratch->keys[vertex].normal == normal)
            return vertex;
    if (model->vertexCount >= build->info->vertexCapacity)
    {
        *status = MODEL_TOO_LARGE;
        return OBJ_NONE;
    }
    const uint32_t vertex = model->vertexCount++;
    scratch->keys[vertex].uv = uv;
    scratch->keys[vertex].normal = normal;
    scratch->keys[vertex].next = scratch->chainHead[position];
    scratch->chainHead[position] = vertex;
    ModelVertex *out = &model->vertices[vertex];
    memcpy(out->position, scratch->positions + (size_t)position * 3u, sizeof(out->position));
    if (normal != OBJ_NONE)
        memcpy(out->normal, scratch->normals + (size_t)normal * 3u, sizeof(out->normal));
    else
        memset(out->normal, 0, sizeof(out->normal));
    if (uv != OBJ_NONE)
    {
        out->uv[0] = scratch->uvs[(size_t)uv * 2u];
        out->uv[1] = scratch->uvs[(size_t)uv * 2u + 1u];
        build->usedUv = true;
    }
    else
    {
        out->uv[0] = 0.0f;
        out->uv[1] = 0.0f;
    }
    out->colorRGBA = scratch->colors != NULL ? scratch->colors[position] : MODEL_WHITE;
    return vertex;
}

// Нормаль грани с весом площади добавляется к вершинам без явной
// нормали: так гладкая поверхность без vn получает сглаженное освещение.
static void ObjAccumulateFaceNormal(ObjBuild *build, uint32_t a, uint32_t b, uint32_t c)
{
    ModelVertex *vertices = build->model->vertices;
    const float *pa = vertices[a].position;
    const float *pb = vertices[b].position;
    const float *pc = vertices[c].position;
    const double u[3] = {(double)pb[0] - pa[0], (double)pb[1] - pa[1], (double)pb[2] - pa[2]};
    const double v[3] = {(double)pc[0] - pa[0], (double)pc[1] - pa[1], (double)pc[2] - pa[2]};
    const double normal[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                              u[0] * v[1] - u[1] * v[0]};
    const uint32_t corners[3] = {a, b, c};
    for (uint32_t corner = 0u; corner < 3u; ++corner)
    {
        const uint32_t vertex = corners[corner];
        if (build->scratch.keys[vertex].normal != OBJ_NONE)
            continue;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double sum = (double)vertices[vertex].normal[axis] + normal[axis];
            vertices[vertex].normal[axis] =
                sum > (double)FLT_MAX ? FLT_MAX : (sum < -(double)FLT_MAX ? -FLT_MAX : (float)sum);
        }
    }
}

static void ObjStoreAxis(const ObjBuild *build, const float source[3], float *destination)
{
    if (build->swapUp)
    {
        // Y-вверх → Z-вверх поворотом вокруг X на +90°: определитель +1,
        // поэтому обход треугольников и их лицевая сторона не меняются.
        destination[0] = source[0];
        destination[1] = -source[2];
        destination[2] = source[1];
    }
    else
    {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
    }
}

static uint8_t ObjColorChannel(float value)
{
    if (!(value > 0.0f))
        return 0u;
    if (value >= 1.0f)
        return 255u;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static ModelStatus ObjParseElements(ObjBuild *build, const uint8_t *bytes, uint32_t sizeBytes)
{
    ObjReader reader = {bytes, sizeBytes, 0u};
    ObjLine line;
    uint32_t positions = 0u;
    uint32_t uvs = 0u;
    uint32_t normals = 0u;
    while (ObjNextLine(&reader, &line))
    {
        ObjCursor cursor = {line.text, line.text + line.length};
        const uint8_t *keyword = NULL;
        uint32_t keywordLength = 0u;
        if (!ObjToken(&cursor, &keyword, &keywordLength))
            continue;
        float values[7] = {0.0f};
        bool valid = false;
        if (ObjTokenIs(keyword, keywordLength, "v"))
        {
            const uint32_t count = ObjNumbers(&cursor, values, 7u, &valid);
            if (!valid || count < 3u || positions >= build->counts.positions)
                return MODEL_CORRUPT;
            ObjStoreAxis(build, values, build->scratch.positions + (size_t)positions * 3u);
            if (build->scratch.colors != NULL)
                build->scratch.colors[positions] =
                    count >= 6u
                        ? (uint32_t)ObjColorChannel(values[3]) |
                              ((uint32_t)ObjColorChannel(values[4]) << 8) |
                              ((uint32_t)ObjColorChannel(values[5]) << 16) | UINT32_C(0xFF000000)
                        : MODEL_WHITE;
            ++positions;
        }
        else if (ObjTokenIs(keyword, keywordLength, "vt"))
        {
            const uint32_t count = ObjNumbers(&cursor, values, 3u, &valid);
            if (!valid || count < 1u || count > 3u || uvs >= build->counts.uvs)
                return MODEL_CORRUPT;
            build->scratch.uvs[(size_t)uvs * 2u] = values[0];
            build->scratch.uvs[(size_t)uvs * 2u + 1u] = build->flipV ? 1.0f - values[1] : values[1];
            if (!Finite(build->scratch.uvs[(size_t)uvs * 2u + 1u]))
                return MODEL_CORRUPT;
            ++uvs;
        }
        else if (ObjTokenIs(keyword, keywordLength, "vn"))
        {
            const uint32_t count = ObjNumbers(&cursor, values, 3u, &valid);
            if (!valid || count != 3u || normals >= build->counts.normals)
                return MODEL_CORRUPT;
            float *normal = build->scratch.normals + (size_t)normals * 3u;
            ObjStoreAxis(build, values, normal);
            NormalizeOrUp(normal);
            ++normals;
        }
    }
    return positions == build->counts.positions && uvs == build->counts.uvs &&
                   normals == build->counts.normals
               ? MODEL_OK
               : MODEL_CORRUPT;
}

static ModelStatus ObjFace(ObjBuild *build, ObjCursor *cursor, uint32_t positions, uint32_t uvs,
                           uint32_t normals)
{
    ModelData *model = build->model;
    uint32_t first = OBJ_NONE;
    uint32_t previous = OBJ_NONE;
    uint32_t corner = 0u;
    const uint8_t *token = NULL;
    uint32_t length = 0u;
    while (ObjToken(cursor, &token, &length))
    {
        // v, v/vt, v//vn или v/vt/vn.
        const uint8_t *fields[3] = {token, NULL, NULL};
        uint32_t lengths[3] = {length, 0u, 0u};
        uint32_t fieldCount = 1u;
        for (uint32_t index = 0u; index < length; ++index)
            if (token[index] == '/')
            {
                if (fieldCount >= 3u)
                    return MODEL_CORRUPT;
                lengths[fieldCount - 1u] = (uint32_t)(token + index - fields[fieldCount - 1u]);
                fields[fieldCount] = token + index + 1u;
                lengths[fieldCount] = (uint32_t)(token + length - fields[fieldCount]);
                ++fieldCount;
            }
        uint32_t position = OBJ_NONE;
        uint32_t uv = OBJ_NONE;
        uint32_t normal = OBJ_NONE;
        if (!ObjResolveIndex(fields[0], lengths[0], positions, build->counts.positions, &position))
            return MODEL_CORRUPT;
        if (fieldCount >= 2u && lengths[1] != 0u &&
            !ObjResolveIndex(fields[1], lengths[1], uvs, build->counts.uvs, &uv))
            return MODEL_CORRUPT;
        if (fieldCount == 3u &&
            !ObjResolveIndex(fields[2], lengths[2], normals, build->counts.normals, &normal))
            return MODEL_CORRUPT;

        if (build->collision)
        {
            float *bounds = build->scratch.groupBounds + (size_t)build->box * 6u;
            const float *point = build->scratch.positions + (size_t)position * 3u;
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                if (point[axis] < bounds[axis])
                    bounds[axis] = point[axis];
                if (point[axis] > bounds[3u + axis])
                    bounds[3u + axis] = point[axis];
            }
            ++corner;
            continue;
        }

        ModelStatus status = MODEL_OK;
        const uint32_t vertex = ObjVertexFor(build, position, uv, normal, &status);
        if (status != MODEL_OK)
            return status;
        if (corner == 0u)
            first = vertex;
        else if (corner >= 2u)
        {
            // Многоугольник режется веером от первой вершины. Для выпуклых
            // граней, которые пишут редакторы, это точная триангуляция.
            if (model->indexCount + 3u > build->info->indexCapacity)
                return MODEL_CORRUPT;
            model->indices[model->indexCount++] = first;
            model->indices[model->indexCount++] = previous;
            model->indices[model->indexCount++] = vertex;
            ObjAccumulateFaceNormal(build, first, previous, vertex);
        }
        previous = vertex;
        ++corner;
    }
    return corner >= 3u ? MODEL_OK : MODEL_CORRUPT;
}

static ModelStatus ObjParseFaces(ObjBuild *build, const uint8_t *bytes, uint32_t sizeBytes)
{
    ObjReader reader = {bytes, sizeBytes, 0u};
    ObjLine line;
    uint32_t positions = 0u;
    uint32_t uvs = 0u;
    uint32_t normals = 0u;
    uint32_t groups = 0u;
    while (ObjNextLine(&reader, &line))
    {
        ObjCursor cursor = {line.text, line.text + line.length};
        const uint8_t *keyword = NULL;
        uint32_t keywordLength = 0u;
        if (!ObjToken(&cursor, &keyword, &keywordLength))
            continue;
        if (ObjTokenIs(keyword, keywordLength, "v"))
            ++positions;
        else if (ObjTokenIs(keyword, keywordLength, "vt"))
            ++uvs;
        else if (ObjTokenIs(keyword, keywordLength, "vn"))
            ++normals;
        else if (ObjTokenIs(keyword, keywordLength, "f"))
        {
            const ModelStatus status = ObjFace(build, &cursor, positions, uvs, normals);
            if (status != MODEL_OK)
                return status;
        }
        else if (ObjTokenIs(keyword, keywordLength, "usemtl"))
        {
            const uint8_t *name = NULL;
            uint32_t nameLength = 0u;
            ObjRestOfLine(&cursor, &name, &nameLength);
            if (nameLength >= MODEL_MATERIAL_NAME_CAPACITY)
                return MODEL_TOO_LARGE;
            ObjClosePart(build);
            memset(build->partName, 0, sizeof(build->partName));
            memcpy(build->partName, name, nameLength);
            if (!MaterialNameValid(build->partName, NULL))
                return MODEL_CORRUPT;
        }
        else if (ObjTokenIs(keyword, keywordLength, "o") || ObjTokenIs(keyword, keywordLength, "g"))
        {
            const uint8_t *name = NULL;
            uint32_t nameLength = 0u;
            ObjRestOfLine(&cursor, &name, &nameLength);
            build->collision = ObjNameIsCollision(name, nameLength);
            if (build->collision)
            {
                if (groups >= build->counts.collisionGroups)
                    return MODEL_CORRUPT;
                build->box = groups++;
                float *bounds = build->scratch.groupBounds + (size_t)build->box * 6u;
                for (uint32_t axis = 0u; axis < 3u; ++axis)
                {
                    bounds[axis] = FLT_MAX;
                    bounds[3u + axis] = -FLT_MAX;
                }
            }
        }
    }
    ObjClosePart(build);
    return MODEL_OK;
}

static ModelStatus ObjDecode(const uint8_t *bytes, uint32_t sizeBytes,
                             const ModelImportOptions *options, const ModelInfo *info,
                             ModelData *model, void *scratch, uint32_t scratchBytes)
{
    ObjBuild build;
    memset(&build, 0, sizeof(build));
    ModelStatus status = ObjCount(bytes, sizeBytes, &build.counts);
    if (status != MODEL_OK)
        return status;
    ModelInfo expected;
    status = ObjInfoFromCounts(&build.counts, &expected);
    if (status != MODEL_OK)
        return status;
    if (info->vertexCapacity < expected.vertexCapacity ||
        info->indexCapacity < expected.indexCapacity ||
        info->partCapacity < expected.partCapacity || info->boxCapacity < expected.boxCapacity ||
        scratchBytes < expected.scratchBytes || (expected.scratchBytes != 0u && scratch == NULL))
        return MODEL_BUFFER_TOO_SMALL;

    build.info = &expected;
    build.model = model;
    const uint32_t flags = options != NULL ? options->flags : 0u;
    build.swapUp = (flags & MODEL_IMPORT_SOURCE_Z_UP) == 0u;
    build.flipV = (flags & MODEL_IMPORT_KEEP_V) == 0u;

    uint8_t *cursor = (uint8_t *)scratch;
    build.scratch.positions = ScratchTake(&cursor, build.counts.positions, 12u);
    build.scratch.chainHead = ScratchTake(&cursor, build.counts.positions, 4u);
    build.scratch.colors =
        build.counts.colors ? ScratchTake(&cursor, build.counts.positions, 4u) : NULL;
    build.scratch.uvs = ScratchTake(&cursor, build.counts.uvs, 8u);
    build.scratch.normals = ScratchTake(&cursor, build.counts.normals, 12u);
    build.scratch.keys = ScratchTake(&cursor, expected.vertexCapacity, 12u);
    build.scratch.groupBounds = ScratchTake(&cursor, build.counts.collisionGroups, 24u);
    for (uint32_t position = 0u; position < build.counts.positions; ++position)
        build.scratch.chainHead[position] = OBJ_NONE;

    model->vertexCount = 0u;
    model->indexCount = 0u;
    model->partCount = 0u;
    model->boxCount = 0u;
    status = ObjParseElements(&build, bytes, sizeBytes);
    if (status == MODEL_OK)
        status = ObjParseFaces(&build, bytes, sizeBytes);
    if (status != MODEL_OK)
        return status;
    if (model->indexCount < 3u || model->partCount == 0u)
        return MODEL_CORRUPT;

    for (uint32_t vertex = 0u; vertex < model->vertexCount; ++vertex)
        if (build.scratch.keys[vertex].normal == OBJ_NONE)
            NormalizeOrUp(model->vertices[vertex].normal);

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        model->boundsMin[axis] = FLT_MAX;
        model->boundsMax[axis] = -FLT_MAX;
    }
    for (uint32_t vertex = 0u; vertex < model->vertexCount; ++vertex)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const float value = model->vertices[vertex].position[axis];
            if (value < model->boundsMin[axis])
                model->boundsMin[axis] = value;
            if (value > model->boundsMax[axis])
                model->boundsMax[axis] = value;
        }

    // Пустая группа столкновений не даёт коробки; у плоской группы
    // толщина подтягивается до миллиметра, чтобы коробка оставалась телом.
    for (uint32_t group = 0u; group < build.counts.collisionGroups; ++group)
    {
        const float *bounds = build.scratch.groupBounds + (size_t)group * 6u;
        if (bounds[0] > bounds[3])
            continue;
        ModelBox *box = &model->boxes[model->boxCount++];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            box->center[axis] = (float)(((double)bounds[axis] + bounds[3u + axis]) * 0.5);
            const float half = (float)(((double)bounds[3u + axis] - bounds[axis]) * 0.5);
            box->halfExtent[axis] = half > 0.0005f ? half : 0.0005f;
        }
        box->rotation[0] = 0.0f;
        box->rotation[1] = 0.0f;
        box->rotation[2] = 0.0f;
        box->rotation[3] = 1.0f;
    }

    model->flags = MODEL_FLAG_NORMALS | (build.usedUv ? MODEL_FLAG_UV : 0u) |
                   (build.counts.colors ? MODEL_FLAG_COLORS : 0u);
    model->sourceModifiedTime = 0u;
    model->sourceSizeBytes = 0u;
    return ModelValidate(model);
}

// === Публичные операции ===

static bool ObjKeywordStart(const uint8_t *bytes, uint32_t sizeBytes)
{
    // Первая значащая строка обязана начинаться с известного слова OBJ:
    // так произвольный текст или двоичный мусор не принимается за модель.
    ObjReader reader = {bytes, sizeBytes, 0u};
    ObjLine line;
    for (uint32_t lines = 0u; lines < 4096u && ObjNextLine(&reader, &line); ++lines)
    {
        if (line.length == UINT32_MAX)
            return false;
        ObjCursor cursor = {line.text, line.text + line.length};
        const uint8_t *keyword = NULL;
        uint32_t length = 0u;
        if (!ObjToken(&cursor, &keyword, &length))
            continue;
        static const char *const keywords[] = {
            "v", "vt", "vn", "f", "o", "g", "s", "usemtl", "mtllib",
        };
        for (uint32_t index = 0u; index < sizeof(keywords) / sizeof(keywords[0]); ++index)
            if (ObjTokenIs(keyword, length, keywords[index]))
                return true;
        return false;
    }
    return false;
}

ModelFormat ModelProbe(const void *bytes, uint32_t sizeBytes)
{
    if (bytes == NULL || sizeBytes < 4u)
        return MODEL_FORMAT_UNKNOWN;
    const uint8_t *data = (const uint8_t *)bytes;
    if (ReadU32(data) == LO_MAGIC)
        return MODEL_FORMAT_LO;
    return ObjKeywordStart(data, sizeBytes) ? MODEL_FORMAT_OBJ : MODEL_FORMAT_UNKNOWN;
}

ModelStatus ModelInspect(const void *bytes, uint32_t sizeBytes, ModelInfo *outInfo)
{
    if (outInfo != NULL)
        memset(outInfo, 0, sizeof(*outInfo));
    if (bytes == NULL || outInfo == NULL)
        return MODEL_INVALID_ARGUMENT;
    if (sizeBytes > MODEL_MAX_FILE_BYTES)
        return MODEL_TOO_LARGE;
    switch (ModelProbe(bytes, sizeBytes))
    {
    case MODEL_FORMAT_LO:
    {
        LoLayout layout;
        const ModelStatus status = LoParseLayout((const uint8_t *)bytes, sizeBytes, &layout);
        if (status != MODEL_OK)
            return status;
        outInfo->format = MODEL_FORMAT_LO;
        outInfo->vertexCapacity = layout.vertexCount;
        outInfo->indexCapacity = layout.indexCount;
        outInfo->partCapacity = layout.partCount;
        outInfo->boxCapacity = layout.boxCount;
        return MODEL_OK;
    }
    case MODEL_FORMAT_OBJ:
        return ObjInspect((const uint8_t *)bytes, sizeBytes, outInfo);
    default:
        return sizeBytes < 4u ? MODEL_TRUNCATED : MODEL_NOT_RECOGNISED;
    }
}

ModelStatus ModelDecode(const void *bytes, uint32_t sizeBytes, const ModelImportOptions *options,
                        const ModelInfo *info, ModelData *inOutModel, void *scratch,
                        uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || inOutModel == NULL || inOutModel->vertices == NULL ||
        inOutModel->indices == NULL || inOutModel->parts == NULL ||
        (info->boxCapacity != 0u && inOutModel->boxes == NULL))
        return MODEL_INVALID_ARGUMENT;
    if (sizeBytes > MODEL_MAX_FILE_BYTES)
        return MODEL_TOO_LARGE;
    switch (ModelProbe(bytes, sizeBytes))
    {
    case MODEL_FORMAT_LO:
        return LoDecode((const uint8_t *)bytes, sizeBytes, info, inOutModel);
    case MODEL_FORMAT_OBJ:
        return ObjDecode((const uint8_t *)bytes, sizeBytes, options, info, inOutModel, scratch,
                         scratchBytes);
    default:
        return sizeBytes < 4u ? MODEL_TRUNCATED : MODEL_NOT_RECOGNISED;
    }
}

typedef struct LoPlan
{
    uint32_t stringBytes;
    uint32_t sectionCount;
    uint32_t offsets[6];
    uint32_t lengths[6];
    uint32_t fileBytes;
} LoPlan;

static ModelStatus LoPlanLayout(const ModelData *model, LoPlan *plan)
{
    memset(plan, 0, sizeof(*plan));
    const ModelStatus status = ModelValidate(model);
    if (status != MODEL_OK)
        return status;
    uint64_t strings = 0u;
    for (uint32_t part = 0u; part < model->partCount; ++part)
    {
        uint32_t length = 0u;
        (void)MaterialNameValid(model->parts[part].material, &length);
        strings += length;
    }
    if (strings > LO_MAX_STRING_BYTES)
        return MODEL_TOO_LARGE;
    plan->stringBytes = (uint32_t)strings;
    plan->sectionCount = model->boxCount != 0u ? 6u : 5u;
    const uint64_t lengths[6] = {
        (uint64_t)model->vertexCount * LO_VERTEX_BYTES,
        (uint64_t)model->indexCount * 4u,
        (uint64_t)model->partCount * LO_PART_BYTES,
        strings,
        LO_BOUNDS_BYTES,
        (uint64_t)model->boxCount * LO_BOX_BYTES,
    };
    uint64_t offset =
        (uint64_t)MODEL_LO_HEADER_BYTES + (uint64_t)plan->sectionCount * LO_SECTION_ENTRY_BYTES;
    for (uint32_t section = 0u; section < plan->sectionCount; ++section)
    {
        offset = (offset + 3u) & ~(uint64_t)3u;
        plan->offsets[section] = (uint32_t)offset;
        plan->lengths[section] = (uint32_t)lengths[section];
        offset += lengths[section];
        if (offset > MODEL_MAX_FILE_BYTES)
            return MODEL_TOO_LARGE;
    }
    plan->fileBytes = (uint32_t)offset;
    return MODEL_OK;
}

ModelStatus ModelEncodedBytes(const ModelData *model, uint32_t *outBytes)
{
    if (outBytes == NULL)
        return MODEL_INVALID_ARGUMENT;
    *outBytes = 0u;
    LoPlan plan;
    const ModelStatus status = LoPlanLayout(model, &plan);
    if (status == MODEL_OK)
        *outBytes = plan.fileBytes;
    return status;
}

ModelStatus ModelEncode(const ModelData *model, void *outBytes, uint32_t capacityBytes,
                        uint32_t *outWritten)
{
    if (outWritten != NULL)
        *outWritten = 0u;
    if (outBytes == NULL)
        return MODEL_INVALID_ARGUMENT;
    LoPlan plan;
    const ModelStatus status = LoPlanLayout(model, &plan);
    if (status != MODEL_OK)
        return status;
    if (capacityBytes < plan.fileBytes)
        return MODEL_BUFFER_TOO_SMALL;

    uint8_t *bytes = (uint8_t *)outBytes;
    memset(bytes, 0, plan.fileBytes);
    WriteU32(bytes, LO_MAGIC);
    WriteU16(bytes + 4, MODEL_LO_VERSION);
    WriteU16(bytes + 6, MODEL_LO_HEADER_BYTES);
    WriteU32(bytes + 8, plan.sectionCount);
    WriteU32(bytes + 12, plan.fileBytes);
    WriteU64(bytes + 16, model->sourceModifiedTime);
    WriteU32(bytes + 24, model->sourceSizeBytes);
    WriteU32(bytes + 28, model->flags);

    static const uint32_t tags[6] = {LO_TAG_VERT, LO_TAG_INDX, LO_TAG_PART,
                                     LO_TAG_STRS, LO_TAG_BNDS, LO_TAG_COLL};
    const uint32_t counts[6] = {model->vertexCount, model->indexCount, model->partCount, 0u, 1u,
                                model->boxCount};
    for (uint32_t section = 0u; section < plan.sectionCount; ++section)
    {
        uint8_t *entry = bytes + MODEL_LO_HEADER_BYTES + section * LO_SECTION_ENTRY_BYTES;
        WriteU32(entry, tags[section]);
        WriteU32(entry + 4, plan.offsets[section]);
        WriteU32(entry + 8, plan.lengths[section]);
        WriteU32(entry + 12, counts[section]);
    }

    uint8_t *vertices = bytes + plan.offsets[0];
    for (uint32_t index = 0u; index < model->vertexCount; ++index)
    {
        const ModelVertex *vertex = &model->vertices[index];
        uint8_t *record = vertices + (size_t)index * LO_VERTEX_BYTES;
        for (uint32_t component = 0u; component < 3u; ++component)
        {
            WriteF32(record + component * 4u, vertex->position[component]);
            WriteF32(record + 12u + component * 4u, vertex->normal[component]);
        }
        WriteF32(record + 24u, vertex->uv[0]);
        WriteF32(record + 28u, vertex->uv[1]);
        WriteU32(record + 32u, vertex->colorRGBA);
    }
    uint8_t *indices = bytes + plan.offsets[1];
    for (uint32_t index = 0u; index < model->indexCount; ++index)
        WriteU32(indices + (size_t)index * 4u, model->indices[index]);
    uint8_t *parts = bytes + plan.offsets[2];
    uint8_t *strings = bytes + plan.offsets[3];
    uint32_t stringCursor = 0u;
    for (uint32_t part = 0u; part < model->partCount; ++part)
    {
        const ModelPart *entry = &model->parts[part];
        uint32_t length = 0u;
        (void)MaterialNameValid(entry->material, &length);
        uint8_t *record = parts + (size_t)part * LO_PART_BYTES;
        WriteU32(record, entry->firstIndex);
        WriteU32(record + 4, entry->indexCount);
        WriteU32(record + 8, length != 0u ? stringCursor : 0u);
        WriteU32(record + 12, length);
        memcpy(strings + stringCursor, entry->material, length);
        stringCursor += length;
    }
    uint8_t *bounds = bytes + plan.offsets[4];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        WriteF32(bounds + axis * 4u, model->boundsMin[axis]);
        WriteF32(bounds + 12u + axis * 4u, model->boundsMax[axis]);
    }
    if (plan.sectionCount == 6u)
    {
        uint8_t *boxes = bytes + plan.offsets[5];
        for (uint32_t box = 0u; box < model->boxCount; ++box)
        {
            const ModelBox *entry = &model->boxes[box];
            uint8_t *record = boxes + (size_t)box * LO_BOX_BYTES;
            for (uint32_t component = 0u; component < 3u; ++component)
            {
                WriteF32(record + component * 4u, entry->center[component]);
                WriteF32(record + 12u + component * 4u, entry->halfExtent[component]);
            }
            for (uint32_t component = 0u; component < 4u; ++component)
                WriteF32(record + 24u + component * 4u, entry->rotation[component]);
        }
    }
    if (outWritten != NULL)
        *outWritten = plan.fileBytes;
    return MODEL_OK;
}

const char *ModelStatusText(ModelStatus status)
{
    switch (status)
    {
    case MODEL_OK:
        return "ok";
    case MODEL_INVALID_ARGUMENT:
        return "invalid argument";
    case MODEL_NOT_RECOGNISED:
        return "not a model";
    case MODEL_TRUNCATED:
        return "file is truncated";
    case MODEL_CORRUPT:
        return "model is corrupt";
    case MODEL_UNSUPPORTED_FEATURE:
        return "unsupported model feature";
    case MODEL_TOO_LARGE:
        return "model exceeds engine limits";
    case MODEL_BUFFER_TOO_SMALL:
        return "output buffer is too small";
    }
    return "unknown status";
}

const char *ModelFormatName(ModelFormat format)
{
    switch (format)
    {
    case MODEL_FORMAT_LO:
        return "LO";
    case MODEL_FORMAT_OBJ:
        return "OBJ";
    case MODEL_FORMAT_UNKNOWN:
        break;
    }
    return "unknown";
}
