#pragma pack_matrix(row_major)

/* Backend-neutral mesh path.  The public V2 ABI uploads this fixed record;
 * the backend still uses a ByteAddressBuffer so D3D12 and Vulkan share the
 * same descriptor layout and do not need a second vertex-input ABI. */
cbuffer FrameConstants : register(b0)
{
    float4x4 viewProjection;
    float3 meshOriginRelative;
    float meshScale;
    float4 instancePadding0;
    float3 instancePadding1;
    float instanceMode;
    int vertexIdOffset;
};

ByteAddressBuffer genericVertices : register(t0);
ByteAddressBuffer genericInstances : register(t3);
/* Generic resources are ordinary 2D images.  The voxel path has its own
 * Texture2DArray declarations in chunk.hlsl; sharing that type here made a
 * public 2D texture handle incompatible with both backends. */
Texture2D genericTexture : register(t1);
SamplerState genericSampler : register(s0);

struct GenericPixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

GenericPixelInput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint stride = 24u;
    /* D3D's system vertex ID excludes the draw's vertex base; Vulkan's
     * includes it. Backends normalize the difference with this bias. */
    const uint base = (vertexId + (uint)vertexIdOffset) * stride;
    float3 localPosition = asfloat(genericVertices.Load3(base));
    float2 uv = asfloat(genericVertices.Load2(base + 12u));
    uint packedColor = genericVertices.Load(base + 20u);

    GenericPixelInput output;
    float3 worldPosition = meshOriginRelative + localPosition * meshScale;
    if (instanceMode > 0.0)
    {
        const uint placement = instanceId * 32u;
        float4 originScale = asfloat(genericInstances.Load4(placement));
        float4 rotation = asfloat(genericInstances.Load4(placement + 16u));
        float3 scaledPosition = localPosition * (meshScale * originScale.w);
        float3 twiceCross = 2.0 * cross(rotation.xyz, scaledPosition);
        float3 rotatedPosition = scaledPosition + rotation.w * twiceCross +
                                 cross(rotation.xyz, twiceCross);
        worldPosition = meshOriginRelative + originScale.xyz + rotatedPosition;
    }
    output.position = mul(float4(worldPosition, 1.0), viewProjection);
    output.uv = uv;
    output.color = float4(
        (packedColor & 255u) / 255.0,
        ((packedColor >> 8u) & 255u) / 255.0,
        ((packedColor >> 16u) & 255u) / 255.0,
        ((packedColor >> 24u) & 255u) / 255.0);
    return output;
}

float4 PSMain(GenericPixelInput input) : SV_TARGET
{
    return genericTexture.Sample(genericSampler, input.uv) * input.color;
}
