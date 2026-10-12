// UI overlay pass of the native window (2026-10-02): textured, vertex-
// coloured triangles in window coordinates (Dear ImGui draw lists recorded
// on the UI thread; see src/realsteel_overlay.cpp).
// 2026-10-12: on an HDR swapchain the UI is drawn at paper white, encoded
// like the present pass (HDR10 PQ or scRGB), so it is neither dim nor glaring.
// Build: tools/replayer/build_present_shaders.sh.
struct VSIn {
  [[vk::location(0)]] float2 pos : POSITION;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] float4 color : COLOR0;
};
struct VSOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};
struct Push {
  float4 xform;  // scale.xy, offset.zw: window coordinates -> clip space
  uint mode;     // 0 SDR, 1 HDR10 (PQ), 2 scRGB
  float paper;   // HDR: nits for UI white
  float2 unused;
};
[[vk::push_constant]] Push pc;
VSOut vs_main(VSIn i) {
  VSOut o;
  o.pos = float4(i.pos * pc.xform.xy + pc.xform.zw, 0.0, 1.0);
  o.uv = i.uv;
  o.color = i.color;
  return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s1);

float3 Pq(float3 l) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  const float3 y = pow(max(l, 0.0), m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 ps_main(VSOut i) : SV_Target0 {
  const float4 c = tex.Sample(smp, i.uv) * i.color;
  if (pc.mode == 0) return c;
  const float3 nits = pow(saturate(c.rgb), 2.2) * pc.paper;
  if (pc.mode == 2) return float4(nits / 80.0, c.a);
  const float3x3 to2020 = float3x3(0.6274, 0.3293, 0.0433,
                                   0.0691, 0.9195, 0.0114,
                                   0.0164, 0.0880, 0.8956);
  return float4(Pq(mul(to2020, nits) / 10000.0), c.a);
}
