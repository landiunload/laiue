// Кодек моделей: разбор OBJ (оси, развёртка, нормали, части, коробки
// столкновений, относительные индексы), круговой проход через `.lo` и
// отказ на повреждённых файлах. Проверяется код, общий для пака моделей
// и офлайн-конвертера.

#include "media/model.h"

#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_VERTICES 256u
#define TEST_INDICES 768u
#define TEST_PARTS 16u
#define TEST_BOXES 8u
#define TEST_SCRATCH 65536u
#define TEST_FILE 65536u

static ModelVertex g_vertices[TEST_VERTICES];
static uint32_t g_indices[TEST_INDICES];
static ModelPart g_parts[TEST_PARTS];
static ModelBox g_boxes[TEST_BOXES];
static uint64_t g_scratch[TEST_SCRATCH / 8u];
static uint8_t g_file[TEST_FILE];

static ModelVertex g_vertices2[TEST_VERTICES];
static uint32_t g_indices2[TEST_INDICES];
static ModelPart g_parts2[TEST_PARTS];
static ModelBox g_boxes2[TEST_BOXES];

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Model codec check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t Length(const char *text)
{
    uint32_t length = 0u;
    while (text[length] != '\0')
        ++length;
    return length;
}

// strcmp нет в no-CRT профиле Windows, где тест тоже собирается.
static bool SameText(const char *left, const char *right)
{
    uint32_t index = 0u;
    while (left[index] != '\0' && left[index] == right[index])
        ++index;
    return left[index] == right[index];
}

static void PutU32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t GetU32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool Near(float left, float right)
{
    const float difference = left - right;
    return difference < 1e-5f && difference > -1e-5f;
}

static ModelStatus DecodeInto(const void *bytes, uint32_t size, uint32_t flags, ModelData *model,
                              ModelVertex *vertices, uint32_t *indices, ModelPart *parts,
                              ModelBox *boxes)
{
    ModelInfo info;
    ModelStatus status = ModelInspect(bytes, size, &info);
    if (status != MODEL_OK)
        return status;
    if (info.vertexCapacity > TEST_VERTICES || info.indexCapacity > TEST_INDICES ||
        info.partCapacity > TEST_PARTS || info.boxCapacity > TEST_BOXES ||
        info.scratchBytes > TEST_SCRATCH)
        return MODEL_BUFFER_TOO_SMALL;
    memset(model, 0, sizeof(*model));
    model->vertices = vertices;
    model->indices = indices;
    model->parts = parts;
    model->boxes = boxes;
    const ModelImportOptions options = {flags};
    return ModelDecode(bytes, size, &options, &info, model, g_scratch, TEST_SCRATCH);
}

static ModelStatus DecodeText(const char *text, uint32_t flags, ModelData *model)
{
    return DecodeInto(text, Length(text), flags, model, g_vertices, g_indices, g_parts, g_boxes);
}

static void CheckTriangleAndAxes(void)
{
    ModelData model;
    const char *triangle = "# одна грань\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    Expect(DecodeText(triangle, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK, "triangle decodes");
    Expect(model.vertexCount == 3u && model.indexCount == 3u && model.partCount == 1u &&
               model.boxCount == 0u,
           "triangle counts");
    Expect(Near(model.vertices[1].position[0], 1.0f) && Near(model.vertices[2].position[1], 1.0f),
           "Z-up source keeps axes");
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
        Expect(Near(model.vertices[vertex].normal[2], 1.0f) &&
                   model.vertices[vertex].colorRGBA == 0xFFFFFFFFu,
               "computed normal faces +Z and default color is white");
    Expect(model.parts[0].material[0] == '\0' && (model.flags & MODEL_FLAG_UV) == 0u,
           "faces before usemtl use the empty material and no UV flag");
    Expect(Near(model.boundsMax[0], 1.0f) && Near(model.boundsMax[1], 1.0f) &&
               Near(model.boundsMax[2], 0.0f),
           "bounds cover the triangle");

    // Y-вверх по умолчанию: (x, y, z) → (x, -z, y), обход сохраняется.
    const char *upright = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 2 4\n";
    Expect(DecodeText(upright, 0u, &model) == MODEL_OK, "Y-up triangle decodes");
    Expect(Near(model.vertices[2].position[2], 1.0f) && Near(model.vertices[2].position[1], 0.0f),
           "source +Y becomes engine +Z");
    Expect(Near(model.vertices[3].position[1], -1.0f), "source +Z becomes engine -Y");
}

static void CheckPolygonsAndIndices(void)
{
    ModelData model;
    const char *quad = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0.5 2 0\nf 1 2 3 4\n"
                       "f 1 2 3 4 5\n";
    Expect(DecodeText(quad, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK, "polygons decode");
    Expect(model.indexCount == 6u + 9u && model.vertexCount == 5u,
           "quad and pentagon are fanned into triangles over shared vertices");
    Expect(model.indices[0] == 0u && model.indices[1] == 1u && model.indices[2] == 2u &&
               model.indices[3] == 0u && model.indices[4] == 2u && model.indices[5] == 3u,
           "fan starts at the first corner");

    const char *relative = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\nv 5 5 5\nf -4 -3 -2\n";
    Expect(DecodeText(relative, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK &&
               model.indexCount == 6u && model.vertexCount == 3u,
           "negative indices count back from elements declared so far");

    // Два треугольника с общим ребром: одинаковые позиция, развёртка и
    // нормаль — одна вершина, иначе — разные.
    const char *shared = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\n"
                         "vn 0 1 0\nf 1/1/1 2/2/1 3/3/1\nf 2/2/1 4/4/1 3/3/1\nf 1//1 2//1 3//1\n";
    Expect(DecodeText(shared, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK, "shared decodes");
    Expect(model.vertexCount == 4u + 3u, "corners with equal attributes are deduplicated");
    Expect((model.flags & MODEL_FLAG_UV) != 0u && Near(model.vertices[0].normal[1], 1.0f),
           "explicit normal is kept");
}

static void CheckAttributes(void)
{
    ModelData model;
    const char *colored = "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nvt 0.25 0.75\n"
                          "f 1/1 2/1 3/1\n";
    Expect(DecodeText(colored, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK, "colored decodes");
    Expect(
        model.vertices[0].colorRGBA == 0xFF0000FFu && model.vertices[1].colorRGBA == 0xFF00FF00u &&
            model.vertices[2].colorRGBA == 0xFFFF0000u && (model.flags & MODEL_FLAG_COLORS) != 0u,
        "vertex colors pack as RGBA8");
    Expect(Near(model.vertices[0].uv[0], 0.25f) && Near(model.vertices[0].uv[1], 0.25f),
           "v is flipped to a top-left texture origin");
    Expect(DecodeText(colored, MODEL_IMPORT_SOURCE_Z_UP | MODEL_IMPORT_KEEP_V, &model) ==
                   MODEL_OK &&
               Near(model.vertices[0].uv[1], 0.75f),
           "KEEP_V keeps the source v");
}

static void CheckPartsAndCollision(void)
{
    ModelData model;
    const char *parts = "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl bark\nf 1 2 3\nusemtl leaves\n"
                        "f 1 2 3\nusemtl leaves\nf 1 2 3\nusemtl bark\nf 1 2 3\n";
    Expect(DecodeText(parts, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK, "parts decode");
    Expect(model.partCount == 3u && SameText(model.parts[0].material, "bark") &&
               SameText(model.parts[1].material, "leaves") && model.parts[1].indexCount == 6u &&
               model.parts[2].firstIndex == 9u,
           "adjacent equal materials merge and others stay separate");

    const char *collision = "v 0 0 0\nv 2 0 0\nv 0 2 0\nv -1 -1 -1\nv 1 1 1\nv 1 -1 1\n"
                            "f 1 2 3\no COL_Body\nf 4 5 6\nf 4 6 5\ng visible\nf 1 3 2\n";
    Expect(DecodeText(collision, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_OK,
           "collision decodes");
    Expect(model.boxCount == 1u && model.indexCount == 6u && model.vertexCount == 3u,
           "COL_ group becomes a box and is not drawn");
    Expect(Near(model.boxes[0].center[0], 0.0f) && Near(model.boxes[0].halfExtent[0], 1.0f) &&
               Near(model.boxes[0].halfExtent[1], 1.0f) && Near(model.boxes[0].rotation[3], 1.0f),
           "box spans the collision geometry");
}

static void CheckRejectedObj(void)
{
    ModelData model;
    struct
    {
        const char *text;
        ModelStatus expected;
    } cases[] = {
        {"v 0 0 0\nv 1 0 0\nf 1 2\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 -4\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1.0.0 0 0\nv 0 1 0\nf 1 2 3\n", MODEL_CORRUPT},
        {"v nan 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", MODEL_CORRUPT},
        {"v 1e60 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\no COL_only\nf 1 2 3\n", MODEL_CORRUPT},
        {"v 0 0 0 1 1\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1/1 2 3\n", MODEL_CORRUPT},
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/2 2/1 3/1\n", MODEL_CORRUPT},
    };
    for (uint32_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index)
        Expect(DecodeText(cases[index].text, MODEL_IMPORT_SOURCE_Z_UP, &model) ==
                   cases[index].expected,
               cases[index].text);

    static char longName[256];
    const char *prefix = "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl ";
    const uint32_t prefixLength = Length(prefix);
    memcpy(longName, prefix, prefixLength);
    memset(longName + prefixLength, 'a', 140u);
    memcpy(longName + prefixLength + 140u, "\nf 1 2 3\n", 10u);
    Expect(DecodeText(longName, MODEL_IMPORT_SOURCE_Z_UP, &model) == MODEL_TOO_LARGE,
           "material names longer than the engine capacity are refused");
    Expect(ModelProbe("hello world", 11u) == MODEL_FORMAT_UNKNOWN &&
               ModelProbe("# c\n\nv 0 0 0\n", 13u) == MODEL_FORMAT_OBJ,
           "probe recognises OBJ by its first keyword");
}

static void CheckLoRoundTrip(void)
{
    ModelData model;
    const char *source = "v 0 0 0 1 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\nvt 0 0\nvt 1 1\n"
                         "usemtl stone\nf 1/1 2/2 3/1\nusemtl moss\nf 1/1 3/2 4/1\n"
                         "o UCX_base\nf 1 2 4\n";
    Expect(DecodeText(source, 0u, &model) == MODEL_OK, "round-trip source decodes");
    model.sourceModifiedTime = 0x0123456789ABCDEFull;
    model.sourceSizeBytes = 777u;
    uint32_t bytes = 0u;
    Expect(ModelEncodedBytes(&model, &bytes) == MODEL_OK && bytes <= TEST_FILE,
           "encoded size is known");
    uint32_t written = 0u;
    Expect(ModelEncode(&model, g_file, TEST_FILE, &written) == MODEL_OK && written == bytes,
           "model encodes");
    Expect(ModelProbe(g_file, written) == MODEL_FORMAT_LO, "probe recognises LO by magic");

    ModelData decoded;
    Expect(DecodeInto(g_file, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) ==
               MODEL_OK,
           "LO decodes");
    Expect(decoded.vertexCount == model.vertexCount && decoded.indexCount == model.indexCount &&
               decoded.partCount == model.partCount && decoded.boxCount == model.boxCount &&
               decoded.flags == model.flags &&
               decoded.sourceModifiedTime == model.sourceModifiedTime &&
               decoded.sourceSizeBytes == 777u,
           "LO keeps counts, flags and source fingerprint");
    Expect(memcmp(decoded.vertices, model.vertices, sizeof(ModelVertex) * model.vertexCount) == 0 &&
               memcmp(decoded.indices, model.indices, 4u * model.indexCount) == 0 &&
               memcmp(decoded.boxes, model.boxes, sizeof(ModelBox) * model.boxCount) == 0 &&
               memcmp(decoded.boundsMin, model.boundsMin, sizeof(model.boundsMin)) == 0,
           "LO keeps geometry bit for bit");
    for (uint32_t part = 0u; part < model.partCount; ++part)
        Expect(SameText(decoded.parts[part].material, model.parts[part].material) &&
                   decoded.parts[part].firstIndex == model.parts[part].firstIndex,
               "LO keeps material names");

    static uint8_t copy[TEST_FILE];
    // Неизвестная секция пропускается: будущий скелет не ломает загрузчик.
    memcpy(copy, g_file, written);
    for (uint32_t section = 0u; section < copy[8]; ++section)
    {
        uint8_t *tag = copy + 40u + section * 16u;
        if (memcmp(tag, "COLL", 4u) == 0)
            memcpy(tag, "SKIN", 4u);
    }
    Expect(DecodeInto(copy, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) ==
                   MODEL_OK &&
               decoded.boxCount == 0u,
           "unknown sections are skipped");

    struct
    {
        uint32_t offset;
        uint32_t value;
        ModelStatus expected;
        const char *name;
    } patches[] = {
        {0u, 0x31444D58u, MODEL_NOT_RECOGNISED, "bad magic is not a model"},
        {4u, 0x00280002u, MODEL_UNSUPPORTED_FEATURE, "future version is refused"},
        {12u, written + 1u, MODEL_TRUNCATED, "declared size beyond data"},
        {12u, written - 4u, MODEL_CORRUPT, "declared size below data"},
        {28u, 0x80u, MODEL_CORRUPT, "unknown flags are refused"},
        {32u, 1u, MODEL_CORRUPT, "reserved fields must be zero"},
        {8u, 17u, MODEL_CORRUPT, "too many sections"},
    };
    for (uint32_t index = 0u; index < sizeof(patches) / sizeof(patches[0]); ++index)
    {
        memcpy(copy, g_file, written);
        PutU32(copy + patches[index].offset, patches[index].value);
        Expect(DecodeInto(copy, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2,
                          g_boxes2) == patches[index].expected,
               patches[index].name);
    }
    for (uint32_t cut = 0u; cut < written; cut += 7u)
        Expect(DecodeInto(g_file, cut, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) !=
                   MODEL_OK,
               "every truncation is refused");

    // Содержимое секций: индекс за вершинами, NaN, габарит меньше вершин,
    // перекрытие секций.
    const uint32_t vertOffset = GetU32(g_file + 44u);
    const uint32_t indexOffset = GetU32(g_file + 60u);
    memcpy(copy, g_file, written);
    copy[indexOffset] = 200u;
    Expect(DecodeInto(copy, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) ==
               MODEL_CORRUPT,
           "index beyond the vertices is refused");
    memcpy(copy, g_file, written);
    copy[vertOffset + 3u] = 0x7Fu;
    copy[vertOffset + 2u] = 0xC0u;
    Expect(DecodeInto(copy, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) ==
               MODEL_CORRUPT,
           "NaN position is refused");
    memcpy(copy, g_file, written);
    PutU32(copy + 60u, vertOffset + 4u);
    Expect(DecodeInto(copy, written, 0u, &decoded, g_vertices2, g_indices2, g_parts2, g_boxes2) ==
               MODEL_CORRUPT,
           "overlapping sections are refused");
}

static void CheckValidation(void)
{
    ModelData model;
    Expect(DecodeText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", MODEL_IMPORT_SOURCE_Z_UP, &model) ==
               MODEL_OK,
           "validation fixture decodes");
    model.boundsMax[0] = 0.5f;
    Expect(ModelValidate(&model) == MODEL_CORRUPT, "bounds that miss a vertex are refused");
    model.boundsMax[0] = 1.0f;
    model.parts[0].indexCount = 6u;
    Expect(ModelValidate(&model) == MODEL_CORRUPT, "part beyond the index buffer is refused");
    model.parts[0].indexCount = 3u;
    ModelBox box = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.5f, 0.5f}};
    model.boxes = &box;
    model.boxCount = 1u;
    Expect(ModelValidate(&model) == MODEL_CORRUPT, "non-unit box rotation is refused");
    box.rotation[2] = 0.0f;
    box.rotation[3] = 1.0f;
    Expect(ModelValidate(&model) == MODEL_OK, "valid model passes");
}

LAIUE_TEST_ENTRY(ModelCodecTestEntryPoint)
{
    CheckTriangleAndAxes();
    CheckPolygonsAndIndices();
    CheckAttributes();
    CheckPartsAndCollision();
    CheckRejectedObj();
    CheckLoRoundTrip();
    CheckValidation();
    LaiueTestRuntimeWrite("Model codec checks passed\r\n");
    LAIUE_TEST_SUCCESS();
}
