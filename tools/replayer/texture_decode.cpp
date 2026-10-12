#include "texture_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rs::capture {
namespace {

// XGAddress2DTiledOffset: element index of (x, y) in a tiled Xenos 2D
// surface. |aligned_width| is the surface row width in elements, rounded up
// to 32; |elem_bytes| must be a power of two in 1..16 (texel bytes for linear
// formats, block bytes for DXT — coordinates are then block coordinates).
uint32_t TiledElementOffset(uint32_t x, uint32_t y, uint32_t aligned_width,
                            uint32_t elem_bytes) {
  const uint32_t log_bpp =
      (elem_bytes >> 2) + ((elem_bytes >> 1) >> (elem_bytes >> 2));
  const uint32_t macro =
      ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                          ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
          ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         log_bpp;
}

// Untiles |src| into a tightly packed row-major buffer of
// width_elems x height_elems elements. Out-of-range source elements (a
// truncated dump) are left zero.
std::vector<uint8_t> UntileSurface(const std::vector<uint8_t>& src,
                                   uint32_t width_elems, uint32_t height_elems,
                                   uint32_t pitch_elems, uint32_t elem_bytes) {
  const uint32_t aligned_w =
      (std::max(pitch_elems, width_elems) + 31u) & ~31u;
  std::vector<uint8_t> dst(size_t(width_elems) * height_elems * elem_bytes, 0);
  for (uint32_t y = 0; y < height_elems; ++y) {
    for (uint32_t x = 0; x < width_elems; ++x) {
      const size_t src_off =
          size_t(TiledElementOffset(x, y, aligned_w, elem_bytes)) * elem_bytes;
      if (src_off + elem_bytes > src.size()) {
        continue;
      }
      std::memcpy(&dst[(size_t(y) * width_elems + x) * elem_bytes],
                  &src[src_off], elem_bytes);
    }
  }
  return dst;
}

// Xenos texture formats we handle.
constexpr uint32_t kFmt_8_8_8_8 = 6;
constexpr uint32_t kFmt_4_4_4_4 = 15;
constexpr uint32_t kFmt_DXT1 = 18;
constexpr uint32_t kFmt_DXT2_3 = 19;
constexpr uint32_t kFmt_DXT4_5 = 20;

uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void DecodeColor565(uint16_t c, uint8_t out[4]) {
  out[0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
  out[2] = uint8_t((c & 0x1F) * 255 / 31);
  out[3] = 255;
}

// BC1 color portion (used by BC1/BC2/BC3). |opaque_mode| selects whether the
// c0<=c1 punch-through path is possible (BC1 only).
void DecodeColorBlock(const uint8_t* block, uint8_t colors[4][4],
                      bool opaque_mode) {
  const uint16_t c0 = ReadU16(block);
  const uint16_t c1 = ReadU16(block + 2);
  DecodeColor565(c0, colors[0]);
  DecodeColor565(c1, colors[1]);
  if (opaque_mode || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
      colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
    }
    colors[2][3] = 255;
    colors[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) {
      colors[2][k] = uint8_t((colors[0][k] + colors[1][k]) / 2);
      colors[3][k] = 0;
    }
    colors[2][3] = 255;
    colors[3][3] = 0;
  }
}

void WriteBlockPixel(DecodedTexture& out, uint32_t x, uint32_t y,
                     const uint8_t rgba[4]) {
  if (x >= out.width || y >= out.height) return;
  std::memcpy(&out.rgba[(size_t(y) * out.width + x) * 4], rgba, 4);
}

void DecodeBcBlock(const uint8_t* block, uint32_t fmt, DecodedTexture& out,
                   uint32_t bx, uint32_t by) {
  uint8_t alpha[16];
  const uint8_t* color_block = block;
  if (fmt == kFmt_DXT2_3) {
    for (int i = 0; i < 16; ++i) {
      const uint8_t pair = block[i / 2];
      const uint8_t a4 = (i & 1) ? (pair >> 4) : (pair & 0xF);
      alpha[i] = uint8_t(a4 * 17);
    }
    color_block = block + 8;
  } else if (fmt == kFmt_DXT4_5) {
    const uint8_t a0 = block[0], a1 = block[1];
    uint8_t table[8];
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
      for (int k = 1; k < 7; ++k) {
        table[k + 1] = uint8_t(((7 - k) * a0 + k * a1) / 7);
      }
    } else {
      for (int k = 1; k < 5; ++k) {
        table[k + 1] = uint8_t(((5 - k) * a0 + k * a1) / 5);
      }
      table[6] = 0;
      table[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
      bits |= uint64_t(block[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
      alpha[i] = table[(bits >> (3 * i)) & 0x7];
    }
    color_block = block + 8;
  } else {
    std::memset(alpha, 0xFF, sizeof(alpha));
  }

  uint8_t colors[4][4];
  DecodeColorBlock(color_block, colors, fmt != kFmt_DXT1);
  const uint32_t indices = uint32_t(color_block[4]) |
                           (uint32_t(color_block[5]) << 8) |
                           (uint32_t(color_block[6]) << 16) |
                           (uint32_t(color_block[7]) << 24);
  for (uint32_t py = 0; py < 4; ++py) {
    for (uint32_t px = 0; px < 4; ++px) {
      const uint32_t i = py * 4 + px;
      uint8_t rgba[4];
      std::memcpy(rgba, colors[(indices >> (2 * i)) & 0x3], 4);
      if (fmt != kFmt_DXT1) {
        rgba[3] = alpha[i];
      } else if (rgba[3] == 0) {
        rgba[0] = rgba[1] = rgba[2] = 0;
      }
      WriteBlockPixel(out, bx * 4 + px, by * 4 + py, rgba);
    }
  }
}

}  // namespace

// Decodes one 2D surface (a plain texture or one cube face) described by
// |fetch| from raw guest bytes |data| into out.rgba (width*height*4).
bool DecodeFace(const std::string& path, const uint32_t fetch[6],
                std::vector<uint8_t> data, DecodedTexture& out) {
  const uint32_t fmt = fetch[1] & 0x3F;
  const uint32_t endian = (fetch[1] >> 6) & 0x3;
  const uint32_t tiled = fetch[0] >> 31;
  const uint32_t pitch_px = ((fetch[0] >> 22) & 0x1FF) << 5;
  out.width = (fetch[2] & 0x1FFF) + 1;
  out.height = ((fetch[2] >> 13) & 0x1FFF) + 1;

  if (out.width == 0 || out.height == 0 || out.width > 8192 ||
      out.height > 8192 ||
      uint64_t(out.width) * out.height * 4 > 128ull * 1024 * 1024) {
    std::printf("  [tex] implausible %ux%u: %s\n", out.width, out.height,
                path.c_str());
    out.width = out.height = 0;
    return false;
  }
  // Endian pre-pass: 1 = swap within 16-bit words, 2 = within 32-bit words,
  // 3 = swap 16-bit halves within 32-bit words. (Element-local, so safe to
  // apply before untiling.)
  if (endian == 1) {
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (endian == 2) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (endian == 3) {
    for (size_t i = 0; i + 3 < data.size(); i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }

  // Untile into a tightly packed buffer, then decode as linear. Note that
  // dumps made before 2026-08-29 under-read tiled DXT surfaces (32-texel
  // instead of 32-block padding); their missing tail decodes black.
  uint32_t row_px = pitch_px != 0 ? pitch_px : ((out.width + 31u) & ~31u);
  // Packed mip tail (2026-10-02, Zeus' 16x16 paint textures came out purple):
  // with the fetch constant's packed_mips bit (dword 5 bit 11) a texture
  // whose smaller side is <= 16 texels keeps even its base level inside the
  // 32-texel tail block, at (16, 0) — or (0, 16) when wider than tall
  // (xenia texture_util::GetPackedMipOffset, mip 0).
  uint32_t pack_x = 0, pack_y = 0;
  if (((fetch[5] >> 11) & 1) != 0 && std::min(out.width, out.height) <= 16) {
    auto log2_ceil = [](uint32_t v) {
      uint32_t l = 0;
      while ((1u << l) < v) ++l;
      return l;
    };
    if (log2_ceil(out.width) > log2_ceil(out.height)) {
      pack_y = 16;
    } else {
      pack_x = 16;
    }
  }
  const uint32_t span_w = out.width + pack_x;   // texels to reach the level
  const uint32_t span_h = out.height + pack_y;
  if (row_px < span_w) row_px = (span_w + 31u) & ~31u;
  if (tiled) {
    if (fmt == kFmt_DXT1 || fmt == kFmt_DXT2_3 || fmt == kFmt_DXT4_5) {
      const uint32_t block_bytes = fmt == kFmt_DXT1 ? 8 : 16;
      data = UntileSurface(data, (span_w + 3) / 4, (span_h + 3) / 4,
                           row_px / 4, block_bytes);
    } else if (fmt == kFmt_8_8_8_8) {
      data = UntileSurface(data, span_w, span_h, row_px, 4);
    } else if (fmt == kFmt_4_4_4_4) {
      data = UntileSurface(data, span_w, span_h, row_px, 2);
    } else {
      std::printf("  [tex] tiled format %u unsupported yet: %s\n", fmt,
                  path.c_str());
      out.width = out.height = 0;
      return false;
    }
    row_px = span_w;  // Tightly packed now.
  }

  out.rgba.assign(size_t(out.width) * out.height * 4, 0);

  if (fmt == kFmt_DXT1 || fmt == kFmt_DXT2_3 || fmt == kFmt_DXT4_5) {
    const uint32_t block_bytes = fmt == kFmt_DXT1 ? 8 : 16;
    const uint32_t blocks_per_row = (row_px + 3) / 4;
    const uint32_t block_rows = (out.height + 3) / 4;
    const uint32_t used_blocks_x = (out.width + 3) / 4;
    for (uint32_t by = 0; by < block_rows; ++by) {
      for (uint32_t bx = 0; bx < used_blocks_x; ++bx) {
        const size_t off = (size_t(by + pack_y / 4) * blocks_per_row + bx +
                            pack_x / 4) * block_bytes;
        if (off + block_bytes > data.size()) break;
        DecodeBcBlock(data.data() + off, fmt, out, bx, by);
      }
    }
    return true;
  }
  if (fmt == kFmt_8_8_8_8) {
    for (uint32_t y = 0; y < out.height; ++y) {
      for (uint32_t x = 0; x < out.width; ++x) {
        const size_t off = (size_t(y + pack_y) * row_px + x + pack_x) * 4;
        if (off + 4 > data.size()) break;
        uint32_t v;
        std::memcpy(&v, &data[off], 4);
        // D3DCOLOR ARGB after the endian pre-pass.
        uint8_t* px = &out.rgba[(size_t(y) * out.width + x) * 4];
        px[0] = uint8_t(v >> 16);
        px[1] = uint8_t(v >> 8);
        px[2] = uint8_t(v);
        px[3] = uint8_t(v >> 24);
      }
    }
    return true;
  }

  if (fmt == kFmt_4_4_4_4) {
    // ARGB nibbles after the endian pre-pass (matches the 8888 D3DCOLOR
    // ordering).
    for (uint32_t y = 0; y < out.height; ++y) {
      for (uint32_t x = 0; x < out.width; ++x) {
        const size_t off = (size_t(y + pack_y) * row_px + x + pack_x) * 2;
        if (off + 2 > data.size()) break;
        const uint16_t v = uint16_t(data[off] | (data[off + 1] << 8));
        uint8_t* px = &out.rgba[(size_t(y) * out.width + x) * 4];
        px[0] = uint8_t(((v >> 8) & 0xF) * 17);
        px[1] = uint8_t(((v >> 4) & 0xF) * 17);
        px[2] = uint8_t((v & 0xF) * 17);
        px[3] = uint8_t(((v >> 12) & 0xF) * 17);
      }
    }
    return true;
  }

  std::printf("  [tex] format %u unsupported yet: %s\n", fmt, path.c_str());
  out.width = out.height = 0;
  return false;
}

bool LoadXtx(const std::string& path, DecodedTexture& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint32_t magic = 0, version = 0, fetch[6] = {}, data_bytes = 0;
  bool header_ok = std::fread(&magic, 4, 1, f) == 1 &&
                   std::fread(&version, 4, 1, f) == 1 &&
                   std::fread(fetch, 4, 6, f) == 6 &&
                   std::fread(&data_bytes, 4, 1, f) == 1;
  if (!header_ok || magic != 0x52535458u || version != 1 ||
      data_bytes > 64u * 1024 * 1024) {
    std::fclose(f);
    std::printf("  [tex] bad header: %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> data(data_bytes);
  const bool data_ok = std::fread(data.data(), 1, data_bytes, f) == data_bytes;
  std::fclose(f);
  if (!data_ok) {
    std::printf("  [tex] truncated: %s\n", path.c_str());
    return false;
  }
  return DecodeGuestTexture(path, fetch, std::move(data), out);
}

bool DecodeGuestTexture(const std::string& path, const uint32_t fetch[6],
                        std::vector<uint8_t> data, DecodedTexture& out) {
  const uint32_t dimension = (fetch[5] >> 9) & 0x3;  // 3 = cube
  if (dimension != 3) {
    out.layers = 1;
    return DecodeFace(path, fetch, std::move(data), out);
  }
  // Cube dump: 6 faces at a 4 KB-aligned face stride (see the game-side
  // dumper). Decode each face as a 2D surface and stack the RGBA planes.
  const uint32_t fmt = fetch[1] & 0x3F;
  const uint32_t tiled = fetch[0] >> 31;
  const uint32_t pitch_px = ((fetch[0] >> 22) & 0x1FF) << 5;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t row_px = pitch_px != 0 ? pitch_px : ((width + 31u) & ~31u);
  const uint32_t padded_h = (height + 31u) & ~31u;
  uint64_t face_bytes;
  if (tiled && (fmt == kFmt_DXT1 || fmt == kFmt_DXT2_3 || fmt == kFmt_DXT4_5)) {
    const uint32_t block_bytes = fmt == kFmt_DXT1 ? 8u : 16u;
    const uint32_t pw_blocks =
        (((row_px > width ? row_px : width) + 3u) / 4u + 31u) & ~31u;
    const uint32_t ph_blocks = ((height + 3u) / 4u + 31u) & ~31u;
    face_bytes = uint64_t(pw_blocks) * ph_blocks * block_bytes;
  } else {
    uint32_t bpp = 0;
    switch (fmt) {
      case kFmt_8_8_8_8: bpp = 32; break;
      case kFmt_4_4_4_4: bpp = 16; break;
      case kFmt_DXT1: bpp = 4; break;
      case kFmt_DXT2_3: case kFmt_DXT4_5: bpp = 8; break;
      default: break;
    }
    face_bytes = uint64_t(row_px) * padded_h * bpp / 8;
  }
  const uint64_t face_stride = (face_bytes + 4095ull) & ~4095ull;
  std::vector<uint8_t> all;
  for (uint32_t f = 0; f < 6; ++f) {
    const uint64_t off = f * face_stride;
    if (off + face_bytes > data.size()) {
      std::printf("  [tex] cube face %u beyond dump (%llu B): %s\n", f,
                  static_cast<unsigned long long>(data.size()), path.c_str());
      out.width = out.height = 0;
      return false;
    }
    std::vector<uint8_t> face(data.begin() + off, data.begin() + off + face_bytes);
    DecodedTexture one;
    if (!DecodeFace(path, fetch, std::move(face), one)) {
      out.width = out.height = 0;
      return false;
    }
    if (f == 0) {
      out.width = one.width;
      out.height = one.height;
      all.reserve(one.rgba.size() * 6);
    }
    all.insert(all.end(), one.rgba.begin(), one.rgba.end());
  }
  out.layers = 6;
  out.rgba = std::move(all);
  return true;
}

}  // namespace rs::capture
