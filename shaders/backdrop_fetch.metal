// Ours (MIT): framebuffer fetch. Each fragment function reads the colour already in attachment 0 ([[color(0)]]) and
// writes a blend the fixed-function blender cannot express, which is how a compositor draws a material over its
// backdrop in one pass.
//
// - backdrop_tint: a material: the backdrop's luminance pulled toward a tint, then source-over by the layer's alpha.
// - backdrop_blend_modes: multiply, screen or overlay of a premultiplied source with the backdrop, chosen per draw.
#include <metal_stdlib>
using namespace metal;

struct IceVertex {
    float4 position [[position]];
    float2 uv;
};

vertex IceVertex ice_fullscreen_vertex(uint vid [[vertex_id]])
{
    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    IceVertex o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = p;
    return o;
}

struct Material {
    float4 tint;        // premultiplied
    float saturation;   // 0 = grey, 1 = unchanged
    float opacity;
    uint mode;          // 0 multiply, 1 screen, 2 overlay
};

fragment half4 backdrop_tint(IceVertex in [[stage_in]], half4 backdrop [[color(0)]],
                             constant Material &m [[buffer(0)]])
{
    float3 dst = float3(backdrop.rgb);
    float luma = dot(dst, float3(0.2126f, 0.7152f, 0.0722f));
    float3 toned = mix(float3(luma), dst, m.saturation);
    float3 material = m.tint.rgb + toned * (1.0f - m.tint.a);
    float3 rgb = mix(dst, material, m.opacity);
    return half4(half3(rgb), backdrop.a);
}

fragment half4 backdrop_blend_modes(IceVertex in [[stage_in]], half4 backdrop [[color(0)]],
                                    texture2d<half> layer [[texture(0)]], sampler s [[sampler(0)]],
                                    constant Material &m [[buffer(0)]])
{
    half4 src = layer.sample(s, in.uv) * half(m.opacity);
    float3 d = float3(backdrop.rgb);
    float3 c = src.a > 0.0h ? float3(src.rgb) / float(src.a) : float3(0.0f);
    float3 blended;
    if (m.mode == 0u) {
        blended = c * d;
    } else if (m.mode == 1u) {
        blended = c + d - c * d;
    } else {
        float3 low = 2.0f * c * d;
        float3 high = 1.0f - 2.0f * (1.0f - c) * (1.0f - d);
        blended = select(high, low, d < float3(0.5f));
    }
    float a = float(src.a);
    float3 rgb = blended * a + d * (1.0f - a);
    return half4(half3(rgb), half(a + float(backdrop.a) * (1.0f - a)));
}
