// Разбор моделей: OBJ и `.lo`. Успешно разобранная модель обязана
// записаться в `.lo` и прочитаться обратно в те же числа — так фаззер
// проверяет и разборщик, и кодировщик, и их согласие.
#include "media/model.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffers
{
    ModelVertex *vertices;
    uint32_t *indices;
    ModelPart *parts;
    ModelBox *boxes;
    ModelSkinVertex *skinVertices;
    void *scratch;
} Buffers;

static void Release(Buffers *buffers)
{
    free(buffers->vertices);
    free(buffers->indices);
    free(buffers->parts);
    free(buffers->boxes);
    free(buffers->skinVertices);
    free(buffers->scratch);
}

static int Decode(const uint8_t *data, uint32_t size, ModelData *model, Buffers *buffers)
{
    memset(buffers, 0, sizeof(*buffers));
    ModelInfo info;
    if (ModelInspect(data, size, &info) != MODEL_OK)
        return 0;
    const uint64_t bytes =
        (uint64_t)info.vertexCapacity * sizeof(ModelVertex) + (uint64_t)info.indexCapacity * 4u +
        (uint64_t)info.partCapacity * sizeof(ModelPart) +
        (uint64_t)info.boxCapacity * sizeof(ModelBox) +
        (uint64_t)info.skinVertexCapacity * sizeof(ModelSkinVertex) + info.scratchBytes;
    if (bytes > (256u << 20))
        return 0;
    buffers->vertices = malloc((size_t)info.vertexCapacity * sizeof(ModelVertex) + 1u);
    buffers->indices = malloc((size_t)info.indexCapacity * 4u + 1u);
    buffers->parts = malloc((size_t)info.partCapacity * sizeof(ModelPart) + 1u);
    buffers->boxes = malloc((size_t)info.boxCapacity * sizeof(ModelBox) + 1u);
    buffers->skinVertices = malloc((size_t)info.skinVertexCapacity * sizeof(ModelSkinVertex) + 1u);
    buffers->scratch = malloc((size_t)info.scratchBytes + 8u);
    if (buffers->vertices == NULL || buffers->indices == NULL || buffers->parts == NULL ||
        buffers->boxes == NULL || buffers->skinVertices == NULL || buffers->scratch == NULL)
        return 0;
    memset(model, 0, sizeof(*model));
    model->vertices = buffers->vertices;
    model->indices = buffers->indices;
    model->parts = buffers->parts;
    model->boxes = buffers->boxes;
    model->skinVertices = buffers->skinVertices;
    return ModelDecode(data, size, NULL, &info, model, buffers->scratch, info.scratchBytes) ==
           MODEL_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 22))
        return 0;
    ModelProbe(data, (uint32_t)size);
    ModelData model;
    Buffers buffers;
    if (!Decode(data, (uint32_t)size, &model, &buffers))
    {
        Release(&buffers);
        return 0;
    }
    uint32_t encodedBytes = 0u;
    if (ModelEncodedBytes(&model, &encodedBytes) != MODEL_OK)
        abort();
    uint8_t *encoded = malloc(encodedBytes);
    uint32_t written = 0u;
    if (ModelEncode(&model, encoded, encodedBytes, &written) != MODEL_OK || written != encodedBytes)
        abort();
    ModelData again;
    Buffers againBuffers;
    if (!Decode(encoded, written, &again, &againBuffers) ||
        again.vertexCount != model.vertexCount || again.indexCount != model.indexCount ||
        again.partCount != model.partCount || again.boxCount != model.boxCount ||
        again.skinVertexCount != model.skinVertexCount ||
        again.skinJointCount != model.skinJointCount ||
        memcmp(again.vertices, model.vertices, sizeof(ModelVertex) * model.vertexCount) != 0 ||
        memcmp(again.indices, model.indices, 4u * (size_t)model.indexCount) != 0 ||
        memcmp(again.skinVertices, model.skinVertices,
               sizeof(ModelSkinVertex) * model.skinVertexCount) != 0)
        abort();
    Release(&againBuffers);
    free(encoded);
    Release(&buffers);
    return 0;
}
