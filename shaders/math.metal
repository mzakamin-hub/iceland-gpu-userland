// Ours (MIT): the vector lengths and inverse square roots a rounded shape, a gradient and a light depend on, each
// written to its own channel so a wrong one is a wrong colour, not a wrong shape somewhere else.
//
// - math_lengths: length, distance, normalize and rsqrt under the compiler's default fast math.
// - math_lengths_precise: the same through the precise:: forms and with half operands.
// - math_radial: a radial gradient whose stop is the distance to a centre, divided by rsqrt's reciprocal.
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

struct MathInputs {
    float4 a;
    float4 b;
    float2 centre;
    float radius;
    float scale;
};

fragment float4 math_lengths(IceVertex in [[stage_in]], constant MathInputs &m [[buffer(0)]])
{
    float2 p = in.position.xy * m.scale;
    float l2 = length(p - m.centre);
    float l3 = length(m.a.xyz + float3(p, 1.0f));
    float d4 = distance(m.a, m.b + float4(p, p));
    float3 n = normalize(m.b.xyz + float3(p, 0.5f));
    float r = rsqrt(dot(p, p) + 1.0f);
    return float4(l2 * r, l3 * r, d4 * r, n.z);
}

fragment float4 math_lengths_precise(IceVertex in [[stage_in]], constant MathInputs &m [[buffer(0)]])
{
    float2 p = in.position.xy * m.scale;
    float l2 = precise::length(p - m.centre);
    float d3 = precise::distance(m.a.xyz, m.b.xyz + float3(p, 0.0f));
    float3 n = precise::normalize(m.b.xyz + float3(p, 0.5f));
    float r = precise::rsqrt(dot(p, p) + 1.0f);
    half2 hp = half2(p);
    half hl = length(hp);
    half hr = rsqrt(dot(hp, hp) + 1.0h);
    return float4(l2 * r, d3 * r, n.z, float(hl * hr));
}

fragment half4 math_radial(IceVertex in [[stage_in]], constant MathInputs &m [[buffer(0)]])
{
    float2 v = in.position.xy - m.centre;
    float inv = rsqrt(max(dot(v, v), 1.0e-12f));
    float t = saturate((1.0f / inv) / max(m.radius, 1.0e-6f));
    float3 inner = m.a.rgb, outer = m.b.rgb;
    return half4(half3(mix(inner, outer, t)), 1.0h);
}
