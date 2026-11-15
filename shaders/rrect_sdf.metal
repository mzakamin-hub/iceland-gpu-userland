// Ours (MIT): a rounded rectangle drawn from a signed distance, the shape of a window corner, a button or a text
// field's end. On the tablet a wrong rounded shape was the first visible sign of a shader path gone wrong (round
// shapes drawn as diamonds), so this is the first case the check compiles.
//
// - rrect_fill: one radius for all four corners, an edge of `edge` pixels.
// - rrect_stroke: the ring between two distances (a focus ring), each corner with its own radius.
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

struct RoundedRect {
    float2 center;      // pixels
    float2 half_size;   // pixels
    float4 radii;       // top-left, top-right, bottom-right, bottom-left
    float4 fill;        // straight alpha
    float edge;         // pixels of antialiasing
    float stroke;       // ring width, pixels
};

static float rrect_distance(float2 p, float2 half_size, float radius)
{
    float2 q = abs(p) - (half_size - radius);
    return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - radius;
}

fragment float4 rrect_fill(IceVertex in [[stage_in]], constant RoundedRect &r [[buffer(0)]])
{
    float d = rrect_distance(in.position.xy - r.center, r.half_size, r.radii.x);
    float coverage = 1.0f - smoothstep(-r.edge, r.edge, d);
    float a = r.fill.a * coverage;
    return float4(r.fill.rgb * a, a);
}

fragment float4 rrect_stroke(IceVertex in [[stage_in]], constant RoundedRect &r [[buffer(0)]])
{
    float2 p = in.position.xy - r.center;
    // The corner's own radius: left or right by x, top or bottom by y.
    float2 side = p.x < 0.0f ? r.radii.xw : r.radii.yz;
    float radius = p.y < 0.0f ? side.x : side.y;
    float d = fabs(rrect_distance(p, r.half_size, radius)) - 0.5f * r.stroke;
    float coverage = saturate(0.5f - d / max(r.edge, 1.0e-3f));
    float a = r.fill.a * coverage;
    return float4(r.fill.rgb * a, a);
}
