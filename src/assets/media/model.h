#pragma once

#include <stdbool.h>
#include <stdint.h>

// Общий контракт моделей: собственный `.lo` и исходный Wavefront OBJ.
// Раскладка `.lo` описана в docs/modelpacks.md.
//
// Как и у звука, своей памяти библиотека не выделяет: ModelInspect
// сообщает ёмкости выходных массивов и объём рабочей памяти, ModelDecode
// пишет в буферы вызывающей стороны. Рабочий буфер обязан быть выровнен
// под указатель.
//
// Оси движка: Z вверх, правая система. OBJ по умолчанию считается
// Y-вверх (так его пишут Blender, Maya и большинство редакторов) и
// поворачивается к Z-вверх; координата v текстуры переворачивается, потому
// что у OBJ начало снизу, а у текстур движка сверху. `.lo` уже хранит оси
// движка, и параметры импорта к нему не применяются.

#define MODEL_LO_VERSION 1u
#define MODEL_LO_HEADER_BYTES 40u
#define MODEL_MAX_VERTICES (UINT32_C(1) << 22)
#define MODEL_MAX_INDICES (UINT32_C(3) << 22)
#define MODEL_MAX_PARTS 4096u
#define MODEL_MAX_BOXES 1024u
#define MODEL_MAX_FILE_BYTES (UINT32_C(256) << 20)
#define MODEL_MATERIAL_NAME_CAPACITY 128u

typedef enum ModelStatus
{
    MODEL_OK = 0,
    MODEL_INVALID_ARGUMENT,
    MODEL_NOT_RECOGNISED,
    MODEL_TRUNCATED,
    MODEL_CORRUPT,
    MODEL_UNSUPPORTED_FEATURE,
    MODEL_TOO_LARGE,
    MODEL_BUFFER_TOO_SMALL,
} ModelStatus;

typedef enum ModelFormat
{
    MODEL_FORMAT_UNKNOWN = 0,
    MODEL_FORMAT_LO,
    MODEL_FORMAT_OBJ,
} ModelFormat;

// Вершина модели: 36 байт без выравнивающих дыр, тот же порядок полей,
// что в секции VERT файла.
typedef struct ModelVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t colorRGBA;
} ModelVertex;

// Часть модели — непрерывный диапазон треугольников с одним материалом.
// Имя материала принадлежит приложению: как и у текстурпака, движок не
// знает, что такое «кора» или «камень», и только передаёт имя дальше.
typedef struct ModelPart
{
    uint32_t firstIndex;
    uint32_t indexCount;
    char material[MODEL_MATERIAL_NAME_CAPACITY];
} ModelPart;

// Коробка столкновений в осях модели. В OBJ её задаёт группа или объект с
// именем, начинающимся на COL_ или UCX_: такая геометрия не рисуется, а
// её габарит становится коробкой.
typedef struct ModelBox
{
    float center[3];
    float halfExtent[3];
    float rotation[4]; // единичный кватернион (x, y, z, w)
} ModelBox;

#define MODEL_FLAG_NORMALS UINT32_C(1) << 0
#define MODEL_FLAG_UV UINT32_C(1) << 1
#define MODEL_FLAG_COLORS UINT32_C(1) << 2

// Импорт исходника. Нулевой указатель на параметры выбирает умолчания:
// Y-вверх и переворот v.
#define MODEL_IMPORT_SOURCE_Z_UP UINT32_C(1) << 0
#define MODEL_IMPORT_KEEP_V UINT32_C(1) << 1

typedef struct ModelImportOptions
{
    uint32_t flags;
} ModelImportOptions;

typedef struct ModelInfo
{
    ModelFormat format;
    // Верхние границы: у OBJ точные числа известны только после
    // разбора, у `.lo` они совпадают с фактическими.
    uint32_t vertexCapacity;
    uint32_t indexCapacity;
    uint32_t partCapacity;
    uint32_t boxCapacity;
    // Рабочая память ModelDecode. Ноль означает, что она не нужна.
    uint32_t scratchBytes;
} ModelInfo;

typedef struct ModelData
{
    // Вход: массивы вызывающего ёмкостью из ModelInfo. Выход: число
    // записанных элементов.
    ModelVertex *vertices;
    uint32_t vertexCount;
    uint32_t *indices;
    uint32_t indexCount;
    ModelPart *parts;
    uint32_t partCount;
    ModelBox *boxes;
    uint32_t boxCount;
    float boundsMin[3];
    float boundsMax[3];
    uint32_t flags;
    // Отпечаток исходника, из которого собран `.lo`: время изменения и
    // размер. Нулевой размер — файл ни из чего не выведен.
    uint64_t sourceModifiedTime;
    uint32_t sourceSizeBytes;
} ModelData;

ModelFormat ModelProbe(const void *bytes, uint32_t sizeBytes);

ModelStatus ModelInspect(const void *bytes, uint32_t sizeBytes, ModelInfo *outInfo);

// Перед вызовом vertices/indices/parts/boxes обязаны указывать на массивы
// ёмкостью не меньше значений из info (boxes может быть NULL при нулевой
// ёмкости).
ModelStatus ModelDecode(const void *bytes, uint32_t sizeBytes, const ModelImportOptions *options,
                        const ModelInfo *info, ModelData *inOutModel, void *scratch,
                        uint32_t scratchBytes);

// Проверяет модель так же строго, как разбор `.lo`: индексы в пределах,
// конечные числа, части внутри индексного буфера, единичные кватернионы.
ModelStatus ModelValidate(const ModelData *model);

// Точный размер `.lo` для модели и запись в буфер вызывающей стороны.
ModelStatus ModelEncodedBytes(const ModelData *model, uint32_t *outBytes);
ModelStatus ModelEncode(const ModelData *model, void *outBytes, uint32_t capacityBytes,
                        uint32_t *outWritten);

const char *ModelStatusText(ModelStatus status);
const char *ModelFormatName(ModelFormat format);
