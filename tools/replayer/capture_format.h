// Real Steel capture format (RSCP v1..v4) — shared by the game-side
// serializer (src/realsteel_native_render.cpp) and the offline replayer.
//
// File layout (all header/record fields little-endian host order):
//   u32 magic 'RSCP' (0x52534350), u32 version,
//   u64 frame_index,
//   u32 draw_count, u32 command_count, u32 command_blob_bytes,
//   u32 payload_blob_bytes,
//   draw_count   x DrawRecord (v1: 11 u32, v2: 13, v3: 21, v4: 41),
//   command_count x CommandRecord (4 u32),
//   command_blob  (raw big-endian guest 16-byte cells),
//   payload_blob  (raw guest data: inline vertex payloads + v4 IB/VB
//                  range snapshots, deduped).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rs::capture {

constexpr uint32_t kMagic = 0x52534350u;  // 'RSCP'
// v1: no per-draw shaders; v2: +vs/ps microcode VAs; v3: +textures[8];
// v4: +indexed-draw IB/VB records and range snapshots in payload_blob;
// v5: +resolve records (cmd-53 destinations with fetch constants);
// v6: +front_buffer_phys (translated resolve ids; front-buffer sampling);
// v7: +end-of-frame constant-bank snapshots (cross-frame state carryover);
// v8: +per-draw constant-bank snapshots (ground truth; capture-armed frames);
// v9: +per-draw viewport (device shadow — the game restores viewports
// through paths invisible to the command stream).
// v10: +sampler slots 8-19 in DrawRecord::textures (cube maps live in
// rs_textures/ as 6-face dumps).
// v11: +per-slot fetch format/dimension/endian (DrawRecord::texture_fmt).
// v12: +per-draw render-state shadow (DrawRecord::rs).
// v13: +per-slot texture content hashes (DrawRecord::texture_hash).
constexpr uint32_t kMaxVersion = 15;
constexpr uint32_t kInvalidPayloadOffset = 0xFFFFFFFFu;
constexpr uint32_t kTextureSlots = 20;  // s0-s15 PS, s16-s19 vertex textures
constexpr uint32_t kTextureSlotsV9 = 8;  // recorded by captures v3-v9
constexpr uint32_t kVertexStreamSlots = 4;

enum class DrawKind : uint32_t {
  kIndexed = 0,
  kInline = 1,
};

struct DrawRecord {
  DrawKind kind;
  uint32_t device;
  uint32_t primitive_type;
  uint32_t arg5;            // indexed: baseVtx-ish arg | inline: vertex_count
  uint32_t arg6;            // indexed: startIdx-ish arg | inline: stride
  uint32_t count;           // index count / vertex count
  uint32_t guest_data;      // inline: guest VA of vertex payload
  uint32_t payload_bytes;   // inline: payload size
  uint32_t payload_offset;  // offset into payload_blob, or kInvalidPayloadOffset
  uint32_t d3d_caller;      // LR inside the D3D helper
  uint32_t dispatch_caller; // LR of the title-level submitter
  // v2+: microcode guest VAs of the shaders the GPU runs for this draw
  // (last IM_LOAD per stage). Match the VAs in rs_shaders/ filenames.
  uint32_t vs_object = 0;
  uint32_t ps_object = 0;
  // v3+: base physical addresses of textures bound to sampler slots 0..7
  // (0 = none). Match the ids in rs_textures/ filenames.
  uint32_t textures[kTextureSlots] = {};
  // v11+: per slot, bits 0-5 Xenos texture format (22 k_24_8, 23
  // k_24_8_FLOAT, 36 k_32_FLOAT are depth), bits 8-9 dimension (3 = cube),
  // bits 12-13 endian. 0 = empty slot or pre-v11 capture.
  uint32_t texture_fmt[kTextureSlots] = {};
  static bool IsDepthFormat(uint32_t fmt) {
    const uint32_t f = fmt & 0x3Fu;
    return f == 22 || f == 23 || f == 36;
  }
  // v4+: indexed-draw geometry sources. ib_va is the first *referenced* index
  // byte (already advanced by startIdx); index width from ib_flags bit31
  // (set = 32-bit), endian bits = (ib_flags<<1)&0xC0000000 as in DRAW_INDX
  // dword3. Snapshots live in payload_blob (deduped ranges; guest byte order).
  uint32_t ib_flags = 0;
  uint32_t ib_va = 0;
  uint32_t ib_bytes = 0;
  uint32_t ib_payload_offset = kInvalidPayloadOffset;
  // Per stream: first bound byte (VA + stream offset), bytes from there to
  // the end of the VB, stride in bytes (0 = stream unused), snapshot offset.
  uint32_t vb_va[kVertexStreamSlots] = {};
  uint32_t vb_bytes[kVertexStreamSlots] = {};
  uint32_t vb_stride[kVertexStreamSlots] = {};
  uint32_t vb_payload_offset[kVertexStreamSlots] = {
      kInvalidPayloadOffset, kInvalidPayloadOffset, kInvalidPayloadOffset,
      kInvalidPayloadOffset};
  // v8+: per-draw device constant-bank snapshots in payload_blob (raw guest
  // BE; VS/PS 4096 bytes, bools 32). The authoritative constants for this
  // draw — prefer over stream replay when present.
  uint32_t vs_bank_offset = kInvalidPayloadOffset;
  uint32_t ps_bank_offset = kInvalidPayloadOffset;
  uint32_t bool_bank_offset = kInvalidPayloadOffset;
  // v9+: the draw's actual viewport (0 = unknown, use inference).
  uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  // v12+: device render-state shadow at draw time: rs[0..7] = dev+10540..
  // 10571 (colour mask, RB_DEPTHCONTROL at [2], RB_COLORCONTROL/alpha test at
  // [4], ...), rs[8..11] = dev+12276..12291 (blend control at [9], D3D
  // blend enables at [10]: bit31 ALPHABLENDENABLE, bit30 SEPARATEALPHA).
  uint32_t rs[12] = {};
  // v13+: FNV-1a of each bound texture's live content in this frame (0 =
  // unknown). rs_textures/tex_<phys>_<hash>.xtx is the exact data; textures
  // stream in after their first bind, so a phys can have several dumps.
  uint64_t texture_hash[kTextureSlots] = {};
  // v14+: device shadow dev+10360..10503: [2] RB_SURFACE_INFO (pitch/16 in
  // bits 0-13, msaa_samples bits 16-17), [4] HiZ info, [35] RB_ALPHA_REF
  // as a float (D3DRS_ALPHAREF / 255).
  uint32_t rs2[36] = {};
  // v15: container FNV of the shaders in effect at the draw (0 = unknown;
  // then the microcode address + manifest is the only identity).
  uint64_t vs_container = 0;
  uint64_t ps_container = 0;

  bool Uses32BitIndices() const { return (ib_flags & 0x80000000u) != 0; }
};

struct CommandRecord {
  uint32_t type;           // 0..66 (Yuke's render command type)
  uint32_t guest_address;  // where the command lived in guest memory
  uint32_t byte_count;     // multiple of 16
  uint32_t blob_offset;    // into command_blob
};

// v5+: destination of the Resolve command at commands[command_index].
struct ResolveRecord {
  uint32_t command_index;
  uint32_t tex_object;  // guest texture object VA (0 = none recorded)
  uint32_t phys;        // destination texture id (matches draw textures[])
  uint32_t fetch[6];    // full fetch constant: dims/format of the target
};

struct FrameData {
  uint64_t index = 0;
  std::vector<DrawRecord> draws;
  std::vector<CommandRecord> commands;
  std::vector<ResolveRecord> resolves;
  uint32_t front_buffer_phys = 0;  // v6+; 0 = unknown
  uint32_t version = 0;            // capture format version of the file
  // v7+: end-of-frame device constant shadows, raw guest big-endian
  // (256 vec4 each; bools 8 dwords). Empty for older captures.
  std::vector<uint8_t> vs_const_bank;
  std::vector<uint8_t> ps_const_bank;
  std::vector<uint8_t> bool_bank;
  std::vector<uint8_t> command_blob;  // big-endian guest cells
  std::vector<uint8_t> payload_blob;  // raw guest vertex data
};

// Loads one rs_frame_*.bin. Returns false and fills |error| on failure.
bool LoadFrame(const std::string& path, FrameData& out, std::string& error);
// Same parser over an in-memory buffer (the in-process path serializes the
// live FrameCapture with the game-side writer and hands it here). |label|
// names the source in error messages.
bool LoadFrameFromMemory(const uint8_t* bytes, size_t size,
                         const std::string& label, FrameData& out,
                         std::string& error);

// Big-endian dword read out of a captured blob.
inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Name for a Yuke's render-command type where known (docs/d3d_surface.md),
// else "type_NN".
const char* CommandTypeName(uint32_t type);

}  // namespace rs::capture
