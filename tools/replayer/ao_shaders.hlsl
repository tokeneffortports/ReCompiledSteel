// Ambient occlusion (2026-10-18, Enhancements > Ambient occlusion): darkens
// creases and contact areas of the 3D scene from its depth buffer. Runs
// once a frame on the main surface after the 3D scene, before the game
// copies it for its bloom / depth of field / composite, so the game's own
// effects see the result. ps_main writes the occlusion factor (one channel);
// ps_blur smooths its per-pixel noise without crossing depth edges and is
// blended as a factor the colour is multiplied by.
// Build: tools/replayer/build_present_shaders.sh (DXC -> present_spv.h).
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
Texture2D<float> depth_tex : register(t0);
Texture2D<float> ao_tex : register(t1);  // ps_blur: ps_main's output
struct Push {
  float near_z;     // the game's camera (sub_82104688): near / far planes
  float far_z;
  float tan_y;      // tan(vertical fov / 2)
  float tan_x;      // tan(horizontal fov / 2) for the picture's shape
  float2 size;      // pixels of the frame area
  float radius;     // world units the occlusion looks around (the game's: ~cm)
  float strength;   // 0..1
};
[[vk::push_constant]] Push pc;

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}

float LinearZ(float d) {
  // D3D-style depth (0 near, 1 far)
  return pc.near_z * pc.far_z / (pc.far_z - d * (pc.far_z - pc.near_z));
}

float3 ViewPos(int2 p) {
  p = clamp(p, int2(0, 0), int2(pc.size) - 1);
  const float d = depth_tex.Load(int3(p, 0));
  const float z = LinearZ(d);
  const float2 ndc = (float2(p) + 0.5) / pc.size * 2.0 - 1.0;
  return float3(ndc.x * z * pc.tan_x, -ndc.y * z * pc.tan_y, z);
}

float4 ps_main(VSOut i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float d = depth_tex.Load(int3(p, 0));
  if (d >= 0.99995) return float4(1, 1, 1, 1);  // sky / far: nothing to occlude
  const float3 P = ViewPos(p);
  // Normal from the smaller of the two differences per axis (fewer halos at
  // depth edges).
  const float3 dxp = ViewPos(p + int2(1, 0)) - P, dxm = P - ViewPos(p - int2(1, 0));
  const float3 dyp = ViewPos(p + int2(0, 1)) - P, dym = P - ViewPos(p - int2(0, 1));
  const float3 dx = abs(dxp.z) < abs(dxm.z) ? dxp : dxm;
  const float3 dy = abs(dyp.z) < abs(dym.z) ? dyp : dym;
  const float3 N = normalize(cross(dy, dx));
  // Screen radius of the world-space radius at this depth.
  const float r_px = pc.radius / (P.z * pc.tan_y) * (pc.size.y * 0.5);
  if (r_px < 1.0) return float4(1, 1, 1, 1);
  // 16 samples on a golden-angle spiral, turned per pixel (interleaved
  // gradient noise) so the pattern does not band.
  const float noise = frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715))));
  float occ = 0.0;
  const int kSamples = 16;
  [unroll] for (int s = 0; s < kSamples; ++s) {
    const float t = (float(s) + 0.5) / kSamples;
    const float a = (float(s) * 2.39996323) + noise * 6.2831853;
    const float2 o = float2(cos(a), sin(a)) * (r_px * sqrt(t));
    const float3 v = ViewPos(p + int2(o)) - P;
    const float len2 = dot(v, v);
    const float vn = dot(v, N) * rsqrt(max(len2, 1e-6));
    // Falls off beyond the radius so far-away geometry does not darken.
    const float fall = saturate(1.0 - len2 / (pc.radius * pc.radius));
    occ += saturate(vn - 0.15) * fall;
  }
  const float ao = saturate(1.0 - pc.strength * occ * (3.0 / kSamples));
  return float4(ao, ao, ao, 1.0);
}

float4 ps_blur(VSOut i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float zc = LinearZ(depth_tex.Load(int3(p, 0)));
  float sum = 0.0, wsum = 0.0;
  [unroll] for (int y = -2; y <= 2; ++y) {
    [unroll] for (int x = -2; x <= 2; ++x) {
      const int2 q = clamp(p + int2(x, y), int2(0, 0), int2(pc.size) - 1);
      const float z = LinearZ(depth_tex.Load(int3(q, 0)));
      // Neighbours on another surface (5% depth apart) do not count.
      const float w = saturate(1.0 - abs(z - zc) / (0.05 * zc));
      sum += ao_tex.Load(int3(q, 0)) * w;
      wsum += w;
    }
  }
  const float ao = sum / max(wsum, 1e-4);
  return float4(ao, ao, ao, 1.0);
}
