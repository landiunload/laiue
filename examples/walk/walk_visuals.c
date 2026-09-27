#include "walk_visuals.h"

#include "media/image.h"
#include "math/scalar.h"
#include "platform/system.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum
{
    WALK_VISUAL_RAGDOLL_BODY_COUNT = 13u,
    WALK_VISUAL_RAGDOLL_HEAD_INDEX = 2u,
    WALK_RAGDOLL_BEVEL_SEGMENTS = 4u,
    WALK_RAGDOLL_HEAD_LONGITUDE_SEGMENTS = 12u,
    WALK_RAGDOLL_HEAD_LATITUDE_SEGMENTS = 8u,
    WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS = 4u,
    WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS = 8u,
};

static bool DeviceFieldPresent(const LaiueGraphicsDeviceV2 *device,
                               size_t offset, size_t size)
{
    return device != NULL && (size_t)device->structSize >= offset &&
           (size_t)device->structSize - offset >= size;
}

static double Minimum(double left, double right)
{
    return left < right ? left : right;
}

static double Maximum(double left, double right)
{
    return left > right ? left : right;
}

static void ReleaseHandle(LaiueGraphicsDeviceV2 *device,
                          LaiueGraphicsHandle *handle)
{
    if (handle == NULL || *handle == 0u)
        return;
    if (DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                           sizeof(device->destroyHandle)) &&
        device->destroyHandle != NULL)
        device->destroyHandle(device, *handle);
    *handle = 0u;
}

static bool UploadVertexBuffer(LaiueGraphicsDeviceV2 *device,
                               const LaiueGraphicsVertexV2 *vertices,
                               uint32_t vertexCount,
                               LaiueGraphicsHandle *outBuffer)
{
    if (outBuffer != NULL)
        *outBuffer = 0u;
    if (device == NULL || vertices == NULL || vertexCount == 0u || outBuffer == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, createBuffer),
                            sizeof(device->createBuffer)) ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, uploadBuffer),
                            sizeof(device->uploadBuffer)) ||
        device->createBuffer == NULL || device->uploadBuffer == NULL)
        return false;
    const LaiueGraphicsBufferDescV1 description = {
        .structSize = sizeof(description),
        .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
        .sizeBytes = (uint64_t)vertexCount * sizeof(*vertices),
    };
    if (device->createBuffer(device, &description, outBuffer) == 0u)
        return false;
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload), .buffer = *outBuffer,
        .data = vertices, .sizeBytes = description.sizeBytes,
    };
    if (device->uploadBuffer(device, &upload) != 0u)
        return true;
    ReleaseHandle(device, outBuffer);
    return false;
}

static bool CreateVertexBuffer(LaiueGraphicsDeviceV2 *device, uint64_t sizeBytes,
                               LaiueGraphicsHandle *outBuffer)
{
    if (outBuffer != NULL)
        *outBuffer = 0u;
    if (device == NULL || sizeBytes == 0u || outBuffer == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, createBuffer),
                            sizeof(device->createBuffer)) ||
        device->createBuffer == NULL)
        return false;
    const LaiueGraphicsBufferDescV1 description = {
        .structSize = sizeof(description),
        .usageFlags = LAIUE_GRAPHICS_BUFFER_USAGE_VERTEX,
        .sizeBytes = sizeBytes,
    };
    return device->createBuffer(device, &description, outBuffer) != 0u;
}

static bool UploadTexture(LaiueGraphicsDeviceV2 *device, WalkReadAssetFn readAsset,
                          void *assetContext, const char *relativePath,
                          LaiueGraphicsHandle *outTexture)
{
    uint8_t *encoded = NULL;
    uint8_t *pixels = NULL;
    uint8_t *scratch = NULL;
    uint8_t *mipPixels = NULL;
    uint32_t encodedBytes = 0u;
    bool succeeded = false;
    if (outTexture != NULL)
        *outTexture = 0u;
    if (device == NULL || readAsset == NULL || relativePath == NULL || outTexture == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, createTexture),
                            sizeof(device->createTexture)) ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, uploadTexture),
                            sizeof(device->uploadTexture)) ||
        device->createTexture == NULL || device->uploadTexture == NULL ||
        !readAsset(assetContext, relativePath, &encoded, &encodedBytes) ||
        encoded == NULL || encodedBytes == 0u)
        goto cleanup;

    ImageInfo info;
    memset(&info, 0, sizeof(info));
    if (ImageInspect(encoded, encodedBytes, &info) != IMAGE_OK ||
        info.frameCount != 1u || info.frameBytes == 0u ||
        info.pixelBytes != info.frameBytes || info.width > UINT32_MAX / 4u)
        goto cleanup;
    pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    if (info.scratchBytes != 0u)
        scratch = (uint8_t *)PlatformAllocate(info.scratchBytes, false);
    if (pixels == NULL || (info.scratchBytes != 0u && scratch == NULL) ||
        ImageDecode(encoded, encodedBytes, &info, pixels, info.pixelBytes,
                    scratch, info.scratchBytes) != IMAGE_OK)
        goto cleanup;

    uint32_t mipLevels = 1u;
    for (uint32_t largest = info.width > info.height ? info.width : info.height;
         largest > 1u; largest >>= 1u)
        ++mipLevels;
    const LaiueGraphicsTextureDescV1 description = {
        .structSize = sizeof(description),
        .format = LAIUE_GRAPHICS_FORMAT_RGBA8_SRGB,
        .extent = {info.width, info.height, 1u},
        .mipLevels = mipLevels,
        .usageFlags = 0u,
    };
    if (device->createTexture(device, &description, outTexture) == 0u)
        goto cleanup;
    const uint8_t *source = pixels;
    uint32_t width = info.width;
    uint32_t height = info.height;
    for (uint32_t level = 0u; level < mipLevels; ++level)
    {
        const LaiueGraphicsTextureUploadV1 upload = {
            .structSize = sizeof(upload), .texture = *outTexture,
            .data = source, .sizeBytes = (uint64_t)width * height * 4u,
            .rowPitchBytes = width * 4u, .mipLevel = level,
        };
        if (device->uploadTexture(device, &upload) == 0u)
            goto cleanup;
        if (level + 1u == mipLevels)
            break;
        const uint32_t nextWidth = width > 1u ? width >> 1u : 1u;
        const uint32_t nextHeight = height > 1u ? height >> 1u : 1u;
        const uint64_t nextBytes = (uint64_t)nextWidth * nextHeight * 4u;
        if (nextBytes > UINT32_MAX)
            goto cleanup;
        uint8_t *next = (uint8_t *)PlatformAllocate((uint32_t)nextBytes, false);
        if (next == NULL)
            goto cleanup;
        ImageResample(source, width, height, next, nextWidth, nextHeight);
        PlatformFree(mipPixels);
        mipPixels = next;
        source = mipPixels;
        width = nextWidth;
        height = nextHeight;
    }
    succeeded = true;

cleanup:
    PlatformFree(mipPixels);
    PlatformFree(scratch);
    PlatformFree(pixels);
    PlatformFree(encoded);
    if (!succeeded)
        ReleaseHandle(device, outTexture);
    return succeeded;
}

bool WalkVisualsCreateTerrain(LaiueGraphicsDeviceV2 *device,
                              WalkReadAssetFn readAsset, void *assetContext,
                              LaiueGraphicsHandle outBuffers[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle outTextures[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle *outSampler)
{
    static const char *const paths[WALK_VISUAL_TEXTURE_COUNT] = {
        "textures/grass.png", "textures/dirt.png", "textures/stone.png",
    };
    if (outBuffers == NULL || outTextures == NULL || outSampler == NULL)
        return false;
    memset(outBuffers, 0, sizeof(*outBuffers) * WALK_VISUAL_TEXTURE_COUNT);
    memset(outTextures, 0, sizeof(*outTextures) * WALK_VISUAL_TEXTURE_COUNT);
    *outSampler = 0u;
    if (device == NULL || readAsset == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, createSampler),
                            sizeof(device->createSampler)) ||
        device->createSampler == NULL)
        return false;
    for (uint32_t index = 0u; index < WALK_VISUAL_TEXTURE_COUNT; ++index)
        if (!UploadTexture(device, readAsset, assetContext, paths[index], &outTextures[index]))
            goto failed;
    const LaiueGraphicsSamplerDescV1 sampler = {
        .structSize = sizeof(sampler), .minFilter = LAIUE_GRAPHICS_FILTER_LINEAR,
        .magFilter = LAIUE_GRAPHICS_FILTER_LINEAR,
        .addressModeU = LAIUE_GRAPHICS_ADDRESS_REPEAT,
        .addressModeV = LAIUE_GRAPHICS_ADDRESS_REPEAT,
        .addressModeW = LAIUE_GRAPHICS_ADDRESS_REPEAT,
    };
    if (device->createSampler(device, &sampler, outSampler) == 0u)
        goto failed;

    /* Geometry now comes from the actual voxel mesher.  Keep this compatibility
     * entrypoint's buffer outputs empty while it owns the shared tile textures. */
    return true;

failed:
    WalkVisualsDestroyTerrain(device, outBuffers, outTextures, outSampler);
    return false;
}

static bool AddInt64(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static int64_t FloorDiv64(int64_t value)
{
    int64_t quotient = value / 64;
    if (value % 64 < 0)
        --quotient;
    return quotient;
}

static bool SubtractInt64(int64_t left, int64_t right, int64_t *out)
{
    if (out == NULL || (right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right))
        return false;
    *out = left - right;
    return true;
}

typedef struct WalkVisualBuildContext
{
    WalkVisualGetBlockFn getBlock;
    void *blockContext;
} WalkVisualBuildContext;

static WorldRegionContents WalkVisualFillRegion(void *opaque,
    int64_t minX, int64_t minY, int64_t minZ,
    int32_t sizeX, int32_t sizeY, int32_t sizeZ, BlockType *blocks)
{
    WalkVisualBuildContext *context = (WalkVisualBuildContext *)opaque;
    if (context == NULL || context->getBlock == NULL || blocks == NULL ||
        sizeX <= 0 || sizeY <= 0 || sizeZ <= 0)
        return WORLD_REGION_ALL_AIR;
    for (int32_t y = 0; y < sizeY; ++y)
        for (int32_t x = 0; x < sizeX; ++x)
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                int64_t worldX = 0, worldY = 0, worldZ = 0;
                uint8_t material = 0u;
                if (AddInt64(minX, x, &worldX) && AddInt64(minY, y, &worldY) &&
                    AddInt64(minZ, z, &worldZ))
                    material = context->getBlock(context->blockContext,
                                                  worldX, worldY, worldZ);
                blocks[((size_t)y * (size_t)sizeX + (size_t)x) *
                           (size_t)sizeZ + (size_t)z] = material;
            }
    return WORLD_REGION_MIXED;
}

static void DestroyChunk(LaiueGraphicsDeviceV2 *device, WalkVisualChunk *chunk)
{
    if (chunk == NULL)
        return;
    for (uint32_t i = 0u; i < WALK_VISUAL_TEXTURE_COUNT; ++i)
        ReleaseHandle(device, &chunk->buffers[i]);
    memset(chunk, 0, sizeof(*chunk));
}

bool WalkVisualsCreateChunkSet(const LaiueMesherServiceV1 *mesher,
                               WalkVisualChunkSet *outSet)
{
    if (outSet == NULL)
        return false;
    memset(outSet, 0, sizeof(*outSet));
    if (mesher == NULL || mesher->structSize < sizeof(*mesher) ||
        mesher->scratchCreate == NULL || mesher->scratchDestroy == NULL ||
        mesher->buildChunkMesh == NULL)
        return false;
    outSet->scratch = mesher->scratchCreate();
    return outSet->scratch != NULL;
}

void WalkVisualsDestroyChunkSet(LaiueGraphicsDeviceV2 *device,
                                const LaiueMesherServiceV1 *mesher,
                                WalkVisualChunkSet *set)
{
    if (set == NULL)
        return;
    for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
        DestroyChunk(device, &set->chunks[i]);
    if (set->scratch != NULL && mesher != NULL && mesher->scratchDestroy != NULL)
        mesher->scratchDestroy(set->scratch);
    memset(set, 0, sizeof(*set));
}

static bool BuildChunkVisual(LaiueGraphicsDeviceV2 *device,
                             const LaiueMesherServiceV1 *mesher,
                             ChunkMesherScratch *scratch,
                             WalkVisualGetBlockFn getBlock, void *blockContext,
                             const int64_t coordinate[3], WalkVisualChunk *outChunk)
{
    if (device == NULL || mesher == NULL || scratch == NULL || getBlock == NULL ||
        coordinate == NULL || outChunk == NULL ||
        coordinate[0] < INT64_MIN / 64 + 1 || coordinate[0] > INT64_MAX / 64 - 1 ||
        coordinate[1] < INT64_MIN / 64 + 1 || coordinate[1] > INT64_MAX / 64 - 1 ||
        coordinate[2] < INT64_MIN / 64 + 1 || coordinate[2] > INT64_MAX / 64 - 1)
        return false;

    WalkVisualBuildContext context = {getBlock, blockContext};
    const ChunkMesherWorldSource source = {&context, WalkVisualFillRegion};
    ChunkQuad *quads = NULL;
    uint32_t quadCount = 0u;
    if (!mesher->buildChunkMesh(&source, scratch, coordinate[0], coordinate[1],
                                coordinate[2], &quads, &quadCount))
        return false;
    uint32_t counts[WALK_VISUAL_TEXTURE_COUNT] = {0u, 0u, 0u};
    for (uint32_t i = 0u; i < quadCount; ++i)
    {
        const uint32_t material = quads[i].positionAndFace >> 24u;
        if (material != 0u)
            ++counts[material <= WALK_VISUAL_TEXTURE_COUNT ? material - 1u : 2u];
    }
    WalkVisualChunk built = {0};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        built.coordinate[axis] = coordinate[axis];
    built.ready = true;
    static const uint8_t faceCorners[6][4] = {
        {5u, 7u, 3u, 1u}, {6u, 4u, 0u, 2u},
        {7u, 6u, 2u, 3u}, {4u, 5u, 1u, 0u},
        {6u, 7u, 5u, 4u}, {3u, 2u, 0u, 1u},
    };
    static const uint8_t triangleCorners[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    static const uint8_t shadeByFace[6] = {204u, 204u, 230u, 179u, 255u, 140u};
    uint32_t written[WALK_VISUAL_TEXTURE_COUNT] = {0u, 0u, 0u};
    for (uint32_t material = 0u; material < WALK_VISUAL_TEXTURE_COUNT; ++material)
    {
        if (counts[material] == 0u)
            continue;
        if (counts[material] > UINT32_MAX / 6u ||
            (uint64_t)counts[material] * 6u * sizeof(LaiueGraphicsVertexV2) > UINT32_MAX)
            goto failed;
        const uint32_t vertexCount = counts[material] * 6u;
        LaiueGraphicsVertexV2 *vertices = (LaiueGraphicsVertexV2 *)PlatformAllocate(
            (uint32_t)((uint64_t)vertexCount * sizeof(*vertices)), false);
        if (vertices == NULL)
            goto failed;
        for (uint32_t q = 0u; q < quadCount; ++q)
        {
            const uint32_t blockMaterial = quads[q].positionAndFace >> 24u;
            const uint32_t materialIndex = blockMaterial == 0u ? UINT32_MAX :
                (blockMaterial <= WALK_VISUAL_TEXTURE_COUNT ? blockMaterial - 1u : 2u);
            if (materialIndex != material)
                continue;
            const uint32_t face = (quads[q].positionAndFace >> 21u) & 7u;
            if (face >= 6u)
                continue;
            const uint32_t start[3] = {
                quads[q].positionAndFace & 127u,
                (quads[q].positionAndFace >> 14u) & 127u,
                (quads[q].positionAndFace >> 7u) & 127u,
            };
            const uint32_t extent[3] = {
                quads[q].extents & 127u,
                (quads[q].extents >> 14u) & 127u,
                (quads[q].extents >> 7u) & 127u,
            };
            LaiueGraphicsVertexV2 corners[4];
            for (uint32_t cornerIndex = 0u; cornerIndex < 4u; ++cornerIndex)
            {
                const uint32_t corner = faceCorners[face][cornerIndex];
                const float x = (float)(start[0] + ((corner & 1u) ? extent[0] : 0u));
                const float y = (float)(start[1] + ((corner & 2u) ? extent[1] : 0u));
                const float z = (float)(start[2] + ((corner & 4u) ? extent[2] : 0u));
                float u, v;
                /* Voxel coordinates are in block units. Repeat each 16x16 tile
                 * once per block instead of stretching it over eight blocks. */
                if (face < 2u) { u = y; v = -z; }
                else if (face < 4u) { u = x; v = -z; }
                else { u = x; v = y; }
                const uint32_t shade = shadeByFace[face];
                corners[cornerIndex] = (LaiueGraphicsVertexV2){
                    .position = {x, y, z}, .uv = {u, v},
                    .colorRGBA = UINT32_C(0xFF000000) | (shade << 16u) |
                                 (shade << 8u) | shade,
                };
            }
            for (uint32_t vertex = 0u; vertex < 6u; ++vertex)
                vertices[written[material]++] = corners[triangleCorners[vertex]];
        }
        const bool uploaded = written[material] == vertexCount &&
            UploadVertexBuffer(device, vertices, vertexCount, &built.buffers[material]);
        PlatformFree(vertices);
        if (!uploaded)
            goto failed;
        built.vertexCounts[material] = vertexCount;
    }
    PlatformFree(quads);
    *outChunk = built;
    return true;

failed:
    PlatformFree(quads);
    DestroyChunk(device, &built);
    return false;
}

bool WalkVisualsUpdateChunkSet(LaiueGraphicsDeviceV2 *device,
                               const LaiueMesherServiceV1 *mesher,
                               WalkVisualChunkSet *set,
                               WalkVisualGetBlockFn getBlock, void *blockContext,
                               const int64_t centerBlock[3])
{
    if (device == NULL || mesher == NULL || set == NULL || set->scratch == NULL ||
        getBlock == NULL || centerBlock == NULL)
        return false;
    int64_t desired[WALK_VISUAL_CHUNK_COUNT][3];
    const int64_t centerX = FloorDiv64(centerBlock[0]);
    const int64_t centerY = FloorDiv64(centerBlock[1]);
    const int64_t centerZ = FloorDiv64(centerBlock[2]);
    uint32_t desiredCount = 0u;
    for (int32_t y = -WALK_VISUAL_CHUNK_RADIUS; y <= WALK_VISUAL_CHUNK_RADIUS; ++y)
        for (int32_t x = -WALK_VISUAL_CHUNK_RADIUS; x <= WALK_VISUAL_CHUNK_RADIUS; ++x)
            for (uint32_t level = 0u; level < WALK_VISUAL_CHUNK_LEVELS; ++level)
            {
                int64_t cx = 0, cy = 0, cz = 0;
                const int64_t verticalOffset =
                    (int64_t)level - (int64_t)(WALK_VISUAL_CHUNK_LEVELS / 2u);
                if (!AddInt64(centerX, x, &cx) || !AddInt64(centerY, y, &cy) ||
                    !AddInt64(centerZ, verticalOffset, &cz))
                    return false;
                desired[desiredCount][0] = cx;
                desired[desiredCount][1] = cy;
                desired[desiredCount][2] = cz;
                ++desiredCount;
            }
    bool used[WALK_VISUAL_CHUNK_COUNT] = {false};
    for (uint32_t d = 0u; d < desiredCount; ++d)
    {
        uint32_t slot = WALK_VISUAL_CHUNK_COUNT;
        for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
            if (!used[i] && set->chunks[i].ready &&
                set->chunks[i].coordinate[0] == desired[d][0] &&
                set->chunks[i].coordinate[1] == desired[d][1] &&
                set->chunks[i].coordinate[2] == desired[d][2])
            {
                slot = i;
                break;
            }
        if (slot != WALK_VISUAL_CHUNK_COUNT)
        {
            used[slot] = true;
            continue;
        }
        for (slot = 0u; slot < WALK_VISUAL_CHUNK_COUNT && used[slot]; ++slot) {}
        if (slot == WALK_VISUAL_CHUNK_COUNT)
            return false;
        WalkVisualChunk replacement = {0};
        if (!BuildChunkVisual(device, mesher, set->scratch, getBlock, blockContext,
                              desired[d], &replacement))
            return false;
        DestroyChunk(device, &set->chunks[slot]);
        set->chunks[slot] = replacement;
        used[slot] = true;
    }
    for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
        if (!used[i])
            DestroyChunk(device, &set->chunks[i]);
    memcpy(set->center, (int64_t[3]){centerX, centerY, centerZ}, sizeof(set->center));
    set->centerValid = true;
    return true;
}

void WalkVisualsInvalidateBlock(WalkVisualChunkSet *set,
                                LaiueGraphicsDeviceV2 *device,
                                int64_t blockX, int64_t blockY, int64_t blockZ)
{
    if (set == NULL)
        return;
    const int64_t block[3] = {blockX, blockY, blockZ};
    int64_t base[3];
    uint32_t edgeMask[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const int64_t chunk = FloorDiv64(block[axis]);
        base[axis] = chunk * 64;
        const int64_t local = block[axis] - base[axis];
        edgeMask[axis] = local == 0 ? 1u : (local == 63 ? 2u : 0u);
    }
    int32_t offsets[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    uint32_t counts[3] = {1u, 1u, 1u};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (edgeMask[axis] == 1u) offsets[axis][counts[axis]++] = -1;
        else if (edgeMask[axis] == 2u) offsets[axis][counts[axis]++] = 1;
    }
    for (uint32_t z = 0u; z < counts[2]; ++z)
        for (uint32_t y = 0u; y < counts[1]; ++y)
            for (uint32_t x = 0u; x < counts[0]; ++x)
                for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
                    if (set->chunks[i].ready &&
                        set->chunks[i].coordinate[0] == FloorDiv64(blockX) + offsets[0][x] &&
                        set->chunks[i].coordinate[1] == FloorDiv64(blockY) + offsets[1][y] &&
                        set->chunks[i].coordinate[2] == FloorDiv64(blockZ) + offsets[2][z])
                        DestroyChunk(device, &set->chunks[i]);
}

uint32_t WalkVisualsBuildChunkDraws(const WalkVisualChunkSet *set,
                                    const int64_t renderOriginBlock[3],
                                    const LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                                    LaiueGraphicsHandle sampler,
                                    LaiueGraphicsDrawItemV2 outDraws[WALK_VISUAL_CHUNK_DRAW_COUNT])
{
    if (set == NULL || renderOriginBlock == NULL || textures == NULL || outDraws == NULL)
        return 0u;
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
    {
        const WalkVisualChunk *chunk = &set->chunks[i];
        if (!chunk->ready)
            continue;
        int64_t relativeOrigin[3];
        bool relativeOriginValid = true;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            if (chunk->coordinate[axis] > INT64_MAX / 64 ||
                chunk->coordinate[axis] < INT64_MIN / 64 ||
                !SubtractInt64(chunk->coordinate[axis] * 64,
                               renderOriginBlock[axis], &relativeOrigin[axis]))
            {
                relativeOriginValid = false;
                break;
            }
        }
        if (!relativeOriginValid)
            continue;
        for (uint32_t material = 0u; material < WALK_VISUAL_TEXTURE_COUNT; ++material)
        {
            if (chunk->vertexCounts[material] == 0u || chunk->buffers[material] == 0u)
                continue;
            LaiueGraphicsDrawItemV2 *draw = &outDraws[count++];
            memset(draw, 0, sizeof(*draw));
            draw->structSize = sizeof(*draw);
            draw->vertexBuffer = chunk->buffers[material];
            draw->indexCount = chunk->vertexCounts[material];
            draw->originRelative[0] = (float)relativeOrigin[0];
            draw->originRelative[1] = (float)relativeOrigin[1];
            draw->originRelative[2] = (float)relativeOrigin[2];
            draw->scale = 1.0f;
            draw->texture = textures[material];
            draw->sampler = sampler;
        }
    }
    return count;
}

static void AppendFarTerrainQuad(LaiueGraphicsVertexV2 vertices[6],
                                 uint32_t *written, float x0, float y0,
                                 float x1, float y1)
{
    const LaiueGraphicsVertexV2 corners[4] = {
        {.position = {x0, y0, 0.0f}, .uv = {x0, y0}, .colorRGBA = UINT32_MAX},
        {.position = {x1, y0, 0.0f}, .uv = {x1, y0}, .colorRGBA = UINT32_MAX},
        {.position = {x1, y1, 0.0f}, .uv = {x1, y1}, .colorRGBA = UINT32_MAX},
        {.position = {x0, y1, 0.0f}, .uv = {x0, y1}, .colorRGBA = UINT32_MAX},
    };
    static const uint8_t triangleCorners[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (uint32_t i = 0u; i < 6u; ++i)
        vertices[(*written)++] = corners[triangleCorners[i]];
}

bool WalkVisualsCreateFarTerrainBuffer(LaiueGraphicsDeviceV2 *device,
                                       LaiueGraphicsHandle *outBuffer)
{
    if (outBuffer != NULL)
        *outBuffer = 0u;
    if (outBuffer == NULL)
        return false;
    const float outer = 8192.0f;
    const float innerMinimum = -64.0f;
    const float innerMaximum = 128.0f;
    LaiueGraphicsVertexV2 vertices[WALK_VISUAL_FAR_TERRAIN_VERTEX_COUNT];
    uint32_t written = 0u;
    /* Leave the active 3x3 chunk window open so edits and holes remain visible. */
    AppendFarTerrainQuad(vertices, &written, -outer, innerMaximum, outer, outer);
    AppendFarTerrainQuad(vertices, &written, -outer, -outer, outer, innerMinimum);
    AppendFarTerrainQuad(vertices, &written, -outer, innerMinimum,
                         innerMinimum, innerMaximum);
    AppendFarTerrainQuad(vertices, &written, innerMaximum, innerMinimum,
                         outer, innerMaximum);
    return written == WALK_VISUAL_FAR_TERRAIN_VERTEX_COUNT &&
        UploadVertexBuffer(device, vertices, written, outBuffer);
}

bool WalkVisualsBuildFarTerrainDraw(
    LaiueGraphicsHandle buffer, const int64_t renderOriginBlock[3],
    const LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
    LaiueGraphicsHandle sampler, LaiueGraphicsDrawItemV2 *outDraw)
{
    if (outDraw == NULL)
        return false;
    memset(outDraw, 0, sizeof(*outDraw));
    if (buffer == 0u || renderOriginBlock == NULL || textures == NULL ||
        textures[0] == 0u || sampler == 0u)
        return false;
    outDraw->structSize = sizeof(*outDraw);
    outDraw->vertexBuffer = buffer;
    outDraw->indexCount = WALK_VISUAL_FAR_TERRAIN_VERTEX_COUNT;
    outDraw->originRelative[2] = (float)(0.95 - (double)renderOriginBlock[2]);
    outDraw->scale = 1.0f;
    outDraw->texture = textures[0];
    outDraw->sampler = sampler;
    return true;
}

void WalkVisualsDestroyBuffer(LaiueGraphicsDeviceV2 *device,
                              LaiueGraphicsHandle *buffer)
{
    ReleaseHandle(device, buffer);
}

void WalkVisualsDestroyTerrain(LaiueGraphicsDeviceV2 *device,
                               LaiueGraphicsHandle buffers[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                               LaiueGraphicsHandle *sampler)
{
    if (buffers != NULL)
        for (uint32_t i = 0u; i < WALK_VISUAL_TEXTURE_COUNT; ++i)
            ReleaseHandle(device, &buffers[i]);
    if (textures != NULL)
        for (uint32_t i = 0u; i < WALK_VISUAL_TEXTURE_COUNT; ++i)
            ReleaseHandle(device, &textures[i]);
    ReleaseHandle(device, sampler);
}

bool WalkVisualsCreateRagdollBuffer(LaiueGraphicsDeviceV2 *device,
                                    LaiueGraphicsHandle *outBuffer)
{
    return CreateVertexBuffer(device,
        (uint64_t)WALK_RAGDOLL_VISUAL_VERTEX_COUNT * sizeof(LaiueGraphicsVertexV2),
        outBuffer);
}

static bool WriteRagdollVertex(LaiueGraphicsVertexV2 *output,
                               const double center[3], const float rotation[9],
                               const double renderOrigin[3], const double local[3],
                               uint32_t color)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double value = center[axis] - renderOrigin[axis] +
            (double)rotation[axis] * local[0] +
            (double)rotation[3u + axis] * local[1] +
            (double)rotation[6u + axis] * local[2];
        if (!isfinite(value) || fabs(value) > 1000000.0)
            return false;
        output->position[axis] = (float)value;
    }
    output->uv[0] = output->uv[1] = 0.0f;
    output->colorRGBA = color;
    return true;
}

static double CornerCoordinate(const VoxelRigidBody *body, uint32_t corner,
                               uint32_t axis)
{
    return (corner & (1u << axis)) != 0u ? body->halfExtent[axis]
                                          : -body->halfExtent[axis];
}

static bool WriteRoundedBodyVertex(LaiueGraphicsVertexV2 *output,
                                   const VoxelRigidBody *body,
                                   const double center[3], const float rotation[9],
                                   const double renderOrigin[3],
                                   const uint8_t faceCorners[4],
                                   double u, double v, uint32_t color)
{
    double local[3];
    const double bevel = Minimum(body->halfExtent[0],
                                 Minimum(body->halfExtent[1], body->halfExtent[2])) * 0.82;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double low = CornerCoordinate(body, faceCorners[0], axis);
        const double high = CornerCoordinate(body, faceCorners[1], axis);
        const double farHigh = CornerCoordinate(body, faceCorners[2], axis);
        const double farLow = CornerCoordinate(body, faceCorners[3], axis);
        const double near = low + (high - low) * u;
        const double far = farLow + (farHigh - farLow) * u;
        local[axis] = near + (far - near) * v;
    }
    double inner[3];
    double offset[3];
    double offsetLengthSquared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double limit = Maximum(0.0, body->halfExtent[axis] - bevel);
        inner[axis] = Maximum(-limit, Minimum(limit, local[axis]));
        offset[axis] = local[axis] - inner[axis];
        offsetLengthSquared += offset[axis] * offset[axis];
    }
    const double offsetLength = ScalarSqrtDouble(offsetLengthSquared);
    if (offsetLength > 1.0e-9)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            local[axis] = inner[axis] + offset[axis] * bevel / offsetLength;
    return WriteRagdollVertex(output, center, rotation, renderOrigin, local, color);
}

static uint32_t ShadeColor(uint32_t base, uint32_t shade)
{
    const uint32_t red = ((base >> 0u) & 0xFFu) * shade / 255u;
    const uint32_t green = ((base >> 8u) & 0xFFu) * shade / 255u;
    const uint32_t blue = ((base >> 16u) & 0xFFu) * shade / 255u;
    return (base & UINT32_C(0xFF000000)) | (blue << 16u) | (green << 8u) | red;
}

bool WalkVisualsUpdateRagdollBuffer(LaiueGraphicsDeviceV2 *device,
                                    LaiueGraphicsHandle buffer,
                                    const VoxelRagdoll *ragdoll,
                                    const double renderOrigin[3],
                                    WalkRagdollVisualScratch *scratch)
{
    /* Keep the corner order cyclic and in lockstep with FACE_CORNERS in
     * shaders/chunk.hlsl.  Corner bits are X=1, Y=2, Z=4; the old table
     * crossed several diagonals, twisting rounded-body faces. */
    static const uint8_t faceCorners[6][4] = {
        {5u, 7u, 3u, 1u}, {6u, 4u, 0u, 2u},
        {7u, 6u, 2u, 3u}, {4u, 5u, 1u, 0u},
        {6u, 7u, 5u, 4u}, {3u, 2u, 0u, 1u},
    };
    static const uint8_t triangles[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    static const uint32_t colors[WALK_VISUAL_RAGDOLL_BODY_COUNT] = {
        UINT32_C(0xFF389AE3), UINT32_C(0xFFD67F52), UINT32_C(0xFFA4C9F0),
        UINT32_C(0xFFD67F52), UINT32_C(0xFFD67F52), UINT32_C(0xFFB8683F),
        UINT32_C(0xFFB8683F), UINT32_C(0xFF6BA64D), UINT32_C(0xFF6BA64D),
        UINT32_C(0xFF508035), UINT32_C(0xFF508035), UINT32_C(0xFF313C45),
        UINT32_C(0xFF313C45),
    };
    if (device == NULL || buffer == 0u || ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_VISUAL_RAGDOLL_BODY_COUNT || renderOrigin == NULL ||
        scratch == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, uploadBuffer),
                            sizeof(device->uploadBuffer)) ||
        device->uploadBuffer == NULL)
        return false;
    LaiueGraphicsVertexV2 *vertices = scratch->vertices;
    uint32_t count = 0u;
    for (uint32_t bodyIndex = 0u; bodyIndex < ragdoll->bodyCount; ++bodyIndex)
    {
        const VoxelRigidBody *body = &ragdoll->bodies[bodyIndex];
        double center[3];
        float rotation[9];
        if (!VoxelRigidBodyLocalPosition(body, center))
            return false;
        VoxelRigidBodyOrientationMatrix(body, rotation);
        if (bodyIndex == WALK_VISUAL_RAGDOLL_HEAD_INDEX)
        {
            const double pi = 3.14159265358979323846;
            for (uint32_t latitude = 0u; latitude < WALK_RAGDOLL_HEAD_LATITUDE_SEGMENTS;
                 ++latitude)
                for (uint32_t longitude = 0u;
                     longitude < WALK_RAGDOLL_HEAD_LONGITUDE_SEGMENTS; ++longitude)
                {
                    const double latitudeAngles[2] = {
                        -0.5 * pi + pi * (double)latitude / WALK_RAGDOLL_HEAD_LATITUDE_SEGMENTS,
                        -0.5 * pi + pi * (double)(latitude + 1u) /
                                         WALK_RAGDOLL_HEAD_LATITUDE_SEGMENTS,
                    };
                    const double longitudeAngles[2] = {
                        2.0 * pi * (double)longitude / WALK_RAGDOLL_HEAD_LONGITUDE_SEGMENTS,
                        2.0 * pi * (double)(longitude + 1u) /
                                         WALK_RAGDOLL_HEAD_LONGITUDE_SEGMENTS,
                    };
                    const double latSin[2] = {ScalarSin((float)latitudeAngles[0]),
                                               ScalarSin((float)latitudeAngles[1])};
                    const double latCos[2] = {ScalarCos((float)latitudeAngles[0]),
                                               ScalarCos((float)latitudeAngles[1])};
                    const double lonSin[2] = {ScalarSin((float)longitudeAngles[0]),
                                               ScalarSin((float)longitudeAngles[1])};
                    const double lonCos[2] = {ScalarCos((float)longitudeAngles[0]),
                                               ScalarCos((float)longitudeAngles[1])};
                    const double points[4][3] = {
                        {body->halfExtent[0] * latCos[0] * lonCos[0],
                         body->halfExtent[1] * latCos[0] * lonSin[0],
                         body->halfExtent[2] * latSin[0]},
                        {body->halfExtent[0] * latCos[0] * lonCos[1],
                         body->halfExtent[1] * latCos[0] * lonSin[1],
                         body->halfExtent[2] * latSin[0]},
                        {body->halfExtent[0] * latCos[1] * lonCos[1],
                         body->halfExtent[1] * latCos[1] * lonSin[1],
                         body->halfExtent[2] * latSin[1]},
                        {body->halfExtent[0] * latCos[1] * lonCos[0],
                         body->halfExtent[1] * latCos[1] * lonSin[0],
                         body->halfExtent[2] * latSin[1]},
                    };
                    for (uint32_t vertex = 0u; vertex < 6u; ++vertex)
                    {
                        const uint32_t corner = triangles[vertex];
                        if (!WriteRagdollVertex(&vertices[count++], center, rotation,
                                                renderOrigin, points[corner],
                                                ShadeColor(colors[bodyIndex], 238u)))
                            return false;
                    }
                }
            continue;
        }
        for (uint32_t face = 0u; face < 6u; ++face)
            for (uint32_t cellY = 0u; cellY < WALK_RAGDOLL_BEVEL_SEGMENTS; ++cellY)
                for (uint32_t cellX = 0u; cellX < WALK_RAGDOLL_BEVEL_SEGMENTS; ++cellX)
            {
                const double uv[6][2] = {
                    {(double)cellX / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)cellY / WALK_RAGDOLL_BEVEL_SEGMENTS},
                    {(double)(cellX + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)cellY / WALK_RAGDOLL_BEVEL_SEGMENTS},
                    {(double)(cellX + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)(cellY + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS},
                    {(double)cellX / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)cellY / WALK_RAGDOLL_BEVEL_SEGMENTS},
                    {(double)(cellX + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)(cellY + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS},
                    {(double)cellX / WALK_RAGDOLL_BEVEL_SEGMENTS,
                     (double)(cellY + 1u) / WALK_RAGDOLL_BEVEL_SEGMENTS},
                };
                for (uint32_t vertex = 0u; vertex < 6u; ++vertex)
                {
                    const uint32_t shade = face == 1u ? 255u : (face == 0u ? 238u : 205u);
                    if (!WriteRoundedBodyVertex(&vertices[count++], body, center, rotation,
                            renderOrigin, faceCorners[face], uv[vertex][0], uv[vertex][1],
                            ShadeColor(colors[bodyIndex], shade)))
                        return false;
                }
            }
    }

    static const float identity[9] = {1.0f, 0.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f};
    const double pi = 3.14159265358979323846;
    double latSin[WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS + 1u];
    double latCos[WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS + 1u];
    double lonSin[WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS + 1u];
    double lonCos[WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS + 1u];
    for (uint32_t i = 0u; i <= WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS; ++i)
    {
        const double angle = -0.5 * pi + pi * (double)i / WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS;
        latSin[i] = ScalarSin((float)angle);
        latCos[i] = ScalarCos((float)angle);
    }
    for (uint32_t i = 0u; i <= WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS; ++i)
    {
        const double angle = 2.0 * pi * (double)i / WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS;
        lonSin[i] = ScalarSin((float)angle);
        lonCos[i] = ScalarCos((float)angle);
    }
    for (uint32_t jointIndex = 0u; jointIndex < ragdoll->jointCount; ++jointIndex)
    {
        const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[jointIndex];
        const VoxelRigidBody *parent = &ragdoll->bodies[joint->bodyA];
        const VoxelRigidBody *child = &ragdoll->bodies[joint->bodyB];
        double center[3];
        float rotation[9];
        if (!VoxelRigidBodyLocalPosition(parent, center))
            return false;
        VoxelRigidBodyOrientationMatrix(parent, rotation);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            center[axis] += (double)rotation[axis] * joint->anchorA[0] +
                            (double)rotation[3u + axis] * joint->anchorA[1] +
                            (double)rotation[6u + axis] * joint->anchorA[2];
        double smallest = DBL_MAX;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            const double extent = Minimum(parent->halfExtent[axis], child->halfExtent[axis]);
            if (extent < smallest)
                smallest = extent;
        }
        const double radius = smallest * 0.88;
        for (uint32_t latitude = 0u; latitude < WALK_RAGDOLL_JOINT_LATITUDE_SEGMENTS;
             ++latitude)
            for (uint32_t longitude = 0u;
                 longitude < WALK_RAGDOLL_JOINT_LONGITUDE_SEGMENTS; ++longitude)
            {
                const double points[4][3] = {
                    {radius * latCos[latitude] * lonCos[longitude],
                     radius * latCos[latitude] * lonSin[longitude],
                     radius * latSin[latitude]},
                    {radius * latCos[latitude] * lonCos[longitude + 1u],
                     radius * latCos[latitude] * lonSin[longitude + 1u],
                     radius * latSin[latitude]},
                    {radius * latCos[latitude + 1u] * lonCos[longitude + 1u],
                     radius * latCos[latitude + 1u] * lonSin[longitude + 1u],
                     radius * latSin[latitude + 1u]},
                    {radius * latCos[latitude + 1u] * lonCos[longitude],
                     radius * latCos[latitude + 1u] * lonSin[longitude],
                     radius * latSin[latitude + 1u]},
                };
                for (uint32_t vertex = 0u; vertex < 6u; ++vertex)
                    if (!WriteRagdollVertex(&vertices[count++], center, identity,
                                            renderOrigin, points[triangles[vertex]],
                                            colors[joint->bodyA]))
                        return false;
            }
    }
    if (count != WALK_RAGDOLL_VISUAL_VERTEX_COUNT)
        return false;
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload), .buffer = buffer,
        .data = vertices, .sizeBytes = sizeof(scratch->vertices),
    };
    return device->uploadBuffer(device, &upload) != 0u;
}
