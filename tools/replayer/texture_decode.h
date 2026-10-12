// Decodes rs_textures/*.xtx dumps (RSTX v1: fetch constant + raw guest
// bytes) into RGBA8 for the software replayer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rs::capture {

struct DecodedTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 1;        // 6 for cube maps (+X -X +Y -Y +Z -Z)
  std::vector<uint8_t> rgba;  // layers*width*height*4, row-major per layer
};

// Returns false (with a printed reason) for unsupported formats.
bool LoadXtx(const std::string& path, DecodedTexture& out);

// Decodes raw guest texture memory described by its fetch constant (2D or
// cube; the cube faces are expected at the 4 KB-aligned face stride the
// game-side dumper uses, which mirrors the hardware layout). |label| is only
// used in messages. This is what the in-process path feeds from guest
// memory instead of a dump file (2026-09-06).
bool DecodeGuestTexture(const std::string& label, const uint32_t fetch[6],
                        std::vector<uint8_t> data, DecodedTexture& out);

}  // namespace rs::capture
