// Ours (MIT): a separable Gaussian blur, one pass of nine taps (five fetches, the pairs taken between texels so the
// sampler's linear filter weighs them), and a box downsample, the two halves of a backdrop blur.
//
// - blur_pass: `direction` is one texel along x or along y.
// - blur_downsample: four bilinear fetches around the pixel centre.
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
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

struct BlurPass {
    float2 direction;   // one texel, in uv
    float2 texel;       // 1 / size
};

fragment half4 blur_pass(IceVertex in [[stage_in]], texture2d<half> source [[texture(0)]],
                         sampler linear_clamp [[sampler(0)]], constant BlurPass &pass [[buffer(0)]])
{
    // Weights 70, 56, 28, 8, 1 of 256 (binomial row 8); the pairs (56, 28) and (8, 1) merged into one fetch each.
    const float w0 = 70.0f / 256.0f, w1 = 84.0f / 256.0f, w2 = 9.0f / 256.0f;
    const float o1 = 1.0f + 28.0f / 84.0f, o2 = 3.0f + 1.0f / 9.0f;
    half4 c = source.sample(linear_clamp, in.uv) * half(w0);
    c += source.sample(linear_clamp, in.uv + pass.direction * o1) * half(w1);
    c += source.sample(linear_clamp, in.uv - pass.direction * o1) * half(w1);
    c += source.sample(linear_clamp, in.uv + pass.direction * o2) * half(w2);
    c += source.sample(linear_clamp, in.uv - pass.direction * o2) * half(w2);
    return c;
}

fragment half4 blur_downsample(IceVertex in [[stage_in]], texture2d<half> source [[texture(0)]],
                               sampler linear_clamp [[sampler(0)]], constant BlurPass &pass [[buffer(0)]])
{
    half4 c = source.sample(linear_clamp, in.uv + pass.texel * float2(-1.0f, -1.0f));
    c += source.sample(linear_clamp, in.uv + pass.texel * float2(1.0f, -1.0f));
    c += source.sample(linear_clamp, in.uv + pass.texel * float2(-1.0f, 1.0f));
    c += source.sample(linear_clamp, in.uv + pass.texel * float2(1.0f, 1.0f));
    return c * 0.25h;
}
