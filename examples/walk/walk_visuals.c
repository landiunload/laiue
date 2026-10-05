#include "walk_visuals.h"
#include "walk_math.h"
#include "walk_physics.h"

#include "media/image.h"
#include "math/scalar.h"
#include "platform/system.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

enum
{
    WALK_VISUAL_RAGDOLL_BODY_COUNT = 13u,
    WALK_VISUAL_RAGDOLL_HEAD_INDEX = 2u,
    WALK_VISUAL_RAGDOLL_JOINT_COUNT = 12u,
    WALK_RAGDOLL_RADIAL_SEGMENTS = 8u,
    WALK_RAGDOLL_PROFILE_RINGS = 5u,
};

static bool DeviceFieldPresent(const LaiueGraphicsDeviceV2 *device, size_t offset, size_t size)
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

static void ReleaseHandle(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle *handle)
{
    if (handle == NULL || *handle == 0u)
        return;
    if (DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, destroyHandle),
                           sizeof(device->destroyHandle)) &&
        device->destroyHandle != NULL)
        device->destroyHandle(device, *handle);
    *handle = 0u;
}

static bool UploadVertexBuffer(LaiueGraphicsDeviceV2 *device, const LaiueGraphicsVertexV2 *vertices,
                               uint32_t vertexCount, LaiueGraphicsHandle *outBuffer)
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
        .structSize = sizeof(upload),
        .buffer = *outBuffer,
        .data = vertices,
        .sizeBytes = description.sizeBytes,
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
        !readAsset(assetContext, relativePath, &encoded, &encodedBytes) || encoded == NULL ||
        encodedBytes == 0u)
        goto cleanup;

    ImageInfo info;
    memset(&info, 0, sizeof(info));
    if (ImageInspect(encoded, encodedBytes, &info) != IMAGE_OK || info.frameCount != 1u ||
        info.frameBytes == 0u || info.pixelBytes != info.frameBytes || info.width > UINT32_MAX / 4u)
        goto cleanup;
    pixels = (uint8_t *)PlatformAllocate(info.pixelBytes, false);
    if (info.scratchBytes != 0u)
        scratch = (uint8_t *)PlatformAllocate(info.scratchBytes, false);
    if (pixels == NULL || (info.scratchBytes != 0u && scratch == NULL) ||
        ImageDecode(encoded, encodedBytes, &info, pixels, info.pixelBytes, scratch,
                    info.scratchBytes) != IMAGE_OK)
        goto cleanup;

    uint32_t mipLevels = 1u;
    for (uint32_t largest = info.width > info.height ? info.width : info.height; largest > 1u;
         largest >>= 1u)
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
            .structSize = sizeof(upload),
            .texture = *outTexture,
            .data = source,
            .sizeBytes = (uint64_t)width * height * 4u,
            .rowPitchBytes = width * 4u,
            .mipLevel = level,
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

bool WalkVisualsCreateTerrain(LaiueGraphicsDeviceV2 *device, WalkReadAssetFn readAsset,
                              void *assetContext,
                              LaiueGraphicsHandle outBuffers[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle outTextures[WALK_VISUAL_TEXTURE_COUNT],
                              LaiueGraphicsHandle *outSampler)
{
    static const char *const paths[WALK_VISUAL_TEXTURE_COUNT] = {
        "textures/grass.png",
        "textures/dirt.png",
        "textures/stone.png",
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
        .structSize = sizeof(sampler),
        .minFilter = LAIUE_GRAPHICS_FILTER_LINEAR,
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

static WorldRegionContents WalkVisualFillRegion(void *opaque, int64_t minX, int64_t minY,
                                                int64_t minZ, int32_t sizeX, int32_t sizeY,
                                                int32_t sizeZ, BlockType *blocks)
{
    WalkVisualBuildContext *context = (WalkVisualBuildContext *)opaque;
    if (context == NULL || context->getBlock == NULL || blocks == NULL || sizeX <= 0 ||
        sizeY <= 0 || sizeZ <= 0)
        return WORLD_REGION_ALL_AIR;
    for (int32_t y = 0; y < sizeY; ++y)
        for (int32_t x = 0; x < sizeX; ++x)
            for (int32_t z = 0; z < sizeZ; ++z)
            {
                int64_t worldX = 0, worldY = 0, worldZ = 0;
                uint8_t material = 0u;
                if (AddInt64(minX, x, &worldX) && AddInt64(minY, y, &worldY) &&
                    AddInt64(minZ, z, &worldZ))
                    material = context->getBlock(context->blockContext, worldX, worldY, worldZ);
                blocks[((size_t)y * (size_t)sizeX + (size_t)x) * (size_t)sizeZ + (size_t)z] =
                    material;
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

bool WalkVisualsCreateChunkSet(const LaiueMesherServiceV1 *mesher, WalkVisualChunkSet *outSet)
{
    if (outSet == NULL)
        return false;
    memset(outSet, 0, sizeof(*outSet));
    if (mesher == NULL || mesher->structSize < sizeof(*mesher) || mesher->scratchCreate == NULL ||
        mesher->scratchDestroy == NULL || mesher->buildChunkMesh == NULL)
        return false;
    outSet->scratch = mesher->scratchCreate();
    return outSet->scratch != NULL;
}

void WalkVisualsInvalidateChunkSet(LaiueGraphicsDeviceV2 *device, WalkVisualChunkSet *set)
{
    if (set == NULL)
        return;
    for (uint32_t i = 0u; i < WALK_VISUAL_CHUNK_COUNT; ++i)
        DestroyChunk(device, &set->chunks[i]);
    set->centerValid = false;
}

void WalkVisualsDestroyChunkSet(LaiueGraphicsDeviceV2 *device, const LaiueMesherServiceV1 *mesher,
                                WalkVisualChunkSet *set)
{
    if (set == NULL)
        return;
    WalkVisualsInvalidateChunkSet(device, set);
    if (set->scratch != NULL && mesher != NULL && mesher->scratchDestroy != NULL)
        mesher->scratchDestroy(set->scratch);
    memset(set, 0, sizeof(*set));
}

static bool BuildChunkVisual(LaiueGraphicsDeviceV2 *device, const LaiueMesherServiceV1 *mesher,
                             ChunkMesherScratch *scratch, WalkVisualGetBlockFn getBlock,
                             void *blockContext, const int64_t coordinate[3],
                             WalkVisualChunk *outChunk)
{
    if (device == NULL || mesher == NULL || scratch == NULL || getBlock == NULL ||
        coordinate == NULL || outChunk == NULL || coordinate[0] < INT64_MIN / 64 + 1 ||
        coordinate[0] > INT64_MAX / 64 - 1 || coordinate[1] < INT64_MIN / 64 + 1 ||
        coordinate[1] > INT64_MAX / 64 - 1 || coordinate[2] < INT64_MIN / 64 + 1 ||
        coordinate[2] > INT64_MAX / 64 - 1)
        return false;

    WalkVisualBuildContext context = {getBlock, blockContext};
    const ChunkMesherWorldSource source = {&context, WalkVisualFillRegion};
    ChunkQuad *quads = NULL;
    uint32_t quadCount = 0u;
    if (!mesher->buildChunkMesh(&source, scratch, coordinate[0], coordinate[1], coordinate[2],
                                &quads, &quadCount))
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
        {5u, 7u, 3u, 1u}, {6u, 4u, 0u, 2u}, {7u, 6u, 2u, 3u},
        {4u, 5u, 1u, 0u}, {6u, 7u, 5u, 4u}, {3u, 2u, 0u, 1u},
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
            const uint32_t materialIndex =
                blockMaterial == 0u
                    ? UINT32_MAX
                    : (blockMaterial <= WALK_VISUAL_TEXTURE_COUNT ? blockMaterial - 1u : 2u);
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
                if (face < 2u)
                {
                    u = y;
                    v = -z;
                }
                else if (face < 4u)
                {
                    u = x;
                    v = -z;
                }
                else
                {
                    u = x;
                    v = y;
                }
                const uint32_t shade = shadeByFace[face];
                corners[cornerIndex] = (LaiueGraphicsVertexV2){
                    .position = {x, y, z},
                    .uv = {u, v},
                    .colorRGBA = UINT32_C(0xFF000000) | (shade << 16u) | (shade << 8u) | shade,
                };
            }
            for (uint32_t vertex = 0u; vertex < 6u; ++vertex)
                vertices[written[material]++] = corners[triangleCorners[vertex]];
        }
        const bool uploaded =
            written[material] == vertexCount &&
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

bool WalkVisualsUpdateChunkSet(LaiueGraphicsDeviceV2 *device, const LaiueMesherServiceV1 *mesher,
                               WalkVisualChunkSet *set, WalkVisualGetBlockFn getBlock,
                               void *blockContext, const int64_t centerBlock[3])
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
            if (!used[i] && set->chunks[i].ready && set->chunks[i].coordinate[0] == desired[d][0] &&
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
        for (slot = 0u; slot < WALK_VISUAL_CHUNK_COUNT && used[slot]; ++slot)
        {
        }
        if (slot == WALK_VISUAL_CHUNK_COUNT)
            return false;
        WalkVisualChunk replacement = {0};
        if (!BuildChunkVisual(device, mesher, set->scratch, getBlock, blockContext, desired[d],
                              &replacement))
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

void WalkVisualsInvalidateBlock(WalkVisualChunkSet *set, LaiueGraphicsDeviceV2 *device,
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
        if (edgeMask[axis] == 1u)
            offsets[axis][counts[axis]++] = -1;
        else if (edgeMask[axis] == 2u)
            offsets[axis][counts[axis]++] = 1;
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
                !SubtractInt64(chunk->coordinate[axis] * 64, renderOriginBlock[axis],
                               &relativeOrigin[axis]))
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

static void AppendFarTerrainQuad(LaiueGraphicsVertexV2 vertices[6], uint32_t *written, float x0,
                                 float y0, float x1, float y1)
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
    AppendFarTerrainQuad(vertices, &written, -outer, innerMinimum, innerMinimum, innerMaximum);
    AppendFarTerrainQuad(vertices, &written, innerMaximum, innerMinimum, outer, innerMaximum);
    return written == WALK_VISUAL_FAR_TERRAIN_VERTEX_COUNT &&
           UploadVertexBuffer(device, vertices, written, outBuffer);
}

bool WalkVisualsBuildFarTerrainDraw(LaiueGraphicsHandle buffer, const int64_t renderOriginBlock[3],
                                    const LaiueGraphicsHandle textures[WALK_VISUAL_TEXTURE_COUNT],
                                    LaiueGraphicsHandle sampler, LaiueGraphicsDrawItemV2 *outDraw)
{
    if (outDraw == NULL)
        return false;
    memset(outDraw, 0, sizeof(*outDraw));
    if (buffer == 0u || renderOriginBlock == NULL || textures == NULL || textures[0] == 0u ||
        sampler == 0u)
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

void WalkVisualsDestroyBuffer(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle *buffer)
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

bool WalkVisualsCreateRagdollBuffer(LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle *outBuffer)
{
    return CreateVertexBuffer(
        device, (uint64_t)WALK_RAGDOLL_VISUAL_VERTEX_COUNT * sizeof(LaiueGraphicsVertexV2),
        outBuffer);
}

static bool WriteRagdollVertex(LaiueGraphicsVertexV2 *output, const double center[3],
                               const float rotation[9], const double renderOrigin[3],
                               const double local[3], uint32_t color)
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        const double value = center[axis] - renderOrigin[axis] + (double)rotation[axis] * local[0] +
                             (double)rotation[3u + axis] * local[1] +
                             (double)rotation[6u + axis] * local[2];
        if (!WalkMathFinite(value) || WalkMathAbs(value) > 1000000.0)
            return false;
        output->position[axis] = (float)value;
    }
    output->uv[0] = output->uv[1] = 0.0f;
    output->colorRGBA = color;
    return true;
}

/* A single small mesh follows the solved physical poses. Limb centerlines use
 * joint anchors, because the diagonal arm colliders are not anatomical bones.
 * Fixed radial tables avoid rebuilding trigonometry for every rendered frame. */
static const double radial8[8][2] = {
    {1.0, 0.0},  {0.7071067811865475, 0.7071067811865475},
    {0.0, 1.0},  {-0.7071067811865475, 0.7071067811865475},
    {-1.0, 0.0}, {-0.7071067811865475, -0.7071067811865475},
    {0.0, -1.0}, {0.7071067811865475, -0.7071067811865475},
};
static const double radial12[12][2] = {
    {1.0, 0.0},  {0.8660254037844386, 0.5},   {0.5, 0.8660254037844386},
    {0.0, 1.0},  {-0.5, 0.8660254037844386},  {-0.8660254037844386, 0.5},
    {-1.0, 0.0}, {-0.8660254037844386, -0.5}, {-0.5, -0.8660254037844386},
    {0.0, -1.0}, {0.5, -0.8660254037844386},  {0.8660254037844386, -0.5},
};
static const double latitude8[9][2] = {
    {0.0, -1.0},
    {0.3826834323650898, -0.9238795325112867},
    {0.7071067811865475, -0.7071067811865475},
    {0.9238795325112867, -0.3826834323650898},
    {1.0, 0.0},
    {0.9238795325112867, 0.3826834323650898},
    {0.7071067811865475, 0.7071067811865475},
    {0.3826834323650898, 0.9238795325112867},
    {0.0, 1.0},
};
static const uint32_t skinColor = UINT32_C(0xFF9BBCE5);
static const uint32_t hairColor = UINT32_C(0xFF29394B);
static const uint32_t shirtColor = UINT32_C(0xFF4488D8);
static const uint32_t trousersColor = UINT32_C(0xFF695342);
static const uint32_t bootColor = UINT32_C(0xFF28313B);

typedef struct WalkRagdollMeshWriter
{
    LaiueGraphicsVertexV2 *vertices;
    uint32_t count;
    const double *origin;
    double center[3];
    float rotation[9];
} WalkRagdollMeshWriter;

static uint32_t LitColor(uint32_t base, const double normal[3], const float rotation[9])
{
    const double length =
        ScalarSqrtDouble(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    double illumination = 0.0;
    static const double light[3] = {-0.45, -0.30, 0.84};
    if (WalkMathFinite(length) && length > 1.0e-9)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            illumination += light[axis] *
                            (rotation[axis] * normal[0] + rotation[3u + axis] * normal[1] +
                             rotation[6u + axis] * normal[2]) /
                            length;
    const uint32_t shade = (uint32_t)(170.0 + 85.0 * Maximum(0.0, Minimum(1.0, illumination)));
    const uint32_t red = (base & 0xFFu) * shade / 255u;
    const uint32_t green = ((base >> 8u) & 0xFFu) * shade / 255u;
    const uint32_t blue = ((base >> 16u) & 0xFFu) * shade / 255u;
    return (base & UINT32_C(0xFF000000)) | (blue << 16u) | (green << 8u) | red;
}

static bool MeshVertex(const WalkRagdollMeshWriter *writer, LaiueGraphicsVertexV2 *vertex,
                       const double local[3], const double normal[3], uint32_t color)
{
    return WriteRagdollVertex(vertex, writer->center, writer->rotation, writer->origin, local,
                              LitColor(color, normal, writer->rotation));
}

static bool MeshTriangle(WalkRagdollMeshWriter *writer, const LaiueGraphicsVertexV2 *a,
                         const LaiueGraphicsVertexV2 *b, const LaiueGraphicsVertexV2 *c)
{
    if (writer->count > WALK_RAGDOLL_VISUAL_VERTEX_COUNT - 3u)
        return false;
    writer->vertices[writer->count++] = *a;
    writer->vertices[writer->count++] = *b;
    writer->vertices[writer->count++] = *c;
    return true;
}

static bool MeshEllipsoid(WalkRagdollMeshWriter *writer, const double offset[3],
                          const double radius[3], uint32_t color, bool head)
{
    const uint32_t longitudeCount = head ? 12u : 8u;
    const uint32_t latitudeCount = head ? 8u : 4u;
    const double (*radial)[2] = head ? radial12 : radial8;
    LaiueGraphicsVertexV2 points[86];
    for (uint32_t latitude = 1u; latitude < latitudeCount; ++latitude)
        for (uint32_t longitude = 0u; longitude < longitudeCount; ++longitude)
        {
            const double *ring = latitude8[latitude * 8u / latitudeCount];
            const double unit[3] = {ring[0] * radial[longitude][0], ring[0] * radial[longitude][1],
                                    ring[1]};
            double local[3], normal[3];
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                local[axis] = offset[axis] + radius[axis] * unit[axis];
                normal[axis] = unit[axis] / radius[axis];
            }
            const bool hair = head && (unit[2] >= 0.70 || (unit[1] < -0.35 && unit[2] > -0.05));
            if (!MeshVertex(writer, &points[(latitude - 1u) * longitudeCount + longitude], local,
                            normal, hair ? hairColor : color))
                return false;
        }
    const uint32_t bottom = (latitudeCount - 1u) * longitudeCount;
    const uint32_t top = bottom + 1u;
    for (uint32_t cap = 0u; cap < 2u; ++cap)
    {
        const double normal[3] = {0.0, 0.0, cap == 0u ? -1.0 : 1.0};
        const double local[3] = {offset[0], offset[1], offset[2] + normal[2] * radius[2]};
        if (!MeshVertex(writer, &points[bottom + cap], local, normal,
                        head && cap != 0u ? hairColor : color))
            return false;
    }
    for (uint32_t longitude = 0u; longitude < longitudeCount; ++longitude)
    {
        const uint32_t next = (longitude + 1u) % longitudeCount;
        const uint32_t last = (latitudeCount - 2u) * longitudeCount;
        if (!MeshTriangle(writer, &points[bottom], &points[next], &points[longitude]) ||
            !MeshTriangle(writer, &points[top], &points[last + longitude], &points[last + next]))
            return false;
        for (uint32_t latitude = 0u; latitude + 2u < latitudeCount; ++latitude)
        {
            const uint32_t low = latitude * longitudeCount;
            const uint32_t high = low + longitudeCount;
            if (!MeshTriangle(writer, &points[low + longitude], &points[low + next],
                              &points[high + next]) ||
                !MeshTriangle(writer, &points[low + longitude], &points[high + next],
                              &points[high + longitude]))
                return false;
        }
    }
    return true;
}

static bool MeshLoft(WalkRagdollMeshWriter *writer, const double bottom[3], const double top[3],
                     double radiusX, double radiusY, const double profile[5], uint32_t color)
{
    static const double levels[5] = {0.0, 0.12, 0.5, 0.88, 1.0};
    double direction[3], side[3];
    double lengthSquared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        direction[axis] = top[axis] - bottom[axis];
        lengthSquared += direction[axis] * direction[axis];
    }
    const double length = ScalarSqrtDouble(lengthSquared);
    if (!(length > 1.0e-6))
        return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        direction[axis] /= length;
    /* All humanoid limb anchors lie in the local XZ plane. Use a stable
     * perpendicular basis even if an asset later supplies a Y component. */
    const double reference[3] = {WalkMathAbs(direction[0]) < 0.9 ? 1.0 : 0.0,
                                 WalkMathAbs(direction[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    const double dot = reference[0] * direction[0] + reference[1] * direction[1];
    double sideLengthSquared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        side[axis] = reference[axis] - dot * direction[axis];
        sideLengthSquared += side[axis] * side[axis];
    }
    const double sideLength = ScalarSqrtDouble(sideLengthSquared);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        side[axis] /= sideLength;
    const double front[3] = {
        direction[1] * side[2] - direction[2] * side[1],
        direction[2] * side[0] - direction[0] * side[2],
        direction[0] * side[1] - direction[1] * side[0],
    };
    LaiueGraphicsVertexV2 points[42];
    for (uint32_t ring = 0u; ring < WALK_RAGDOLL_PROFILE_RINGS; ++ring)
        for (uint32_t radial = 0u; radial < WALK_RAGDOLL_RADIAL_SEGMENTS; ++radial)
        {
            double local[3], normal[3];
            const double capSlope = ring == 0u ? -0.8 : (ring == 4u ? 0.8 : 0.0);
            for (uint32_t axis = 0u; axis < 3u; ++axis)
            {
                local[axis] = bottom[axis] + direction[axis] * length * levels[ring] +
                              profile[ring] * (side[axis] * radial8[radial][0] * radiusX +
                                               front[axis] * radial8[radial][1] * radiusY);
                normal[axis] = side[axis] * radial8[radial][0] + front[axis] * radial8[radial][1] +
                               capSlope * direction[axis];
            }
            if (!MeshVertex(writer, &points[ring * 8u + radial], local, normal, color))
                return false;
        }
    double bottomNormal[3] = {-direction[0], -direction[1], -direction[2]};
    if (!MeshVertex(writer, &points[40], bottom, bottomNormal, color) ||
        !MeshVertex(writer, &points[41], top, direction, color))
        return false;
    for (uint32_t radial = 0u; radial < WALK_RAGDOLL_RADIAL_SEGMENTS; ++radial)
    {
        const uint32_t next = (radial + 1u) % WALK_RAGDOLL_RADIAL_SEGMENTS;
        if (!MeshTriangle(writer, &points[40], &points[next], &points[radial]) ||
            !MeshTriangle(writer, &points[41], &points[32u + radial], &points[32u + next]))
            return false;
        for (uint32_t ring = 0u; ring + 1u < WALK_RAGDOLL_PROFILE_RINGS; ++ring)
        {
            const uint32_t low = ring * 8u, high = low + 8u;
            if (!MeshTriangle(writer, &points[low + radial], &points[low + next],
                              &points[high + next]) ||
                !MeshTriangle(writer, &points[low + radial], &points[high + next],
                              &points[high + radial]))
                return false;
        }
    }
    return true;
}

static bool MeshEye(WalkRagdollMeshWriter *writer, double x, double y, double z, double width,
                    double height, uint32_t color)
{
    const double normal[3] = {0.0, 1.0, 0.0};
    const double center[3] = {x, y, z};
    LaiueGraphicsVertexV2 points[9];
    if (!MeshVertex(writer, &points[8], center, normal, color))
        return false;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const double point[3] = {x + width * radial8[i][0], y, z + height * radial8[i][1]};
        if (!MeshVertex(writer, &points[i], point, normal, color))
            return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i)
        if (!MeshTriangle(writer, &points[8], &points[(i + 1u) % 8u], &points[i]))
            return false;
    return true;
}

bool WalkVisualsUpdateRagdollBuffer(const WalkPhysicsContext *context,
                                    LaiueGraphicsDeviceV2 *device, LaiueGraphicsHandle buffer,
                                    const VoxelRagdoll *ragdoll, const double renderOrigin[3],
                                    WalkRagdollVisualScratch *scratch)
{
    if (device == NULL || buffer == 0u || ragdoll == NULL || !ragdoll->initialized ||
        ragdoll->bodyCount != WALK_VISUAL_RAGDOLL_BODY_COUNT ||
        ragdoll->jointCount != WALK_VISUAL_RAGDOLL_JOINT_COUNT || renderOrigin == NULL ||
        scratch == NULL ||
        !DeviceFieldPresent(device, offsetof(LaiueGraphicsDeviceV2, uploadBuffer),
                            sizeof(device->uploadBuffer)) ||
        device->uploadBuffer == NULL)
        return false;
    for (uint32_t joint = 0u; joint < ragdoll->jointCount; ++joint)
        if (ragdoll->joints[joint].bodyA >= ragdoll->bodyCount ||
            ragdoll->joints[joint].bodyB >= ragdoll->bodyCount)
            return false;
    for (uint32_t body = 0u; body < ragdoll->bodyCount; ++body)
    {
        double normSquared = 0.0;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
            if (!WalkMathFinite(ragdoll->bodies[body].halfExtent[axis]) ||
                !(ragdoll->bodies[body].halfExtent[axis] > 0.0))
                return false;
        for (uint32_t axis = 0u; axis < 4u; ++axis)
            normSquared +=
                ragdoll->bodies[body].orientation[axis] * ragdoll->bodies[body].orientation[axis];
        if (!WalkMathFinite(normSquared) || WalkMathAbs(normSquared - 1.0) > 1.0e-5)
            return false;
    }

    WalkRagdollMeshWriter writer = {.vertices = scratch->vertices, .origin = renderOrigin};
    static const double zero[3] = {0.0, 0.0, 0.0};
    static const double limbProfile[5] = {0.68, 0.94, 1.0, 0.94, 0.68};
    static const double torsoProfile[5] = {0.76, 0.88, 1.0, 1.12, 0.93};
    static const double pelvisProfile[5] = {0.78, 0.96, 1.0, 0.93, 0.77};
    for (uint32_t index = 0u; index < ragdoll->bodyCount; ++index)
    {
        const VoxelRigidBody *body = &ragdoll->bodies[index];
        if (!WalkBodyLocalPosition(context, body, writer.center))
            return false;
        WalkBodyOrientationMatrix(context, body, writer.rotation);
        if (index == WALK_VISUAL_RAGDOLL_HEAD_INDEX)
        {
            if (!MeshEllipsoid(&writer, zero, body->halfExtent, skinColor, true))
                return false;
            /* +Y is the shared controller's forward direction. Features rotate
             * with the physical head, making facing direction readable. */
            for (uint32_t eye = 0u; eye < 2u; ++eye)
            {
                const double x = (eye == 0u ? -0.36 : 0.36) * body->halfExtent[0];
                if (!MeshEye(&writer, x, body->halfExtent[1] * 1.005, body->halfExtent[2] * 0.12,
                             body->halfExtent[0] * 0.21, body->halfExtent[2] * 0.24,
                             UINT32_C(0xFFF3F3F3)) ||
                    !MeshEye(&writer, x, body->halfExtent[1] * 1.02, body->halfExtent[2] * 0.10,
                             body->halfExtent[0] * 0.095, body->halfExtent[2] * 0.14,
                             UINT32_C(0xFF202B36)))
                    return false;
            }
            const double nose[3] = {0.0, body->halfExtent[1] * 0.97, -body->halfExtent[2] * 0.12};
            const double noseSize[3] = {body->halfExtent[0] * 0.14, body->halfExtent[1] * 0.20,
                                        body->halfExtent[2] * 0.16};
            if (!MeshEllipsoid(&writer, nose, noseSize, skinColor, false) ||
                !MeshEye(&writer, 0.0, body->halfExtent[1] * 0.94, -body->halfExtent[2] * 0.42,
                         body->halfExtent[0] * 0.22, body->halfExtent[2] * 0.038,
                         UINT32_C(0xFF536998)))
                return false;
            continue;
        }
        double bottom[3] = {0.0, 0.0, -body->halfExtent[2]};
        double top[3] = {0.0, 0.0, body->halfExtent[2]};
        double width = body->halfExtent[0], depth = body->halfExtent[1];
        uint32_t color = index == 1u || (index >= 3u && index <= 6u)
                             ? shirtColor
                             : (index < 11u ? trousersColor : bootColor);
        const double *profile =
            index == 0u ? pelvisProfile : (index == 1u ? torsoProfile : limbProfile);
        if (index >= 3u && index <= 10u)
        {
            for (uint32_t jointIndex = 0u; jointIndex < ragdoll->jointCount; ++jointIndex)
            {
                const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[jointIndex];
                if (joint->bodyB == index)
                    memcpy(top, joint->anchorB, sizeof(top));
                if (joint->bodyA == index)
                    memcpy(bottom, joint->anchorA, sizeof(bottom));
            }
            /* The upper-arm box spans a diagonal bone; its X extent measures
             * that diagonal, not arm thickness. */
            width = Minimum(width, depth) * 0.94;
            depth *= 0.94;
        }
        if (index == 5u || index == 6u)
        {
            /* The forearm drives the palm as a rigid attachment. With the
             * 1.8 m humanoid, the fingertips reach about 0.82 m at rest. */
            const double hand[3] = {bottom[0], bottom[1], bottom[2]};
            const double handSize[3] = {width * 0.95, depth * 0.74, depth * 1.55};
            if (!MeshEllipsoid(&writer, hand, handSize, skinColor, false))
                return false;
            bottom[2] += depth * 0.8;
        }
        if (!MeshLoft(&writer, bottom, top, width, depth, profile, color))
            return false;
    }
    for (uint32_t index = 0u; index < ragdoll->jointCount; ++index)
    {
        const VoxelRagdollBallJointDefinition *joint = &ragdoll->joints[index];
        const VoxelRigidBody *parent = &ragdoll->bodies[joint->bodyA];
        const VoxelRigidBody *child = &ragdoll->bodies[joint->bodyB];
        if (!WalkBodyLocalPosition(context, parent, writer.center))
            return false;
        WalkBodyOrientationMatrix(context, parent, writer.rotation);
        const double radius = Minimum(Minimum(parent->halfExtent[0], parent->halfExtent[1]),
                                      Minimum(child->halfExtent[0], child->halfExtent[1])) *
                              (joint->bodyB == WALK_VISUAL_RAGDOLL_HEAD_INDEX ? 0.50 : 0.96);
        const double size[3] = {radius, radius, radius};
        const uint32_t color =
            joint->bodyB == 2u ? skinColor : (joint->bodyB < 7u ? shirtColor : trousersColor);
        if (!MeshEllipsoid(&writer, joint->anchorA, size, color, false))
            return false;
    }
    if (writer.count != WALK_RAGDOLL_VISUAL_VERTEX_COUNT)
        return false;
    const LaiueGraphicsBufferUploadV1 upload = {
        .structSize = sizeof(upload),
        .buffer = buffer,
        .data = writer.vertices,
        .sizeBytes = sizeof(scratch->vertices),
    };
    return device->uploadBuffer(device, &upload) != 0u;
}
