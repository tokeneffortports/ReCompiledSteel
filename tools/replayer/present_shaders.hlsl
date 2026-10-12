// Present pass for the native window (2026-10-02): scales the 1280x720
// output to the swapchain and applies the output tone curve (the xenos
// presenter's ~pow(1.28), see RenderOptions::output_gamma).
// 2026-10-03: optional sharpening (RenderOptions::sharpen), a clamped
// unsharp mask over the four neighbours - it cannot ring past the local
// range, so edges get crisper without halos.
// 2026-10-12: optional FXAA (the classic console variant, in source texels)
// and HDR output. The game's picture is SDR; for an HDR swapchain the
// display signal is taken back to linear light, everyday content sits at
// "paper white" and only the brightest part of the range is stretched up
// to the display's peak (inverse tone map on luminance, hue kept), then
// encoded as HDR10 (BT.2020 + PQ) or scRGB (linear BT.709, 1.0 = 80 nits).
// Build: tools/replayer/build_present_shaders.sh (DXC -> present_spv.h).
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
Texture2D src : register(t0);
SamplerState smp : register(s1);
struct Push {
  float gamma;
  float sharpen;   // 0 = off, 1 = strong
  float2 texel;    // 1 / source size
  uint mode;       // 0 SDR, 1 HDR10 (PQ), 2 scRGB
  float paper;     // HDR: nits for SDR white
  float peak;      // HDR: nits for the brightest highlight
  uint fxaa;       // 1 = on
};
[[vk::push_constant]] Push pc;

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 Fxaa(float2 uv) {
  const float2 t = pc.texel;
  const float3 nw = src.SampleLevel(smp, uv + float2(-0.5, -0.5) * t, 0).rgb;
  const float3 ne = src.SampleLevel(smp, uv + float2(0.5, -0.5) * t, 0).rgb;
  const float3 sw = src.SampleLevel(smp, uv + float2(-0.5, 0.5) * t, 0).rgb;
  const float3 se = src.SampleLevel(smp, uv + float2(0.5, 0.5) * t, 0).rgb;
  const float3 m = src.SampleLevel(smp, uv, 0).rgb;
  const float lnw = Luma(nw), lne = Luma(ne), lsw = Luma(sw), lse = Luma(se), lm = Luma(m);
  const float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
  const float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
  float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
  const float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
  const float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp_min, -8.0, 8.0) * t;
  const float3 a = 0.5 * (src.SampleLevel(smp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                          src.SampleLevel(smp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
  const float3 b = a * 0.5 + 0.25 * (src.SampleLevel(smp, uv + dir * -0.5, 0).rgb +
                                     src.SampleLevel(smp, uv + dir * 0.5, 0).rgb);
  const float lb = Luma(b);
  return (lb < lmin || lb > lmax) ? a : b;
}

float3 Pq(float3 l) {  // l: 0..1 = 0..10000 nits
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const float3 y = pow(max(l, 0.0), m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 ps_main(VSOut i) : SV_Target0 {
  float3 c = pc.fxaa != 0 ? Fxaa(i.uv) : src.Sample(smp, i.uv).rgb;
  if (pc.sharpen > 0.0) {
    const float3 n = src.Sample(smp, i.uv + float2(0.0, -pc.texel.y)).rgb;
    const float3 s = src.Sample(smp, i.uv + float2(0.0, pc.texel.y)).rgb;
    const float3 e = src.Sample(smp, i.uv + float2(pc.texel.x, 0.0)).rgb;
    const float3 w = src.Sample(smp, i.uv + float2(-pc.texel.x, 0.0)).rgb;
    const float3 lo = min(c, min(min(n, s), min(e, w)));
    const float3 hi = max(c, max(max(n, s), max(e, w)));
    const float3 blur = (n + s + e + w) * 0.25;
    c = clamp(c + (c - blur) * (pc.sharpen * 2.0), lo, hi);
  }
  const float3 sdr = pow(saturate(c), pc.gamma);
  if (pc.mode == 0) return float4(sdr, 1.0);
  // SDR display signal -> linear light (relative to white).
  float3 lin = pow(sdr, 2.2);
  const float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
  const float ratio = max(pc.peak / pc.paper, 1.0);
  const float knee = 0.7;
  const float t = saturate((y - knee) / (1.0 - knee));
  lin *= 1.0 + (ratio - 1.0) * t * t;  // white reaches the peak, below the knee untouched
  const float3 nits = lin * pc.paper;
  if (pc.mode == 2) return float4(nits / 80.0, 1.0);
  const float3x3 to2020 = float3x3(0.6274, 0.3293, 0.0433,
                                   0.0691, 0.9195, 0.0114,
                                   0.0164, 0.0880, 0.8956);
  return float4(Pq(mul(to2020, nits) / 10000.0), 1.0);
}
