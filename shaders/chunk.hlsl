#pragma pack_matrix(row_major)

// Вершина несёт только упакованную локальную позицию и номер грани;
// цвет и затенение восстанавливаются в пиксельном шейдере — благодаря
// этому greedy meshing может сливать грани разных блоков.
cbuffer FrameConstants : register(b0)
{
    float4x4 viewProjection;
    float3 chunkOrigin;
};

struct VertexInput
{
    uint packedData : DATA;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : WORLDPOSITION;
    nointerpolation uint face : FACEINDEX;
};

PixelInput VSMain(VertexInput input)
{
    uint packedData = input.packedData;
    float3 localPosition = float3(
        packedData & 127,
        (packedData >> 7) & 127,
        (packedData >> 14) & 127);

    float3 worldPosition = chunkOrigin + localPosition;

    PixelInput output;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.worldPosition = worldPosition;
    output.face = (packedData >> 21) & 7;
    return output;
}

// Порядок граней: +X, -X, +Y, -Y, +Z, -Z (совпадает с мешером).
static const float3 FACE_NORMALS[6] =
{
    float3( 1, 0, 0), float3(-1, 0, 0),
    float3( 0, 1, 0), float3( 0,-1, 0),
    float3( 0, 0, 1), float3( 0, 0,-1),
};

// Простое направленное затенение по граням.
static const float FACE_SHADE[6] = { 0.80, 0.80, 1.00, 0.55, 0.90, 0.70 };

float4 PSMain(PixelInput input) : SV_TARGET
{
    // Блок, которому принадлежит грань: полшага внутрь от плоскости грани.
    float3 normal = FACE_NORMALS[input.face];
    int3 blockCoordinate = (int3)floor(input.worldPosition - normal * 0.5);

    // Процедурный цвет земли — тот же хеш, что был на CPU.
    uint hash = (uint)blockCoordinate.x * 374761393u
              + (uint)blockCoordinate.y * 668265263u
              + (uint)blockCoordinate.z * 1274126177u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    uint variation = (hash >> 16) & 31u;

    float3 baseColor = float3(
        101.0 + (variation & 7),
        67.0 + ((variation >> 3) & 7),
        33.0) / 255.0;

    return float4(baseColor * FACE_SHADE[input.face], 1.0);
}
