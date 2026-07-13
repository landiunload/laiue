#pragma pack_matrix(row_major)

cbuffer FrameConstants : register(b0)
{
    float4x4 viewProjection;
    float3 chunkOriginRelative;
    uint3 chunkBaseLow;
};

ByteAddressBuffer quadBuffer : register(t0);

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 localPosition : LOCALPOSITION;
    nointerpolation uint face : FACEINDEX;
};

static const uint CORNER_PATTERN[6] = { 0, 1, 2, 0, 2, 3 };

// Четыре угла каждой грани (обход по часовой стрелке снаружи).
// Порядок граней: +X, -X, +Y(2ndH), -Y, +Z(высота), -Z.
// Компоненты: (X, old_Y=height, old_Z=2ndH) — в коде ниже old_Y
// маппится в Z (высота), old_Z — в Y (2ndH).
static const uint3 FACE_CORNERS[6][4] =
{
    { uint3(1,1,0), uint3(1,1,1), uint3(1,0,1), uint3(1,0,0) },  // +X
    { uint3(0,1,1), uint3(0,1,0), uint3(0,0,0), uint3(0,0,1) },  // -X
    { uint3(1,1,1), uint3(0,1,1), uint3(0,0,1), uint3(1,0,1) },  // +Y (2ndH)  — old +Z
    { uint3(0,1,0), uint3(1,1,0), uint3(1,0,0), uint3(0,0,0) },  // -Y          — old -Z
    { uint3(0,1,1), uint3(1,1,1), uint3(1,1,0), uint3(0,1,0) },  // +Z (высота) — old +Y
    { uint3(1,0,1), uint3(0,0,1), uint3(0,0,0), uint3(1,0,0) },  // -Z          — old -Y
};

static const float SEAM_INFLATE = 0.0015;

PixelInput VSMain(uint vertexId : SV_VertexID)
{
    uint quadIndex = vertexId / 6;
    uint cornerIndex = CORNER_PATTERN[vertexId % 6];

    uint2 quad = quadBuffer.Load2(quadIndex * 8);
    uint3 start = uint3(quad.x & 127, (quad.x >> 14) & 127, (quad.x >> 7) & 127);
    uint face = (quad.x >> 21) & 7;
    uint3 extent = uint3(quad.y & 127, (quad.y >> 14) & 127, (quad.y >> 7) & 127);

    uint3 corner = FACE_CORNERS[face][cornerIndex];
    // old_Y(height) маппится в новую Z, old_Z(2ndH) — в новую Y
    uint3 remapped = uint3(corner.x, corner.z, corner.y);
    float3 localPosition = (float3)(start + remapped * extent);
    float3 worldPosition = chunkOriginRelative + localPosition;

    float viewDepth = mul(float4(worldPosition, 1.0), viewProjection).w;
    float3 cornerSign = (float3)remapped * 2.0 - 1.0;
    uint originalAxis = face >> 1;
    uint normalAxis = originalAxis;
    float3 inPlane = float3(normalAxis != 0, normalAxis != 1, normalAxis != 2);
    worldPosition += cornerSign * inPlane * (SEAM_INFLATE * viewDepth);

    PixelInput output;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.localPosition = localPosition;
    output.face = face;
    return output;
}

// Нормали: +X, -X, +Y(2ndH), -Y, +Z(высота), -Z.
static const float3 FACE_NORMALS[6] =
{
    float3( 1, 0, 0), float3(-1, 0, 0),
    float3( 0, 1, 0), float3( 0,-1, 0),
    float3( 0, 0, 1), float3( 0, 0,-1),
};

static const float FACE_SHADE[6] = { 0.80, 0.80, 0.90, 0.70, 1.00, 0.55 };

float4 PSMain(PixelInput input) : SV_TARGET
{
    float3 normal = FACE_NORMALS[input.face];
    int3 localBlock = (int3)floor(input.localPosition - normal * 0.5);

    uint3 blockLow = chunkBaseLow + (uint3)localBlock;

    uint hash = blockLow.x * 374761393u
              + blockLow.y * 668265263u
              + blockLow.z * 1274126177u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    uint variation = (hash >> 16) & 31u;

    float3 baseColor = float3(
        101.0 + (variation & 7),
        67.0 + ((variation >> 3) & 7),
        33.0) / 255.0;

    return float4(baseColor * FACE_SHADE[input.face], 1.0);
}
