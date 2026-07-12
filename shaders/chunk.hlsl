#pragma pack_matrix(row_major)

// Vertex pulling: вершинных буферов нет — квады читаются из
// ByteAddressBuffer по SV_VertexID (6 вершин на квад).
// Раскладка квада (8 байт) описана в src/render/chunk_geometry.h —
// держать в синхроне!
//
// Origin rebasing: камера всегда около нуля; chunkOriginRelative —
// смещение чанка относительно блока камеры, chunkBaseLow — низшие
// 32 бита мировых блочных координат угла чанка (для цвета).
cbuffer FrameConstants : register(b0)
{
    float4x4 viewProjection;     // dword 0..15
    float3 chunkOriginRelative;  // dword 16..18
    uint3 chunkBaseLow;          // dword 20..22
};

ByteAddressBuffer quadBuffer : register(t0);

struct PixelInput
{
    float4 position : SV_POSITION;
    float3 localPosition : LOCALPOSITION;
    nointerpolation uint face : FACEINDEX;
};

// Два треугольника квада: вершины 0,1,2 и 0,2,3.
static const uint CORNER_PATTERN[6] = { 0, 1, 2, 0, 2, 3 };

// Четыре угла каждой грани (обход по часовой стрелке снаружи —
// под back-face culling). Порядок граней: +X, -X, +Y, -Y, +Z, -Z.
static const uint3 FACE_CORNERS[6][4] =
{
    { uint3(1,1,0), uint3(1,1,1), uint3(1,0,1), uint3(1,0,0) },
    { uint3(0,1,1), uint3(0,1,0), uint3(0,0,0), uint3(0,0,1) },
    { uint3(0,1,1), uint3(1,1,1), uint3(1,1,0), uint3(0,1,0) },
    { uint3(1,0,1), uint3(0,0,1), uint3(0,0,0), uint3(1,0,0) },
    { uint3(1,1,1), uint3(0,1,1), uint3(0,0,1), uint3(1,0,1) },
    { uint3(0,1,0), uint3(1,1,0), uint3(1,0,0), uint3(0,0,0) },
};

// Доля глубины, на которую квад раздувается в своей плоскости для смыкания
// швов. Масштаб на глубину даёт постоянный размер на экране (~полпикселя
// при 720p/60 град.); больше — толще силуэт, меньше — могут вернуться щели.
static const float SEAM_INFLATE = 0.0015;

PixelInput VSMain(uint vertexId : SV_VertexID)
{
    uint quadIndex = vertexId / 6;
    uint cornerIndex = CORNER_PATTERN[vertexId % 6];

    uint2 quad = quadBuffer.Load2(quadIndex * 8);
    uint3 start = uint3(quad.x & 127, (quad.x >> 7) & 127, (quad.x >> 14) & 127);
    uint face = (quad.x >> 21) & 7;
    uint3 extent = uint3(quad.y & 127, (quad.y >> 7) & 127, (quad.y >> 14) & 127);

    uint3 corner = FACE_CORNERS[face][cornerIndex];
    float3 localPosition = (float3)(start + corner * extent);
    float3 worldPosition = chunkOriginRelative + localPosition;

    // Смыкание T-стыков greedy-меша без MSAA: раздуваем квад в его плоскости
    // наружу на постоянную (в экране) долю пикселя. Масштаб ~ глубине (clip.w),
    // поэтому на любом расстоянии раздутие одинаково. Соседние квады
    // перекрываются по общим кромкам — субпиксельные щели, сквозь которые
    // просвечивал фон, пропадают; силуэт растёт на доли пикселя, без «пикселей».
    float viewDepth = mul(float4(worldPosition, 1.0), viewProjection).w;
    float3 cornerSign = (float3)corner * 2.0 - 1.0;
    uint normalAxis = face >> 1;
    float3 inPlane = float3(normalAxis != 0, normalAxis != 1, normalAxis != 2);
    worldPosition += cornerSign * inPlane * (SEAM_INFLATE * viewDepth);

    PixelInput output;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.localPosition = localPosition;
    output.face = face;
    return output;
}

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
    int3 localBlock = (int3)floor(input.localPosition - normal * 0.5);

    // Низшие 32 бита мировых координат блока: переполнение uint
    // в точности повторяет низ int64 — цвет стабилен во всём мире.
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
