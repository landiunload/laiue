#pragma pack_matrix(row_major)

// Слой интерфейса: квады в пиксельных координатах разворачиваются
// вершинным шейдером по SV_VertexID (vertex pulling, как у чанков).
// Скруглённые углы — SDF прямоугольника в пиксельном шейдере,
// текст — альфа из атласа глифов.
//
// Раскладка квада (48 байт, держать в синхроне с RendererUiQuad):
//   0..15  — прямоугольник x0, y0, x1, y1 (px)
//  16..31  — прямоугольник UV u0, v0, u1, v1
//  32..35  — цвет RGBA8 (R в младшем байте)
//  36..39  — радиус скругления, px
//  40..43  — флаги: бит 0 — брать альфу из атласа
//  44..47  — резерв

cbuffer UiConstants : register(b0)
{
    float2 screenSize;
};

ByteAddressBuffer uiQuads : register(t0);
Texture2D<float4> fontAtlas : register(t1);
SamplerState fontSampler : register(s0);

struct UiPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 local : TEXCOORD1;     // смещение от центра квада, px
    float2 halfSize : TEXCOORD2;  // полуразмеры квада, px
    nointerpolation float cornerRadius : TEXCOORD3;
    nointerpolation uint flags : TEXCOORD4;
};

static const uint CORNER_PATTERN[6] = { 0, 1, 2, 0, 2, 3 };

UiPixelInput VSMain(uint vertexId : SV_VertexID)
{
    uint quadIndex = vertexId / 6;
    uint corner = CORNER_PATTERN[vertexId % 6];
    uint base = quadIndex * 48;

    float4 rect = asfloat(uiQuads.Load4(base));
    float4 uvRect = asfloat(uiQuads.Load4(base + 16));
    uint packedColor = uiQuads.Load(base + 32);
    float cornerRadius = asfloat(uiQuads.Load(base + 36));
    uint flags = uiQuads.Load(base + 40);

    // Углы: 0 — (x0,y0), 1 — (x1,y0), 2 — (x1,y1), 3 — (x0,y1).
    float2 cornerWeight = float2(
        corner == 1 || corner == 2 ? 1.0 : 0.0,
        corner >= 2 ? 1.0 : 0.0);

    float2 pixel = lerp(rect.xy, rect.zw, cornerWeight);

    UiPixelInput output;
    output.position = float4(
        pixel.x * 2.0 / screenSize.x - 1.0,
        1.0 - pixel.y * 2.0 / screenSize.y,
        0.0, 1.0);
    output.uv = lerp(uvRect.xy, uvRect.zw, cornerWeight);
    // Цвета интерфейса заданы в sRGB; цель кадра кодирует линейный свет,
    // поэтому декодируем здесь (приближение гаммой 2.2).
    float3 srgb = float3(
        (packedColor & 255) / 255.0,
        ((packedColor >> 8) & 255) / 255.0,
        ((packedColor >> 16) & 255) / 255.0);
    output.color = float4(pow(srgb, 2.2), (packedColor >> 24) / 255.0);
    output.local = (cornerWeight - 0.5) * (rect.zw - rect.xy);
    output.halfSize = (rect.zw - rect.xy) * 0.5;
    output.cornerRadius = cornerRadius;
    output.flags = flags;
    return output;
}

float4 PSMain(UiPixelInput input) : SV_TARGET
{
    float alpha = input.color.a;
    if ((input.flags & 1u) != 0u)
    {
        alpha *= fontAtlas.Sample(fontSampler, input.uv).r;
    }

    if (input.cornerRadius > 0.0)
    {
        // SDF скруглённого прямоугольника, антиалиасинг ~1 px.
        float2 edgeDistance = abs(input.local) - (input.halfSize - input.cornerRadius);
        float outside = length(max(edgeDistance, 0.0));
        float inside = min(max(edgeDistance.x, edgeDistance.y), 0.0);
        float signedDistance = outside + inside - input.cornerRadius;
        alpha *= saturate(0.5 - signedDistance);
    }

    return float4(input.color.rgb, alpha);
}
