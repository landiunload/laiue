#pragma pack_matrix(row_major)

cbuffer FrameConstants : register(b0)
{
    float4x4 viewProjection;
    float3 chunkOriginRelative;
    float meshScale;
    float3 sunDirection;    // единичный, от источника света к миру
    float materialCount;
    float3 sunColor;
    float reserved;
    float3 ambientColor;
    float gammaInverse;     // 1/gamma; упакован в свободный w ambientColor
    // Слой, который каждый материал показывает в этом кадре: по байту на
    // материал, 64 материала в 64 байтах. Кадр выбирает процессор — на
    // GPU это значило бы считать одно и то же время для каждой вершины
    // каждого чанка.
    uint4 materialSlices[4];
};

ByteAddressBuffer quadBuffer : register(t0);
ByteAddressBuffer meshInstances : register(t3);
Texture2DArray blockTextures : register(t1);
Texture2DArray blockNormals : register(t2);   // RGB — нормаль, A — AO
SamplerState blockSampler : register(s0);

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 textureCoordinates : TEXCOORD0;
    nointerpolation uint surface : SURFACE;
    // Поворот инстанса. Касательное пространство грани задано в мировых
    // осях, поэтому у повёрнутого тела его нужно повернуть тоже — иначе
    // кувыркающийся куб освещается так, будто лежит неподвижно.
    nointerpolation float4 rotation : TEXCOORD1;
};

float3 RotateByQuaternion(float4 rotation, float3 value)
{
    float3 axis = rotation.xyz;
    float3 doubled = 2.0 * cross(axis, value);
    return value + rotation.w * doubled + cross(axis, doubled);
}

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

PixelInput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint quadIndex = vertexId / 6;
    uint cornerIndex = CORNER_PATTERN[vertexId % 6];

    uint2 quad = quadBuffer.Load2(quadIndex * 8);
    uint3 start = uint3(quad.x & 127, (quad.x >> 14) & 127, (quad.x >> 7) & 127);
    uint face = (quad.x >> 21) & 7;
    uint blockType = quad.x >> 24;
    uint3 extent = uint3(quad.y & 127, (quad.y >> 14) & 127, (quad.y >> 7) & 127);

    uint3 corner = FACE_CORNERS[face][cornerIndex];
    // old_Y(height) маппится в новую Z, old_Z(2ndH) — в новую Y
    uint3 remapped = uint3(corner.x, corner.z, corner.y);
    float3 localPosition = (float3)(start + remapped * extent);
    float3 origin = chunkOriginRelative;
    float scale = meshScale;
    float4 rotation = float4(0.0, 0.0, 0.0, 0.0);
    [branch]
    if (meshScale < 0.0f)
    {
        float4 placement = asfloat(meshInstances.Load4(instanceId * 32));
        origin = placement.xyz;
        scale = placement.w;
        rotation = asfloat(meshInstances.Load4(instanceId * 32 + 16));
    }
    float3 offset = localPosition * scale;
    // Нулевой кватернион — отсутствие поворота, а не вырожденный поворот.
    if (dot(rotation, rotation) > 0.0001)
    {
        offset = RotateByQuaternion(rotation, offset);
    }
    float3 worldPosition = origin + offset;

    PixelInput output;
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    if (face < 2)
    {
        output.textureCoordinates = float2(localPosition.y, -localPosition.z);
    }
    else if (face < 4)
    {
        output.textureCoordinates = float2(localPosition.x, -localPosition.z);
    }
    else
    {
        output.textureCoordinates = localPosition.xy;
    }

    // Material id 0 — воздух и обычно не попадает в меш. Для
    // защиты некорректного меша он тоже отображается первым материалом.
    uint materials = max((uint)materialCount, 1u);
    uint materialIndex = blockType > 0u ? blockType - 1u : 0u;
    materialIndex = min(materialIndex, materials - 1u);
    uint4 slicePack = materialSlices[materialIndex >> 4];
    uint sliceWord = slicePack[(materialIndex >> 2) & 3u];
    uint textureLayer = (sliceWord >> ((materialIndex & 3u) * 8u)) & 0xFFu;
    output.surface = face | (textureLayer << 3);
    output.rotation = rotation;
    return output;
}

// Небесная окклюзия граней: имитация того, что верх видит больше неба.
static const float FACE_SHADE[6] = { 0.80, 0.80, 0.90, 0.70, 1.00, 0.55 };

// Касательные пространства граней в координатах мира: T — вдоль +U
// текстуры, B — «вверх» текстуры (в сторону уменьшения V), N — наружу.
// Согласовано с выбором UV в VSMain.
static const float3 FACE_TANGENT[6] =
{
    float3(0, 1, 0), float3(0, 1, 0),   // +X, -X: U вдоль Y
    float3(1, 0, 0), float3(1, 0, 0),   // +Y, -Y: U вдоль X
    float3(1, 0, 0), float3(1, 0, 0),   // +Z, -Z: U вдоль X
};
static const float3 FACE_BITANGENT[6] =
{
    float3(0, 0, 1), float3(0, 0, 1),   // V = -Z => вверх текстуры +Z
    float3(0, 0, 1), float3(0, 0, 1),
    float3(0, -1, 0), float3(0, -1, 0), // V = +Y => вверх текстуры -Y
};
static const float3 FACE_NORMAL[6] =
{
    float3(1, 0, 0), float3(-1, 0, 0),
    float3(0, 1, 0), float3(0, -1, 0),
    float3(0, 0, 1), float3(0, 0, -1),
};

float4 PSMain(PixelInput input) : SV_TARGET
{
    uint face = input.surface & 7;
    uint textureLayer = input.surface >> 3;
    float3 textureLocation = float3(input.textureCoordinates, textureLayer);
    float3 baseColor = blockTextures.Sample(blockSampler, textureLocation).rgb;

    float4 normalSample = blockNormals.Sample(blockSampler, textureLocation);
    float3 tangentNormal = normalSample.rgb * 2.0 - 1.0;
    float occlusion = normalSample.a;

    float3 tangent = FACE_TANGENT[face];
    float3 bitangent = FACE_BITANGENT[face];
    float3 faceNormal = FACE_NORMAL[face];
    float shade = FACE_SHADE[face];
    if (dot(input.rotation, input.rotation) > 0.0001)
    {
        tangent = RotateByQuaternion(input.rotation, tangent);
        bitangent = RotateByQuaternion(input.rotation, bitangent);
        faceNormal = RotateByQuaternion(input.rotation, faceNormal);
        // Небесная окклюзия принадлежит направлению, а не грани тела:
        // у повёрнутого куба она берётся по той оси мира, на которую
        // грань теперь смотрит. Неповёрнутая геометрия идёт прежним
        // путём и остаётся пиксель в пиксель той же.
        float3 magnitude = abs(faceNormal);
        uint dominant = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z
            ? 0u : (magnitude.y >= magnitude.z ? 2u : 4u);
        float along = dominant == 0u ? faceNormal.x
            : (dominant == 2u ? faceNormal.y : faceNormal.z);
        shade = FACE_SHADE[along < 0.0 ? dominant + 1u : dominant];
    }

    float3 worldNormal = normalize(
        tangent * tangentNormal.x
        + bitangent * tangentNormal.y
        + faceNormal * tangentNormal.z);

    // Ламберт от солнца/луны + ambient с небесной окклюзией грани.
    // AO гасит ambient целиком, прямой свет — наполовину.
    float diffuse = saturate(dot(worldNormal, -sunDirection));
    float3 light = ambientColor * (shade * occlusion)
        + sunColor * (diffuse * (0.55 + 0.45 * occlusion));
    float3 shaded = pow(saturate(baseColor * light), gammaInverse);
    return float4(shaded, 1.0);
}
