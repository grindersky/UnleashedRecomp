#define IMGUI_MULTISAMPLED
#include "imgui_common.hlsli"

// The game's multisampled depth buffer (its depth in x) or the collision overlay's multisampled target. Both have
// g_PushConstants.SampleCount samples (passed in rather than asked of the texture, which drivers may get wrong).
Texture2DMS<float4> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

// The standard sample positions (Direct3D's; Vulkan's are the same), in 16ths of a pixel from its centre.
static const int2 SAMPLE_POSITIONS_2X[2] = { int2(4, 4), int2(-4, -4) };
static const int2 SAMPLE_POSITIONS_4X[4] = { int2(-2, -6), int2(6, -2), int2(-6, 2), int2(2, 6) };
static const int2 SAMPLE_POSITIONS_8X[8] = { int2(1, -3), int2(-1, 3), int2(5, 1), int2(-3, -5), int2(-5, 5), int2(-7, -1), int2(3, 7), int2(7, -7) };

float2 GetSamplePosition(uint sampleCount, uint index)
{
    if (sampleCount == 2)
        return SAMPLE_POSITIONS_2X[index] / 16.0;
    if (sampleCount == 4)
        return SAMPLE_POSITIONS_4X[index] / 16.0;
    if (sampleCount == 8)
        return SAMPLE_POSITIONS_8X[index] / 16.0;

    return 0.0;
}

float4 main(in Interpolators interpolators, out uint coverage : SV_Coverage) : SV_Target
{
    coverage = 0xFFFFFFFF;

    if (g_PushConstants.ShaderModifier == IMGUI_SHADER_MODIFIER_MULTISAMPLED_RESOLVE)
    {
        // The overlay's target is the backbuffer's size. Its colours are premultiplied by their alpha: blended onto a
        // cleared target.
        Texture2DMS<float4> target = g_Texture2DMSDescriptorHeap[g_PushConstants.Texture2DDescriptorIndex & 0x7FFFFFFF];
        uint sampleCount = g_PushConstants.SampleCount;

        int2 texel = int2(interpolators.Position.xy);
        float4 color = 0.0;
        for (uint i = 0; i < sampleCount; i++)
            color += target.Load(texel, i);

        return color / max(sampleCount, 1);
    }

    // The vertices' depth is in the texture coordinates' x, linear on the screen: at a sample, it's the pixel centre's
    // plus how much it changes over the sample's offset.
    float depth = interpolators.UV.x;
    float2 depthSlope = float2(ddx(depth), ddy(depth));

    if (g_PushConstants.ShaderModifier == IMGUI_SHADER_MODIFIER_MULTISAMPLED_DEPTH_TEST)
    {
        // The depth buffer may be at another resolution than the display; this target has as many samples as it.
        Texture2DMS<float4> depthBuffer = g_Texture2DMSDescriptorHeap[g_PushConstants.Texture2DDescriptorIndex & 0x7FFFFFFF];
        uint width, height, depthSampleCount;
        depthBuffer.GetDimensions(width, height, depthSampleCount);
        uint sampleCount = g_PushConstants.SampleCount;

        int2 texel = int2(interpolators.Position.xy * g_PushConstants.InverseDisplaySize * float2(width, height));

        // Only the samples in front of what the game drew are covered. Its depth is reversed: 1 at the near plane.
        coverage = 0;
        for (uint i = 0; i < sampleCount; i++)
        {
            if (depth + dot(depthSlope, GetSamplePosition(sampleCount, i)) >= depthBuffer.Load(texel, i).x)
                coverage |= 1u << i;
        }

        if (coverage == 0)
            discard;
    }

    return interpolators.Color;
}
