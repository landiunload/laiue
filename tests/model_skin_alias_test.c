#include "media/model.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Large physical arrays let every alias fixture describe an accessible
 * range: failures must come from ownership checks, not a short allocation. */
static _Alignas(8) uint8_t g_file[1024];
static uint8_t g_savedFile[sizeof(g_file)];
static _Alignas(8) uint8_t g_encoded[1024];
static uint8_t g_savedEncoded[sizeof(g_encoded)];
static ModelVertex g_vertices[32];
static ModelVertex g_savedVertices[32];
static uint32_t g_indices[256];
static uint32_t g_savedIndices[256];
static ModelPart g_parts[8];
static ModelPart g_savedParts[8];
static ModelBox g_boxes[32];
static ModelBox g_savedBoxes[32];
static ModelSkinVertex g_skin[32];
static ModelSkinVertex g_savedSkin[32];
static union
{
    ModelData model;
    uint8_t bytes[1024];
} g_descriptor;
static uint8_t g_savedDescriptor[sizeof(g_descriptor)];
static union
{
    ModelInfo info;
    uint8_t bytes[1024];
} g_info;
static uint8_t g_savedInfo[sizeof(g_info)];
static uint32_t g_fileBytes;

static void Expect(bool condition, const char *message)
{
    if (condition)
        return;
    LaiueTestRuntimeWrite("Model skin alias: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

static void Snapshot(void)
{
    memcpy(g_savedFile, g_file, sizeof(g_file));
    memcpy(g_savedEncoded, g_encoded, sizeof(g_encoded));
    memcpy(g_savedVertices, g_vertices, sizeof(g_vertices));
    memcpy(g_savedIndices, g_indices, sizeof(g_indices));
    memcpy(g_savedParts, g_parts, sizeof(g_parts));
    memcpy(g_savedBoxes, g_boxes, sizeof(g_boxes));
    memcpy(g_savedSkin, g_skin, sizeof(g_skin));
    memcpy(g_savedDescriptor, &g_descriptor, sizeof(g_descriptor));
    memcpy(g_savedInfo, &g_info, sizeof(g_info));
}

static bool Unchanged(void)
{
    return memcmp(g_savedFile, g_file, sizeof(g_file)) == 0 &&
           memcmp(g_savedEncoded, g_encoded, sizeof(g_encoded)) == 0 &&
           memcmp(g_savedVertices, g_vertices, sizeof(g_vertices)) == 0 &&
           memcmp(g_savedIndices, g_indices, sizeof(g_indices)) == 0 &&
           memcmp(g_savedParts, g_parts, sizeof(g_parts)) == 0 &&
           memcmp(g_savedBoxes, g_boxes, sizeof(g_boxes)) == 0 &&
           memcmp(g_savedSkin, g_skin, sizeof(g_skin)) == 0 &&
           memcmp(g_savedDescriptor, &g_descriptor, sizeof(g_descriptor)) == 0 &&
           memcmp(g_savedInfo, &g_info, sizeof(g_info)) == 0;
}

static void Source(void)
{
    memset(&g_descriptor, 0, sizeof(g_descriptor));
    memset(g_vertices, 0, sizeof(g_vertices));
    memset(g_indices, 0, sizeof(g_indices));
    memset(g_parts, 0, sizeof(g_parts));
    memset(g_boxes, 0, sizeof(g_boxes));
    memset(g_skin, 0, sizeof(g_skin));
    memset(g_encoded, 0xa5, sizeof(g_encoded));
    ModelData *model = &g_descriptor.model;
    model->vertices = g_vertices;
    model->vertexCount = 3u;
    model->indices = g_indices;
    model->indexCount = 3u;
    model->parts = g_parts;
    model->partCount = 1u;
    model->skinVertices = g_skin;
    model->skinVertexCount = 3u;
    model->skinJointCount = 2u;
    model->boundsMax[0] = 1.0f;
    model->boundsMax[1] = 1.0f;
    g_vertices[1].position[0] = 1.0f;
    g_vertices[2].position[1] = 1.0f;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
    {
        g_vertices[vertex].normal[2] = 1.0f;
        g_indices[vertex] = vertex;
        g_skin[vertex].joints[0] = vertex & 1u;
        g_skin[vertex].weights[0] = 0.75f;
        g_skin[vertex].joints[1] = 1u;
        g_skin[vertex].weights[1] = 0.25f;
    }
    g_parts[0].indexCount = 3u;
}

static void Destination(void)
{
    memset(&g_descriptor, 0, sizeof(g_descriptor));
    g_descriptor.model.vertices = g_vertices;
    g_descriptor.model.indices = g_indices;
    g_descriptor.model.parts = g_parts;
    g_descriptor.model.skinVertices = g_skin;
    memset(g_vertices, 0x69, sizeof(g_vertices));
    memset(g_indices, 0x69, sizeof(g_indices));
    memset(g_parts, 0x69, sizeof(g_parts));
    memset(g_boxes, 0x69, sizeof(g_boxes));
    memset(g_skin, 0x69, sizeof(g_skin));
}

static void RejectDecode(const char *message)
{
    Snapshot();
    Expect(ModelDecode(g_file, g_fileBytes, NULL, &g_info.info, &g_descriptor.model, NULL, 0u) ==
                   MODEL_INVALID_ARGUMENT &&
               Unchanged(),
           message);
}

static void DecodeAliases(void)
{
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(void *)(g_file + 320u);
    RejectDecode("SKIN output cannot overwrite the serialized last records");
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(void *)g_vertices;
    RejectDecode("SKIN output cannot overwrite freshly decoded VERT records");
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(void *)(g_indices + 1u);
    RejectDecode("partial SKIN/index output overlap is rejected before any write");
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(void *)&g_descriptor.model;
    RejectDecode("SKIN output cannot replace its pointer-owning descriptor");
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(void *)&g_info.info;
    RejectDecode("SKIN output cannot overwrite the capacity metadata");
    Destination();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(UINTPTR_MAX - 31u);
    RejectDecode("a wrapping output region rejects before accessing memory");
    Destination();
    g_descriptor.model.indices = (uint32_t *)(void *)(g_vertices + 1u);
    RejectDecode("geometry output regions retain separate ownership");
    Destination();
    Snapshot();
    Expect(ModelDecode(g_file, g_fileBytes, NULL, &g_info.info, &g_descriptor.model, g_skin,
                       sizeof(g_skin)) == MODEL_INVALID_ARGUMENT &&
               Unchanged(),
           "scratch cannot overwrite weights even on a no-scratch LO path");
    Destination();
    Snapshot();
    const ModelImportOptions *options = (const ModelImportOptions *)(const void *)g_skin;
    Expect(ModelDecode(g_file, g_fileBytes, options, &g_info.info, &g_descriptor.model, NULL, 0u) ==
                   MODEL_INVALID_ARGUMENT &&
               Unchanged(),
           "writable output cannot alias read-only import options");
}

static void RejectEncode(void *output, uint32_t *written, const char *message)
{
    Snapshot();
    Expect(ModelEncode(&g_descriptor.model, output, sizeof(g_encoded), written) ==
                   MODEL_INVALID_ARGUMENT &&
               Unchanged(),
           message);
}

static void EncodeAliases(void)
{
    uint32_t written = 77u;
    Source();
    RejectEncode(g_skin, &written, "encode cannot memset its still-needed source weights");
    Expect(written == 77u, "alias rejection happens before clearing the size result");
    Source();
    RejectEncode(g_vertices, &written, "encode cannot overwrite source VERT records");
    Source();
    RejectEncode(g_indices, &written, "encode cannot overwrite source indices");
    Source();
    RejectEncode(g_parts, &written, "encode cannot overwrite source material records");
    Source();
    RejectEncode(&g_descriptor.model, &written,
                 "encode cannot overwrite its input pointer-owning descriptor");
    Source();
    RejectEncode(g_encoded, &g_descriptor.model.skinJointCount,
                 "outWritten cannot clear a field in source metadata");
    Source();
    RejectEncode(g_encoded, &g_skin[0].joints[0],
                 "outWritten cannot clear a still-needed source joint index");
    Source();
    RejectEncode(g_encoded, (uint32_t *)(void *)(g_encoded + 4u),
                 "outWritten cannot corrupt the encoded file header");
    Source();
    g_descriptor.model.skinVertices = (ModelSkinVertex *)(UINTPTR_MAX - 31u);
    RejectEncode(g_encoded, &written, "a wrapping source range rejects before validation");
    Source();
    RejectEncode((void *)(UINTPTR_MAX - 7u), &written,
                 "a wrapping encoded output range rejects before writing");
}

LAIUE_TEST_ENTRY(ModelSkinAliasTestEntryPoint)
{
    Source();
    memset(g_file, 0, sizeof(g_file));
    Expect(ModelEncode(&g_descriptor.model, g_file, sizeof(g_file), &g_fileBytes) == MODEL_OK &&
               g_fileBytes == 400u,
           "valid disjoint source encodes a complete SKIN fixture");
    memset(&g_info, 0, sizeof(g_info));
    Expect(ModelInspect(g_file, g_fileBytes, &g_info.info) == MODEL_OK &&
               g_info.info.skinVertexCapacity == 3u,
           "fixture metadata has exact capacities");
    DecodeAliases();
    EncodeAliases();
    Destination();
    /* Unused pointers remain permitted. They neither name output storage
     * nor need dereferencing when their capacities are zero. */
    g_descriptor.model.boxes = (ModelBox *)(uintptr_t)1u;
    Expect(ModelDecode(g_file, g_fileBytes, NULL, &g_info.info, &g_descriptor.model,
                       (void *)(uintptr_t)1u, 0u) == MODEL_OK &&
               g_descriptor.model.skinJointCount == 2u &&
               g_descriptor.model.skinVertexCount == 3u && g_skin[2].weights[0] == 0.75f,
           "valid disjoint decoding preserves zero-capacity optional-array semantics");
    uint32_t written = 77u;
    Expect(ModelEncode(&g_descriptor.model, g_encoded, sizeof(g_encoded), &written) == MODEL_OK &&
               written == g_fileBytes && memcmp(g_file, g_encoded, written) == 0,
           "valid disjoint roundtrip remains byte-identical");
    memset(g_encoded, 0xa5, sizeof(g_encoded));
    Expect(ModelEncode(&g_descriptor.model, g_encoded, g_fileBytes - 1u, &written) ==
                   MODEL_BUFFER_TOO_SMALL &&
               written == 0u && g_encoded[0] == 0xa5u,
           "disjoint short-capacity encoding retains its prior result semantics");
    LaiueTestRuntimeWrite("Model skin alias checks passed\n");
    LAIUE_TEST_SUCCESS();
}
