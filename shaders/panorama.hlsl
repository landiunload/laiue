#pragma pack_matrix(row_major)

// Резолв широкого угла: полноэкранный треугольник переводит пиксель
// экрана в направление в пространстве вида и читает кубмапу сцены
// (грани куба ориентированы по осям вида, +Z — вперёд).

cbuffer ResolveConstants : register(b0)
{
    float fovHalfRadians;   // половина горизонтального поля зрения
    float verticalScale;    // рыбий глаз: высота/ширина; цилиндр: tan половины вертикали
    uint mapping;           // 0 — равноудалённый рыбий глаз, 1 — цилиндр
};

TextureCube<float4> sceneCube : register(t0);
SamplerState cubeSampler : register(s0);

struct ResolveInput
{
    float4 position : SV_POSITION;
    float2 ndc : TEXCOORD0;   // x, y в [-1, 1], y вверх
};

ResolveInput VSMain(uint vertexId : SV_VertexID)
{
    // Треугольник, накрывающий весь экран: (-1,-1), (3,-1), (-1,3).
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    ResolveInput output;
    output.ndc = corner * 2.0 - 1.0;
    output.position = float4(output.ndc, 0.0, 1.0);
    return output;
}

float4 PSMain(ResolveInput input) : SV_TARGET
{
    float3 direction;
    if (mapping == 0)
    {
        // Равноудалённая проекция: угол от оси взгляда пропорционален
        // расстоянию от центра экрана — честные 360 во все стороны.
        float2 radial = float2(input.ndc.x, input.ndc.y * verticalScale);
        float radius = length(radial);
        float theta = radius * fovHalfRadians;
        float sinTheta;
        float cosTheta;
        sincos(theta, sinTheta, cosTheta);
        float2 azimuth = radius > 1e-5 ? radial / radius : float2(0.0, 0.0);
        direction = float3(azimuth * sinTheta, cosTheta);
    }
    else
    {
        // Цилиндр: горизонталь линейна по углу (до 360), вертикаль
        // перспективная — вертикальные линии мира остаются прямыми.
        float psi = input.ndc.x * fovHalfRadians;
        float sinPsi;
        float cosPsi;
        sincos(psi, sinPsi, cosPsi);
        float tangent = input.ndc.y * verticalScale;
        direction = float3(sinPsi, tangent, cosPsi);
    }
    return float4(sceneCube.Sample(cubeSampler, direction).rgb, 1.0);
}
