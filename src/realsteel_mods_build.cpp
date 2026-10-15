// realsteel - mod robots built by the game from their source files. See the header.
//
// Check against the Python reference: start the game with the environment
// variable REALSTEEL_MODS_DUMP=<folder>; every part file made here is also
// written there as <robot>/ed<slot>_<kind>_<item>.bin, the names
// robot_folder.py build uses, so a binary compare with its output works.

#include "realsteel_mods_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

#include <rex/logging.h>

namespace realsteel::mods {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct BuildError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

Bytes read_all(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw BuildError("cannot read " + path.filename().string());
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void app_be32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }
void app_be16(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 8), uint8_t(v)}); }
void app_bef(Bytes& b, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  app_be32(b, u);
}
float get_bef(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void put_bef(uint8_t* p, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put_be32(p, u);
}
float get_lef(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);   // little-endian hosts only (all our targets)
  return f;
}
void put_lef(uint8_t* p, double v) {
  const float f = float(v);
  std::memcpy(p, &f, 4);
}

// --- inflate (zlib streams in PNGs) -------------------------------------------

class Inflate {
 public:
  Inflate(const uint8_t* data, size_t size) : in_(data), size_(size) {}

  Bytes Run() {
    if (size_ < 2) throw BuildError("damaged PNG data");
    pos_ = 2;   // zlib header (CMF, FLG); the adler-32 at the end is not checked
    int last = 0;
    while (!last) {
      last = Bits(1);
      const int type = Bits(2);
      if (type == 0) Stored();
      else if (type == 1) Fixed();
      else if (type == 2) Dynamic();
      else throw BuildError("damaged PNG data");
    }
    return std::move(out_);
  }

 private:
  struct Huffman {
    std::array<int16_t, 16> count{};
    std::vector<int16_t> symbol;
  };

  int Bits(int need) {
    uint64_t val = bitbuf_;
    while (bitcnt_ < need) {
      if (pos_ >= size_) throw BuildError("damaged PNG data");
      val |= uint64_t(in_[pos_++]) << bitcnt_;
      bitcnt_ += 8;
    }
    bitbuf_ = val >> need;
    bitcnt_ -= need;
    return int(val & ((1ull << need) - 1));
  }

  void Stored() {
    bitbuf_ = 0;
    bitcnt_ = 0;
    if (pos_ + 4 > size_) throw BuildError("damaged PNG data");
    const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
    pos_ += 4;
    if (pos_ + len > size_) throw BuildError("damaged PNG data");
    out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
    pos_ += len;
  }

  static void Build(Huffman& h, const int16_t* lengths, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
    std::array<int16_t, 16> offs{};
    for (int len = 1; len < 15; ++len) offs[len + 1] = int16_t(offs[len] + h.count[len]);
    h.symbol.assign(n, 0);
    for (int s = 0; s < n; ++s)
      if (lengths[s] != 0) h.symbol[offs[lengths[s]]++] = int16_t(s);
  }

  int Decode(const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      code |= Bits(1);
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    throw BuildError("damaged PNG data");
  }

  void Codes(const Huffman& lencode, const Huffman& distcode) {
    static const int16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                         35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                          3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                          6145, 8193, 12289, 16385, 24577};
    static const int16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                           7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int symbol = Decode(lencode);
      if (symbol < 256) {
        out_.push_back(uint8_t(symbol));
      } else if (symbol == 256) {
        return;
      } else {
        symbol -= 257;
        if (symbol >= 29) throw BuildError("damaged PNG data");
        const int len = kLenBase[symbol] + Bits(kLenExtra[symbol]);
        const int dsym = Decode(distcode);
        if (dsym >= 30) throw BuildError("damaged PNG data");
        const size_t dist = size_t(kDistBase[dsym]) + size_t(Bits(kDistExtra[dsym]));
        if (dist > out_.size()) throw BuildError("damaged PNG data");
        const size_t from = out_.size() - dist;
        for (int i = 0; i < len; ++i) out_.push_back(out_[from + i]);
      }
    }
  }

  void Fixed() {
    if (!fixed_built_) {
      int16_t lengths[288 + 30];
      int s = 0;
      for (; s < 144; ++s) lengths[s] = 8;
      for (; s < 256; ++s) lengths[s] = 9;
      for (; s < 280; ++s) lengths[s] = 7;
      for (; s < 288; ++s) lengths[s] = 8;
      Build(fixed_len_, lengths, 288);
      for (s = 0; s < 30; ++s) lengths[s] = 5;
      Build(fixed_dist_, lengths, 30);
      fixed_built_ = true;
    }
    Codes(fixed_len_, fixed_dist_);
  }

  void Dynamic() {
    static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
    if (nlen > 286 || ndist > 30) throw BuildError("damaged PNG data");
    int16_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = int16_t(Bits(3));
    Huffman lencode, distcode;
    Build(lencode, lengths, 19);
    int index = 0;
    while (index < nlen + ndist) {
      int symbol = Decode(lencode);
      if (symbol < 16) {
        lengths[index++] = int16_t(symbol);
      } else {
        int len = 0, repeat;
        if (symbol == 16) {
          if (index == 0) throw BuildError("damaged PNG data");
          len = lengths[index - 1];
          repeat = 3 + Bits(2);
        } else if (symbol == 17) {
          repeat = 3 + Bits(3);
        } else {
          repeat = 11 + Bits(7);
        }
        if (index + repeat > nlen + ndist) throw BuildError("damaged PNG data");
        while (repeat--) lengths[index++] = int16_t(len);
      }
    }
    Build(lencode, lengths, nlen);
    Build(distcode, lengths + nlen, ndist);
    Codes(lencode, distcode);
  }

  const uint8_t* in_;
  size_t size_, pos_ = 0;
  uint64_t bitbuf_ = 0;
  int bitcnt_ = 0;
  Bytes out_;
  Huffman fixed_len_, fixed_dist_;
  bool fixed_built_ = false;
};

// --- PNG (8-bit, not interlaced) -> RGB, as pngio.read_png -----------------------

struct Image {
  int w = 0, h = 0;
  Bytes rgb;   // h * w * 3, top row first
  const uint8_t* at(int x, int y) const { return &rgb[(size_t(y) * w + x) * 3]; }
};

Image read_png(const fs::path& path) {
  const Bytes d = read_all(path);
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (d.size() < 8 || std::memcmp(d.data(), kSig, 8) != 0) throw BuildError(path.filename().string() + " is not a PNG file");
  size_t pos = 8;
  int w = 0, h = 0, ctype = -1;
  Bytes idat, palette;
  while (pos + 8 <= d.size()) {
    const uint32_t n = be32(&d[pos]);
    const char* kind = reinterpret_cast<const char*>(&d[pos + 4]);
    if (pos + 12 + size_t(n) > d.size()) throw BuildError(path.filename().string() + " is damaged");
    const uint8_t* body = &d[pos + 8];
    if (std::memcmp(kind, "IHDR", 4) == 0) {
      w = int(be32(body));
      h = int(be32(body + 4));
      const int depth = body[8], interlace = body[12];
      ctype = body[9];
      if (depth != 8 || interlace != 0 || !(ctype == 0 || ctype == 2 || ctype == 3 || ctype == 4 || ctype == 6))
        throw BuildError(path.filename().string() + ": only 8-bit, non-interlaced PNGs are supported");
    } else if (std::memcmp(kind, "PLTE", 4) == 0) {
      palette.assign(body, body + n);
    } else if (std::memcmp(kind, "IDAT", 4) == 0) {
      idat.insert(idat.end(), body, body + n);
    }
    pos += 12 + n;
  }
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) throw BuildError(path.filename().string() + " has no usable picture");
  const int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
  const Bytes raw = Inflate(idat.data(), idat.size()).Run();
  const size_t stride = size_t(w) * ch;
  if (raw.size() < (stride + 1) * h) throw BuildError(path.filename().string() + " is damaged");
  Bytes px(stride * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t f = raw[y * (stride + 1)];
    const uint8_t* line = &raw[y * (stride + 1) + 1];
    uint8_t* cur = &px[y * stride];
    const uint8_t* prev = y > 0 ? &px[(y - 1) * stride] : nullptr;
    for (size_t i = 0; i < stride; ++i) {
      const int a = i >= size_t(ch) ? cur[i - ch] : 0;
      const int b = prev ? prev[i] : 0;
      const int c = (prev && i >= size_t(ch)) ? prev[i - ch] : 0;
      int pred = 0;
      switch (f) {
        case 0: pred = 0; break;
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) >> 1; break;
        case 4: {
          const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
          pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: throw BuildError(path.filename().string() + " is damaged");
      }
      cur[i] = uint8_t(line[i] + pred);
    }
  }
  Image img;
  img.w = w;
  img.h = h;
  img.rgb.resize(size_t(w) * h * 3);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    uint8_t* o = &img.rgb[i * 3];
    const uint8_t* s = &px[i * ch];
    if (ctype == 3) {
      const size_t k = size_t(s[0]) * 3;
      if (k + 3 > palette.size()) throw BuildError(path.filename().string() + " has a bad palette");
      o[0] = palette[k];
      o[1] = palette[k + 1];
      o[2] = palette[k + 2];
    } else if (ch <= 2) {
      o[0] = o[1] = o[2] = s[0];
    } else {
      o[0] = s[0];
      o[1] = s[1];
      o[2] = s[2];
    }
  }
  return img;
}

Image resize_nearest(const Image& img, int w, int h) {
  Image out;
  out.w = w;
  out.h = h;
  out.rgb.resize(size_t(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    const int sy = int(int64_t(y) * img.h / h);
    for (int x = 0; x < w; ++x) {
      const int sx = int(int64_t(x) * img.w / w);
      std::memcpy(&out.rgb[(size_t(y) * w + x) * 3], img.at(sx, sy), 3);
    }
  }
  return out;
}

// pngio.resize: averages down by whole multiples (rounding half to even, as
// numpy's rint), otherwise nearest.
Image resize(const Image& img, int w, int h) {
  if (img.w == w && img.h == h) return img;
  if (img.w % w != 0 || img.h % h != 0) return resize_nearest(img, w, h);
  const int fx = img.w / w, fy = img.h / h;
  Image out;
  out.w = w;
  out.h = h;
  out.rgb.resize(size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) {
        int sum = 0;
        for (int j = 0; j < fy; ++j)
          for (int i = 0; i < fx; ++i) sum += img.at(x * fx + i, y * fy + j)[c];
        const float mean = float(sum) / float(fx * fy);
        out.rgb[(size_t(y) * w + x) * 3 + c] = uint8_t(std::nearbyint(mean));
      }
  return out;
}

// pngio.quantize: up to 256 colours as they are; more are binned at 5 bits a
// channel, the 256 fullest bins become the palette (ties: lower bin first)
// and every bin takes its nearest palette colour.
void quantize(const Image& img, std::vector<std::array<uint8_t, 3>>& palette, Bytes& index) {
  const size_t n = size_t(img.w) * img.h;
  std::vector<uint32_t> keys(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = &img.rgb[i * 3];
    keys[i] = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  }
  std::vector<uint32_t> uniq = keys;
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
  index.resize(n);
  palette.clear();
  if (uniq.size() <= 256) {
    for (uint32_t k : uniq) palette.push_back({uint8_t(k >> 16), uint8_t(k >> 8), uint8_t(k)});
    for (size_t i = 0; i < n; ++i)
      index[i] = uint8_t(std::lower_bound(uniq.begin(), uniq.end(), keys[i]) - uniq.begin());
    return;
  }
  std::vector<uint32_t> coarse(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = &img.rgb[i * 3];
    coarse[i] = (uint32_t(p[0] >> 3) << 10) | (uint32_t(p[1] >> 3) << 5) | uint32_t(p[2] >> 3);
  }
  std::vector<uint32_t> bins = coarse;
  std::sort(bins.begin(), bins.end());
  bins.erase(std::unique(bins.begin(), bins.end()), bins.end());
  std::vector<int64_t> counts(bins.size(), 0);
  std::vector<std::array<double, 3>> sums(bins.size(), {0.0, 0.0, 0.0});
  std::vector<uint32_t> binv(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t b = uint32_t(std::lower_bound(bins.begin(), bins.end(), coarse[i]) - bins.begin());
    binv[i] = b;
    counts[b]++;
    const uint8_t* p = &img.rgb[i * 3];
    for (int c = 0; c < 3; ++c) sums[b][c] += p[c];
  }
  std::vector<uint32_t> order(bins.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return counts[a] > counts[b]; });
  const size_t colours = std::min<size_t>(256, order.size());
  std::vector<std::array<int, 3>> pal(colours);
  for (size_t k = 0; k < colours; ++k)
    for (int c = 0; c < 3; ++c) pal[k][c] = int(std::nearbyint(sums[order[k]][c] / double(counts[order[k]])));
  std::vector<uint8_t> nearest(bins.size());
  for (size_t b = 0; b < bins.size(); ++b) {
    double centre[3];
    for (int c = 0; c < 3; ++c) centre[c] = sums[b][c] / double(counts[b]);
    double best = 0.0;
    size_t best_k = 0;
    for (size_t k = 0; k < colours; ++k) {
      double dist = 0.0;
      for (int c = 0; c < 3; ++c) {
        const double d = centre[c] - double(pal[k][c]);
        dist += d * d;
      }
      if (k == 0 || dist < best) {
        best = dist;
        best_k = k;
      }
    }
    nearest[b] = uint8_t(best_k);
  }
  for (const auto& p : pal) palette.push_back({uint8_t(p[0]), uint8_t(p[1]), uint8_t(p[2])});
  for (size_t i = 0; i < n; ++i) index[i] = nearest[binv[i]];
}

// A TGA like the template (the stock texture) but w x h (robot_folder.tga).
Bytes tga(const Bytes& tmpl, int w, int h, const Bytes& pixels, const Bytes* palette) {
  if (tmpl.size() < 18) throw BuildError("damaged base texture");
  Bytes out(tmpl.begin(), tmpl.begin() + 18);
  out[12] = uint8_t(w);
  out[13] = uint8_t(w >> 8);
  out[14] = uint8_t(h);
  out[15] = uint8_t(h >> 8);
  if (palette != nullptr) {
    out.insert(out.end(), palette->begin(), palette->end());
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
  }
  const size_t old_w = tmpl[12] | (tmpl[13] << 8), old_h = tmpl[14] | (tmpl[15] << 8);
  out.insert(out.end(), pixels.begin(), pixels.end());
  const size_t tail = 18 + old_w * old_h * 4;
  if (tail < tmpl.size()) out.insert(out.end(), tmpl.begin() + tail, tmpl.end());
  return out;
}

// --- the model format (JBOY), as jboy.py / jboy_write.py ------------------------

constexpr size_t kHeaderSize = 0x40, kMeshSize = 0xB4;

struct JMesh {
  size_t offset = 0;   // in the file
  std::vector<uint32_t> bones;
  uint32_t weights = 0, param_count = 0, param_table = 0, vertex_block = 0, group = 0;
};

struct JModel {
  const Bytes* data = nullptr;
  uint32_t bone_count = 0, texture_count = 0, bone_table = 0, texture_table = 0, group_table = 0, group_count = 0;
  std::vector<JMesh> meshes;
};

JModel parse_model(const Bytes& d) {
  if (d.size() < 8 + kHeaderSize || std::memcmp(d.data(), "JBOY", 4) != 0) throw BuildError("damaged base model");
  JModel m;
  m.data = &d;
  auto h = [&](int k) { return be32(&d[4 + k * 4]); };
  const uint32_t mesh_count = h(5) & 0x7FFFFFFF, mesh_table = h(6);
  m.bone_count = h(7);
  m.texture_count = h(8);
  m.bone_table = h(9);
  m.texture_table = h(10);
  m.group_table = h(11);
  m.group_count = h(12);
  for (uint32_t i = 0; i < mesh_count; ++i) {
    JMesh mesh;
    mesh.offset = 8 + mesh_table + size_t(i) * kMeshSize;
    if (mesh.offset + kMeshSize > d.size()) throw BuildError("damaged base model");
    const uint8_t* o = &d[mesh.offset];
    for (int b = 0; b < 20; ++b) {
      const uint32_t bone = be32(o + 0x0C + b * 4);
      if (bone != 0xFFFFFFFF) mesh.bones.push_back(bone);
    }
    mesh.weights = be32(o + 0x5C);
    mesh.vertex_block = be32(o + 0x68);
    mesh.group = be32(o + 0x74);
    mesh.param_count = be32(o + 0x90);
    mesh.param_table = be32(o + 0x94);
    m.meshes.push_back(std::move(mesh));
  }
  return m;
}

struct MeshSpec {
  const JMesh* tmpl = nullptr;
  std::vector<uint32_t> bones;
  uint32_t weights = 1;
  std::vector<std::array<double, 6>> vertices;
  std::vector<std::vector<std::pair<uint32_t, double>>> vertex_weights;
  std::vector<std::array<double, 2>> uvs;
  std::vector<uint16_t> strip;
};

Bytes encode_pof0(std::vector<uint32_t> positions) {
  std::sort(positions.begin(), positions.end());
  Bytes body;
  uint32_t last = 0;
  for (uint32_t pos : positions) {
    const uint32_t delta = (pos - last) / 4;
    if (delta < 0x40) {
      body.push_back(uint8_t(0x40 | delta));
    } else if (delta < 0x4000) {
      body.push_back(uint8_t(0x80 | (delta >> 8)));
      body.push_back(uint8_t(delta & 0xFF));
    } else {
      body.push_back(uint8_t(0xC0 | (delta >> 24)));
      body.push_back(uint8_t((delta >> 16) & 0xFF));
      body.push_back(uint8_t((delta >> 8) & 0xFF));
      body.push_back(uint8_t(delta & 0xFF));
    }
    last = pos;
  }
  while (body.size() % 4) body.push_back(0);
  Bytes out = {'P', 'O', 'F', '0'};
  app_be32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

std::array<double, 4> sphere_of(const std::vector<std::array<double, 6>>& vertices) {
  if (vertices.empty()) return {0.0, 0.0, 0.0, 0.05};
  double lo[3], hi[3], centre[3];
  for (int k = 0; k < 3; ++k) {
    lo[k] = hi[k] = vertices[0][k];
    for (const auto& v : vertices) {
      lo[k] = std::min(lo[k], v[k]);
      hi[k] = std::max(hi[k], v[k]);
    }
    centre[k] = (lo[k] + hi[k]) / 2;
  }
  double radius = 0.0;
  bool first = true;
  for (const auto& v : vertices) {
    double s = 0.0;
    for (int k = 0; k < 3; ++k) s = s + std::pow(v[k] - centre[k], 2.0);
    const double r = std::pow(s, 0.5);
    if (first || r > radius) radius = r;
    first = false;
  }
  return {centre[0], centre[1], centre[2], radius};
}

Bytes write_model(const JModel& t, const std::vector<MeshSpec>& specs, const Bytes& bone_table) {
  const Bytes& td = *t.data;
  Bytes out(kHeaderSize + kMeshSize * specs.size(), 0);
  std::vector<uint32_t> pointers = {0x14, 0x20, 0x24, 0x28};
  std::memcpy(out.data(), &td[8], kHeaderSize);
  put_be32(&out[0x10], 0x80000000u | uint32_t(specs.size()));
  put_be32(&out[0x14], uint32_t(kHeaderSize));
  for (size_t i = 0; i < specs.size(); ++i) {
    const MeshSpec& spec = specs[i];
    const JMesh& tm = *spec.tmpl;
    const size_t mo = kHeaderSize + i * kMeshSize;
    std::memcpy(&out[mo], &td[tm.offset], kMeshSize);
    const size_t n = spec.vertices.size();
    put_be32(&out[mo], uint32_t(n));
    put_be32(&out[mo + 4], 1);
    put_be32(&out[mo + 8], uint32_t(spec.bones.size()));
    for (int b = 0; b < 20; ++b) put_be32(&out[mo + 0x0C + b * 4], b < int(spec.bones.size()) ? spec.bones[b] : 0xFFFFFFFFu);
    put_be32(&out[mo + 0x5C], spec.weights);
    put_be32(&out[mo + 0x74], tm.group);
    // shader parameters: the template's block, pointers moved
    const uint32_t param_table = uint32_t(out.size());
    if (8 + size_t(tm.vertex_block) > td.size() || tm.vertex_block < tm.param_table) throw BuildError("damaged base model");
    out.insert(out.end(), td.begin() + 8 + tm.param_table, td.begin() + 8 + tm.vertex_block);
    const int64_t delta = int64_t(param_table) - int64_t(tm.param_table);
    for (uint32_t k = 0; k < tm.param_count; ++k) {
      const uint32_t p = uint32_t(int64_t(be32(&td[8 + tm.param_table + k * 4])) + delta);
      put_be32(&out[param_table + k * 4], p);
      pointers.push_back(param_table + k * 4);
    }
    put_be32(&out[mo + 0x94], param_table);
    const uint32_t vertex_block = uint32_t(out.size());
    app_be32(out, vertex_block + 4);
    pointers.push_back(vertex_block);
    for (const auto& v : spec.vertices) {
      for (int k = 0; k < 6; ++k) app_bef(out, v[k]);
      app_be32(out, 0xFFFFFFFFu);
    }
    put_be32(&out[mo + 0x68], vertex_block);
    const uint32_t weight_block = uint32_t(out.size());
    for (const auto& entry : spec.vertex_weights)
      for (const auto& [slot, weight] : entry) {
        app_be32(out, slot);
        app_bef(out, weight);
      }
    put_be32(&out[mo + 0x6C], weight_block);
    const uint32_t uv_block = uint32_t(out.size());
    for (const auto& uv : spec.uvs) {
      app_bef(out, uv[0]);
      app_bef(out, uv[1]);
    }
    put_be32(&out[mo + 0x70], uv_block);
    const uint32_t strip_table = uint32_t(out.size());
    app_be32(out, 6);
    app_be32(out, uint32_t(spec.strip.size()));
    app_be32(out, strip_table + 12);
    pointers.push_back(strip_table + 8);
    for (uint16_t s : spec.strip) app_be16(out, s);
    while (out.size() % 4) out.push_back(0);
    put_be32(&out[mo + 0x98], strip_table);
    put_be32(&out[mo + 0x9C], uint32_t(n));
    const auto sphere = sphere_of(spec.vertices);
    for (int k = 0; k < 4; ++k) put_bef(&out[mo + 0xA4 + k * 4], sphere[k]);
    for (uint32_t off : {0x68u, 0x6Cu, 0x70u, 0x94u, 0x98u}) pointers.push_back(uint32_t(mo) + off);
  }
  while (out.size() % 16) out.push_back(0);
  const uint32_t bone_off = uint32_t(out.size());
  out.insert(out.end(), bone_table.begin(), bone_table.end());
  put_be32(&out[0x20], bone_off);
  const uint32_t texture_table = uint32_t(out.size());
  out.insert(out.end(), td.begin() + 8 + t.texture_table, td.begin() + 8 + t.texture_table + 16 * size_t(t.texture_count));
  put_be32(&out[0x24], texture_table);
  const uint32_t group_table = uint32_t(out.size());
  Bytes groups(td.begin() + 8 + t.group_table, td.begin() + 8 + t.group_table + 32 * size_t(t.group_count));
  if (t.group_count == 1) {
    put_be32(&groups[20], 0);
    put_be32(&groups[24], uint32_t(specs.size()));
  }
  out.insert(out.end(), groups.begin(), groups.end());
  put_be32(&out[0x28], group_table);
  const uint32_t fixups = uint32_t(out.size());
  const Bytes pof = encode_pof0(pointers);
  out.insert(out.end(), pof.begin(), pof.end());
  put_be32(&out[0x04], fixups);
  Bytes data(td.begin(), td.begin() + 8);
  data.insert(data.end(), out.begin(), out.end());
  put_be32(&data[4], fixups);
  put_be32(&data[0x0C], fixups);
  return data;
}

// --- the folder's models (robot_folder.read_obj / read_weights) -------------------

struct PartDef {
  const char* name;
  int slot;
  std::vector<std::pair<const char*, uint32_t>> groups;   // group -> bone
  std::array<double, 4> rect;                              // where its texture sits (comet_robot.SLOTS)
};

const std::vector<PartDef>& part_defs() {
  static const std::vector<PartDef> kParts = {
      {"body", 1, {{"chest", 3}, {"abdomen", 2}, {"hips", 1}}, {0.0, 0.5, 1.0, 0.5}},
      {"legs", 2, {{"thigh_l", 26}, {"shin_l", 27}, {"foot_l", 28}, {"toe_l", 29},
                   {"thigh_r", 30}, {"shin_r", 31}, {"foot_r", 32}, {"toe_r", 33}}, {0.0, 0.5, 1.0, 0.5}},
      {"head", 3, {{"head", 5}}, {0.0, 0.0, 0.5, 0.5}},
      {"arm_l", 4, {{"upper", 11}, {"forearm", 13}}, {0.5, 0.0, 0.5, 0.5}},
      {"arm_r", 5, {{"upper", 19}, {"forearm", 21}}, {0.0, 0.0, 0.5, 0.5}},
      {"fist_l", 6, {{"fist", 14}}, {0.5, 0.0, 0.5, 0.25}},
      {"fist_r", 7, {{"fist", 22}}, {0.5, 0.25, 0.5, 0.25}},
  };
  return kParts;
}

using Weights = std::vector<std::pair<uint32_t, double>>;

struct Corner {
  std::array<double, 3> pos{}, normal{};
  double u = 0.0, v = 0.0;    // v measured from the top of the texture
  int blend = -1;             // index into Part::blends, -1 = rigid
};

struct Face {
  std::array<Corner, 4> c;
  int n = 0;
};

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

std::vector<std::string> split_ws(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(uint8_t(line[i]))) ++i;
    const size_t start = i;
    while (i < line.size() && !std::isspace(uint8_t(line[i]))) ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

double to_double(const std::string& s, bool& ok) {
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  ok = end != s.c_str() && *end == 0;
  return v;
}

}  // namespace

struct RobotSource::Part {
  const PartDef* def = nullptr;
  std::vector<std::pair<uint32_t, std::vector<Face>>> faces;   // by bone, in the file's order
  std::vector<Weights> blends;
  std::map<std::string, std::optional<Image>> images;          // role -> picture (loaded on first use)
};

namespace {

// Lines of a text file (the OBJ / weights files can be large).
std::vector<std::string> read_lines(const fs::path& path) {
  const Bytes data = read_all(path);
  std::vector<std::string> lines;
  size_t start = 0;
  for (size_t i = 0; i <= data.size(); ++i) {
    if (i == data.size() || data[i] == '\n') {
      size_t end = i;
      if (end > start && data[end - 1] == '\r') --end;
      lines.emplace_back(reinterpret_cast<const char*>(data.data()) + start, end - start);
      start = i + 1;
    }
  }
  return lines;
}

}  // namespace

RobotSource::RobotSource(fs::path folder) : folder_(std::move(folder)) {}
RobotSource::~RobotSource() = default;

bool RobotSource::HasSource(const fs::path& folder) { return fs::is_regular_file(folder / "body.obj"); }

std::vector<fs::path> RobotSource::SourceFiles(const fs::path& folder) {
  std::vector<fs::path> files;
  for (const PartDef& def : part_defs()) {
    for (const char* suffix : {".obj", "_weights.txt", "_diffuse.png", "_normal.png", "_gloss.png", "_ao.png", "_glow.png"}) {
      const fs::path p = folder / (std::string(def.name) + suffix);
      if (fs::is_regular_file(p)) files.push_back(p);
    }
  }
  if (fs::is_regular_file(folder / "skeleton_scale.txt")) files.push_back(folder / "skeleton_scale.txt");
  return files;
}

std::vector<std::string> RobotSource::Check() {
  std::vector<std::string> problems;
  parts_.clear();
  skeleton_scale_ = 1.0;
  if (fs::is_regular_file(folder_ / "skeleton_scale.txt")) {
    std::ifstream f(folder_ / "skeleton_scale.txt");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool ok = false;
    std::string trimmed = text;
    while (!trimmed.empty() && std::isspace(uint8_t(trimmed.back()))) trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(uint8_t(trimmed.front()))) trimmed.erase(trimmed.begin());
    const double s = to_double(trimmed, ok);
    if (!ok || !(s > 0.2 && s < 5.0)) problems.push_back("skeleton_scale.txt must hold one number (the robot's size)");
    else skeleton_scale_ = s;
  }
  for (const PartDef& def : part_defs()) {
    auto part = std::make_unique<Part>();
    part->def = &def;
    const std::string name = def.name;
    const fs::path obj = folder_ / (name + ".obj");
    if (!fs::is_regular_file(obj)) {
      problems.push_back(name + ".obj is missing");
      continue;
    }
    std::map<std::string, uint32_t> groups;
    for (const auto& [g, bone] : def.groups) groups[g] = bone;
    std::map<long long, int> blend_of;   // OBJ vertex number -> blends index
    std::vector<std::string> local;      // this file's problems (the first few are kept)
    try {
      const fs::path weights = folder_ / (name + "_weights.txt");
      if (fs::is_regular_file(weights)) {
        int number = 0;
        for (const std::string& raw : read_lines(weights)) {
          ++number;
          const std::vector<std::string> t = split_ws(raw.substr(0, raw.find('#')));
          if (t.empty()) continue;
          bool ok = t.size() >= 3 && (t.size() - 1) % 2 == 0;
          Weights pairs;
          double total = 0.0;
          long long vertex = 0;
          if (ok) {
            char* end = nullptr;
            vertex = std::strtoll(t[0].c_str(), &end, 10);
            ok = *end == 0;
          }
          for (size_t k = 1; ok && k + 1 < t.size(); k += 2) {
            const auto it = groups.find(lower(t[k]));
            const double w = to_double(t[k + 1], ok);
            if (it == groups.end()) ok = false;
            if (ok) {
              total += w;
              if (w > 0) pairs.push_back({it->second, w});
            }
          }
          if (!ok || t.size() > 9 || std::fabs(total - 1.0) > 0.01) {
            local.push_back(name + "_weights.txt line " + std::to_string(number) + ": expected <vertex> <group> <weight> ..., weights summing to 1");
            continue;
          }
          blend_of[vertex] = int(part->blends.size());
          part->blends.push_back(pairs);
        }
      }
      std::vector<std::array<double, 3>> verts, normals;
      std::vector<std::array<double, 2>> uvs;
      std::string group;
      bool have_group = false;
      int number = 0;
      for (const std::string& line : read_lines(obj)) {
        ++number;
        if (local.size() >= 20) break;
        const std::vector<std::string> t = split_ws(line);
        if (t.empty() || t[0][0] == '#') continue;
        bool ok = true;
        if (t[0] == "v" && t.size() >= 4) {
          verts.push_back({to_double(t[1], ok), to_double(t[2], ok), to_double(t[3], ok)});
        } else if (t[0] == "vt" && t.size() >= 3) {
          uvs.push_back({to_double(t[1], ok), to_double(t[2], ok)});
        } else if (t[0] == "vn" && t.size() >= 4) {
          normals.push_back({to_double(t[1], ok), to_double(t[2], ok), to_double(t[3], ok)});
        } else if (t[0] == "g" || t[0] == "o") {
          group = t.size() > 1 ? lower(t[1]) : std::string();
          have_group = t.size() > 1;
          // tolerate exporter decorations such as "chest_Mesh.001"
          std::string best;
          for (const auto& [g, bone] : groups)
            if (group == g || group.rfind(g + "_", 0) == 0 || group.rfind(g + ".", 0) == 0)
              if (g.size() > best.size()) best = g;
          if (!best.empty()) group = best;
        } else if (t[0] == "f") {
          const auto gi = groups.find(group);
          if (!have_group || gi == groups.end()) {
            local.push_back(name + ".obj line " + std::to_string(number) + ": face in group '" + group + "' - not one of this part's groups");
            continue;
          }
          std::vector<Corner> corners;
          std::vector<bool> has_normal;
          bool bad = false;
          for (size_t k = 1; k < t.size() && !bad; ++k) {
            std::string idx[3];
            size_t part_no = 0;
            for (char c : t[k]) {
              if (c == '/') {
                if (++part_no > 2) break;
              } else {
                idx[part_no] += c;
              }
            }
            auto resolve = [](const std::string& s, size_t count) -> long long {
              const long long i = std::strtoll(s.c_str(), nullptr, 10);
              return i > 0 ? i - 1 : (i == 0 ? 0 : (long long)count + i);
            };
            long long vi = std::strtoll(idx[0].c_str(), nullptr, 10);
            vi = vi > 0 ? vi : (long long)verts.size() + 1 + vi;
            if (vi < 1 || vi > (long long)verts.size()) {
              local.push_back(name + ".obj line " + std::to_string(number) + ": bad vertex number");
              bad = true;
              break;
            }
            if (idx[1].empty()) {
              local.push_back(name + ".obj line " + std::to_string(number) + ": face corner without a texture coordinate");
              bad = true;
              break;
            }
            const long long ti = resolve(idx[1], uvs.size());
            if (ti < 0 || ti >= (long long)uvs.size()) {
              local.push_back(name + ".obj line " + std::to_string(number) + ": bad texture coordinate number");
              bad = true;
              break;
            }
            Corner c;
            c.pos = verts[size_t(vi - 1)];
            c.u = uvs[size_t(ti)][0];
            c.v = uvs[size_t(ti)][1];
            bool hn = false;
            if (!idx[2].empty()) {
              const long long ni = resolve(idx[2], normals.size());
              if (ni >= 0 && ni < (long long)normals.size()) {
                c.normal = normals[size_t(ni)];
                hn = true;
              }
            }
            const auto bl = blend_of.find(vi);
            c.blend = bl == blend_of.end() ? -1 : bl->second;
            corners.push_back(c);
            has_normal.push_back(hn);
          }
          if (bad || corners.size() < 3) continue;
          // the face's own normal, for corners without one
          const auto& p0 = corners[0].pos;
          const auto& p1 = corners[1].pos;
          const auto& p2 = corners[2].pos;
          const double a[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
          const double b[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
          double fn[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
          const double len = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
          if (len < 1e-12) continue;   // degenerate face: nothing to draw
          for (double& x : fn) x /= len;
          for (size_t k = 0; k < corners.size(); ++k) {
            Corner& c = corners[k];
            if (!(-0.001 <= c.u && c.u <= 1.001 && -0.001 <= c.v && c.v <= 1.001)) {
              char msg[160];
              std::snprintf(msg, sizeof(msg), ".obj line %d: texture coordinate %.3f, %.3f outside 0..1", number, c.u, c.v);
              local.push_back(name + msg);
            }
            if (!has_normal[k]) c.normal = {fn[0], fn[1], fn[2]};
            c.u = std::min(1.0, std::max(0.0, c.u));
            c.v = 1 - std::min(1.0, std::max(0.0, c.v));
          }
          const uint32_t bone = gi->second;
          auto slot = std::find_if(part->faces.begin(), part->faces.end(), [&](const auto& e) { return e.first == bone; });
          if (slot == part->faces.end()) {
            part->faces.push_back({bone, {}});
            slot = part->faces.end() - 1;
          }
          if (corners.size() <= 4) {
            Face f;
            f.n = int(corners.size());
            for (size_t k = 0; k < corners.size(); ++k) f.c[k] = corners[k];
            slot->second.push_back(f);
          } else {   // n-gon: fan of triangles
            for (size_t k = 1; k + 1 < corners.size(); ++k) {
              Face f;
              f.n = 3;
              f.c[0] = corners[0];
              f.c[1] = corners[k];
              f.c[2] = corners[k + 1];
              slot->second.push_back(f);
            }
          }
        }
        if (!ok) local.push_back(name + ".obj line " + std::to_string(number) + ": not a number");
      }
    } catch (const std::exception& e) {
      local.push_back(e.what());
    }
    for (const auto& [bone, faces] : part->faces) {
      if (faces.size() > 20000) {
        std::string gname;
        for (const auto& [g, b] : def.groups)
          if (b == bone) gname = g;
        local.push_back(name + ".obj group " + gname + " has " + std::to_string(faces.size()) + " faces, the limit is 20000");
      }
    }
    if (part->faces.empty() && local.empty()) local.push_back(name + ".obj has no faces");
    if (!fs::is_regular_file(folder_ / (name + "_diffuse.png"))) local.push_back(name + "_diffuse.png is missing");
    if (local.size() > 20) local.resize(20);
    problems.insert(problems.end(), local.begin(), local.end());
    parts_[def.slot] = std::move(part);
  }
  return problems;
}

namespace {

// The vertex-sharing key of robot_folder.build: position, flipped normal,
// texture coordinate and weights, compared as numbers (so -0.0 == 0.0).
struct VertexKey {
  std::array<double, 8> v;
  Weights w;
  bool operator==(const VertexKey& o) const {
    if (w.size() != o.w.size()) return false;
    for (int i = 0; i < 8; ++i)
      if (!(v[i] == o.v[i])) return false;
    for (size_t i = 0; i < w.size(); ++i)
      if (w[i].first != o.w[i].first || !(w[i].second == o.w[i].second)) return false;
    return true;
  }
};
struct VertexKeyHash {
  size_t operator()(const VertexKey& k) const {
    size_t h = 1469598103934665603ull;
    auto mix = [&](double d) {
      if (d == 0.0) d = 0.0;   // -0.0 hashes as 0.0
      uint64_t u;
      std::memcpy(&u, &d, 8);
      h = (h ^ u) * 1099511628211ull;
    };
    for (double d : k.v) mix(d);
    for (const auto& [b, w] : k.w) {
      h = (h ^ b) * 1099511628211ull;
      mix(w);
    }
    return h;
  }
};

}  // namespace

std::optional<Bytes> RobotSource::Build(int slot, char kind, uint32_t item, const Bytes& template_data) {
  auto it = parts_.find(slot);
  if (it == parts_.end()) return std::nullopt;
  Part& part = *it->second;
  const PartDef& def = *part.def;
  const double bones = skeleton_scale_;

  if (kind == 'M' && item == uint32_t(2000 + slot)) {
    const JModel model = parse_model(template_data);
    std::vector<MeshSpec> specs;
    for (const auto& [bone, all_faces] : part.faces) {
      std::vector<const Face*> rigid, blended;
      for (const Face& f : all_faces) {
        bool any = false;
        for (int k = 0; k < f.n; ++k) any = any || f.c[k].blend >= 0;
        (any ? blended : rigid).push_back(&f);
      }
      for (const std::vector<const Face*>* faces : {&rigid, &blended}) {
        if (faces->empty()) continue;
        std::vector<uint32_t> others;
        uint32_t entries = 1;
        for (const Face* f : *faces)
          for (int k = 0; k < f->n; ++k)
            if (f->c[k].blend >= 0) {
              const Weights& w = part.blends[f->c[k].blend];
              entries = std::max<uint32_t>(entries, uint32_t(w.size()));
              for (const auto& [b, x] : w)
                if (b != bone) others.push_back(b);
            }
        std::sort(others.begin(), others.end());
        others.erase(std::unique(others.begin(), others.end()), others.end());
        std::vector<uint32_t> mesh_bones = {bone};
        mesh_bones.insert(mesh_bones.end(), others.begin(), others.end());
        const JMesh* source = nullptr;
        if (entries == 1) {
          for (const JMesh& m : model.meshes)
            if (!source && m.bones.size() == 1 && m.bones[0] == bone && m.weights == 1) source = &m;
        } else {
          for (const JMesh& m : model.meshes)
            if (!source && m.weights == entries && std::find(m.bones.begin(), m.bones.end(), bone) != m.bones.end()) source = &m;
          for (const JMesh& m : model.meshes)
            if (!source && m.weights == entries) source = &m;
        }
        if (!source) {
          if (model.meshes.empty()) throw BuildError("damaged base model");
          source = &model.meshes[0];
        }
        MeshSpec* spec = nullptr;
        std::unordered_map<VertexKey, uint32_t, VertexKeyHash> index;
        for (const Face* f : *faces) {
          std::array<Corner, 4> corners;
          for (int k = 0; k < f->n; ++k) {
            corners[k] = f->c[k];
            corners[k].u = def.rect[0] + f->c[k].u * def.rect[2];
            corners[k].v = def.rect[1] + f->c[k].v * def.rect[3];
          }
          if (f->n == 3) corners[3] = corners[2];   // a triangle is a quad with a repeated corner
          if (spec == nullptr || spec->vertices.size() > 65000) {
            specs.emplace_back();
            spec = &specs.back();
            spec->tmpl = source;
            spec->bones = mesh_bones;
            spec->weights = entries;
            index.clear();
          }
          uint32_t ids[4];
          for (int k = 0; k < 4; ++k) {
            const Corner& c = corners[k];
            VertexKey key;
            key.v = {c.pos[0], c.pos[1], c.pos[2], -c.normal[0], -c.normal[1], -c.normal[2], c.u, c.v};
            key.w = c.blend >= 0 ? part.blends[c.blend] : Weights{{bone, 1.0}};
            const auto found = index.find(key);
            if (found != index.end()) {
              ids[k] = found->second;
              continue;
            }
            const uint32_t id = uint32_t(spec->vertices.size());
            index.emplace(key, id);
            spec->vertices.push_back({key.v[0], key.v[1], key.v[2], key.v[3], key.v[4], key.v[5]});
            std::vector<std::pair<uint32_t, double>> entry;
            for (const auto& [b, w] : key.w) {
              const uint32_t pos = uint32_t(std::find(mesh_bones.begin(), mesh_bones.end(), b) - mesh_bones.begin());
              entry.push_back({pos << 24, w});
            }
            while (entry.size() < entries) entry.push_back({0xFFu << 24, 0.0});
            spec->vertex_weights.push_back(entry);
            spec->uvs.push_back({c.u, c.v});
            ids[k] = id;
          }
          const uint32_t a = ids[0], b = ids[1], c = ids[2], e = ids[3];
          for (uint32_t s : {a, a, b, e, c, c}) spec->strip.push_back(uint16_t(s));
        }
      }
    }
    const Bytes& td = template_data;
    if (8 + model.bone_table + 0x50 * size_t(model.bone_count) > td.size()) throw BuildError("damaged base model");
    Bytes bone_table(td.begin() + 8 + model.bone_table, td.begin() + 8 + model.bone_table + 0x50 * size_t(model.bone_count));
    if (bones != 1.0)
      for (uint32_t i = 0; i < model.bone_count; ++i)
        for (int k = 0; k < 3; ++k) {
          uint8_t* p = &bone_table[i * 0x50 + 16 + k * 4];
          put_bef(p, double(get_bef(p)) * bones);
        }
    return write_model(model, specs, bone_table);
  }

  if (kind == 'M' && item == uint32_t(2400 + slot)) {
    // the part's joint records: 20-byte entries (joint, bone, x, y, z) saying
    // where the neighbouring parts plug in - the sockets move with the skeleton
    Bytes data = template_data;
    for (size_t o = 0; o + 19 < data.size(); o += 20)
      for (int k = 0; k < 3; ++k) {
        uint8_t* p = &data[o + 8 + k * 4];
        put_lef(p, double(get_lef(p)) * bones);
      }
    return data;
  }

  if (kind != 'T') return std::nullopt;
  const char* role = nullptr;
  if (item == uint32_t(1000 + slot - 1)) role = "diffuse";
  else if (item == uint32_t(1100 + slot - 1)) role = "normal";
  else if (item == uint32_t(1400 + slot - 1)) role = "ao";
  else if (item == uint32_t(1600 + slot - 1)) role = "glow";
  if (role == nullptr) return std::nullopt;
  auto image = [&](const std::string& r) -> const Image* {
    auto found = part.images.find(r);
    if (found == part.images.end()) {
      const fs::path p = folder_ / (std::string(def.name) + "_" + r + ".png");
      std::optional<Image> img;
      if (fs::is_regular_file(p)) img = read_png(p);
      found = part.images.emplace(r, std::move(img)).first;
    }
    return found->second ? &*found->second : nullptr;
  };
  if (template_data.size() < 18) throw BuildError("damaged base texture");
  const int w = template_data[12] | (template_data[13] << 8), h = template_data[14] | (template_data[15] << 8);
  const std::string r = role;
  if (r == "diffuse") {
    const Image* diffuse = image("diffuse");
    if (!diffuse) throw BuildError(std::string(def.name) + "_diffuse.png is missing");
    std::vector<std::array<uint8_t, 3>> palette;
    Bytes index;
    quantize(resize_nearest(*diffuse, w, h), palette, index);
    Bytes table(1024, 0);
    for (size_t i = 0; i < palette.size(); ++i) {
      table[i * 4] = palette[i][2];
      table[i * 4 + 1] = palette[i][1];
      table[i * 4 + 2] = palette[i][0];
      table[i * 4 + 3] = 255;
    }
    Bytes rows(size_t(w) * h);
    for (int y = 0; y < h; ++y) std::memcpy(&rows[size_t(y) * w], &index[size_t(h - 1 - y) * w], size_t(w));
    return tga(template_data, w, h, rows, &table);
  }
  if (r == "ao") {
    Bytes shade(size_t(w) * h, 255);
    if (const Image* ao = image("ao")) {
      const Image s = resize(*ao, w, h);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) shade[size_t(y) * w + x] = s.at(x, h - 1 - y)[0];
    }
    Bytes ramp;
    for (int i = 0; i < 256; ++i) ramp.insert(ramp.end(), {uint8_t(i), uint8_t(i), uint8_t(i), 255});
    return tga(template_data, w, h, shade, &ramp);
  }
  if (r == "normal") {
    // blue green red = the normal's z y x (green turned over), alpha = gloss
    Bytes px(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
      px[i * 4] = 0xFF;
      px[i * 4 + 1] = 0x80;
      px[i * 4 + 2] = 0x80;
      px[i * 4 + 3] = 0x50;
    }
    if (const Image* normal = image("normal")) {
      const Image n = resize(*normal, w, h);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const uint8_t* s = n.at(x, h - 1 - y);
          uint8_t* d = &px[(size_t(y) * w + x) * 4];
          d[0] = s[2];
          d[2] = s[0];
          d[1] = uint8_t(255 - s[1]);
        }
    }
    if (const Image* gloss = image("gloss")) {
      const Image g = resize(*gloss, w, h);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px[(size_t(y) * w + x) * 4 + 3] = g.at(x, h - 1 - y)[0];
    }
    return tga(template_data, w, h, px, nullptr);
  }
  // glow: the mask in red (the glow colour picked in the game)
  Bytes px(size_t(w) * h * 4, 0);
  if (const Image* glow = image("glow")) {
    const Image g = resize(*glow, w, h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const uint8_t* s = g.at(x, h - 1 - y);
        px[(size_t(y) * w + x) * 4 + 2] = std::max(s[0], std::max(s[1], s[2]));
      }
  }
  return tga(template_data, w, h, px, nullptr);
}

}  // namespace realsteel::mods
