#include "walk_visuals.h"

#include "media/image.h"
#include "math/scalar.h"
#include "platform/system.h"

#include <float.h>
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

static LaiueGraphicsVertexV2 TerrainVertex(float x, float y, float z,
                                           float u, float v)
{
    return (LaiueGraphicsVertexV2){
        .position = {x, y, z}, .uv = {u, v}, .colorRGBA = UINT32_MAX,
    };
}

static void AppendTerrainQuad(LaiueGraphicsVertexV2 *vertices, uint32_t *count,
                              const float positions[4][3], const float uv[4][2])
{
    const LaiueGraphicsVertexV2 corners[4] = {
        TerrainVertex(positions[0][0], positions[0][1], positions[0][2], uv[0][0], uv[0][1]),
        TerrainVertex(positions[1][0], positions[1][1], positions[1][2], uv[1][0], uv[1][1]),
        TerrainVertex(positions[2][0], positions[2][1], positions[2][2], uv[2][0], uv[2][1]),
        TerrainVertex(positions[3][0], positions[3][1], positions[3][2], uv[3][0], uv[3][1]),
    };
    static const uint8_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (uint32_t index = 0u; index < 6u; ++index)
        vertices[(*count)++] = corners[indices[index]];
}

static void BuildTerrainSkirt(LaiueGraphicsVertexV2 *vertices,
                              float bottom, float top)
{
    const float pad = 0.01f;
    const float uv[4][2] = {{0.0f, 0.0f}, {24.0f, 0.0f},
                            {24.0f, (top - bottom) / 8.0f},
                            {0.0f, (top - bottom) / 8.0f}};
    const float sides[4][4][3] = {
        {{-64.0f, -64.0f - pad, bottom}, {128.0f, -64.0f - pad, bottom},
         {128.0f, -64.0f - pad, top}, {-64.0f, -64.0f - pad, top}},
        {{128.0f + pad, -64.0f, bottom}, {128.0f + pad, 128.0f, bottom},
         {128.0f + pad, 128.0f, top}, {128.0f + pad, -64.0f, top}},
        {{128.0f, 128.0f + pad, bottom}, {-64.0f, 128.0f + pad, bottom},
         {-64.0f, 128.0f + pad, top}, {128.0f, 128.0f + pad, top}},
        {{-64.0f - pad, 128.0f, bottom}, {-64.0f - pad, -64.0f, bottom},
         {-64.0f - pad, -64.0f, top}, {-64.0f - pad, 128.0f, top}},
    };
    uint32_t count = 0u;
    for (uint32_t side = 0u; side < 4u; ++side)
        AppendTerrainQuad(vertices, &count, sides[side], uv);
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

    LaiueGraphicsVertexV2 grass[6];
    const float topPositions[4][3] = {
        {-64.0f, -64.0f, 1.01f}, {128.0f, -64.0f, 1.01f},
        {128.0f, 128.0f, 1.01f}, {-64.0f, 128.0f, 1.01f},
    };
    const float topUv[4][2] = {{0.0f, 0.0f}, {24.0f, 0.0f},
                               {24.0f, 24.0f}, {0.0f, 24.0f}};
    uint32_t count = 0u;
    AppendTerrainQuad(grass, &count, topPositions, topUv);
    LaiueGraphicsVertexV2 dirt[WALK_TERRAIN_SKIRT_VERTEX_COUNT];
    LaiueGraphicsVertexV2 stone[WALK_TERRAIN_SKIRT_VERTEX_COUNT];
    BuildTerrainSkirt(dirt, -3.0f, 1.0f);
    BuildTerrainSkirt(stone, -7.0f, -3.0f);
    if (!UploadVertexBuffer(device, grass, 6u, &outBuffers[0]) ||
        !UploadVertexBuffer(device, dirt, WALK_TERRAIN_SKIRT_VERTEX_COUNT,
                            &outBuffers[1]) ||
        !UploadVertexBuffer(device, stone, WALK_TERRAIN_SKIRT_VERTEX_COUNT,
                            &outBuffers[2]))
        goto failed;
    return true;

failed:
    WalkVisualsDestroyTerrain(device, outBuffers, outTextures, outSampler);
    return false;
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
    static const uint8_t faceCorners[6][4] = {
        {0u, 1u, 2u, 3u}, {4u, 7u, 6u, 5u}, {0u, 4u, 5u, 1u},
        {3u, 2u, 6u, 7u}, {0u, 3u, 7u, 4u}, {1u, 5u, 6u, 2u},
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
