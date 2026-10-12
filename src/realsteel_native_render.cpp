#include "realsteel_gpu.h"
#include "realsteel_native_render.h"
#include "realsteel_keyboard.h"
#include "realsteel_overlay.h"

#include "realsteel_gamma_dump.h"
#include "realsteel_native_present.h"
#include "capture_format.h"

#include "realsteel_shader_dump.h"
#include "realsteel_texture_dump.h"

#include "generated/default_final/realsteel_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/kernel_state.h>
#include <string>
#include <thread>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#ifndef _WIN32
#include <atomic>
#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>
#endif

#if __has_include(<rex/graphics/native_guest_renderer.h>)
#define REALSTEEL_HAS_NATIVE_GUEST_RENDERER 1
#include <rex/graphics/native_guest_renderer.h>
#else
#define REALSTEEL_HAS_NATIVE_GUEST_RENDERER 0
#endif

REXCVAR_DEFINE_BOOL(realsteel_native_render_capture, true, "Real Steel",
                    "Capture Real Steel draw submissions for the native renderer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(realsteel_native_render_log_interval, 120, "Real Steel",
                     "Guest frames between native-render capture summaries (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(realsteel_capture_frames, 4, "Real Steel",
                     "Frames serialized to rs_captures/ per capture trigger")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(realsteel_capture_now, false, "Real Steel",
                    "Rising edge triggers a capture-to-disk of the next N frames "
                    "(toggle off and on again to re-arm)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace realsteel::native_render {
namespace {

enum class DrawKind : uint8_t {
  kIndexed,
  kInline,
};

// Deliberately stores the original guest arguments without prematurely
// assigning D3D names to the two fields whose exact ABI is still being
// verified. This keeps captures useful even if later symbol recovery changes
// their interpretation.
constexpr uint32_t kInvalidPayloadOffset = 0xFFFFFFFFu;

// v4: vertex streams recorded per indexed draw. The guest API accepts streams
// 0..17 (SetStreamSource, docs/d3d_surface.md); slots above
// kVertexStreamSlots-1 log a warning once so the format can be widened if a
// scene ever uses them.
constexpr uint32_t kVertexStreamSlots = 4;
constexpr uint32_t kVertexStreamScan = 18;

// Device/resource offsets recovered in docs/d3d_surface.md (2026-08-29):
//   dev+12812              current index-buffer object (SetIndices 0x82253380)
//   dev+12836+4s           current VB object per stream
//   dev+12904+s            u8 per-stream vertex stride in dwords
//   dev+1916-8s            vfetch const 95-s dword1: stream byte size
//   IB obj: +0 flags (bit31 = 32-bit indices), +24 data VA, +28 byte size.
//   VB obj: +24/+28 are a pre-encoded vertex fetch constant —
//     +24 = data VA | type(3), +28 = endian(2 bits) | size_in_dwords<<2.
constexpr uint32_t kDevIndexBuffer = 12812;
constexpr uint32_t kDevStreamObjectBase = 12836;
constexpr uint32_t kDevStreamStrideBase = 12904;
constexpr uint32_t kDevStreamSizeBase = 1916;  // minus 8*s
constexpr uint32_t kResFlags = 0;
constexpr uint32_t kResDataVa = 24;
constexpr uint32_t kResByteSize = 28;

struct DrawRecord {
  DrawKind kind;
  uint32_t device;
  uint32_t primitive_type;
  uint32_t arg5;
  uint32_t arg6;
  uint32_t count;
  uint32_t guest_data;
  uint32_t payload_bytes;
  uint32_t d3d_caller;
  uint32_t dispatch_caller;
  // Offset into FrameCapture::payload_blob of the inline vertex payload copied
  // at the frame boundary, or kInvalidPayloadOffset when not snapshotted.
  uint32_t payload_offset = kInvalidPayloadOffset;
  // Microcode guest VAs of the shaders the GPU is running for this draw
  // (from the last IM_LOAD seen by the shader flush hook). These match the
  // VAs embedded in rs_shaders/ filenames. Device fields 13068/13072 turned
  // out to be a staging slot the flush consumes, so they read 0 at draw time.
  uint32_t vs_object = 0;
  uint32_t ps_object = 0;
  // v3: base physical addresses of textures bound to sampler slots at draw
  // time (0 = none); 8 slots through v9, 20 (s0-s19) from v10. Match the ids
  // in rs_textures/ filenames.
  uint32_t textures[realsteel::texture_dump::kSlotCount] = {};
  // v11: per-slot fetch format/dimension/endian (see CaptureBoundTextures).
  uint32_t texture_fmt[realsteel::texture_dump::kSlotCount] = {};
  // v13: content hash of each bound texture's live data during armed frames
  // (0 = not hashed). Names the exact rs_textures/tex_<phys>_<hash>.xtx.
  uint64_t texture_hash[realsteel::texture_dump::kSlotCount] = {};
  // v4: indexed-draw geometry sources, read from the device at draw time and
  // snapshotted into payload_blob at the frame boundary (deduped by range).
  // ib_va is the first *referenced* index byte (data VA + startIdx*isize);
  // isize comes from ib_flags bit31 (1 = 32-bit indices). All zero/invalid for
  // inline draws or when no IB is bound.
  uint32_t ib_flags = 0;
  uint32_t ib_va = 0;
  uint32_t ib_bytes = 0;
  uint32_t ib_payload_offset = kInvalidPayloadOffset;
  // Per stream: first byte of the bound range (data VA + stream offset), byte
  // size remaining from that offset, stride in bytes, snapshot offset.
  uint32_t vb_va[kVertexStreamSlots] = {};
  uint32_t vb_bytes[kVertexStreamSlots] = {};
  uint32_t vb_stride[kVertexStreamSlots] = {};
  uint32_t vb_payload_offset[kVertexStreamSlots] = {
      kInvalidPayloadOffset, kInvalidPayloadOffset, kInvalidPayloadOffset,
      kInvalidPayloadOffset};
  // v8: per-draw snapshots of the device's full constant banks (payload-blob
  // offsets; raw guest BE bytes; content-deduped). Captured only while a
  // disk-capture burst is armed. This is ground truth — the engine writes
  // constants through paths no hook can cover (direct bank writes).
  uint32_t vs_bank_offset = kInvalidPayloadOffset;  // 4096 bytes
  uint32_t ps_bank_offset = kInvalidPayloadOffset;  // 4096 bytes
  uint32_t bool_bank_offset = kInvalidPayloadOffset;  // 32 bytes
  // v9: the device's viewport shadow (dev+13024, floats) at draw time —
  // ground truth; the game restores viewports via paths outside the stream.
  uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  // v12: device render-state shadow at draw time — dev+10540..10571 (colour
  // mask, ?, RB_DEPTHCONTROL, ?, RB_COLORCONTROL/alpha test, alpha ref?, ..)
  // and dev+12276..12291 (blend control at +12280, D3D blend enables at
  // +12284). Materials set these through the effect system, invisible to
  // the command stream (docs/d3d_surface.md shadow homes).
  uint32_t rs[12] = {};
  // v14: a second window, dev+10360..10503 — RB_SURFACE_INFO shadow at
  // dev+10368 ([2]: pitch/16 bits 0-13, msaa_samples bits 16-17), HiZ info
  // at +10376 ([4]), RB_ALPHA_REF as a float at dev+10500 ([35], written
  // by sub_82251310 = D3DRS_ALPHAREF / 255).
  uint32_t rs2[36] = {};
  // v15 (2026-10-02): content identity (container FNV) of the shaders in
  // effect at the draw. The microcode address is not an identity: D3D
  // re-patches a vertex shader in place for each vertex declaration it is
  // drawn with, so within one frame the same address holds different
  // programs (Zeus' shell: stride-32 parts drawn with the stride-44 patch).
  uint64_t vs_container = 0;
  uint64_t ps_container = 0;
};
static_assert(kVertexStreamSlots == 4,
              "vb_payload_offset initializer and the v4 disk format assume 4");

struct RenderCommandRecord {
  uint32_t type;
  uint32_t guest_address;
  uint32_t byte_count;
  uint32_t blob_offset;
};

// v5: Resolve (cmd 53) destinations. The command's +24 is a guest texture
// object whose embedded fetch constant (+28, classic D3DBaseTexture) names
// the destination memory; the offline replayer implements Resolve as a
// native copy into that texture identity.
struct ResolveRecord {
  uint32_t command_index;  // into FrameCapture::commands
  uint32_t tex_object;     // guest object VA (0 = none recorded)
  uint32_t phys;           // fetch-constant base address (texture id)
  uint32_t fetch[6];       // full fetch constant (dims/format for the copy)
};

struct FrameCapture {
  uint64_t index = 0;
  std::vector<DrawRecord> draws;
  std::vector<RenderCommandRecord> commands;
  std::vector<ResolveRecord> resolves;
  // v6: front buffer address (dev+11024 GPU-shared block, +8). Draws that
  // sample this id read the previous presented frame (Swap-time resolve);
  // the replayer emulates it with a snapshot of the main attachment.
  uint32_t front_buffer_phys = 0;
  // v7: the device's full constant shadows at frame end (raw guest
  // big-endian). The stream only carries constants that CHANGED this frame;
  // everything else — lighting, materials, UI fade levels — persists across
  // frames and must seed the replayer's banks.
  uint8_t vs_const_bank[256 * 16] = {};
  uint8_t ps_const_bank[256 * 16] = {};
  uint8_t bool_bank[32] = {};
  std::vector<uint8_t> command_blob;
  std::vector<uint8_t> payload_blob;
  uint64_t indexed_count = 0;
  uint64_t inline_count = 0;
  uint64_t indices = 0;
  uint64_t inline_vertices = 0;
  uint64_t inline_bytes = 0;
  uint32_t max_inline_bytes = 0;
};

std::mutex g_capture_mutex;
FrameCapture g_building_frame;
FrameCapture g_completed_frame;
std::atomic<uint64_t> g_completed_sequence{0};
std::atomic<bool> g_installed{false};

using Clock = std::chrono::steady_clock;
Clock::time_point g_log_window_start;
uint64_t g_log_window_frames = 0;
double g_log_window_frame_ms = 0.0;
double g_log_window_max_ms = 0.0;
Clock::time_point g_previous_frame;

constexpr uint32_t kRenderCommandTypeCount = 67;
constexpr uint32_t kRenderCommandCellBytes = 16;
constexpr uint32_t kMaxRenderCommandBytes = 256 * 1024;

// Inline-payload snapshot limits. Payloads are copied out of guest memory at
// the frame boundary (Swap), before the inline-data heap segments can recycle.
constexpr uint32_t kMaxInlinePayloadBytes = 4 * 1024 * 1024;
// v4 also snapshots index/vertex-buffer ranges into the same blob; a full 3D
// scene's unique VBs can be far larger than the 2D inline traffic.
constexpr uint32_t kMaxBufferSnapshotBytes = 8 * 1024 * 1024;
constexpr size_t kMaxPayloadBlobBytes = 64 * 1024 * 1024;

// Per-frame dedupe of buffer-range snapshots: (va<<32 | bytes) -> blob offset.
// Guarded by g_capture_mutex; cleared each frame with capacity retained.
std::unordered_map<uint64_t, uint32_t> g_frame_range_offsets;
// Per-frame dedupe of constant-bank snapshots: content hash -> blob offset.
std::unordered_map<uint64_t, uint32_t> g_frame_bank_offsets;

// 2026-10-03: XXH3 instead of a byte-at-a-time FNV-1a. The three banks are
// 8 KB per draw; at ~900 draws a frame the FNV loop alone was 5-8 ms of the
// guest thread on the Steam Deck ("banks" in the draw-hook timing).
uint64_t BankFnv1a64(const uint8_t* data, size_t size) {
  return XXH3_64bits(data, size);
}

// Snapshots |bytes| of guest memory into the building frame's payload blob,
// deduped by content. Caller must hold g_capture_mutex.
uint32_t SnapshotBank(uint8_t* base, uint32_t va, uint32_t bytes) {
  const uint8_t* src = base + va;
  const uint64_t hash = BankFnv1a64(src, bytes) ^ bytes;
  const auto it = g_frame_bank_offsets.find(hash);
  if (it != g_frame_bank_offsets.end()) {
    return it->second;
  }
  const size_t old_size = g_building_frame.payload_blob.size();
  if (old_size + bytes > kMaxPayloadBlobBytes) {
    return kInvalidPayloadOffset;
  }
  g_building_frame.payload_blob.resize(old_size + bytes);
  std::memcpy(g_building_frame.payload_blob.data() + old_size, src, bytes);
  const uint32_t offset = static_cast<uint32_t>(old_size);
  g_frame_bank_offsets.emplace(hash, offset);
  return offset;
}

// Host-side SetStreamSource shadow (v4.1). The first v4 capture read the
// device-struct stream shadow (dev+12836/dev+1916-8s) at draw time and found
// it empty despite cmd-14 binds in the same frame, so the bind call itself is
// hooked instead: stream binding and draw happen on the same guest thread
// (the Yuke's dispatcher drains its queue sequentially), so a thread_local
// shadow needs no locking. The device-memory read remains as a fallback in
// OnDrawIndexed until the discrepancy is understood (see the one-shot
// diagnostic there).
struct StreamBinding {
  uint32_t vb = 0;      // guest VB object ptr (+24/+28 = encoded vfetch const)
  uint32_t offset = 0;  // byte offset into the VB
  uint32_t stride = 0;  // vertex stride in bytes
  // Guest frame the binding was made in (g_completed_sequence at bind time).
  // Draw capture only trusts bindings at most one frame old: current bindings
  // are kept alive by the runtime's deferred-release lists, while older ones
  // can point at freed objects and must not be dereferenced.
  uint64_t frame = 0;
};
thread_local StreamBinding g_tls_streams[kVertexStreamScan] = {};

// Capture-to-disk trigger state (guarded by g_capture_mutex).
bool g_capture_trigger_seen = false;
bool g_capture_key_seen = false;
int32_t g_capture_frames_remaining = 0;
// Per-draw bank snapshots, IB/VB range copies and live texture hashing are
// needed both for a disk-capture burst and for the in-process native
// renderer (which consumes the same FrameCapture every frame).
// Decided once per frame (at the previous frame's end): the native path only
// instruments frames its worker will render, so a slow renderer costs the
// guest thread nothing on the frames it skips.
bool g_native_capture_this_frame = false;
bool FullCaptureActive() {
  return g_capture_frames_remaining > 0 || g_native_capture_this_frame;
}
bool g_capture_write_failed = false;

// rs_captures/rs_frame_NNNNNN.bin, format version 5:
//   u32 magic 'RSCP' (0x52534350), u32 version,
//   u64 frame_index,
//   u32 draw_count, u32 command_count, u32 command_blob_bytes,
//   u32 payload_blob_bytes,
//   v5+: u32 resolve_count, v6+: u32 front_buffer_phys,
//   draw_count   x 41 u32: kind, device, primitive_type, arg5, arg6, count,
//                          guest_data, payload_bytes, payload_offset,
//                          d3d_caller, dispatch_caller, vs_object, ps_object,
//                          textures[8] (v10+: textures[20]),
//                          v11+: texture_fmt[20],
//                          ib_flags, ib_va, ib_bytes, ib_payload_offset,
//                          4 x {vb_va, vb_bytes, vb_stride, vb_payload_offset}
//                          (v1 ended at dispatch_caller; v2 at ps_object;
//                           v3 at textures[8]; v10 widens to 20 slots)
//   command_count x 4 u32: type, guest_address, byte_count, blob_offset
//   v5+: resolve_count x 9 u32: command_index, tex_object, phys, fetch[6]
//   command_blob bytes (raw big-endian guest cells),
//   payload_blob bytes (raw guest data: inline vertex payloads plus v4
//                       index/vertex-buffer range snapshots, deduped).
// All header/record fields are host-endian (little-endian x86).
void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
  const size_t n = out.size();
  out.resize(n + sizeof(v));
  std::memcpy(out.data() + n, &v, sizeof(v));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
  const size_t n = out.size();
  out.resize(n + sizeof(v));
  std::memcpy(out.data() + n, &v, sizeof(v));
}

void SerializeFrame(const FrameCapture& frame, std::vector<uint8_t>& buf) {
  buf.clear();
  buf.reserve(64 + frame.draws.size() * 164 + frame.commands.size() * 16 +
              frame.command_blob.size() + frame.payload_blob.size());
  AppendU32(buf, 0x52534350u);  // 'RSCP'
  AppendU32(buf, 15u);  // v15: +vs/ps container hashes; v14: +rs2[36], type-204 gamma record; v13: +texture_hash[20]; v12: +rs[12]
  AppendU64(buf, frame.index);
  AppendU32(buf, static_cast<uint32_t>(frame.draws.size()));
  AppendU32(buf, static_cast<uint32_t>(frame.commands.size()));
  AppendU32(buf, static_cast<uint32_t>(frame.command_blob.size()));
  AppendU32(buf, static_cast<uint32_t>(frame.payload_blob.size()));
  AppendU32(buf, static_cast<uint32_t>(frame.resolves.size()));
  AppendU32(buf, frame.front_buffer_phys);  // v6
  // v7: constant-bank snapshots (raw guest big-endian bytes).
  {
    const size_t n = buf.size();
    buf.resize(n + sizeof(frame.vs_const_bank) + sizeof(frame.ps_const_bank) +
               sizeof(frame.bool_bank));
    uint8_t* p = buf.data() + n;
    std::memcpy(p, frame.vs_const_bank, sizeof(frame.vs_const_bank));
    p += sizeof(frame.vs_const_bank);
    std::memcpy(p, frame.ps_const_bank, sizeof(frame.ps_const_bank));
    p += sizeof(frame.ps_const_bank);
    std::memcpy(p, frame.bool_bank, sizeof(frame.bool_bank));
  }
  for (const DrawRecord& draw : frame.draws) {
    AppendU32(buf, static_cast<uint32_t>(draw.kind));
    AppendU32(buf, draw.device);
    AppendU32(buf, draw.primitive_type);
    AppendU32(buf, draw.arg5);
    AppendU32(buf, draw.arg6);
    AppendU32(buf, draw.count);
    AppendU32(buf, draw.guest_data);
    AppendU32(buf, draw.payload_bytes);
    AppendU32(buf, draw.payload_offset);
    AppendU32(buf, draw.d3d_caller);
    AppendU32(buf, draw.dispatch_caller);
    AppendU32(buf, draw.vs_object);
    AppendU32(buf, draw.ps_object);
    for (uint32_t t : draw.textures) {
      AppendU32(buf, t);
    }
    for (uint32_t t : draw.texture_fmt) {  // v11
      AppendU32(buf, t);
    }
    AppendU32(buf, draw.ib_flags);
    AppendU32(buf, draw.ib_va);
    AppendU32(buf, draw.ib_bytes);
    AppendU32(buf, draw.ib_payload_offset);
    for (uint32_t s = 0; s < kVertexStreamSlots; ++s) {
      AppendU32(buf, draw.vb_va[s]);
      AppendU32(buf, draw.vb_bytes[s]);
      AppendU32(buf, draw.vb_stride[s]);
      AppendU32(buf, draw.vb_payload_offset[s]);
    }
    AppendU32(buf, draw.vs_bank_offset);   // v8
    AppendU32(buf, draw.ps_bank_offset);
    AppendU32(buf, draw.bool_bank_offset);
    AppendU32(buf, draw.vp_x);             // v9
    AppendU32(buf, draw.vp_y);
    AppendU32(buf, draw.vp_w);
    AppendU32(buf, draw.vp_h);
    for (uint32_t r : draw.rs) {  // v12
      AppendU32(buf, r);
    }
    for (uint64_t h : draw.texture_hash) {  // v13
      AppendU32(buf, static_cast<uint32_t>(h));
      AppendU32(buf, static_cast<uint32_t>(h >> 32));
    }
    for (uint32_t r : draw.rs2) {  // v14
      AppendU32(buf, r);
    }
    AppendU32(buf, static_cast<uint32_t>(draw.vs_container));  // v15
    AppendU32(buf, static_cast<uint32_t>(draw.vs_container >> 32));
    AppendU32(buf, static_cast<uint32_t>(draw.ps_container));
    AppendU32(buf, static_cast<uint32_t>(draw.ps_container >> 32));
  }
  for (const RenderCommandRecord& command : frame.commands) {
    AppendU32(buf, command.type);
    AppendU32(buf, command.guest_address);
    AppendU32(buf, command.byte_count);
    AppendU32(buf, command.blob_offset);
  }
  for (const ResolveRecord& resolve : frame.resolves) {
    AppendU32(buf, resolve.command_index);
    AppendU32(buf, resolve.tex_object);
    AppendU32(buf, resolve.phys);
    for (uint32_t f : resolve.fetch) {
      AppendU32(buf, f);
    }
  }
  buf.insert(buf.end(), frame.command_blob.begin(), frame.command_blob.end());
  buf.insert(buf.end(), frame.payload_blob.begin(), frame.payload_blob.end());
}

// The completed frame as the renderer's FrameData, without the byte form
// (2026-10-12: serialize + parse were ~2.6 + 1.4 ms a frame on the Nova in
// the WRB arena). Fields are copied per draw (a few hundred bytes); the two
// blobs are swapped, so |frame| keeps |out|'s earlier storage - the capture
// clears them when that storage is reused for a frame being built.
void ToFrameData(FrameCapture& frame, rs::capture::FrameData& out) {
  out.index = frame.index;
  out.version = 15;
  out.front_buffer_phys = frame.front_buffer_phys;
  out.draws.resize(frame.draws.size());
  for (size_t i = 0; i < frame.draws.size(); ++i) {
    const DrawRecord& d = frame.draws[i];
    rs::capture::DrawRecord& o = out.draws[i];
    o.kind = static_cast<rs::capture::DrawKind>(d.kind);
    o.device = d.device;
    o.primitive_type = d.primitive_type;
    o.arg5 = d.arg5;
    o.arg6 = d.arg6;
    o.count = d.count;
    o.guest_data = d.guest_data;
    o.payload_bytes = d.payload_bytes;
    o.payload_offset = d.payload_offset;
    o.d3d_caller = d.d3d_caller;
    o.dispatch_caller = d.dispatch_caller;
    o.vs_object = d.vs_object;
    o.ps_object = d.ps_object;
    static_assert(rs::capture::kTextureSlots == realsteel::texture_dump::kSlotCount);
    std::memcpy(o.textures, d.textures, sizeof(o.textures));
    std::memcpy(o.texture_fmt, d.texture_fmt, sizeof(o.texture_fmt));
    std::memcpy(o.texture_hash, d.texture_hash, sizeof(o.texture_hash));
    o.ib_flags = d.ib_flags;
    o.ib_va = d.ib_va;
    o.ib_bytes = d.ib_bytes;
    o.ib_payload_offset = d.ib_payload_offset;
    std::memcpy(o.vb_va, d.vb_va, sizeof(o.vb_va));
    std::memcpy(o.vb_bytes, d.vb_bytes, sizeof(o.vb_bytes));
    std::memcpy(o.vb_stride, d.vb_stride, sizeof(o.vb_stride));
    std::memcpy(o.vb_payload_offset, d.vb_payload_offset, sizeof(o.vb_payload_offset));
    o.vs_bank_offset = d.vs_bank_offset;
    o.ps_bank_offset = d.ps_bank_offset;
    o.bool_bank_offset = d.bool_bank_offset;
    o.vp_x = d.vp_x;
    o.vp_y = d.vp_y;
    o.vp_w = d.vp_w;
    o.vp_h = d.vp_h;
    std::memcpy(o.rs, d.rs, sizeof(o.rs));
    std::memcpy(o.rs2, d.rs2, sizeof(o.rs2));
    o.vs_container = d.vs_container;
    o.ps_container = d.ps_container;
  }
  out.commands.resize(frame.commands.size());
  for (size_t i = 0; i < frame.commands.size(); ++i) {
    out.commands[i] = {frame.commands[i].type, frame.commands[i].guest_address,
                       frame.commands[i].byte_count, frame.commands[i].blob_offset};
  }
  out.resolves.resize(frame.resolves.size());
  for (size_t i = 0; i < frame.resolves.size(); ++i) {
    rs::capture::ResolveRecord& r = out.resolves[i];
    r.command_index = frame.resolves[i].command_index;
    r.tex_object = frame.resolves[i].tex_object;
    r.phys = frame.resolves[i].phys;
    std::memcpy(r.fetch, frame.resolves[i].fetch, sizeof(r.fetch));
  }
  out.vs_const_bank.assign(frame.vs_const_bank, frame.vs_const_bank + sizeof(frame.vs_const_bank));
  out.ps_const_bank.assign(frame.ps_const_bank, frame.ps_const_bank + sizeof(frame.ps_const_bank));
  out.bool_bank.assign(frame.bool_bank, frame.bool_bank + sizeof(frame.bool_bank));
  out.command_blob.swap(frame.command_blob);
  out.payload_blob.swap(frame.payload_blob);
}

bool WriteFrameCaptureToDisk(const FrameCapture& frame) {
  std::error_code ec;
  std::filesystem::create_directories("rs_captures", ec);

  char name[80];
  std::snprintf(name, sizeof(name), "rs_captures/rs_frame_%06llu.bin",
                static_cast<unsigned long long>(frame.index));

  std::vector<uint8_t> buf;
  SerializeFrame(frame, buf);

  std::ofstream file(name, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
  return file.good();
}

// The five direct callers below are the game's small D3D submission helpers.
// Their own LR identifies the title-level mesh/effect routine above them,
// which is much more useful than repeatedly reporting a fixed D3D call site.
thread_local uint32_t g_dispatch_caller = 0;

bool CaptureEnabled() { return REXCVAR_GET(realsteel_native_render_capture); }

// Reads the device's viewport shadow (floats at dev+13024) into rounded
// integer x/y/w/h.
void ReadViewportShadow(uint8_t* base, uint32_t device, uint32_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t bits = REX_LOAD_U32(device + 13024 + i * 4);
    float f;
    std::memcpy(&f, &bits, 4);
    out[i] = (f >= 0.0f && f <= 8192.0f) ? uint32_t(f + 0.5f) : 0u;
  }
}


uint32_t SaturatingPayloadSize(uint32_t count, uint32_t stride) {
  const uint64_t bytes = uint64_t(count) * uint64_t(stride);
  return bytes > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(bytes);
}

#if REALSTEEL_HAS_NATIVE_GUEST_RENDERER
// The upgraded ReXGlue callback is registered from the first migration build.
// Returning false explicitly yields to Xenos until the replay side can serve a
// complete frame. This lets capture and native replay be developed without a
// black-screen intermediate build or accidental draw suppression.
bool NativeGuestOutputProbe(
    const rex::graphics::NativeGuestOutputRenderContext& /*context*/,
    void* /*user_data*/) {
  return false;
}
#endif

}  // namespace

class DispatcherCallerScope {
 public:
  explicit DispatcherCallerScope(uint32_t caller)
      : previous_(g_dispatch_caller) {
    g_dispatch_caller = caller;
  }

  ~DispatcherCallerScope() { g_dispatch_caller = previous_; }

  DispatcherCallerScope(const DispatcherCallerScope&) = delete;
  DispatcherCallerScope& operator=(const DispatcherCallerScope&) = delete;

 private:
  uint32_t previous_;
};

void Install() {
  if (g_installed.exchange(true)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_building_frame.draws.reserve(2048);
    g_completed_frame.draws.reserve(2048);
    g_building_frame.commands.reserve(4096);
    g_completed_frame.commands.reserve(4096);
    g_building_frame.command_blob.reserve(2 * 1024 * 1024);
    g_completed_frame.command_blob.reserve(2 * 1024 * 1024);
    g_building_frame.payload_blob.reserve(2 * 1024 * 1024);
    g_completed_frame.payload_blob.reserve(2 * 1024 * 1024);
  }

#if REALSTEEL_HAS_NATIVE_GUEST_RENDERER
  rex::graphics::SetNativeGuestOutputRenderer(&NativeGuestOutputProbe, nullptr);
  REXLOG_INFO(
      "[native-render] capture installed; native guest-output handoff available "
      "(yielding to Xenos until replay is complete)");
#else
  REXLOG_INFO(
      "[native-render] capture installed; current SDK has no native guest-output "
      "handoff (capture-only compatibility mode)");
#endif
}

void OnRenderCommand(uint8_t* base, uint32_t command) {
  if (!CaptureEnabled() || command == 0) {
    return;
  }

  const uint32_t type = REX_LOAD_U32(command + 0);
  const uint32_t following_cells = REX_LOAD_U32(command + 8);
  const uint64_t byte_count_64 =
      (uint64_t(following_cells) + 1) * kRenderCommandCellBytes;
  if (type >= kRenderCommandTypeCount || byte_count_64 < kRenderCommandCellBytes ||
      byte_count_64 > kMaxRenderCommandBytes) {
    return;
  }

  const uint32_t byte_count = static_cast<uint32_t>(byte_count_64);
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  const size_t old_size = g_building_frame.command_blob.size();
  if (old_size > std::numeric_limits<uint32_t>::max() - byte_count) {
    return;
  }

  g_building_frame.commands.push_back(
      {type, command, byte_count, static_cast<uint32_t>(old_size)});
  g_building_frame.command_blob.resize(old_size + byte_count);
  std::memcpy(g_building_frame.command_blob.data() + old_size, base + command,
              byte_count);

  // Synthetic type 203 (2026-09-05): the colour/depth surface objects a
  // SetRenderTargetOrDS names (+16 / +20) — 16 dwords each, so the replayer
  // can read the EDRAM surface format (gamma vs linear colour) that the
  // command stream never states. Stored as [colour_ptr, depth_ptr, colour
  // object[16], depth object[16]] big-endian like real payloads.
  if (type == 39 && byte_count >= 32) {
    const uint32_t color_obj = REX_LOAD_U32(command + 16);
    const uint32_t depth_obj = REX_LOAD_U32(command + 20);
    // The +16/+20 pointers are the title's RT descriptors (dword 0 = the
    // D3D surface object, then a name for some); record 16 dwords of each
    // descriptor and 16 dwords of the D3D object it points at.
    uint32_t words[66] = {color_obj, depth_obj};
    auto valid = [](uint32_t va) { return va >= 0x80000000u && va < 0xC0000000u; };
    const uint32_t color_d3d = valid(color_obj) ? REX_LOAD_U32(color_obj) : 0u;
    const uint32_t depth_d3d = valid(depth_obj) ? REX_LOAD_U32(depth_obj) : 0u;
    for (uint32_t i = 0; i < 16; ++i) {
      words[2 + i] = valid(color_obj) ? REX_LOAD_U32(color_obj + i * 4) : 0u;
      words[18 + i] = valid(depth_obj) ? REX_LOAD_U32(depth_obj + i * 4) : 0u;
      words[34 + i] = valid(color_d3d) ? REX_LOAD_U32(color_d3d + i * 4) : 0u;
      words[50 + i] = valid(depth_d3d) ? REX_LOAD_U32(depth_d3d + i * 4) : 0u;
    }
    const size_t at = g_building_frame.command_blob.size();
    g_building_frame.commands.push_back(
        {203u, command, uint32_t(sizeof(words)), static_cast<uint32_t>(at)});
    g_building_frame.command_blob.resize(at + sizeof(words));
    uint8_t* q = g_building_frame.command_blob.data() + at;
    for (uint32_t i = 0; i < 66; ++i) {
      q[i * 4 + 0] = uint8_t(words[i] >> 24);
      q[i * 4 + 1] = uint8_t(words[i] >> 16);
      q[i * 4 + 2] = uint8_t(words[i] >> 8);
      q[i * 4 + 3] = uint8_t(words[i]);
    }
  }

  // v5: Resolve destinations were read here from the type-53 command
  // (texture object at +24). Since 2026-09-15 they are recorded by the D3D
  // Resolve hook (OnD3DResolve) instead, which also catches the resolves
  // issued from inside other commands.
}

void OnD3DResolve(uint8_t* base, uint32_t dest_texture) {
  if (!CaptureEnabled() || dest_texture == 0) return;
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  if (g_building_frame.commands.empty()) return;
  ResolveRecord resolve{};
  resolve.command_index =
      static_cast<uint32_t>(g_building_frame.commands.size() - 1);
  resolve.tex_object = dest_texture;
  for (uint32_t i = 0; i < 6; ++i) {
    resolve.fetch[i] = REX_LOAD_U32(resolve.tex_object + 28 + i * 4);
  }
  // Object-side fetch constants hold untranslated addresses; apply the GPU
  // translation (incl. the 0x1000 bank adjust) so the id matches the
  // translated ids the draw-time texture capture records.
  const uint32_t va = (resolve.fetch[1] >> 12) << 12;
  resolve.phys = (va & 0x1FFFFFFFu) +
                 ((((va >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
  g_building_frame.resolves.push_back(resolve);
}

void OnSetStreamSource(uint32_t device, uint32_t stream, uint32_t vb,
                       uint32_t offset_bytes, uint32_t stride_bytes) {
  if (stream >= kVertexStreamScan) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_INFO("[native-render] SetStreamSource stream {} out of scan range",
                  stream);
    }
    return;
  }
  g_tls_streams[stream] = {vb, offset_bytes, stride_bytes,
                           g_completed_sequence.load(std::memory_order_relaxed)};
}

void OnViewportSet(uint8_t* base, uint32_t device) {
  if (!CaptureEnabled() || device == 0) {
    return;
  }
  // The game calls D3D SetViewport directly (not through the 67-type
  // dispatcher), so small render-target passes never announce their render
  // area in the command stream. Append a synthetic record (type 200) with
  // the device's viewport shadow (7 dwords at dev+13024), stored big-endian
  // like real command payloads.
  uint32_t vp[7];
  for (uint32_t i = 0; i < 7; ++i) {
    vp[i] = REX_LOAD_U32(device + 13024 + i * 4);
  }
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  const size_t old_size = g_building_frame.command_blob.size();
  if (old_size > std::numeric_limits<uint32_t>::max() - sizeof(vp)) {
    return;
  }
  g_building_frame.commands.push_back(
      {200u, 0u, uint32_t(sizeof(vp)), static_cast<uint32_t>(old_size)});
  g_building_frame.command_blob.resize(old_size + sizeof(vp));
  uint8_t* p = g_building_frame.command_blob.data() + old_size;
  for (uint32_t i = 0; i < 7; ++i) {
    p[i * 4 + 0] = uint8_t(vp[i] >> 24);
    p[i * 4 + 1] = uint8_t(vp[i] >> 16);
    p[i * 4 + 2] = uint8_t(vp[i] >> 8);
    p[i * 4 + 3] = uint8_t(vp[i]);
  }
}

void OnShaderConstF(uint8_t* base, bool is_pixel, uint32_t start_reg,
                    uint32_t data_va, uint32_t vec4_count) {
  if (!CaptureEnabled() || data_va == 0 || vec4_count == 0 ||
      vec4_count > 256) {
    return;
  }
  // The type-34 per-draw callbacks (and other game code) call the D3D
  // constant setters directly, bypassing the 67-type stream — the world's
  // transforms travel this way. Synthetic records 201 (VS) / 202 (PS):
  // {startReg BE, count BE, count*16 bytes of guest (BE) float data}.
  const uint32_t bytes = 8 + vec4_count * 16;
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  const size_t old_size = g_building_frame.command_blob.size();
  if (old_size > std::numeric_limits<uint32_t>::max() - bytes) {
    return;
  }
  g_building_frame.commands.push_back({is_pixel ? 202u : 201u, 0u, bytes,
                                       static_cast<uint32_t>(old_size)});
  g_building_frame.command_blob.resize(old_size + bytes);
  uint8_t* p = g_building_frame.command_blob.data() + old_size;
  const uint32_t header[2] = {start_reg, vec4_count};
  for (int i = 0; i < 2; ++i) {
    p[i * 4 + 0] = uint8_t(header[i] >> 24);
    p[i * 4 + 1] = uint8_t(header[i] >> 16);
    p[i * 4 + 2] = uint8_t(header[i] >> 8);
    p[i * 4 + 3] = uint8_t(header[i]);
  }
  std::memcpy(p + 8, base + data_va, size_t(vec4_count) * 16);
}

// Profiling (2026-09-15): time spent in the draw hooks during a frame
// (constant-bank snapshots, texture fetch capture + content stamps).
uint64_t g_draw_hook_ns = 0;
uint64_t g_hook_tex_ns = 0;   // CaptureBoundTextures
uint64_t g_hook_bank_ns = 0;  // constant-bank snapshots
struct HookTimer {
  uint64_t* sink = &g_draw_hook_ns;
  Clock::time_point t0 = Clock::now();
  HookTimer() = default;
  explicit HookTimer(uint64_t* s) : sink(s) {}
  ~HookTimer() {
    *sink += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0)
            .count());
  }
};

void OnDrawIndexed(uint8_t* base, uint32_t device,
                   uint32_t primitive_type, uint32_t arg5, uint32_t arg6,
                   uint32_t index_count, uint32_t caller) {
  if (!CaptureEnabled()) {
    return;
  }
  HookTimer hook_timer;

  // Current GPU shaders = last IM_LOADed microcode (see DrawRecord comment).
  DrawRecord rec{};
  rec.kind = DrawKind::kIndexed;
  rec.device = device;
  rec.primitive_type = primitive_type;
  rec.arg5 = arg5;  // baseVertex (VGT_INDX_OFFSET)
  rec.arg6 = arg6;  // startIndex
  rec.count = index_count;
  rec.d3d_caller = caller;
  rec.dispatch_caller = g_dispatch_caller;
  rec.vs_object = realsteel::shader_dump::BoundVertexShaderVa();
  rec.ps_object = realsteel::shader_dump::BoundPixelShaderVa();
  {
    bool is_vs = false;
    realsteel::shader_dump::LookupUcode(rec.vs_object, &rec.vs_container, &is_vs);
    realsteel::shader_dump::LookupUcode(rec.ps_object, &rec.ps_container, &is_vs);
    // The object's last load is the better identity (covers immediate loads).
    if (const uint64_t h = realsteel::shader_dump::BoundShaderContainer(true)) rec.vs_container = h;
    if (const uint64_t h = realsteel::shader_dump::BoundShaderContainer(false)) rec.ps_container = h;
  }
  {
    HookTimer tex_timer(&g_hook_tex_ns);
    realsteel::texture_dump::CaptureBoundTextures(
        base, device, rec.textures, rec.texture_fmt, rec.texture_hash,
        FullCaptureActive(), g_building_frame.index);
  }

  // v4: geometry sources. The IB comes from the SetIndices slot; vertex
  // streams from the SetStreamSource shadow (docs/d3d_surface.md). Resource
  // data VAs are read directly — they are CPU-addressable guest pointers (the
  // draw routine itself GPU-translates the same +24 field).
  if (device != 0) {
    // The current IB/VB objects are alive: the draw that just ran used them,
    // and the runtime's deferred-release lists (SetIndices/SetStreamSource)
    // keep bound resources valid while the GPU may still read them. Plain
    // loads only here — a per-draw VirtualQuery guard cost ~2ms/draw and
    // dropped the game to 1 FPS (2026-08-29).
    const uint32_t ib = REX_LOAD_U32(device + kDevIndexBuffer);
    if (ib != 0) {
      rec.ib_flags = REX_LOAD_U32(ib + kResFlags);
      const uint32_t ib_data_va = REX_LOAD_U32(ib + kResDataVa);
      const uint32_t index_size = (rec.ib_flags & 0x80000000u) ? 4u : 2u;
      const uint64_t bytes = uint64_t(index_count) * index_size;
      if (ib_data_va != 0 && bytes != 0 && bytes <= kMaxBufferSnapshotBytes) {
        rec.ib_va = ib_data_va + arg6 * index_size;
        rec.ib_bytes = static_cast<uint32_t>(bytes);
      }
    }
    const uint64_t current_frame =
        g_completed_sequence.load(std::memory_order_relaxed);
    for (uint32_t s = 0; s < kVertexStreamScan; ++s) {
      // VB objects store pre-encoded vertex fetch constants (runtime diag
      // 2026-08-29): +24 = data VA | type(low 2 bits, 3 = vertex fetch),
      // +28 = endian(low 2 bits) | size_in_dwords << 2. The first v4 build
      // read +28 as a raw byte size (0x100005C2-style values) and rejected
      // every stream against the snapshot cap — hence "vb streams 0/0".
      // Source: our SetStreamSource hook shadow only (same thread as the
      // draw; runtime-verified to match the device slot). The former
      // device-slot fallback is gone — its slot array overlaps the stride
      // table at stream 17, so blind scans read garbage "pointers".
      const StreamBinding& bind = g_tls_streams[s];
      const uint32_t vb_obj = bind.vb;
      const uint32_t offset = bind.offset;
      const uint32_t stride = bind.stride;
      // Skip unbound streams and bindings older than the previous frame —
      // those may reference freed objects (see StreamBinding::frame).
      if (vb_obj == 0 || bind.frame + 2 <= current_frame) {
        continue;
      }
      const uint32_t fetch_dword0 = REX_LOAD_U32(vb_obj + kResDataVa);
      const uint32_t fetch_dword1 = REX_LOAD_U32(vb_obj + kResByteSize);
      // Low 2 bits of dword0 are the fetch-constant type; 3 = vertex fetch.
      // Anything else means the binding is stale or the object is garbage.
      if ((fetch_dword0 & 3u) != 3u) {
        continue;
      }
      const uint32_t data_va = fetch_dword0 & ~3u;
      const uint32_t total_bytes = ((fetch_dword1 >> 2) & 0xFFFFFFu) * 4u;
      if (data_va == 0 || total_bytes == 0 || offset >= total_bytes) {
        continue;
      }
      const uint32_t bind_bytes = total_bytes - offset;
      if (bind_bytes > kMaxBufferSnapshotBytes) {
        continue;
      }
      if (s >= kVertexStreamSlots) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          REXLOG_INFO(
              "[native-render] vertex stream {} active but capture format "
              "records only {} slots — widen kVertexStreamSlots",
              s, kVertexStreamSlots);
        }
        continue;
      }
      rec.vb_va[s] = data_va + offset;
      rec.vb_bytes[s] = bind_bytes;
      rec.vb_stride[s] = stride;
    }

  }

  if (device != 0) {
    uint32_t vp[4];
    ReadViewportShadow(base, device, vp);
    for (uint32_t i = 0; i < 8; ++i) rec.rs[i] = REX_LOAD_U32(device + 10540 + i * 4);
    for (uint32_t i = 0; i < 4; ++i) rec.rs[8 + i] = REX_LOAD_U32(device + 12276 + i * 4);
    for (uint32_t i = 0; i < 36; ++i) rec.rs2[i] = REX_LOAD_U32(device + 10360 + i * 4);
    rec.vp_x = vp[0];
    rec.vp_y = vp[1];
    rec.vp_w = vp[2];
    rec.vp_h = vp[3];
  }

  std::lock_guard<std::mutex> lock(g_capture_mutex);
  HookTimer bank_timer(&g_hook_bank_ns);
  if (FullCaptureActive() && device != 0) {
    rec.vs_bank_offset = SnapshotBank(base, device + 1920, 4096);
    rec.ps_bank_offset = SnapshotBank(base, device + 6016, 4096);
    rec.bool_bank_offset = SnapshotBank(base, device + 10112, 32);
  }
  g_building_frame.draws.push_back(rec);
  ++g_building_frame.indexed_count;
  g_building_frame.indices += index_count;
}

void OnBeginVertices(uint8_t* base, uint32_t device,
                     uint32_t primitive_type, uint32_t vertex_count,
                     uint32_t vertex_stride, uint32_t guest_data,
                     uint32_t caller) {
  if (!CaptureEnabled()) {
    return;
  }
  HookTimer hook_timer;

  const uint32_t payload_bytes =
      SaturatingPayloadSize(vertex_count, vertex_stride);
  const uint32_t vs = realsteel::shader_dump::BoundVertexShaderVa();
  const uint32_t ps = realsteel::shader_dump::BoundPixelShaderVa();
  uint32_t textures[realsteel::texture_dump::kSlotCount];
  uint32_t texture_fmt[realsteel::texture_dump::kSlotCount];
  uint64_t texture_hash[realsteel::texture_dump::kSlotCount];
  {
    HookTimer tex_timer(&g_hook_tex_ns);
    realsteel::texture_dump::CaptureBoundTextures(
        base, device, textures, texture_fmt, texture_hash,
        FullCaptureActive(), g_building_frame.index);
  }
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  g_building_frame.draws.push_back(
      {DrawKind::kInline, device, primitive_type, vertex_count, vertex_stride,
       vertex_count, guest_data, payload_bytes, caller, g_dispatch_caller,
       kInvalidPayloadOffset, vs, ps});
  std::memcpy(g_building_frame.draws.back().textures, textures,
              sizeof(textures));
  std::memcpy(g_building_frame.draws.back().texture_fmt, texture_fmt,
              sizeof(texture_fmt));
  std::memcpy(g_building_frame.draws.back().texture_hash, texture_hash,
              sizeof(texture_hash));
  {
    DrawRecord& rec = g_building_frame.draws.back();
    bool is_vs = false;
    realsteel::shader_dump::LookupUcode(vs, &rec.vs_container, &is_vs);
    realsteel::shader_dump::LookupUcode(ps, &rec.ps_container, &is_vs);
    if (const uint64_t h = realsteel::shader_dump::BoundShaderContainer(true)) rec.vs_container = h;
    if (const uint64_t h = realsteel::shader_dump::BoundShaderContainer(false)) rec.ps_container = h;
    if (device != 0) {
      uint32_t vp[4];
      ReadViewportShadow(base, device, vp);
    for (uint32_t i = 0; i < 8; ++i) rec.rs[i] = REX_LOAD_U32(device + 10540 + i * 4);
    for (uint32_t i = 0; i < 4; ++i) rec.rs[8 + i] = REX_LOAD_U32(device + 12276 + i * 4);
    for (uint32_t i = 0; i < 36; ++i) rec.rs2[i] = REX_LOAD_U32(device + 10360 + i * 4);
      rec.vp_x = vp[0];
      rec.vp_y = vp[1];
      rec.vp_w = vp[2];
      rec.vp_h = vp[3];
      if (FullCaptureActive()) {
        rec.vs_bank_offset = SnapshotBank(base, device + 1920, 4096);
        rec.ps_bank_offset = SnapshotBank(base, device + 6016, 4096);
        rec.bool_bank_offset = SnapshotBank(base, device + 10112, 32);
      }
    }
  }
  ++g_building_frame.inline_count;
  g_building_frame.inline_vertices += vertex_count;
  g_building_frame.inline_bytes += payload_bytes;
  g_building_frame.max_inline_bytes =
      std::max(g_building_frame.max_inline_bytes, payload_bytes);
}

namespace {

// Guest memory can contain stale VB bindings whose backing pages are gone; a
// plain memcpy over such a range killed the game at the press-start
// transition (2026-08-29). Guard the copy so a bad range fails the snapshot
// instead of the process.
#ifdef _WIN32
// 2026-10-02: structured exception handling instead of walking the range
// with VirtualQuery. Without the xenos plugin the guest heaps consist of
// very many small regions, and the walk cost 2 s per frame in 3D scenes
// (1 frame every 2-3 s; with the plugin it had been ~4 ms). A fault on a
// stale binding simply fails the snapshot.
bool GuardedCopy(void* dst, const void* src, size_t bytes) {
  // No VirtualQuery at all: the stall watchdog caught this thread inside
  // NtQueryVirtualMemory (a single query takes about a millisecond in this
  // address space without the plugin; thousands per frame = seconds).
  __try {
    std::memcpy(dst, src, bytes);
    return true;
  } __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
    return false;
  }
}
#else
// Linux: no structured exceptions. process_vm_readv on ourselves copies
// through the kernel, which reports an unmapped range (EFAULT) instead of
// faulting. Where the call itself is unavailable (seccomp sandbox) the copy
// is unguarded, as it was before.
bool GuardedCopy(void* dst, const void* src, size_t bytes) {
  static std::atomic<bool> unavailable{false};
  if (bytes == 0) return true;
  if (!unavailable.load(std::memory_order_relaxed)) {
    struct iovec local{dst, bytes};
    struct iovec remote{const_cast<void*>(src), bytes};
    const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n == static_cast<ssize_t>(bytes)) return true;
    if (n >= 0 || errno == EFAULT) return false;
    unavailable.store(true);
    REXLOG_INFO("[native-render] process_vm_readv unavailable (errno {}): "
                "guest copies are unguarded", errno);
  }
  std::memcpy(dst, src, bytes);
  return true;
}
#endif

// Copies [va, va+bytes) of guest memory into the building frame's payload
// blob, deduping identical ranges within the frame. Returns the blob offset
// or kInvalidPayloadOffset. Caller must hold g_capture_mutex.
uint32_t SnapshotGuestRange(uint8_t* base, uint32_t va, uint32_t bytes) {
  if (va == 0 || bytes == 0 || bytes > kMaxBufferSnapshotBytes ||
      uint64_t(va) + bytes > REX_MEMORY_SIZE) {
    return kInvalidPayloadOffset;
  }
  const uint64_t key = (uint64_t(va) << 32) | bytes;
  const auto it = g_frame_range_offsets.find(key);
  if (it != g_frame_range_offsets.end()) {
    return it->second;
  }
  const size_t old_size = g_building_frame.payload_blob.size();
  if (old_size + bytes > kMaxPayloadBlobBytes) {
    return kInvalidPayloadOffset;
  }
  g_building_frame.payload_blob.resize(old_size + bytes);
  // REX_RAW_ADDR applies the host physical-mirror offset for VAs >= 0xE0000000.
  if (!GuardedCopy(g_building_frame.payload_blob.data() + old_size,
                   REX_RAW_ADDR(va), bytes)) {
    g_building_frame.payload_blob.resize(old_size);
    static std::atomic<uint32_t> fault_logs{0};
    if (fault_logs.fetch_add(1) < 8) {
      REXLOG_INFO("[native-render] snapshot fault at {:#010x} ({} B) — skipped",
                  va, bytes);
    }
    return kInvalidPayloadOffset;
  }
  const uint32_t offset = static_cast<uint32_t>(old_size);
  g_frame_range_offsets.emplace(key, offset);
  return offset;
}

}  // namespace

void OnFrameEnd(uint8_t* base) {
  const auto now = Clock::now();

  std::lock_guard<std::mutex> lock(g_capture_mutex);

  // Capture-to-disk triggers are detected before the snapshot loop so that
  // the frame ending right now is the first of an armed burst. Triggers (any
  // rising edge arms realsteel_capture_frames frames):
  //   1. realsteel_capture_now cvar (works as a launch flag for boot frames).
  //   2. F10 key on Windows (works even though guest input is pad-only).
  //   3. A file named "capture.trigger" appearing next to the executable
  //      (portable; on the Deck: `touch capture.trigger` over SSH). The file
  //      is deleted on pickup so it self-resets.
  bool trigger = false;
  const bool capture_now = REXCVAR_GET(realsteel_capture_now);
  if (capture_now && !g_capture_trigger_seen) {
    trigger = true;
  }
  g_capture_trigger_seen = capture_now;
#ifdef _WIN32
  const bool key_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  if (key_down && !g_capture_key_seen) {
    trigger = true;
  }
  g_capture_key_seen = key_down;
#endif
  if (g_building_frame.index % 30 == 0) {
    std::error_code trigger_ec;
    if (std::filesystem::exists("capture.trigger", trigger_ec)) {
      std::filesystem::remove("capture.trigger", trigger_ec);
      trigger = true;
    }
  }
  bool just_armed = false;
  if (trigger && g_capture_frames_remaining == 0) {
    // The current frame is skipped below (no write, no decrement): per-draw
    // bank snapshots only exist for draws made while armed, so the burst
    // covers the NEXT N frames, fully instrumented from their first draw.
    g_capture_frames_remaining = REXCVAR_GET(realsteel_capture_frames);
    g_capture_write_failed = false;
    just_armed = true;
    REXLOG_INFO("[native-render] capture armed: writing {} frame(s) to rs_captures/",
                g_capture_frames_remaining);
  }

  // Snapshot draw geometry before the frame is published. Inline payloads:
  // the guest wrote them right after BeginVertices returned; at Swap time the
  // inline heap segments for this frame have not been recycled yet — these
  // are snapshotted every frame (cheap, and consumed by the future native
  // path). Indexed-draw IB/VB ranges (v4) are snapshotted only while a
  // disk-capture burst is active: they are only consumed by the capture
  // files, and stale stream bindings can reference freed guest memory (the
  // GuardedCopy above skips those; a press-start crash on 2026-08-29 came
  // from copying such a range every frame).
  const bool snapshot_ranges = FullCaptureActive();
  for (DrawRecord& draw : g_building_frame.draws) {
    if (draw.kind == DrawKind::kIndexed) {
      if (!snapshot_ranges) {
        continue;
      }
      draw.ib_payload_offset = SnapshotGuestRange(base, draw.ib_va,
                                                  draw.ib_bytes);
      for (uint32_t s = 0; s < kVertexStreamSlots; ++s) {
        draw.vb_payload_offset[s] =
            SnapshotGuestRange(base, draw.vb_va[s], draw.vb_bytes[s]);
      }
      continue;
    }
    if (draw.kind != DrawKind::kInline || draw.guest_data == 0 ||
        draw.payload_bytes == 0 ||
        draw.payload_bytes > kMaxInlinePayloadBytes) {
      continue;
    }
    const size_t old_size = g_building_frame.payload_blob.size();
    if (old_size + draw.payload_bytes > kMaxPayloadBlobBytes) {
      continue;
    }
    g_building_frame.payload_blob.resize(old_size + draw.payload_bytes);
    std::memcpy(g_building_frame.payload_blob.data() + old_size,
                base + draw.guest_data, draw.payload_bytes);
    draw.payload_offset = static_cast<uint32_t>(old_size);
  }
  g_frame_range_offsets.clear();
  g_frame_bank_offsets.clear();

  // v6: front buffer address from the GPU-shared block of the frame's device.
  // v7: full constant-bank snapshots (raw guest bytes, no swap needed for a
  // straight copy through the host mapping).
  if (!g_building_frame.draws.empty()) {
    const uint32_t dev = g_building_frame.draws[0].device;
    if (dev != 0) {
      const uint32_t shared_block = REX_LOAD_U32(dev + 11024);
      if (shared_block != 0) {
        g_building_frame.front_buffer_phys = REX_LOAD_U32(shared_block + 8);
      }
      std::memcpy(g_building_frame.vs_const_bank, base + dev + 1920,
                  sizeof(g_building_frame.vs_const_bank));
      std::memcpy(g_building_frame.ps_const_bank, base + dev + 6016,
                  sizeof(g_building_frame.ps_const_bank));
      std::memcpy(g_building_frame.bool_bank, base + dev + 10112,
                  sizeof(g_building_frame.bool_bank));
    }
  }
  if (g_previous_frame.time_since_epoch().count() != 0) {
    const double frame_ms =
        std::chrono::duration<double, std::milli>(now - g_previous_frame)
            .count();
    g_log_window_frame_ms += frame_ms;
    g_log_window_max_ms = std::max(g_log_window_max_ms, frame_ms);
  } else {
    g_log_window_start = now;
  }
  g_previous_frame = now;
  ++g_log_window_frames;

  ++g_building_frame.index;
  std::swap(g_building_frame, g_completed_frame);
  g_building_frame.index = g_completed_frame.index;
  // The swap handed us the previous frame's storage. Clear its contents while
  // retaining capacity so normal gameplay remains allocation-free.
  g_building_frame.draws.clear();
  g_building_frame.commands.clear();
  g_building_frame.resolves.clear();
  g_building_frame.front_buffer_phys = 0;
  g_building_frame.command_blob.clear();
  g_building_frame.payload_blob.clear();
  g_building_frame.indexed_count = 0;
  g_building_frame.inline_count = 0;
  g_building_frame.indices = 0;
  g_building_frame.inline_vertices = 0;
  g_building_frame.inline_bytes = 0;
  g_building_frame.max_inline_bytes = 0;
  g_native_capture_this_frame = realsteel::native_present::Enabled();
  realsteel::texture_dump::SetDiskCaptureArmed(g_capture_frames_remaining > 0);
  g_completed_sequence.store(g_completed_frame.index,
                             std::memory_order_release);

  // In-process native renderer: hand the completed frame over (same bytes
  // a disk capture would hold). Runs on the guest thread, synchronously.
  if (g_native_capture_this_frame && realsteel::native_present::Enabled() &&
      !just_armed) {
    static std::vector<uint8_t> live_bytes;
    static rs::capture::FrameData live_frame;
    const auto t_ser = Clock::now();
    // The byte form only when something writes it to disk (a capture burst
    // reads g_completed_frame after this, frame dumps write the bytes).
    if (g_capture_frames_remaining > 0 || realsteel::native_present::WantsFrameBytes()) {
      SerializeFrame(g_completed_frame, live_bytes);
      realsteel::native_present::OnFrameBytes(base, live_bytes);
    } else {
      ToFrameData(g_completed_frame, live_frame);
      realsteel::native_present::OnFrameData(base, live_frame);
    }
    const auto t_done = Clock::now();
    realsteel::native_present::NoteGuestTiming(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_done - now).count()),
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_done - t_ser).count()),
        g_draw_hook_ns);
  }
  {
    static uint64_t frames = 0, total = 0, tex = 0, bank = 0;
    total += g_draw_hook_ns;
    tex += g_hook_tex_ns;
    bank += g_hook_bank_ns;
    if (++frames % 120 == 0) {
      REXLOG_INFO("[native-render] draw hooks per frame: total {:.1f} ms, "
                  "textures {:.1f} ms, banks {:.1f} ms",
                  total / 120.0e6, tex / 120.0e6, bank / 120.0e6);
      total = tex = bank = 0;
    }
  }
  g_draw_hook_ns = 0;
  g_hook_tex_ns = 0;
  g_hook_bank_ns = 0;

  // Synchronous writes are fine for a development operation.
  if (g_capture_frames_remaining > 0 && !just_armed) {
    // v14: synthetic type 204 — the latest display gamma ramp the title
    // built (see realsteel_gamma_dump.h). Payload: 9 header u32 (valid,
    // which, flag, dest_va, temp_va, type[2], level_bits[2]) then the raw
    // guest bytes of the three dest tables and the builder's temp table.
    {
      realsteel::gamma_dump::Snapshot snap;
      if (realsteel::gamma_dump::Latest(&snap)) {
        FrameCapture& f = g_completed_frame;
        const uint32_t header[9] = {snap.valid, snap.which, snap.flag,
                                    snap.dest_va, snap.temp_va, snap.type[0],
                                    snap.type[1], snap.level_bits[0],
                                    snap.level_bits[1]};
        const size_t at = f.command_blob.size();
        const uint32_t bytes = 9 * 4 + realsteel::gamma_dump::kDestBytes +
                               realsteel::gamma_dump::kTempBytes;
        f.commands.push_back({204u, 0u, bytes, static_cast<uint32_t>(at)});
        f.command_blob.resize(at + bytes);
        uint8_t* q = f.command_blob.data() + at;
        for (uint32_t i = 0; i < 9; ++i) {
          q[i * 4 + 0] = uint8_t(header[i] >> 24);
          q[i * 4 + 1] = uint8_t(header[i] >> 16);
          q[i * 4 + 2] = uint8_t(header[i] >> 8);
          q[i * 4 + 3] = uint8_t(header[i]);
        }
        std::memcpy(q + 36, snap.dest, realsteel::gamma_dump::kDestBytes);
        std::memcpy(q + 36 + realsteel::gamma_dump::kDestBytes, snap.temp,
                    realsteel::gamma_dump::kTempBytes);
      }
    }
    if (WriteFrameCaptureToDisk(g_completed_frame)) {
      --g_capture_frames_remaining;
      if (g_capture_frames_remaining == 0) {
        REXLOG_INFO(
            "[native-render] capture complete (last frame {}, draws={} "
            "commands={} payload_kib={:.1f})",
            g_completed_frame.index, g_completed_frame.draws.size(),
            g_completed_frame.commands.size(),
            double(g_completed_frame.payload_blob.size()) / 1024.0);
      }
    } else if (!g_capture_write_failed) {
      g_capture_write_failed = true;
      g_capture_frames_remaining = 0;
      REXLOG_INFO("[native-render] capture write FAILED; aborting burst");
    }
  }

  const int32_t interval = REXCVAR_GET(realsteel_native_render_log_interval);
  if (interval <= 0 ||
      g_completed_frame.index % static_cast<uint64_t>(interval) != 0) {
    return;
  }

  const double elapsed_s =
      std::chrono::duration<double>(now - g_log_window_start).count();
  const double guest_hz =
      elapsed_s > 0.0 ? double(g_log_window_frames) / elapsed_s : 0.0;
  const double avg_ms = g_log_window_frames > 1
                            ? g_log_window_frame_ms /
                                  double(g_log_window_frames - 1)
                            : 0.0;

  REXLOG_INFO(
      "[native-render] frame={} guest={:.2f}Hz avg/max={:.2f}/{:.2f}ms "
      "draws={} indexed={} inline={} indices={} inline_vtx={} inline_kib={:.1f} "
      "max_inline={}B commands={} command_kib={:.1f}",
      g_completed_frame.index, guest_hz, avg_ms, g_log_window_max_ms,
      g_completed_frame.draws.size(), g_completed_frame.indexed_count,
      g_completed_frame.inline_count, g_completed_frame.indices,
      g_completed_frame.inline_vertices,
      double(g_completed_frame.inline_bytes) / 1024.0,
      g_completed_frame.max_inline_bytes, g_completed_frame.commands.size(),
      double(g_completed_frame.command_blob.size()) / 1024.0);

  std::unordered_map<uint32_t, uint32_t> command_type_counts;
  for (const RenderCommandRecord& command : g_completed_frame.commands) {
    ++command_type_counts[command.type];
  }
  std::vector<std::pair<uint32_t, uint32_t>> hot_command_types(
      command_type_counts.begin(), command_type_counts.end());
  const size_t hot_command_count =
      std::min<size_t>(6, hot_command_types.size());
  std::partial_sort(hot_command_types.begin(),
                    hot_command_types.begin() + hot_command_count,
                    hot_command_types.end(),
                    [](const auto& a, const auto& b) {
                      return a.second > b.second;
                    });
  while (hot_command_types.size() < 6) {
    hot_command_types.emplace_back(0, 0);
  }
  REXLOG_INFO(
      "[native-render] hot render command types: {}={} {}={} {}={} {}={} "
      "{}={} {}={}",
      hot_command_types[0].first, hot_command_types[0].second,
      hot_command_types[1].first, hot_command_types[1].second,
      hot_command_types[2].first, hot_command_types[2].second,
      hot_command_types[3].first, hot_command_types[3].second,
      hot_command_types[4].first, hot_command_types[4].second,
      hot_command_types[5].first, hot_command_types[5].second);

  // The dispatch wrapper's entry LR is the title-level caller above the small
  // D3D helper. Ranking it turns one Deck run into a map of the game's actual
  // mesh/effect submission routines.
  std::unordered_map<uint32_t, uint32_t> caller_counts;
  for (const DrawRecord& draw : g_completed_frame.draws) {
    const uint32_t caller =
        draw.dispatch_caller != 0 ? draw.dispatch_caller : draw.d3d_caller;
    if (caller != 0) {
      ++caller_counts[caller];
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> hot_callers(caller_counts.begin(),
                                                         caller_counts.end());
  const size_t hot_count = std::min<size_t>(4, hot_callers.size());
  std::partial_sort(
      hot_callers.begin(), hot_callers.begin() + hot_count, hot_callers.end(),
      [](const auto& a, const auto& b) { return a.second > b.second; });
  while (hot_callers.size() < 4) {
    hot_callers.emplace_back(0, 0);
  }
  REXLOG_INFO(
      "[native-render] hot guest render callers: {:#010x}={} {:#010x}={} "
      "{:#010x}={} {:#010x}={}",
      hot_callers[0].first, hot_callers[0].second, hot_callers[1].first,
      hot_callers[1].second, hot_callers[2].first, hot_callers[2].second,
      hot_callers[3].first, hot_callers[3].second);

  g_log_window_start = now;
  g_log_window_frames = 0;
  g_log_window_frame_ms = 0.0;
  g_log_window_max_ms = 0.0;
}

}  // namespace realsteel::native_render

// Real Steel's own renderer queue dispatcher. r4 points to one variable-size
// command made of 16-byte cells; word 0 is a type in [0, 66] and word 2 is the
// number of following cells. Capturing here preserves the complete title-level
// render stream before the original function translates it to D3D/Xenos.
extern "C" REX_FUNC(sub_821B1C68) {
  realsteel::native_render::OnRenderCommand(base, ctx.r4.u32);
  __imp__sub_821B1C68(ctx, base);
}

// Direct callers of DrawIndexedVertices / BeginVertices. These wrappers do
// not alter arguments or results; they only retain the LR one level above the
// D3D helper while its original body runs.
#define REALSTEEL_WRAP_DRAW_DISPATCH(name)                              \
  extern "C" REX_FUNC(name) {                                          \
    realsteel::native_render::DispatcherCallerScope caller_scope(ctx.lr); \
    __imp__##name(ctx, base);                                           \
  }

REALSTEEL_WRAP_DRAW_DISPATCH(sub_821B0B08)
REALSTEEL_WRAP_DRAW_DISPATCH(sub_821D99D8)
REALSTEEL_WRAP_DRAW_DISPATCH(sub_821DA120)
REALSTEEL_WRAP_DRAW_DISPATCH(sub_8224F7A8)
REALSTEEL_WRAP_DRAW_DISPATCH(sub_821D9FA0)

#undef REALSTEEL_WRAP_DRAW_DISPATCH

// D3DDevice DrawIndexedVertices. This routine emits PM4_DRAW_INDX (0x22).
// Capture post-call so the device's pending shader/constants/state have been
// flushed into its shadow banks, matching the approach used by Skate 3.
extern "C" REX_FUNC(sub_822570D0) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t arg5 = ctx.r5.u32;
  const uint32_t arg6 = ctx.r6.u32;
  const uint32_t index_count = ctx.r7.u32;
  const uint32_t caller = ctx.lr;
  __imp__sub_822570D0(ctx, base);
  realsteel::native_render::OnDrawIndexed(
      base, device, primitive_type, arg5, arg6, index_count, caller);
}

// D3DDevice Resolve(dev, flags, srcRect, destTexture, destPoint, ...).
// Post-call; the destination texture object (r6) names the target memory.
extern "C" REX_FUNC(sub_8224E748) {
  const uint32_t dest_texture = ctx.r6.u32;
  __imp__sub_8224E748(ctx, base);
  realsteel::native_render::OnD3DResolve(base, dest_texture);
}

// D3D SetVertexShaderConstantF / SetPixelShaderConstantF
// (dev, startReg, data, vec4Count, runMask). Pre-call: the data pointer's
// contents are what the call applies.
extern "C" REX_FUNC(sub_82255D48) {
  realsteel::native_render::OnShaderConstF(base, false, ctx.r4.u32,
                                           ctx.r5.u32, ctx.r6.u32);
  __imp__sub_82255D48(ctx, base);
}

extern "C" REX_FUNC(sub_82255E20) {
  realsteel::native_render::OnShaderConstF(base, true, ctx.r4.u32,
                                           ctx.r5.u32, ctx.r6.u32);
  __imp__sub_82255E20(ctx, base);
}

// D3D SetViewport core (0x82253770). Post-call so the device's viewport
// shadow at dev+13024 holds the settled values.
extern "C" REX_FUNC(sub_82253770) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_82253770(ctx, base);
  realsteel::native_render::OnViewportSet(base, device);
}

// D3DDevice SetStreamSource(dev, stream, vb, offsetBytes, stride, dirtyMask).
// Shadowed host-side before the original runs; the original remains the sole
// owner of device state (docs/d3d_surface.md).
extern "C" REX_FUNC(sub_82253260) {
  realsteel::native_render::OnSetStreamSource(ctx.r3.u32, ctx.r4.u32,
                                              ctx.r5.u32, ctx.r6.u32,
                                              ctx.r7.u32);
  __imp__sub_82253260(ctx, base);
}

// D3DDevice BeginVertices. The guest fills the returned memory immediately
// after this call; recording the pointer lets the replay builder read the
// settled payload at the frame boundary.
extern "C" REX_FUNC(sub_82256C10) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t vertex_stride = ctx.r6.u32;
  const uint32_t caller = ctx.lr;
  __imp__sub_82256C10(ctx, base);
  realsteel::native_render::OnBeginVertices(
      base, device, primitive_type, vertex_count, vertex_stride, ctx.r3.u32,
      caller);
}

// Guest D3D Swap: publish the completed capture before the original routine
// submits its swap packet. The original remains the sole owner of presentation.
// Development: memory triggers beside the game, read once a frame when
// --realsteel_dev_triggers is on. "scan.trigger" holds a decimal number: every
// 32-bit guest word equal to it is logged (first 64). "poke.trigger" holds
// "<hex address> <decimal value>": the word is written. Both files are
// removed when read. Used to find where the game keeps things (the cash).
REXCVAR_DEFINE_BOOL(realsteel_dev_triggers, false, "Real Steel",
                    "Development: read scan.trigger / poke.trigger each frame");

namespace {
void DevTriggers(uint8_t* base) {
  if (!REXCVAR_GET(realsteel_dev_triggers)) return;
  std::error_code ec;
  if (std::filesystem::exists("scan.trigger", ec)) {
    std::ifstream f("scan.trigger");
    unsigned long long value = 0, value2 = 0, window = 0;
    f >> value >> value2 >> window;  // optional: a second word within <window> bytes
    f.close();
    std::filesystem::remove("scan.trigger", ec);
    const uint32_t want = uint32_t(value);
    const uint32_t want2 = uint32_t(value2);
    const uint32_t span = uint32_t(window);
    uint32_t hits = 0;
    // The game's own memory: 0x80000000-0x9FFFFFFF virtual (heap, statics)
    // and its physical mirror share the same bytes; scan the virtual range.
#ifdef _WIN32
    // Only committed, readable pages: the guest range is reserved as a whole
    // and touching an uncommitted page would fault.
    uint32_t addr = 0x00010000u;
    while (addr < 0xFFFF0000u) {
      MEMORY_BASIC_INFORMATION info{};
      if (VirtualQuery(base + addr, &info, sizeof(info)) == 0) break;
      const uint32_t region_end = uint32_t(std::min<uint64_t>(
          0xFFFF0000ull, uint64_t(reinterpret_cast<const uint8_t*>(info.BaseAddress) - base) + info.RegionSize));
      const bool readable = info.State == MEM_COMMIT && (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                            (info.Protect & PAGE_GUARD) == 0;
      if (readable) {
        for (uint32_t a = addr; a + 4 <= region_end; a += 4) {
          if (REX_LOAD_U32(a) == want) {
            bool ok = span == 0;
            for (uint32_t b = a >= span ? a - span : 0; !ok && b + 4 <= region_end && b <= a + span; b += 4) {
              if (b != a && b >= addr && REX_LOAD_U32(b) == want2) ok = true;
            }
            if (!ok) continue;
            if (hits < 256) REXLOG_INFO("[dev] scan {} at {:#010x}", want, a);
            ++hits;
          }
        }
      }
      if (region_end <= addr) break;
      addr = region_end;
    }
#endif
    REXLOG_INFO("[dev] scan {}: {} hit(s)", want, hits);
  }
  // "find.trigger" holds text: every guest address holding those bytes is
  // logged (first 64).
  if (std::filesystem::exists("find.trigger", ec)) {
    std::ifstream f("find.trigger", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::filesystem::remove("find.trigger", ec);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    uint32_t hits = 0;
#ifdef _WIN32
    uint32_t addr = 0x00010000u;
    while (addr < 0xFFFF0000u && !text.empty()) {
      MEMORY_BASIC_INFORMATION info{};
      if (VirtualQuery(base + addr, &info, sizeof(info)) == 0) break;
      const uint32_t region_end = uint32_t(std::min<uint64_t>(
          0xFFFF0000ull, uint64_t(reinterpret_cast<const uint8_t*>(info.BaseAddress) - base) + info.RegionSize));
      const bool readable = info.State == MEM_COMMIT && (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                            (info.Protect & PAGE_GUARD) == 0;
      if (readable && region_end > addr + text.size()) {
        const uint8_t* begin = base + addr;
        const uint8_t* end = base + region_end;
        for (const uint8_t* p = begin; (p = static_cast<const uint8_t*>(std::memchr(p, text[0], end - p))) != nullptr; ++p) {
          if (size_t(end - p) < text.size()) break;
          if (std::memcmp(p, text.data(), text.size()) == 0) {
            if (hits < 256) REXLOG_INFO("[dev] find at {:#010x}", uint32_t(p - base));
            ++hits;
          }
        }
      }
      if (region_end <= addr) break;
      addr = region_end;
    }
#endif
    REXLOG_INFO("[dev] find '{}': {} hit(s)", text.substr(0, 32), hits);
  }
  // "filter.trigger" holds a decimal value. The first use collects every
  // guest word equal to it; each later use keeps only the candidates that
  // now hold the new value ("filter.trigger" with "reset" starts over). For
  // finding a variable that follows something on screen, like a cursor.
  if (std::filesystem::exists("filter.trigger", ec)) {
    static std::vector<uint32_t> candidates;
    static bool started = false;
    std::ifstream f("filter.trigger");
    std::string word;
    f >> word;
    f.close();
    std::filesystem::remove("filter.trigger", ec);
    if (word == "reset") {
      candidates.clear();
      started = false;
      REXLOG_INFO("[dev] filter reset");
    } else {
      const uint32_t want = uint32_t(std::strtoul(word.c_str(), nullptr, 10));
      if (!started) {
#ifdef _WIN32
        uint32_t addr = 0x00010000u;
        while (addr < 0xC0000000u) {  // the 0xC/0xE ranges mirror 0xA
          MEMORY_BASIC_INFORMATION info{};
          if (VirtualQuery(base + addr, &info, sizeof(info)) == 0) break;
          const uint32_t region_end = uint32_t(std::min<uint64_t>(
              0xC0000000ull, uint64_t(reinterpret_cast<const uint8_t*>(info.BaseAddress) - base) + info.RegionSize));
          const bool writable = info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0 &&
                                (info.Protect & PAGE_GUARD) == 0;
          if (writable && !(addr >= 0x90000000u && addr < 0xA0000000u)) {  // 0x9 mirrors 0x8
            for (uint32_t a = addr; a + 4 <= region_end; a += 4) {
              if (REX_LOAD_U32(a) == want) candidates.push_back(a);
            }
          }
          if (region_end <= addr) break;
          addr = region_end;
        }
#endif
        started = true;
      } else {
        std::vector<uint32_t> kept;
        for (uint32_t a : candidates) {
          if (REX_LOAD_U32(a) == want) kept.push_back(a);
        }
        candidates.swap(kept);
      }
      REXLOG_INFO("[dev] filter {}: {} candidate(s)", want, candidates.size());
      for (size_t i = 0; i < candidates.size() && i < 48; ++i) {
        REXLOG_INFO("[dev] filter candidate {:#010x}", candidates[i]);
      }
    }
  }
  // "sample.trigger" holds a count: every thread's stack is logged that many
  // times (a poor man's profiler; tools/test/stack_profile.py reads it).
  if (std::filesystem::exists("sample.trigger", ec)) {
    std::ifstream f("sample.trigger");
    int count = 0;
    f >> count;
    f.close();
    std::filesystem::remove("sample.trigger", ec);
    realsteel::gpu::SampleStacks(count > 0 ? std::min(count, 1000) : 100);
  }
  // "peek.trigger" holds "<hex address> <byte count>": the bytes are logged.
  if (std::filesystem::exists("peek.trigger", ec)) {
    std::ifstream f("peek.trigger");
    std::string addr_text;
    unsigned long long count = 0;
    f >> addr_text >> count;
    f.close();
    std::filesystem::remove("peek.trigger", ec);
    const uint32_t addr = uint32_t(std::strtoul(addr_text.c_str(), nullptr, 16));
    if (addr >= 0x00010000u && addr < 0xFFFF0000u) {
      count = std::min<unsigned long long>(count, 4096);
      for (uint32_t row = 0; row < count; row += 32) {
        std::string line;
        char buf[16];
        for (uint32_t i = row; i < row + 32 && i < count; i += 4) {
          std::snprintf(buf, sizeof(buf), "%08x ", REX_LOAD_U32(addr + i));
          line += buf;
        }
        REXLOG_INFO("[dev] peek {:#010x}: {}", addr + row, line);
      }
    }
  }
  if (std::filesystem::exists("poke.trigger", ec)) {
    std::ifstream f("poke.trigger");
    std::string addr_text;
    unsigned long long value = 0;
    f >> addr_text >> value;
    f.close();
    std::filesystem::remove("poke.trigger", ec);
    const uint32_t addr = uint32_t(std::strtoul(addr_text.c_str(), nullptr, 16));
    if (addr >= 0x00010000u && addr < 0xFFFF0000u) {
      REX_STORE_U32(addr, uint32_t(value));
      REXLOG_INFO("[dev] poke {:#010x} = {}", addr, value);
    }
  }
}
}  // namespace

extern "C" REX_FUNC(sub_822502E8) {
  // Bring-up diagnostics (2026-10-02, GPU service): the first swaps, so a
  // stall can be placed before, inside or after the guest's Swap.
  static std::atomic<uint32_t> swaps{0};
  const uint32_t n = swaps.fetch_add(1);
  if (n < 4) REXLOG_INFO("[native-render] guest Swap #{} enter", n);
  DevTriggers(base);
  realsteel::native_render::OnFrameEnd(base);
  if (n < 4) REXLOG_INFO("[native-render] guest Swap #{} frame-end hook done", n);
  __imp__sub_822502E8(ctx, base);
  if (n < 4) REXLOG_INFO("[native-render] guest Swap #{} returned", n);
}

// Scripted pad for unattended testing (2026-10-03): --realsteel_input_script
// names a text file of lines "<first frame> <last frame> <buttons>", buttons
// being names joined by '+': A B X Y START BACK UP DOWN LEFT RIGHT LB RB L3 R3.
// While a script is loaded, pad 0 reads as connected and carries the buttons
// of the line covering the current guest frame (none otherwise).
REXCVAR_DEFINE_STRING(realsteel_input_script, "", "Real Steel",
                      "Text file of scripted pad input for testing "
                      "(\"<first frame> <last frame> <buttons>\" per line)");

REXCVAR_DEFINE_STRING(realsteel_input_live, "", "Real Steel",
                      "Text file holding the pad buttons to press right now "
                      "(same names as the input script; re-read every few "
                      "frames) - remote control for testing");
REXCVAR_DEFINE_STRING(realsteel_input_live2, "", "Real Steel",
                      "Like realsteel_input_live, for pad 1 (player 2), which "
                      "then reports as connected - testing local multiplayer");

namespace {
uint16_t ParseButtons(char* names) {
  static const struct { const char* name; uint16_t bit; } kNames[] = {
      {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
      {"START", 0x0010}, {"BACK", 0x0020}, {"L3", 0x0040}, {"R3", 0x0080},
      {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000},
      {"X", 0x4000}, {"Y", 0x8000}};
  uint16_t buttons = 0;
  for (char* tok = std::strtok(names, "+ \r\n"); tok != nullptr;
       tok = std::strtok(nullptr, "+ \r\n")) {
    for (const auto& n : kNames) {
      if (std::strcmp(tok, n.name) == 0) buttons |= n.bit;
    }
  }
  return buttons;
}

// Buttons from the live control file, refreshed every 4 frames.
bool LiveInputActive(uint32_t pad = 0) {
  static const bool active[2] = {!REXCVAR_GET(realsteel_input_live).empty(),
                                 !REXCVAR_GET(realsteel_input_live2).empty()};
  return pad < 2 && active[pad];
}
// The file holds "<sequence number> <buttons>": every new sequence number is
// one tap of those buttons, held for exactly 6 frames however the writer's
// timing falls (holding by wall-clock either missed or auto-repeated).
// Test input beyond buttons (2026-10-11): LSUP / LSDOWN / LSLEFT / LSRIGHT
// push the left stick fully, LT / RT pull the triggers; "NAME@90" holds the
// tap for 90 frames instead of 6.
struct LiveAnalog {
  int16_t lx = 0, ly = 0;
  uint8_t lt = 0, rt = 0;
};
LiveAnalog g_live_analog[2];
uint64_t g_live_hold_until[2] = {0, 0};

LiveAnalog LiveAnalogNow(uint64_t frame, uint32_t pad = 0) {
  if (pad > 1 || frame >= g_live_hold_until[pad]) return {};
  return g_live_analog[pad];
}

uint16_t LiveButtons(uint64_t frame, uint32_t pad = 0) {
  static std::mutex mutex;
  static uint64_t read_frames[2] = {~0ull, ~0ull}, holds[2] = {0, 0};
  static unsigned long long last_seqs[2] = {0, 0};
  static uint16_t pad_buttons[2] = {0, 0};
  if (pad > 1) return 0;
  uint64_t& read_frame = read_frames[pad];
  uint64_t& hold_until = holds[pad];
  unsigned long long& last_seq = last_seqs[pad];
  uint16_t& buttons = pad_buttons[pad];
  std::lock_guard<std::mutex> lock(mutex);
  if (frame != read_frame && frame % 4 == 0) {
    read_frame = frame;
    static const std::string paths[2] = {REXCVAR_GET(realsteel_input_live),
                                         REXCVAR_GET(realsteel_input_live2)};
    if (FILE* f = std::fopen(paths[pad].c_str(), "r")) {
      char line[128] = {};
      unsigned long long seq = 0;
      char names[100] = {};
      if (std::fgets(line, sizeof(line), f) != nullptr &&
          std::sscanf(line, "%llu %99s", &seq, names) >= 1 && seq != last_seq) {
        last_seq = seq;
        uint64_t hold = 6;
        if (char* at = std::strchr(names, '@')) {
          hold = std::max<uint64_t>(1, std::strtoull(at + 1, nullptr, 10));
          *at = 0;
        }
        LiveAnalog analog;
        {
          char copy[100];
          std::snprintf(copy, sizeof(copy), "%s", names);
          for (char* tok = std::strtok(copy, "+ "); tok != nullptr; tok = std::strtok(nullptr, "+ ")) {
            if (std::strcmp(tok, "LSUP") == 0) analog.ly = 32767;
            if (std::strcmp(tok, "LSDOWN") == 0) analog.ly = -32767;
            if (std::strcmp(tok, "LSLEFT") == 0) analog.lx = -32767;
            if (std::strcmp(tok, "LSRIGHT") == 0) analog.lx = 32767;
            if (std::strcmp(tok, "LT") == 0) analog.lt = 255;
            if (std::strcmp(tok, "RT") == 0) analog.rt = 255;
          }
        }
        g_live_analog[pad] = analog;
        buttons = ParseButtons(names);
        hold_until = frame + hold;
        g_live_hold_until[pad] = hold_until;
      }
      std::fclose(f);
    }
  }
  return frame < hold_until ? buttons : uint16_t(0);
}

struct ScriptedInput {
  uint64_t first = 0, last = 0;
  uint16_t buttons = 0;
};
const std::vector<ScriptedInput>& InputScript() {
  static const std::vector<ScriptedInput> script = [] {
    std::vector<ScriptedInput> out;
    const std::string path = REXCVAR_GET(realsteel_input_script);
    if (path.empty()) return out;
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
      REXLOG_INFO("[input-script] cannot open {}", path);
      return out;
    }
    static const struct { const char* name; uint16_t bit; } kNames[] = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
        {"START", 0x0010}, {"BACK", 0x0020}, {"L3", 0x0040}, {"R3", 0x0080},
        {"LB", 0x0100}, {"RB", 0x0200}, {"A", 0x1000}, {"B", 0x2000},
        {"X", 0x4000}, {"Y", 0x8000}};
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      unsigned long long first = 0, last = 0;
      char names[128] = {};
      if (line[0] == '#' || std::sscanf(line, "%llu %llu %127s", &first, &last, names) < 2) {
        continue;
      }
      ScriptedInput in{first, last, 0};
      for (char* tok = std::strtok(names, "+"); tok != nullptr; tok = std::strtok(nullptr, "+")) {
        for (const auto& n : kNames) {
          if (std::strcmp(tok, n.name) == 0) in.buttons |= n.bit;
        }
      }
      out.push_back(in);
    }
    std::fclose(f);
    REXLOG_INFO("[input-script] {} line(s) from {}", out.size(), path);
    return out;
  }();
  return script;
}
}  // namespace

// XInputGetState(user r3, state r4) - the game's only pad read. Feeds pad 0
// to the settings overlay and hides it from the game while the page is open
// (X_INPUT_STATE: packet u32, buttons u16, triggers 2x u8, sticks 4x s16, BE).
extern "C" REX_FUNC(sub_82237E20) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82237E20(ctx, base);
  {
    static std::atomic<uint32_t> per_user[5];
    const uint32_t n = per_user[user < 4 ? user : 4].fetch_add(1);
    if (n == 0 || n == 2000) {
      REXLOG_INFO("[input-script] GetState user {} call {} -> {:#x}", user, n, ctx.r3.u32);
    }
  }
  if (user == 1 && state != 0 && LiveInputActive(1)) {
    const uint64_t frame =
        realsteel::native_render::g_completed_sequence.load(std::memory_order_relaxed);
    const uint16_t buttons = LiveButtons(frame, 1);
    uint8_t* p = base + state;
    std::memset(p, 0, 16);
    p[0] = uint8_t(frame >> 24);
    p[1] = uint8_t(frame >> 16);
    p[2] = uint8_t(frame >> 8);
    p[3] = uint8_t(frame);
    p[4] = uint8_t(buttons >> 8);
    p[5] = uint8_t(buttons);
    static std::atomic<uint32_t> logs{0};
    if (buttons != 0 && logs.fetch_add(1) < 8) {
      REXLOG_INFO("[input-script] pad 1 buttons {:#06x} at frame {}", buttons, frame);
    }
    ctx.r3.u64 = 0;  // connected
    return;
  }
  if (user != 0 || state == 0) return;
  uint8_t* p = base + state;
  const std::vector<ScriptedInput>& script = InputScript();
  if (!script.empty() || LiveInputActive()) {
    const uint64_t frame =
        realsteel::native_render::g_completed_sequence.load(std::memory_order_relaxed);
    uint16_t scripted = LiveInputActive() ? LiveButtons(frame) : 0;
    for (const ScriptedInput& in : script) {
      if (frame >= in.first && frame <= in.last) scripted |= in.buttons;
    }
    {
      static std::atomic<uint32_t> calls{0}, hits{0};
      const uint32_t n = calls.fetch_add(1);
      if (n < 3 || n % 3000 == 0 || (scripted != 0 && hits.fetch_add(1) < 4)) {
        REXLOG_INFO("[input-script] GetState call {} frame {} buttons {:#06x} "
                    "(runtime said {:#x})", n, frame, scripted, ctx.r3.u32);
      }
    }
    std::memset(p, 0, 16);
    p[0] = uint8_t(frame >> 24);  // packet number: changes with the frame
    p[1] = uint8_t(frame >> 16);
    p[2] = uint8_t(frame >> 8);
    p[3] = uint8_t(frame);
    p[4] = uint8_t(scripted >> 8);
    p[5] = uint8_t(scripted);
    if (LiveInputActive()) {
      const LiveAnalog a = LiveAnalogNow(frame);
      p[6] = a.lt;
      p[7] = a.rt;
      p[8] = uint8_t(uint16_t(a.lx) >> 8);
      p[9] = uint8_t(uint16_t(a.lx));
      p[10] = uint8_t(uint16_t(a.ly) >> 8);
      p[11] = uint8_t(uint16_t(a.ly));
    }
    ctx.r3.u64 = 0;  // connected
    // Like a real pad: the settings / achievements pages see the buttons and
    // hide them from the game while open.
    if (realsteel::overlay::OnGuestPad(scripted, 0, 0)) std::memset(p + 4, 0, 12);
    return;
  }
  realsteel::keyboard::PadState keys;
  if (realsteel::keyboard::Read(&keys)) {
    {
      static std::atomic<uint32_t> logs{0};
      if (keys.buttons != 0 && logs.fetch_add(1) < 3) {
        REXLOG_INFO("[keyboard] pad 0 buttons {:#06x} from keys (runtime said {:#x}, packet {:02x}{:02x}{:02x}{:02x} pad buttons {:02x}{:02x})",
                    keys.buttons, ctx.r3.u32, p[0], p[1], p[2], p[3], p[4], p[5]);
      }
    }
    if (ctx.r3.u32 != 0) {  // no pad: the keyboard is pad 0
      std::memset(p, 0, 16);
      ctx.r3.u64 = 0;
    }
    const uint64_t frame =
        realsteel::native_render::g_completed_sequence.load(std::memory_order_relaxed);
    const uint32_t packet = uint32_t((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) + uint32_t(frame);
    p[0] = uint8_t(packet >> 24);
    p[1] = uint8_t(packet >> 16);
    p[2] = uint8_t(packet >> 8);
    p[3] = uint8_t(packet);
    p[4] |= uint8_t(keys.buttons >> 8);
    p[5] |= uint8_t(keys.buttons);
    if (keys.left_trigger > p[6]) p[6] = keys.left_trigger;
    if (keys.right_trigger > p[7]) p[7] = keys.right_trigger;
    const int16_t axes[4] = {keys.lx, keys.ly, keys.rx, keys.ry};
    for (int i = 0; i < 4; ++i) {
      const int16_t pad = int16_t((p[8 + i * 2] << 8) | p[9 + i * 2]);
      const int16_t v = axes[i] != 0 ? axes[i] : pad;
      p[8 + i * 2] = uint8_t(uint16_t(v) >> 8);
      p[9 + i * 2] = uint8_t(uint16_t(v));
    }
  }
  if (ctx.r3.u32 != 0) return;
  const uint16_t buttons = uint16_t((p[4] << 8) | p[5]);
  const int16_t lx = int16_t((p[8] << 8) | p[9]);
  const int16_t ly = int16_t((p[10] << 8) | p[11]);
  if (realsteel::overlay::OnGuestPad(buttons, lx, ly)) std::memset(p + 4, 0, 12);
}

// XInputGetCapabilities(user r3, flags r4, XINPUT_CAPABILITIES* r5): with an
// input script loaded, pad 0 reports as a connected standard gamepad even
// when the machine has none.
extern "C" REX_FUNC(sub_82237E18) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t caps = ctx.r5.u32;
  __imp__sub_82237E18(ctx, base);
  {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 8) {
      REXLOG_INFO("[keyboard] GetCapabilities user {} -> {:#x} (keyboard {})", user, ctx.r3.u32, realsteel::keyboard::Enabled());
    }
  }
  const bool pad1 = user == 1 && LiveInputActive(1);
  if (!pad1 && (user != 0 || caps == 0 || ctx.r3.u32 == 0 ||
                (InputScript().empty() && !LiveInputActive() && !realsteel::keyboard::Enabled()))) {
    return;
  }
  if (caps == 0) return;
  uint8_t* p = base + caps;
  std::memset(p, 0, 20);
  p[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
  p[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  std::memset(p + 4, 0xFF, 12);  // every button / axis present
  ctx.r3.u64 = 0;
}

// XInputGetKeystroke(user r3, flags r4, XINPUT_KEYSTROKE* r5) - the menus read
// button presses as keystrokes. With an input script loaded, the scripted
// buttons are delivered here as VK_PAD_* key-down / key-up events for pad 0
// (keystroke: VirtualKey u16, Unicode u16, Flags u16, UserIndex u8, HidCode
// u8, big-endian).
extern "C" REX_FUNC(sub_82237EF8) {
  const std::vector<ScriptedInput>& script = InputScript();
  const uint32_t user = ctx.r3.u32;
  const uint32_t out = ctx.r5.u32;
  {
    static std::atomic<uint32_t> calls{0};
    const uint32_t n = calls.fetch_add(1);
    if (n < 3 || n % 3000 == 0) {
      REXLOG_INFO("[input-script] GetKeystroke call {} user {:#x} flags {:#x}", n, user,
                  ctx.r4.u32);
    }
  }
  realsteel::keyboard::PadState keys;
  const bool keyboard = realsteel::keyboard::Read(&keys);
  if ((script.empty() && !keyboard) || out == 0 || (user != 0 && user != 0xFFu)) {
    __imp__sub_82237EF8(ctx, base);
    return;
  }
  static const struct { uint16_t bit; uint16_t vk; } kKeys[] = {
      {0x0001, 0x5810}, {0x0002, 0x5811}, {0x0004, 0x5812}, {0x0008, 0x5813},
      {0x0010, 0x5814}, {0x0020, 0x5815}, {0x0040, 0x5816}, {0x0080, 0x5817},
      {0x0100, 0x5805}, {0x0200, 0x5804}, {0x1000, 0x5800}, {0x2000, 0x5801},
      {0x4000, 0x5802}, {0x8000, 0x5803}};
  static std::mutex mutex;
  static uint16_t reported = 0;
  const uint64_t frame =
      realsteel::native_render::g_completed_sequence.load(std::memory_order_relaxed);
  uint16_t scripted = keys.buttons;
  for (const ScriptedInput& in : script) {
    if (frame >= in.first && frame <= in.last) scripted |= in.buttons;
  }
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& key : kKeys) {
    if ((scripted ^ reported) & key.bit) {
      const bool down = (scripted & key.bit) != 0;
      reported = uint16_t(down ? (reported | key.bit) : (reported & ~key.bit));
      uint8_t* p = base + out;
      p[0] = uint8_t(key.vk >> 8);
      p[1] = uint8_t(key.vk);
      p[2] = p[3] = 0;
      p[4] = 0;
      p[5] = down ? 1 : 2;  // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
      p[6] = 0;             // user index
      p[7] = 0;
      ctx.r3.u64 = 0;
      return;
    }
  }
  if (script.empty()) {           // keyboard only: the real pad's keystrokes still flow
    __imp__sub_82237EF8(ctx, base);
    return;
  }
  ctx.r3.u64 = 0x10D2;  // ERROR_EMPTY
}

// XContentCreateEx(user r3, root name r4, XCONTENT_DATA* r5, flags r6, ...).
// The add-on (DLC) packages are mounted through this as "dlc0", "dlc1", ...
// (2026-10-03). The runtime completes the call on a worker thread and reads
// the root name from guest memory then (and again when it completes) - but
// the game keeps the name in a buffer on its caller's stack, so by that time
// it held garbage: the package was mounted under an empty name, or the
// completion died on "Invalid UTF-8". The name is therefore copied into
// guest memory of our own that never goes away, one slot per name.
extern "C" REX_FUNC(sub_82237AC0) {
  constexpr uint32_t kSlots = 64, kSlotBytes = 32;
  static std::mutex mutex;
  static uint32_t pool = 0;
  static std::string names[kSlots];
  const uint32_t root = ctx.r4.u32;
  char root_name[kSlotBytes] = {};
  if (root != 0) {
    std::strncpy(root_name, reinterpret_cast<const char*>(base + root), kSlotBytes - 1);
  }
  if (root_name[0] != 0) {
    std::lock_guard<std::mutex> lock(mutex);
    if (pool == 0) {
      pool = rex::system::kernel_memory()->SystemHeapAlloc(kSlots * kSlotBytes);
      if (pool != 0) std::memset(base + pool, 0, kSlots * kSlotBytes);
    }
    for (uint32_t i = 0; pool != 0 && i < kSlots; ++i) {
      if (names[i].empty()) {
        names[i] = root_name;
        std::memcpy(base + pool + i * kSlotBytes, root_name, kSlotBytes);
      }
      if (names[i] == root_name) {
        ctx.r4.u64 = pool + i * kSlotBytes;
        break;
      }
    }
  }
  __imp__sub_82237AC0(ctx, base);
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 4) {
    REXLOG_INFO("[content] XContentCreateEx root='{}' name at {:#010x} -> {:#x}",
                root_name, ctx.r4.u32, ctx.r3.u32);
  }
}

// XLaunchNewImage(path r3, flags r4): the game leaves for the dashboard
// ("QUIT GAME" on the main menu, formerly "RETURN TO GAME LIBRARY"). There is
// no dashboard: the program ends instead.
extern "C" REX_FUNC(sub_82088778) {
  REXLOG_INFO("[native-render] the game asked to return to the dashboard: quitting");
  realsteel::overlay::QuitGame();
  ctx.r3.u64 = 0;
}

// Main menu rows (2026-10-11). The menu object (constructor sub_8215BA20)
// keeps one state word per row at this+340: 0 shown, 2 greyed out, 3 hidden.
// Rows: 0 MATCH MODES, 1 ASSEMBLY MODE, 2 MULTIPLAYER, 3 LEADERBOARDS,
// 4 ACHIEVEMENTS, 5 HELP AND OPTIONS, 6 UNLOCK FULL GAME, 7 DOWNLOAD CONTENT,
// 8 QUIT GAME. sub_8215A228 fills them in and then lays out the visible rows;
// the full game hides row 6 that way, a trial copy hides row 7. Its last
// check before the layout is sub_82115DB0 (called with the menu in r30, from
// 0x8215A2CC): there DOWNLOAD CONTENT, which only opened the Xbox
// marketplace, is hidden the same way.
REXCVAR_DEFINE_BOOL(realsteel_hide_download_content, true, "Real Steel",
                    "Leave DOWNLOAD CONTENT (the Xbox marketplace) off the main menu");
extern "C" REX_FUNC(sub_82115DB0) {
  if (uint32_t(ctx.lr) == 0x8215A2CCu && REXCVAR_GET(realsteel_hide_download_content)) {
    REX_STORE_U32(ctx.r30.u32 + 340 + 7 * 4, 3u);
    ctx.r3.u64 = 0;  // and not greyed out over it
    return;
  }
  __imp__sub_82115DB0(ctx, base);
}

// XamShowAchievementsUI(user r3, ...) - the main menu's ACHIEVEMENTS. The SDK
// only stubs it, and the game then waits for the system screen it opened to
// close (XN_SYS_UI = false), ignoring all input meanwhile: the menu froze
// (2026-10-11, on the Nova). Our achievements page stands in, framed by the
// same notifications the console's screen sends.
extern "C" REX_FUNC(sub_822378C8) {
  auto* ks = rex::system::kernel_state();
  if (ks != nullptr) ks->BroadcastNotification(0x9, 1);
  const bool shown = realsteel::overlay::ShowAchievements([] {
    REXLOG_INFO("[achievements] telling the game the system screen closed");
    if (auto* k = rex::system::kernel_state()) k->BroadcastNotification(0x9, 0);
  });
  if (!shown) {
    // No overlay: the screen "closes" at once.
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (auto* k = rex::system::kernel_state()) k->BroadcastNotification(0x9, 0);
    }).detach();
  }
  REXLOG_INFO("[achievements] the game opened the achievements screen ({})",
              shown ? "page shown" : "no overlay");
  ctx.r3.u64 = 0;  // ERROR_SUCCESS
}

// XInputSetState(user r3, XINPUT_VIBRATION* r4: two big-endian u16 motor
// speeds) - the "Vibration" setting: with it off the motors are told to stop
// instead (the game's own values are restored afterwards).
extern "C" REX_FUNC(sub_82237E30) {
  const uint32_t vibration = ctx.r4.u32;
  if (vibration == 0 || realsteel::overlay::VibrationEnabled()) {
    __imp__sub_82237E30(ctx, base);
    return;
  }
  uint8_t saved[4];
  std::memcpy(saved, base + vibration, 4);
  std::memset(base + vibration, 0, 4);
  __imp__sub_82237E30(ctx, base);
  std::memcpy(base + vibration, saved, 4);
}
