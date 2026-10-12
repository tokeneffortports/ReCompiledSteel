#include "capture_format.h"

#include <cstdio>
#include <cstring>

namespace rs::capture {
namespace {

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t pos = 0;

  bool ReadU32(uint32_t& v) {
    if (pos + 4 > size) return false;
    std::memcpy(&v, data + pos, 4);
    pos += 4;
    return true;
  }
  bool ReadU64(uint64_t& v) {
    if (pos + 8 > size) return false;
    std::memcpy(&v, data + pos, 8);
    pos += 8;
    return true;
  }
  bool ReadBytes(std::vector<uint8_t>& out, size_t n) {
    if (pos + n > size) return false;
    out.assign(data + pos, data + pos + n);
    pos += n;
    return true;
  }
};

}  // namespace

bool LoadFrame(const std::string& path, FrameData& out, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long file_size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  const size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (read != bytes.size()) {
    error = "short read on " + path;
    return false;
  }
  return LoadFrameFromMemory(bytes.data(), bytes.size(), path, out, error);
}

bool LoadFrameFromMemory(const uint8_t* data, size_t size,
                         const std::string& path, FrameData& out,
                         std::string& error) {
  Reader r{data, size};
  uint32_t magic = 0, version = 0;
  uint32_t draw_count = 0, command_count = 0;
  uint32_t command_blob_bytes = 0, payload_blob_bytes = 0;
  if (!r.ReadU32(magic) || magic != kMagic) {
    error = path + ": bad magic";
    return false;
  }
  if (!r.ReadU32(version) || version < 1 || version > kMaxVersion) {
    error = path + ": unsupported version";
    return false;
  }
  out.version = version;
  if (!r.ReadU64(out.index) || !r.ReadU32(draw_count) ||
      !r.ReadU32(command_count) || !r.ReadU32(command_blob_bytes) ||
      !r.ReadU32(payload_blob_bytes)) {
    error = path + ": truncated header";
    return false;
  }
  uint32_t resolve_count = 0;
  if (version >= 5 && !r.ReadU32(resolve_count)) {
    error = path + ": truncated header (v5)";
    return false;
  }
  if (version >= 6 && !r.ReadU32(out.front_buffer_phys)) {
    error = path + ": truncated header (v6)";
    return false;
  }
  if (version >= 7) {
    if (!r.ReadBytes(out.vs_const_bank, 256 * 16) ||
        !r.ReadBytes(out.ps_const_bank, 256 * 16) ||
        !r.ReadBytes(out.bool_bank, 32)) {
      error = path + ": truncated constant banks (v7)";
      return false;
    }
  }
  if (draw_count > 1u << 20 || command_count > 1u << 22) {
    error = path + ": implausible counts";
    return false;
  }

  out.draws.resize(draw_count);
  for (DrawRecord& d : out.draws) {
    uint32_t kind = 0;
    if (!r.ReadU32(kind) || !r.ReadU32(d.device) ||
        !r.ReadU32(d.primitive_type) || !r.ReadU32(d.arg5) ||
        !r.ReadU32(d.arg6) || !r.ReadU32(d.count) || !r.ReadU32(d.guest_data) ||
        !r.ReadU32(d.payload_bytes) || !r.ReadU32(d.payload_offset) ||
        !r.ReadU32(d.d3d_caller) || !r.ReadU32(d.dispatch_caller)) {
      error = path + ": truncated draw records";
      return false;
    }
    if (version >= 2 &&
        (!r.ReadU32(d.vs_object) || !r.ReadU32(d.ps_object))) {
      error = path + ": truncated draw records (v2)";
      return false;
    }
    if (version >= 3) {
      const uint32_t slots = version >= 10 ? kTextureSlots : kTextureSlotsV9;
      for (uint32_t t = 0; t < slots; ++t) {
        if (!r.ReadU32(d.textures[t])) {
          error = path + ": truncated draw records (v3)";
          return false;
        }
      }
    }
    if (version >= 11) {
      for (uint32_t t = 0; t < kTextureSlots; ++t) {
        if (!r.ReadU32(d.texture_fmt[t])) {
          error = path + ": truncated draw records (v11)";
          return false;
        }
      }
    }
    if (version >= 4) {
      bool ok = r.ReadU32(d.ib_flags) && r.ReadU32(d.ib_va) &&
                r.ReadU32(d.ib_bytes) && r.ReadU32(d.ib_payload_offset);
      for (uint32_t s = 0; ok && s < kVertexStreamSlots; ++s) {
        ok = r.ReadU32(d.vb_va[s]) && r.ReadU32(d.vb_bytes[s]) &&
             r.ReadU32(d.vb_stride[s]) && r.ReadU32(d.vb_payload_offset[s]);
      }
      if (!ok) {
        error = path + ": truncated draw records (v4)";
        return false;
      }
    }
    if (version >= 8 &&
        (!r.ReadU32(d.vs_bank_offset) || !r.ReadU32(d.ps_bank_offset) ||
         !r.ReadU32(d.bool_bank_offset))) {
      error = path + ": truncated draw records (v8)";
      return false;
    }
    if (version >= 9 &&
        (!r.ReadU32(d.vp_x) || !r.ReadU32(d.vp_y) || !r.ReadU32(d.vp_w) ||
         !r.ReadU32(d.vp_h))) {
      error = path + ": truncated draw records (v9)";
      return false;
    }
    if (version >= 12) {
      for (uint32_t k = 0; k < 12; ++k) {
        if (!r.ReadU32(d.rs[k])) {
          error = path + ": truncated draw records (v12)";
          return false;
        }
      }
    }
    if (version >= 13) {
      for (uint32_t t = 0; t < kTextureSlots; ++t) {
        uint32_t lo = 0, hi = 0;
        if (!r.ReadU32(lo) || !r.ReadU32(hi)) {
          error = path + ": truncated draw records (v13)";
          return false;
        }
        d.texture_hash[t] = (uint64_t(hi) << 32) | lo;
      }
    }
    if (version >= 14) {
      for (uint32_t k = 0; k < 36; ++k) {
        if (!r.ReadU32(d.rs2[k])) {
          error = path + ": truncated draw records (v14)";
          return false;
        }
      }
    }
    if (version >= 15) {
      uint32_t w[4];
      for (uint32_t& x : w) {
        if (!r.ReadU32(x)) {
          error = path + ": truncated draw records (v15)";
          return false;
        }
      }
      d.vs_container = (uint64_t(w[1]) << 32) | w[0];
      d.ps_container = (uint64_t(w[3]) << 32) | w[2];
    }
    d.kind = static_cast<DrawKind>(kind);
  }
  out.commands.resize(command_count);
  for (CommandRecord& c : out.commands) {
    if (!r.ReadU32(c.type) || !r.ReadU32(c.guest_address) ||
        !r.ReadU32(c.byte_count) || !r.ReadU32(c.blob_offset)) {
      error = path + ": truncated command records";
      return false;
    }
  }
  out.resolves.resize(resolve_count);
  for (ResolveRecord& res : out.resolves) {
    bool ok = r.ReadU32(res.command_index) && r.ReadU32(res.tex_object) &&
              r.ReadU32(res.phys);
    for (uint32_t f = 0; ok && f < 6; ++f) {
      ok = r.ReadU32(res.fetch[f]);
    }
    if (!ok) {
      error = path + ": truncated resolve records";
      return false;
    }
  }
  if (!r.ReadBytes(out.command_blob, command_blob_bytes) ||
      !r.ReadBytes(out.payload_blob, payload_blob_bytes)) {
    error = path + ": truncated blobs";
    return false;
  }
  return true;
}

const char* CommandTypeName(uint32_t type) {
  // Identities recovered in docs/d3d_surface.md and docs/command_stream.md.
  switch (type) {
    case 1: return "BeginFrameHook";
    case 3: return "SetRenderState";
    case 4: return "SetSamplerState";
    case 5: return "SetVertexDeclaration?";
    case 6: return "BindEffectPassChecked";
    case 7: return "BeginMaterialInline";
    case 8: return "BeginMaterial";
    case 9: return "EndMaterial";
    case 11: return "MaterialParamBlock";
    case 12: return "SetIndices";
    case 14: return "SetStreamSource";
    case 15: return "DrawIndexedVertices";
    case 18: return "DrawInlineVertices";
    case 19: return "SetVertexShaderConstantF";
    case 21: return "SetVSConstF.y(single)";
    case 22: return "SetVSConstF.z(single)";
    case 23: return "SetVSConstF.w(single)";
    case 25: return "SetVertexShaderConstantBI";
    case 26: return "SetPixelShaderConstantF";
    case 29: return "SetPSConstF.z(single)";
    case 30: return "SetPSConstF.w(single)";
    case 32: return "SetPixelShaderConstantBI";
    case 33: return "SetViewport";
    case 34: return "PerDrawObjectCallback";
    case 35: return "SetTextureFetchConst";
    case 36: return "SetTexture";
    case 37: return "GetTexture";
    case 38: return "SetTexture(alias)";
    case 39: return "SetRenderTargetOrDS";
    case 40: return "GetRenderTarget";
    case 43: return "ReleaseResource";
    case 44: return "ReleaseResource2";
    case 49: return "ReleaseResourceSlot";
    case 52: return "Clear";
    case 53: return "Resolve";
    case 54: return "DebugAssert54";
    case 55: return "DebugAssert55";
    case 56: return "UserCallback";
    case 60: return "SetShaderGPRAllocation";
    case 61: return "SubRendererBegin";
    case 62: return "SubRendererEnd";
    case 63: return "SetFrameParams";
    case 64: return "SetScissorRect";
    case 65: return "SetDefaultAlphaBlendState";
    case 200: return "SetViewport(synthetic)";
    case 201: return "VSConstF(synthetic)";
    case 202: return "PSConstF(synthetic)";
    case 203: return "SurfaceObjects(synthetic)";
    case 204: return "GammaRamp(synthetic)";
    default: {
      static thread_local char buf[16];
      std::snprintf(buf, sizeof(buf), "type_%02u", type);
      return buf;
    }
  }
}

}  // namespace rs::capture
