// realsteel - Xenos shader microcode capture (Phase 2 groundwork).
//
// Strategy (see docs/d3d_surface.md): the guest D3D shader flush
// (sub_8225E108) uploads microcode with PM4 IM_LOAD packets:
//   0xC0012700, addr|type, start<<16|sizeDwords
// Scanning the command-buffer range the flush wrote yields the microcode
// physical address, size, and shader type (0 = vertex, 1 = pixel) with no
// assumptions about the shader object layout. Unique blobs are hashed and
// dumped to rs_shaders/ for offline translation (XenosRecomp evaluation).
//
// The SetShader hooks (sub_822560A0 -> device+13068, sub_822562A8 ->
// device+13072) log each unique shader object's header fields once, to settle
// the VS/PS orientation question and recover the object layout.

#include "realsteel_shader_dump.h"

#include "generated/default_final/realsteel_init.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(realsteel_shader_dump, true, "Real Steel",
                    "Track the shader containers the game loads (the native "
                    "renderer resolves its shaders through this table)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(realsteel_dump_files, false, "Real Steel",
                    "Development: also write the captured microcode, containers "
                    "and the container manifest to rs_shaders/ for the offline "
                    "tools")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace realsteel::shader_dump {
namespace {

constexpr uint32_t kPm4ImLoad = 0xC0012700u;
constexpr uint32_t kMaxFlushBytes = 256 * 1024;
constexpr uint32_t kMaxMicrocodeBytes = 256 * 1024;
constexpr size_t kMaxObjectLogs = 128;

// Device struct offsets (docs/d3d_surface.md).
constexpr uint32_t kDevWritePtr = 48;
constexpr uint32_t kDevShaderA = 13068;  // set by sub_822560A0 (suspected PS)
constexpr uint32_t kDevShaderB = 13072;  // set by sub_822562A8 (suspected VS)

std::mutex g_mutex;
std::atomic<uint32_t> g_last_vs_va{0};
std::atomic<uint32_t> g_last_ps_va{0};
// Bound shader objects (SetVertexShader/SetPixelShader hooks) and the
// microcode VA each object was IM_LOADed from (paired in the flush hook).
// Per-draw records use these: the "last IM_LOAD" VA goes stale when the
// game binds an already-resident shader (2026-09-05: big 44-byte meshes
// attributed to the 2D post-process VS).
std::atomic<uint32_t> g_bound_vs_obj{0};
std::atomic<uint32_t> g_bound_ps_obj{0};
std::mutex g_object_mutex;
std::unordered_map<uint32_t, uint32_t> g_object_ucode_va;
std::unordered_set<uint64_t> g_seen_microcode;   // hashes already dumped
std::unordered_set<uint32_t> g_logged_objects;   // shader object ptrs logged
std::unordered_set<uint32_t> g_container_objects;  // objects container-dumped
std::unordered_set<uint64_t> g_seen_containers;    // container hashes written
std::unordered_set<uint64_t> g_seen_manifest;      // (va ^ hash) manifest lines
struct LiveManifest {
  uint64_t hash;
  bool is_vs;
};
std::unordered_map<uint32_t, LiveManifest> g_live_manifest;  // ucode va -> container
// Every container seen this session, by hash: the live renderer takes its
// bytes from here (2026-10-10), so nothing has to exist in rs_shaders/.
std::unordered_map<uint64_t, std::vector<uint8_t>> g_container_bytes;

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  return hash;
}

// The IM_LOAD address is a GPU physical address. All 512MB of physical memory
// is visible through the 0xA0000000 cached mirror in the guest address space.
uint32_t PhysToGuestVa(uint32_t phys) { return 0xA0000000u | (phys & 0x1FFFFFFFu); }

void DumpMicrocode(uint8_t* base, uint32_t shader_type, uint32_t phys,
                   uint32_t size_bytes, uint32_t start, uint32_t object_hint) {
  if (size_bytes == 0 || size_bytes > kMaxMicrocodeBytes) {
    return;
  }
  const uint32_t va = PhysToGuestVa(phys);
  const uint8_t* bytes = base + va;
  const uint64_t hash = Fnv1a64(bytes, size_bytes);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_seen_microcode.insert(hash).second) {
    return;
  }

  const bool to_disk = REXCVAR_GET(realsteel_dump_files);
  std::ofstream file;
  if (to_disk) {
    std::error_code ec;
    std::filesystem::create_directories("rs_shaders", ec);
    char name[96];
    std::snprintf(name, sizeof(name), "rs_shaders/%cs_%08X_%016llX.bin",
                  shader_type == 0 ? 'v' : 'p', va,
                  static_cast<unsigned long long>(hash));
    file.open(name, std::ios::binary | std::ios::trunc);
    if (file) {
      file.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(size_bytes));
    }
  }

  // First dwords logged for sanity: garbage here means the 0xA0000000 mirror
  // assumption is wrong and the dump path needs adjusting.
  REXLOG_INFO(
      "[shader-dump] new {} microcode: va={:#010x} bytes={} start={} "
      "hash={:016X} obj={:#010x} dw0={:08X} dw1={:08X} written={}",
      shader_type == 0 ? "VERTEX" : "PIXEL", va, size_bytes, start, hash,
      object_hint, REX_LOAD_U32(va + 0), REX_LOAD_U32(va + 4),
      to_disk && file.good());
}

// --- XenosRecomp container reconstruction -----------------------------------
//
// XenosRecomp consumes the on-disk Xbox 360 shader container: a header whose
// first dword is 0x102A11xx, followed by virtualSize bytes of metadata
// (constant table, definitions, vertex elements/interpolators) and
// physicalSize bytes of microcode. Its directory scanner matches
//   (flags & 0xFFFFFF00) == 0x102A1100 && field1C == 0 && field20 == 0
// and takes virtualSize+physicalSize bytes from the magic.
//
// At runtime the game keeps the metadata inside the shader object (magic seen
// at obj+40 for PS, obj+872 for VS) but the microcode lives in physical
// memory (the IM_LOAD address). We reconstruct the file container by
// concatenating the two. Shader::physicalOffset (first dword of the Shader
// record at container+shaderOffset) locates the microcode inside the physical
// region, so region base = imload_addr - physicalOffset.

constexpr uint32_t kContainerMagicMask = 0xFFFFFF00u;
constexpr uint32_t kContainerMagic = 0x102A1100u;

uint32_t FindContainerMagic(uint8_t* base, uint32_t object) {
  // Known homes first, then a bounded scan.
  static constexpr uint32_t kKnownOffsets[] = {40, 872};
  for (uint32_t off : kKnownOffsets) {
    if ((REX_LOAD_U32(object + off) & kContainerMagicMask) == kContainerMagic) {
      return object + off;
    }
  }
  for (uint32_t off = 0; off <= 1024; off += 4) {
    if ((REX_LOAD_U32(object + off) & kContainerMagicMask) == kContainerMagic) {
      return object + off;
    }
  }
  return 0;
}

// Container hash of the program last loaded for each shader object (regular
// IM_LOAD or IM_LOAD_IMMEDIATE) - what a draw with that object bound runs.
std::unordered_map<uint32_t, uint64_t> g_object_container;

// Fast path (2026-10-12, WRB arena: 5 % of the render thread was spent
// copying containers and FNV-hashing them byte by byte on every shader load,
// though almost every load repeats the last one): XXH3 straight from guest
// memory per (object, microcode address); unchanged content keeps its
// container hash and skips the copy.
struct QuickEntry {
  uint64_t quick;
  uint64_t hash;
};
std::unordered_map<uint64_t, QuickEntry> g_quick;

// |inline_region_va| != 0: the load was an IM_LOAD_IMMEDIATE whose payload
// (the whole physical region, patched for the current vertex declaration)
// sits in the command buffer at that address; there is no microcode address.
void TryDumpContainer(uint8_t* base, uint32_t shader_type, uint32_t object,
                      uint32_t ucode_va, uint32_t ucode_bytes,
                      uint32_t inline_region_va = 0) {
  // 2026-10-02: this used to run once per shader OBJECT. D3D re-patches a
  // vertex shader's vfetch block in place for every vertex declaration it is
  // drawn with and re-issues the IM_LOAD, so one object (and one microcode
  // address) carries several programs over time; only the first was ever
  // seen, and the other declaration's draws were paired with the wrong
  // layout (Zeus' shell on the versus screen). Every load is hashed now;
  // files and manifest lines stay deduplicated by content below.
  bool first_for_object = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    first_for_object = g_container_objects.insert(object).second;
  }

  const uint32_t c = FindContainerMagic(base, object);
  if (c == 0) {
    if (first_for_object) {
      REXLOG_INFO("[shader-dump] no container magic in obj={:#010x}", object);
    }
    return;
  }
  const uint32_t flags = REX_LOAD_U32(c + 0);
  const uint32_t virtual_size = REX_LOAD_U32(c + 4);
  const uint32_t physical_size = REX_LOAD_U32(c + 8);
  const uint32_t shader_offset = REX_LOAD_U32(c + 24);
  const uint32_t field_1c = REX_LOAD_U32(c + 28);
  const uint32_t field_20 = REX_LOAD_U32(c + 32);
  if (virtual_size < 0x24 || virtual_size > 64 * 1024 || physical_size == 0 ||
      physical_size > 512 * 1024 || shader_offset + 24 > virtual_size) {
    if (first_for_object) REXLOG_INFO(
        "[shader-dump] implausible container obj={:#010x} at +{}: flags={:08X} "
        "vsize={} psize={} soff={}",
        object, c - object, flags, virtual_size, physical_size, shader_offset);
    return;
  }
  // Shader record inside the virtual part: physicalOffset locates this
  // shader's microcode within the physical region; size is its byte count.
  const uint32_t ucode_offset = REX_LOAD_U32(c + shader_offset + 0);
  const uint32_t ucode_size = REX_LOAD_U32(c + shader_offset + 4);
  // Immediate loads carry the microcode only (physical size minus the
  // literal-constant prefix of |ucode_offset| bytes; 2026-10-02 log: 1476 B of
  // a 1540 B region). The prefix is read from the object's own region, whose
  // physical address is at object+32 (sub_8225DDC0 computes its copy source
  // as translate([obj+32] + ucode offset)).
  if (inline_region_va != 0 &&
      (ucode_offset >= physical_size || ucode_bytes != physical_size - ucode_offset)) {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 8) {
      REXLOG_INFO("[shader-dump] immediate load obj={:#010x}: payload {}B != "
                  "physical size {}B - ucode_off {} - not reconstructed",
                  object, ucode_bytes, physical_size, ucode_offset);
    }
    return;
  }
  if (inline_region_va == 0 &&
      (ucode_offset >= physical_size || ucode_va < ucode_offset)) {
    if (first_for_object) REXLOG_INFO(
        "[shader-dump] container obj={:#010x}: bad ucode_offset={} psize={}",
        object, ucode_offset, physical_size);
    return;
  }
  const uint32_t region_va =
      inline_region_va != 0 ? inline_region_va : ucode_va - ucode_offset;
  const uint64_t quick_key = (uint64_t(object) << 32) | ucode_va;
  uint64_t quick = 0;
  if (inline_region_va == 0) {
    quick = XXH3_64bits(base + c, virtual_size) ^ (XXH3_64bits(base + region_va, physical_size) * 31);
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_quick.find(quick_key);
    if (it != g_quick.end() && it->second.quick == quick) {
      g_object_container[object] = it->second.hash;
      g_live_manifest[ucode_va] = LiveManifest{it->second.hash, shader_type == 0};
      return;
    }
  }

  std::vector<uint8_t> out(size_t(virtual_size) + physical_size);
  std::memcpy(out.data(), base + c, virtual_size);
  if (inline_region_va != 0) {
    // Prefix source: the object's own region, known from its regular load
    // (the 11:47 build took it from translate([obj+32]) and got unrelated
    // bytes; with the right prefix the rebuilt containers match their
    // templates - verified offline on that run's dumps).
    uint32_t prefix_va = 0;
    {
      std::lock_guard<std::mutex> lock(g_object_mutex);
      const auto it = g_object_ucode_va.find(object);
      if (it != g_object_ucode_va.end() && it->second >= ucode_offset) {
        prefix_va = it->second - ucode_offset;
      }
    }
    if (prefix_va == 0) {
      static std::atomic<uint32_t> logs{0};
      if (logs.fetch_add(1) < 8) {
        REXLOG_INFO("[shader-dump] immediate load obj={:#010x}: no regular "
                    "load seen yet - not reconstructed", object);
      }
      return;
    }
    std::memcpy(out.data() + virtual_size, base + prefix_va, ucode_offset);
    std::memcpy(out.data() + virtual_size + ucode_offset, base + inline_region_va,
                physical_size - ucode_offset);
  } else {
    std::memcpy(out.data() + virtual_size, base + region_va, physical_size);
  }

  const uint64_t hash = Fnv1a64(out.data(), out.size());
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (inline_region_va == 0) g_quick[quick_key] = QuickEntry{quick, hash};
    g_object_container[object] = hash;
    if (inline_region_va == 0) {
      g_live_manifest[ucode_va] = LiveManifest{hash, shader_type == 0};
    }
    g_container_bytes.try_emplace(hash, out);
  }

  // Manifest: microcode VA -> container hash, the link the offline replayer
  // needs to pair a captured draw's shader VA with a container file (and via
  // XXH3 of that file, a shader-cache entry). Appended even when the
  // container itself was already written — multiple VAs can share one
  // container, and each pairing matters.
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (REXCVAR_GET(realsteel_dump_files) && inline_region_va == 0 &&
        g_seen_manifest.insert(uint64_t(ucode_va) * 0x9E3779B97F4A7C15ull ^
                               hash).second) {
      std::error_code manifest_ec;
      std::filesystem::create_directories("rs_shaders/containers", manifest_ec);
      std::ofstream manifest("rs_shaders/containers/manifest.txt",
                             std::ios::app);
      if (manifest) {
        char line[64];
        std::snprintf(line, sizeof(line), "%c %08X %016llX\n",
                      shader_type == 0 ? 'v' : 'p', ucode_va,
                      static_cast<unsigned long long>(hash));
        manifest << line;
      }
    }
    if (!g_seen_containers.insert(hash).second) {
      return;
    }
  }

  bool written = false;
  if (REXCVAR_GET(realsteel_dump_files)) {
    std::error_code ec;
    std::filesystem::create_directories("rs_shaders/containers", ec);
    char name[112];
    std::snprintf(name, sizeof(name), "rs_shaders/containers/%cs_%016llX.xpu",
                  shader_type == 0 ? 'v' : 'p',
                  static_cast<unsigned long long>(hash));
    std::ofstream file(name, std::ios::binary | std::ios::trunc);
    if (file) {
      file.write(reinterpret_cast<const char*>(out.data()),
                 static_cast<std::streamsize>(out.size()));
      written = file.good();
    }
  }

  REXLOG_INFO(
      "[shader-dump] container {} obj={:#010x}+{} flags={:08X} vsize={} "
      "psize={} soff={} ucode_off={} ucode_size={} (imload {}B) f1C={:08X} "
      "f20={:08X} hash={:016X} written={}",
      shader_type == 0 ? "VERTEX" : "PIXEL", object, c - object, flags,
      virtual_size, physical_size, shader_offset, ucode_offset, ucode_size,
      ucode_bytes, field_1c, field_20, hash, written);
}

void LogShaderObjectOnce(const char* setter, uint8_t* base, uint32_t object) {
  if (object == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logged_objects.size() >= kMaxObjectLogs ||
        !g_logged_objects.insert(object).second) {
      return;
    }
  }
  // Fields chosen from static analysis: +0 Common, +40/+60 usage-mask record
  // (13068-style object), +872/+892/+896 microcode record (13072-style).
  REXLOG_INFO(
      "[shader-dump] {} obj={:#010x} common={:08X} f40={:08X} f60={:08X} "
      "f872={:08X} f892={:08X} f896={:08X}",
      setter, object, REX_LOAD_U32(object + 0), REX_LOAD_U32(object + 40),
      REX_LOAD_U32(object + 60), REX_LOAD_U32(object + 872),
      REX_LOAD_U32(object + 892), REX_LOAD_U32(object + 896));
}

}  // namespace

uint32_t LastVertexShaderVa() {
  return g_last_vs_va.load(std::memory_order_relaxed);
}
uint32_t LastPixelShaderVa() {
  return g_last_ps_va.load(std::memory_order_relaxed);
}

uint64_t BoundShaderContainer(bool vertex) {
  const uint32_t obj = (vertex ? g_bound_vs_obj : g_bound_ps_obj)
                           .load(std::memory_order_relaxed);
  if (obj == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_object_container.find(obj);
  return it == g_object_container.end() ? 0 : it->second;
}

uint32_t BoundVertexShaderVa() {
  const uint32_t obj = g_bound_vs_obj.load(std::memory_order_relaxed);
  if (obj != 0) {
    std::lock_guard<std::mutex> lock(g_object_mutex);
    const auto it = g_object_ucode_va.find(obj);
    if (it != g_object_ucode_va.end()) return it->second;
  }
  return LastVertexShaderVa();
}

uint32_t BoundPixelShaderVa() {
  const uint32_t obj = g_bound_ps_obj.load(std::memory_order_relaxed);
  if (obj != 0) {
    std::lock_guard<std::mutex> lock(g_object_mutex);
    const auto it = g_object_ucode_va.find(obj);
    if (it != g_object_ucode_va.end()) return it->second;
  }
  return LastPixelShaderVa();
}

bool LookupUcode(uint32_t ucode_va, uint64_t* container_hash, bool* is_vs) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_live_manifest.find(ucode_va);
  if (it == g_live_manifest.end()) return false;
  *container_hash = it->second.hash;
  *is_vs = it->second.is_vs;
  return true;
}

bool ContainerBytes(uint64_t container_hash, std::vector<uint8_t>* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_container_bytes.find(container_hash);
  if (it == g_container_bytes.end()) return false;
  *out = it->second;
  return true;
}

}  // namespace realsteel::shader_dump

// Shader flush: scan the PM4 range this call emitted for IM_LOAD packets.
extern "C" REX_FUNC(sub_8225E108) {
  using namespace realsteel::shader_dump;

  const uint32_t device = ctx.r3.u32;
  const bool enabled = REXCVAR_GET(realsteel_shader_dump);
  uint32_t wp_before = 0;
  uint32_t shader_a = 0;
  uint32_t shader_b = 0;
  if (enabled && device != 0) {
    wp_before = REX_LOAD_U32(device + kDevWritePtr);
    shader_a = REX_LOAD_U32(device + kDevShaderA);
    shader_b = REX_LOAD_U32(device + kDevShaderB);
  }

  __imp__sub_8225E108(ctx, base);

  if (!enabled || device == 0 || wp_before == 0) {
    return;
  }
  const uint32_t wp_after = REX_LOAD_U32(device + kDevWritePtr);
  // A kick inside the flush moves the cursor to a new segment; skip those
  // (rare, and the same shaders re-upload on later flushes anyway).
  if (wp_after <= wp_before || wp_after - wp_before > kMaxFlushBytes) {
    return;
  }
  // wp points at the last dword written (stwu pre-increment): the emitted
  // range is (wp_before+4 .. wp_after). An IM_LOAD needs 2 payload dwords.
  for (uint32_t p = wp_before + 4; p + 8 <= wp_after; p += 4) {
    const uint32_t header = REX_LOAD_U32(p);
    // IM_LOAD_IMMEDIATE (0xC0nn2B00, sub_8225DDC0): the shader's physical
    // region copied into the command buffer and patched there for the bound
    // vertex declaration. Payload: type, start<<16|size, then the region.
    if ((header & 0xC000FFFFu) == 0xC0002B00u) {
      const uint32_t dwords = ((header >> 16) & 0x3FFFu) + 1;
      const uint32_t end = p + 4 + dwords * 4;
      if (dwords >= 3 && end <= wp_after + 4) {
        const uint32_t shader_type = REX_LOAD_U32(p + 4) & 0x3u;
        if (shader_type <= 1) {
          const uint32_t object = shader_type == 0 ? shader_b : shader_a;
          {
            static std::atomic<uint32_t> logs{0};
            if (logs.fetch_add(1) < 16) {
              REXLOG_INFO("[shader-bind] immediate load type={} obj={:#010x} "
                          "{} dwords start/size={:#010x}",
                          shader_type, object, dwords - 2, REX_LOAD_U32(p + 8));
            }
          }
          if (object != 0) {
            TryDumpContainer(base, shader_type, object, 0, (dwords - 2) * 4,
                             p + 12);
          }
        }
        p = end - 4;
        continue;
      }
    }
    if (header != kPm4ImLoad) {
      continue;
    }
    const uint32_t addr_type = REX_LOAD_U32(p + 4);
    const uint32_t start_size = REX_LOAD_U32(p + 8);
    const uint32_t shader_type = addr_type & 0x3u;
    const uint32_t phys = addr_type & ~0x3u;
    const uint32_t size_bytes = (start_size & 0xFFFFu) * 4;
    const uint32_t start = start_size >> 16;
    if (shader_type > 1) {
      continue;
    }
    const uint32_t object = shader_type == 0 ? shader_b : shader_a;
    // Track what the GPU is actually running for per-draw correlation.
    if (shader_type == 0) {
      g_last_vs_va.store(PhysToGuestVa(phys), std::memory_order_relaxed);
    } else {
      g_last_ps_va.store(PhysToGuestVa(phys), std::memory_order_relaxed);
    }
    DumpMicrocode(base, shader_type, phys, size_bytes, start, object);
    {
      static std::atomic<uint32_t> logs{0};
      if (logs.fetch_add(1) < 48) {
        REXLOG_INFO("[shader-bind] flush type={} obj={:#010x} ucode={:#010x} "
                    "(dev+13068={:#010x} dev+13072={:#010x})",
                    shader_type, object, PhysToGuestVa(phys), shader_a, shader_b);
      }
    }
    if (object != 0) {
      std::lock_guard<std::mutex> lock(g_object_mutex);
      g_object_ucode_va[object] = PhysToGuestVa(phys);
    }
    if (object != 0) {
      TryDumpContainer(base, shader_type, object, PhysToGuestVa(phys),
                       size_bytes);
    }
    p += 8;
  }
}

// D3D's immediate vertex-shader load (called from FlushShaders when the bound
// vertex declaration is not the one the shader object was patched for):
// reserves command-buffer space, writes 0xC0003B00 0x100, then
// IM_LOAD_IMMEDIATE 0xC0nn2B00 {0, size dwords} followed by a copy of the
// shader's physical region, and patches that copy for the declaration.
// Hooked directly (2026-10-02): the reservation usually starts a new
// command-buffer segment, which the flush hook's range scan skips.
extern "C" REX_FUNC(sub_8225DDC0) {
  using namespace realsteel::shader_dump;
  const uint32_t device = ctx.r3.u32;
  const uint32_t object = ctx.r5.u32;
  __imp__sub_8225DDC0(ctx, base);
  if (!REXCVAR_GET(realsteel_shader_dump) || device == 0 || object == 0) return;
  const uint32_t c = FindContainerMagic(base, object);
  if (c == 0) return;
  const uint32_t region_size = REX_LOAD_U32(c + 8);
  const uint32_t shader_offset = REX_LOAD_U32(c + 24);
  if (region_size == 0 || region_size > 512 * 1024 || shader_offset > 64 * 1024) return;
  const uint32_t ucode_offset = REX_LOAD_U32(c + shader_offset);
  if (ucode_offset >= region_size) return;
  const uint32_t physical_size = region_size - ucode_offset;  // the payload
  // The device write pointer names the last dword written = end of the copy.
  const uint32_t region_va = REX_LOAD_U32(device + kDevWritePtr) + 4 - physical_size;
  const uint32_t header = REX_LOAD_U32(region_va - 12);
  static std::atomic<uint32_t> logs{0};
  const bool log = logs.fetch_add(1) < 16;
  if ((header & 0xC000FFFFu) != 0xC0002B00u ||
      (REX_LOAD_U32(region_va - 4) & 0x3FFFu) != ((physical_size / 4) & 0x3FFFu)) {
    if (log) {
      REXLOG_INFO("[shader-bind] immediate load obj={:#010x}: packet not where "
                  "expected (header {:#010x}, size word {:#010x}, physical {}B)",
                  object, header, REX_LOAD_U32(region_va - 4), physical_size);
    }
    return;
  }
  if (log) {
    REXLOG_INFO("[shader-bind] immediate load obj={:#010x} region={:#010x} {}B",
                object, region_va, physical_size);
  }
  TryDumpContainer(base, 0, object, 0, physical_size, region_va);
}

// SetShader hooks: log each unique shader object header once.
extern "C" REX_FUNC(sub_822560A0) {
  const uint32_t object = ctx.r4.u32;
  realsteel::shader_dump::g_bound_ps_obj.store(object, std::memory_order_relaxed);
  {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 24) {
      REXLOG_INFO("[shader-bind] SetPixelShader dev={:#010x} obj={:#010x}",
                  ctx.r3.u32, object);
    }
  }
  __imp__sub_822560A0(ctx, base);
  if (REXCVAR_GET(realsteel_shader_dump)) {
    realsteel::shader_dump::LogShaderObjectOnce("SetShaderA(dev+13068)", base,
                                                object);
  }
}

extern "C" REX_FUNC(sub_822562A8) {
  const uint32_t object = ctx.r4.u32;
  realsteel::shader_dump::g_bound_vs_obj.store(object, std::memory_order_relaxed);
  {
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 24) {
      REXLOG_INFO("[shader-bind] SetVertexShader dev={:#010x} obj={:#010x}",
                  ctx.r3.u32, object);
    }
  }
  __imp__sub_822562A8(ctx, base);
  if (REXCVAR_GET(realsteel_shader_dump)) {
    realsteel::shader_dump::LogShaderObjectOnce("SetShaderB(dev+13072)", base,
                                                object);
  }
}
