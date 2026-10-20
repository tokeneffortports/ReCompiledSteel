// Fighter import: shared pieces (see primport_core.h).
#include "primport_core.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace rs::primport {
namespace fs = std::filesystem;

// --- bytes -----------------------------------------------------------------
float bef(const uint8_t* p) {
  const uint32_t v = be32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void put_be32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void put_be16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void put_le32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24); }
void put_bef(uint8_t* p, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put_be32(p, u);
}
void app_be32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }
void app_be16(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 8), uint8_t(v)}); }
void app_le32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
void app_bef(Bytes& b, double v) {
  uint8_t t[4];
  put_bef(t, v);
  b.insert(b.end(), t, t + 4);
}
void app(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }
void app(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }

Bytes read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw Error("cannot read " + p.string());
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const Bytes& b) {
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("cannot write " + p.string());
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    if (!f) throw Error("cannot write " + p.string());
  }
  std::error_code ec;
  fs::remove(p, ec);
  fs::rename(tmp, p);
}

// --- maths -----------------------------------------------------------------
Mat3 operator*(const Mat3& a, const Mat3& b) {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r(i, j) = a(i, 0) * b(0, j) + a(i, 1) * b(1, j) + a(i, 2) * b(2, j);
  return r;
}
Vec3 operator*(const Mat3& a, const Vec3& v) {
  return {a(0, 0) * v[0] + a(0, 1) * v[1] + a(0, 2) * v[2], a(1, 0) * v[0] + a(1, 1) * v[1] + a(1, 2) * v[2],
          a(2, 0) * v[0] + a(2, 1) * v[1] + a(2, 2) * v[2]};
}
Mat3 transpose(const Mat3& a) {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r(i, j) = a(j, i);
  return r;
}
Mat3 rot_x(double a) {
  Mat3 r;
  const double c = std::cos(a), s = std::sin(a);
  r.m = {1, 0, 0, 0, c, -s, 0, s, c};
  return r;
}
Mat3 rot_y(double a) {
  Mat3 r;
  const double c = std::cos(a), s = std::sin(a);
  r.m = {c, 0, s, 0, 1, 0, -s, 0, c};
  return r;
}
static Mat3 rot_z(double a) {
  Mat3 r;
  const double c = std::cos(a), s = std::sin(a);
  r.m = {c, -s, 0, s, c, 0, 0, 0, 1};
  return r;
}
Mat3 euler(double rx, double ry, double rz) { return rot_z(rz) * rot_y(ry) * rot_x(rx); }

Mat3 rotation_between(Vec3 a, Vec3 b) {
  a = a * (1.0 / norm(a));
  b = b * (1.0 / norm(b));
  const Vec3 v = cross(a, b);
  const double c = dot(a, b);
  Mat3 r;
  if (c < -0.9999) {
    r.m = {-1, 0, 0, 0, -1, 0, 0, 0, -1};
    return r;
  }
  Mat3 k;
  k.m = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
  const Mat3 kk = k * k;
  for (int i = 0; i < 9; ++i) r.m[i] = (i % 4 == 0 ? 1.0 : 0.0) + k.m[i] + kk.m[i] / (1 + c);
  return r;
}

Mat3 quat_mat(const Quat& q) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  Mat3 r;
  r.m = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
         2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
         2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
  return r;
}

Quat mat_quat(const Mat3& m) {
  double x, y, z, w;
  const double t = m(0, 0) + m(1, 1) + m(2, 2);
  if (t > 0) {
    const double s = std::sqrt(t + 1.0) * 2;
    w = 0.25 * s, x = (m(2, 1) - m(1, 2)) / s, y = (m(0, 2) - m(2, 0)) / s, z = (m(1, 0) - m(0, 1)) / s;
  } else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2)) {
    const double s = std::sqrt(1.0 + m(0, 0) - m(1, 1) - m(2, 2)) * 2;
    w = (m(2, 1) - m(1, 2)) / s, x = 0.25 * s, y = (m(0, 1) + m(1, 0)) / s, z = (m(0, 2) + m(2, 0)) / s;
  } else if (m(1, 1) > m(2, 2)) {
    const double s = std::sqrt(1.0 + m(1, 1) - m(0, 0) - m(2, 2)) * 2;
    w = (m(0, 2) - m(2, 0)) / s, x = (m(0, 1) + m(1, 0)) / s, y = 0.25 * s, z = (m(1, 2) + m(2, 1)) / s;
  } else {
    const double s = std::sqrt(1.0 + m(2, 2) - m(0, 0) - m(1, 1)) * 2;
    w = (m(1, 0) - m(0, 1)) / s, x = (m(0, 2) + m(2, 0)) / s, y = (m(1, 2) + m(2, 1)) / s, z = 0.25 * s;
  }
  const double n = std::sqrt(x * x + y * y + z * z + w * w);
  Quat q{x / n, y / n, z / n, w / n};
  if (q[3] < 0) q = {-q[0], -q[1], -q[2], -q[3]};
  return q;
}

Quat slerp(Quat a, Quat b, double t) {
  double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) {
    b = {-b[0], -b[1], -b[2], -b[3]};
    d = -d;
  }
  Quat q;
  if (d > 0.9995) {
    for (int i = 0; i < 4; ++i) q[i] = a[i] + (b[i] - a[i]) * t;
  } else {
    const double th = std::acos(std::min(1.0, d)), s = std::sin(th);
    const double w0 = std::sin((1 - t) * th) / s, w1 = std::sin(t * th) / s;
    for (int i = 0; i < 4; ++i) q[i] = a[i] * w0 + b[i] * w1;
  }
  double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n == 0) n = 1;
  for (double& c : q) c /= n;
  return q;
}

// --- archives ----------------------------------------------------------------
bool is_bpe(const Bytes& b) { return b.size() >= 16 && std::memcmp(b.data(), "BPE ", 4) == 0; }

Bytes bpe_decode(const uint8_t* data, size_t size) {
  if (size < 16 || std::memcmp(data, "BPE ", 4) != 0) throw Error("not a BPE block");
  const uint32_t csize = le32(data + 8), usize = le32(data + 12);
  if (16 + size_t(csize) > size) throw Error("BPE block past its end");
  const uint8_t* src = data + 16;
  const size_t n = csize;
  Bytes out;
  out.reserve(usize);
  size_t p = 0;
  uint8_t left[256], right[256];
  std::vector<uint8_t> stack;
  while (p < n && out.size() < usize) {
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i), right[i] = 0;
    int count = src[p++];
    int c = 0;
    while (true) {
      if (count > 127) {
        c += count - 127;
        count = 0;
      }
      if (c == 256) break;
      for (int k = 0; k <= count; ++k) {
        if (p >= n) throw Error("BPE table past its end");
        left[c] = src[p++];
        if (c != left[c]) right[c] = src[p++];
        ++c;
      }
      if (c == 256) break;
      count = src[p++];
    }
    const size_t len = size_t(src[p]) | (size_t(src[p + 1]) << 8);
    p += 2;
    const size_t end = std::min(n, p + len);
    stack.clear();
    while (true) {
      uint8_t ch;
      if (!stack.empty()) {
        ch = stack.back();
        stack.pop_back();
      } else if (p < end) {
        ch = src[p++];
      } else {
        break;
      }
      const uint8_t l = left[ch];
      if (ch == l) {
        out.push_back(ch);
      } else {
        stack.push_back(right[ch]);
        stack.push_back(l);
      }
    }
  }
  out.resize(std::min<size_t>(out.size(), usize));
  return out;
}

Bytes bpe_store(const Bytes& data) {
  Bytes body;
  for (size_t i = 0; i < data.size(); i += 0x1000) {
    const size_t len = std::min<size_t>(0x1000, data.size() - i);
    body.insert(body.end(), {0xFF, 0x80, 0xFE, uint8_t(len), uint8_t(len >> 8)});
    body.insert(body.end(), data.begin() + i, data.begin() + i + len);
  }
  Bytes out = {'B', 'P', 'E', ' '};
  app_le32(out, 0x100);
  app_le32(out, uint32_t(body.size()));
  app_le32(out, uint32_t(data.size()));
  app(out, body);
  return out;
}

Bytes PachItem::data() const { return is_bpe(raw) ? bpe_decode(raw.data(), raw.size()) : raw; }

std::vector<PachItem> pach_items(const Bytes& d) {
  if (d.size() < 8 || std::memcmp(d.data(), "PACH", 4) != 0) throw Error("not a PACH container");
  const uint32_t count = le32(&d[4]);
  const size_t base = 8 + size_t(count) * 12;
  if (base > d.size()) throw Error("PACH table past its end");
  std::vector<PachItem> out;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = le32(&d[8 + i * 12]), off = le32(&d[12 + i * 12]), size = le32(&d[16 + i * 12]);
    if (base + size_t(off) + size > d.size()) throw Error("PACH item past its end");
    out.push_back({id, Bytes(d.begin() + base + off, d.begin() + base + off + size)});
  }
  return out;
}

Bytes pach_build(const std::vector<PachItem>& items) {
  Bytes table, body;
  const size_t base = 8 + 12 * items.size();
  for (const PachItem& it : items) {
    while ((base + body.size()) % 16) body.push_back(0);
    app_le32(table, it.id);
    app_le32(table, uint32_t(body.size()));
    app_le32(table, uint32_t(it.raw.size()));
    app(body, it.raw);
  }
  Bytes out = {'P', 'A', 'C', 'H'};
  app_le32(out, uint32_t(items.size()));
  app(out, table);
  app(out, body);
  while (out.size() % 0x100) out.push_back(0);
  return out;
}

Archive::Archive(const fs::path& path) : data_(read_file(path)) { parse(); }
Archive::Archive(Bytes data) : data_(std::move(data)) { parse(); }

void Archive::parse() {
  const Bytes& d = data_;
  if (d.size() < 0x800) throw Error("archive too short");
  const bool epk8 = std::memcmp(d.data(), "EPK8", 4) == 0;
  if (!epk8 && std::memcmp(d.data(), "EPAC", 4) != 0) throw Error("not an EPAC/EPK8 archive");
  const uint32_t dir_size = le32(&d[4]);
  constexpr size_t kDataStart = 0x4000;
  size_t off = 0x800;
  const size_t end = 0x800 + size_t(dir_size);
  while (off + 12 <= end) {
    std::string g(reinterpret_cast<const char*>(&d[off]), 4);
    while (!g.empty() && (g.back() == ' ' || g.back() == 0)) g.pop_back();
    const uint32_t head = le32(&d[off + 4]);
    uint32_t words = head;
    size_t name_len = 4;
    if (epk8) {
      words = head & 0xFFFF;
      name_len = (head >> 16) & 1 ? 8 : 4;
    }
    const size_t per = name_len / 4 + 2;
    off += 12;
    for (uint32_t i = 0; i < words / per; ++i) {
      std::string name(reinterpret_cast<const char*>(&d[off]), name_len);
      while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
      const uint32_t sector = le32(&d[off + name_len]), units = le32(&d[off + name_len + 4]);
      const size_t start = kDataStart + size_t(sector) * 0x800, size = size_t(units) * 0x100;
      if (start + size > d.size()) throw Error("archive entry past its end");
      entries_.push_back({g, name, Bytes(d.begin() + start, d.begin() + start + size)});
      off += name_len + 8;
    }
  }
}

static std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> out;
  size_t s = 0;
  while (s < path.size()) {
    if (path[s] == '/') {
      ++s;
      continue;
    }
    size_t e = path.find('/', s);
    if (e == std::string::npos) e = path.size();
    out.push_back(path.substr(s, e - s));
    s = e;
  }
  return out;
}

static std::string hex4(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%04X", v);
  return b;
}

bool Archive::find(const std::string& path, Bytes* out) const {
  const std::vector<std::string> parts = split_path(path);
  if (parts.size() < 2) return false;
  for (const Entry& e : entries_) {
    if (e.group != parts[0] || e.name != parts[1]) continue;
    Bytes cur = is_bpe(e.raw) ? bpe_decode(e.raw.data(), e.raw.size()) : e.raw;
    for (size_t k = 2; k < parts.size(); ++k) {
      if (cur.size() < 8 || std::memcmp(cur.data(), "PACH", 4) != 0) return false;
      bool found = false;
      for (const PachItem& it : pach_items(cur)) {
        if (hex4(it.id) == parts[k]) {
          cur = it.data();
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    *out = std::move(cur);
    return true;
  }
  return false;
}

bool Archive::children(const std::string& path, std::vector<std::pair<std::string, Bytes>>* out) const {
  Bytes d;
  if (!find(path, &d) || d.size() < 8 || std::memcmp(d.data(), "PACH", 4) != 0) return false;
  out->clear();
  for (const PachItem& it : pach_items(d)) out->emplace_back(hex4(it.id), it.data());
  return true;
}

// --- JBOY ------------------------------------------------------------------
Model::Model(Bytes d) : data(std::move(d)) {
  if (data.size() < 0x40 || std::memcmp(data.data(), "JBOY", 4) != 0) throw Error("not a JBOY model");
  const uint8_t* p = data.data();
  pr = be32(p + 8) != 0;
  const uint32_t mesh_count = be32(p + 0x18) & 0x7FFFFFFF, mesh_table = be32(p + 0x1C);
  bone_count = be32(p + 0x20);
  texture_count = be32(p + 0x24);
  bone_table = be32(p + 0x28);
  texture_table = be32(p + 0x2C);
  group_table = be32(p + 0x30);
  group_count = be32(p + 0x34);
  const uint32_t mesh_size = pr ? 0xB8 : 0xB4;
  for (uint32_t i = 0; i < mesh_count; ++i) {
    const uint32_t o = 8 + mesh_table + i * mesh_size;
    if (o + mesh_size > data.size()) throw Error("JBOY mesh table past its end");
    Mesh m;
    m.offset = o;
    m.vertex_count = be32(p + o);
    m.weights = be32(p + o + 8);
    for (int k = 0; k < 20; ++k) {
      const uint32_t b = be32(p + o + 0x0C + 4 * k);
      if (b != 0xFFFFFFFF) m.palette.push_back(b);
    }
    m.influences = be32(p + o + 0x5C);
    m.group = be32(p + o + 0x60);
    m.vertex_block = be32(p + o + 0x68);
    m.weight_block = be32(p + o + 0x6C);
    m.uv_block = be32(p + o + 0x70);
    if (pr) {
      m.extra_block = be32(p + o + 0x74);
      m.param_count = be32(p + o + 0x94);
      m.param_table = be32(p + o + 0x98);
      m.strip_table = be32(p + o + 0x9C);
    } else {
      m.group74 = be32(p + o + 0x74);
      m.param_count = be32(p + o + 0x90);
      m.param_table = be32(p + o + 0x94);
      m.strip_table = be32(p + o + 0x98);
    }
    meshes.push_back(m);
  }
  for (uint32_t i = 0; i < bone_count; ++i) {
    const uint32_t o = 8 + bone_table + i * 0x50;
    if (o + 0x50 > data.size()) throw Error("JBOY bone table past its end");
    Bone b;
    b.name = std::string(reinterpret_cast<const char*>(p + o), strnlen(reinterpret_cast<const char*>(p + o), 16));
    b.pos = {bef(p + o + 16), bef(p + o + 20), bef(p + o + 24)};
    b.rot = {bef(p + o + 32), bef(p + o + 36), bef(p + o + 40)};
    b.parent = int32_t(be32(p + o + 48));
    std::memcpy(b.raw.data(), p + o, 0x50);
    bones.push_back(b);
  }
  for (uint32_t i = 0; i < group_count; ++i) {
    const uint32_t o = 8 + group_table + i * 32;
    group_names.emplace_back(reinterpret_cast<const char*>(p + o), strnlen(reinterpret_cast<const char*>(p + o), 16));
  }
}

void Model::vertices(const Mesh& m, std::vector<std::array<double, 6>>* v, std::vector<uint32_t>* colour) const {
  const uint8_t* p = data.data();
  const uint32_t ptr = be32(p + 8 + m.vertex_block);
  if (8 + size_t(ptr) + size_t(m.vertex_count) * 28 > data.size()) throw Error("JBOY vertices past the end");
  v->resize(m.vertex_count);
  if (colour) colour->resize(m.vertex_count);
  for (uint32_t i = 0; i < m.vertex_count; ++i) {
    const uint8_t* q = p + 8 + ptr + i * 28;
    for (int k = 0; k < 6; ++k) (*v)[i][k] = bef(q + 4 * k);
    if (colour) (*colour)[i] = be32(q + 24);
  }
}

std::vector<std::array<double, 2>> Model::uvs(const Mesh& m) const {
  std::vector<std::array<double, 2>> out(m.vertex_count);
  const uint8_t* p = data.data() + 8 + m.uv_block;
  for (uint32_t i = 0; i < m.vertex_count; ++i) out[i] = {bef(p + i * 8), bef(p + i * 8 + 4)};
  return out;
}

std::vector<std::vector<std::pair<int, double>>> Model::skin(const Mesh& m) const {
  std::vector<std::vector<std::pair<int, double>>> out(m.vertex_count);
  const uint8_t* p = data.data() + 8 + m.weight_block;
  for (uint32_t i = 0; i < m.vertex_count; ++i) {
    for (uint32_t k = 0; k < m.influences; ++k) {
      const uint8_t* q = p + (size_t(i) * m.influences + k) * 8;
      const uint32_t idx = be32(q) >> 24;
      const double w = bef(q + 4);
      if (idx != 0xFF && w > 0 && idx < m.palette.size()) out[i].emplace_back(int(m.palette[idx]) - 1, w);
    }
  }
  return out;
}

std::vector<std::array<int, 3>> strip_triangles(const std::vector<uint16_t>& s) {
  std::vector<std::array<int, 3>> out;
  for (size_t i = 0; i + 2 < s.size(); ++i) {
    const int a = s[i], b = s[i + 1], c = s[i + 2];
    if (a == b || b == c || a == c) continue;
    out.push_back(i % 2 == 0 ? std::array<int, 3>{a, b, c} : std::array<int, 3>{b, a, c});
  }
  return out;
}

std::vector<std::array<int, 3>> Model::triangles(const Mesh& m) const {
  const uint8_t* p = data.data() + 8 + m.strip_table;
  const uint32_t count = be32(p + 4), offset = be32(p + 8);
  std::vector<uint16_t> s(count);
  for (uint32_t i = 0; i < count; ++i) s[i] = be16(data.data() + 8 + offset + 2 * i);
  return strip_triangles(s);
}

// --- textures --------------------------------------------------------------
DdsInfo dds_info(const Bytes& d) {
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) throw Error("not a DDS texture");
  DdsInfo i;
  i.h = int(le32(&d[12]));
  i.w = int(le32(&d[16]));
  i.mips = std::max<int>(1, int(le32(&d[28])));
  i.fourcc.assign(reinterpret_cast<const char*>(&d[84]), 4);
  return i;
}

static size_t block_bytes(const std::string& f) { return f == "DXT1" ? 8 : 16; }
static size_t level_size(int w, int h, const std::string& f) {
  return size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * block_bytes(f);
}

std::vector<std::pair<std::array<int, 2>, Bytes>> dds_levels(const Bytes& d) {
  const DdsInfo info = dds_info(d);
  std::vector<std::pair<std::array<int, 2>, Bytes>> out;
  size_t o = 128;
  int w = info.w, h = info.h;
  for (int i = 0; i < info.mips; ++i) {
    const size_t s = level_size(w, h, info.fourcc);
    if (o + s > d.size()) throw Error("DDS level past the end");
    out.push_back({{w, h}, Bytes(d.begin() + o, d.begin() + o + s)});
    o += s;
    w = std::max(1, w / 2), h = std::max(1, h / 2);
  }
  return out;
}

Bytes dds_header(const Bytes& tmpl, int w, int h, int mips, const char* fourcc) {
  Bytes hd(tmpl.begin(), tmpl.begin() + 128);
  put_le32(&hd[12], uint32_t(h));
  put_le32(&hd[16], uint32_t(w));
  put_le32(&hd[20], uint32_t(level_size(w, h, fourcc)));
  put_le32(&hd[28], uint32_t(mips));
  put_le32(&hd[8], le32(&hd[8]) | 0x20000);
  std::memcpy(&hd[84], fourcc, 4);
  return hd;
}

static void palette565(uint32_t c0, uint32_t c1, bool dxt1, float pal[4][3], bool* three) {
  auto unpack = [](uint32_t c, float* o) {
    o[0] = float((c >> 11) & 31) * 255.0f / 31.0f;
    o[1] = float((c >> 5) & 63) * 255.0f / 63.0f;
    o[2] = float(c & 31) * 255.0f / 31.0f;
  };
  unpack(c0, pal[0]);
  unpack(c1, pal[1]);
  *three = dxt1 && c0 <= c1;
  for (int k = 0; k < 3; ++k) {
    if (*three) {
      pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
      pal[3][k] = 0;
    } else {
      pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
      pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
  }
}

static uint8_t to_u8(float v) {
  const float r = std::floor(v + 0.5f);
  return uint8_t(std::clamp(r, 0.0f, 255.0f));
}

Image dds_decode(const Bytes& d) {
  const DdsInfo info = dds_info(d);
  const bool dxt1 = info.fourcc == "DXT1";
  if (!dxt1 && info.fourcc != "DXT5") throw Error("unsupported DDS format " + info.fourcc);
  const int bw = std::max(1, info.w / 4), bh = std::max(1, info.h / 4);
  const size_t bs = dxt1 ? 8 : 16;
  if (128 + size_t(bw) * bh * bs > d.size()) throw Error("DDS data past the end");
  Image img;
  img.w = info.w, img.h = info.h;
  img.px.assign(size_t(img.w) * img.h * 4, 0);
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      const uint8_t* b = &d[128 + (size_t(by) * bw + bx) * bs];
      float alpha[16];
      const uint8_t* cb = b;
      if (!dxt1) {
        const float a0 = b[0], a1 = b[1];
        uint64_t bits = 0;
        for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
        float pal[8];
        pal[0] = a0, pal[1] = a1;
        if (a0 > a1) {
          for (int i = 1; i <= 6; ++i) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
        } else {
          for (int i = 1; i <= 4; ++i) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5;
          pal[6] = 0, pal[7] = 255;
        }
        for (int k = 0; k < 16; ++k) alpha[k] = pal[(bits >> (3 * k)) & 7];
        cb = b + 8;
      } else {
        for (float& a : alpha) a = 255;
      }
      const uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
      float pal[4][3];
      bool three;
      palette565(c0, c1, dxt1, pal, &three);
      const uint32_t bits = le32(cb + 4);
      for (int k = 0; k < 16; ++k) {
        const int idx = (bits >> (2 * k)) & 3;
        const int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
        if (x >= img.w || y >= img.h) continue;
        uint8_t* o = img.at(x, y);
        for (int c = 0; c < 3; ++c) o[c] = to_u8(pal[idx][c]);
        o[3] = to_u8(dxt1 && three && idx == 3 ? 0.0f : alpha[k]);
      }
    }
  }
  return img;
}

std::vector<std::pair<std::string, Bytes>> texpack_read(const Bytes& d) {
  std::vector<std::pair<std::string, Bytes>> out;
  const uint32_t n = le32(&d[0]);
  for (uint32_t i = 0; i < n; ++i) {
    const size_t o = 16 + size_t(i) * 32;
    std::string name(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 16));
    const uint32_t size = le32(&d[o + 20]), off = le32(&d[o + 24]);
    if (size_t(off) + size > d.size()) throw Error("texture pack entry past the end");
    out.emplace_back(name, Bytes(d.begin() + off, d.begin() + off + size));
  }
  return out;
}

Bytes texpack_write(const std::vector<std::pair<std::string, Bytes>>& t) {
  const uint32_t n = uint32_t(t.size());
  Bytes head, entries, body;
  app_le32(head, n);
  app_le32(head, 0x100);
  app_le32(head, 0);
  app_le32(head, 0x10);
  const uint32_t off = 16 + 32 * n;
  for (const auto& [name, dds] : t) {
    uint8_t nm[16] = {};
    std::memcpy(nm, name.data(), std::min<size_t>(16, name.size()));
    app(entries, nm, 16);
    entries.insert(entries.end(), {'d', 'd', 's', 0});
    app_le32(entries, uint32_t(dds.size()));
    app_le32(entries, off + uint32_t(body.size()));
    app_le32(entries, 0);
    app(body, dds);
    while (body.size() % 16) body.push_back(0);
  }
  app(head, entries);
  app(head, body);
  return head;
}

static uint8_t clip_rint(double v) { return uint8_t(std::clamp(std::nearbyint(v), 0.0, 255.0)); }

static void four_colour(uint8_t* b) {
  const uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  if (c0 > c1) return;
  uint32_t bits = le32(b + 4), out = 0;
  for (int k = 0; k < 16; ++k) {
    uint32_t idx = (bits >> (2 * k)) & 3;
    if (idx == 3) idx = 0;      // the darker endpoint (c0 <= c1)
    out |= idx << (2 * k);
  }
  put_le32(b + 4, out);
}

Bytes dxt5_from(const Bytes& colour_dxt1, const Bytes& alpha_dxt5, AlphaMap map) {
  const DdsInfo ci = dds_info(colour_dxt1), ai = dds_info(alpha_dxt5);
  if (ci.fourcc != "DXT1" || ai.fourcc != "DXT5" || ci.w != ai.w || ci.h != ai.h) throw Error("colour / gloss textures do not match");
  const int mips = std::min(ci.mips, ai.mips);
  Bytes out = dds_header(alpha_dxt5, ci.w, ci.h, mips, "DXT5");
  const auto cl = dds_levels(colour_dxt1), al = dds_levels(alpha_dxt5);
  for (int i = 0; i < mips; ++i) {
    const Bytes& c = cl[i].second;
    const Bytes& a = al[i].second;
    const size_t n = c.size() / 8;
    for (size_t k = 0; k < n; ++k) {
      uint8_t blk[16];
      std::memcpy(blk, &a[k * 16], 8);
      std::memcpy(blk + 8, &c[k * 8], 8);
      four_colour(blk + 8);
      if (map) {
        blk[0] = clip_rint(map(blk[0]));
        blk[1] = clip_rint(map(blk[1]));
        if (blk[0] == blk[1]) std::memset(blk + 2, 0, 6);
      }
      app(out, blk, 16);
    }
  }
  return out;
}

Bytes dds_drop_top(const Bytes& d, int levels) {
  const DdsInfo info = dds_info(d);
  const auto lv = dds_levels(d);
  if (int(lv.size()) <= levels) throw Error("texture has too few levels");
  Bytes out = dds_header(d, lv[levels].first[0], lv[levels].first[1], int(lv.size()) - levels, info.fourcc.c_str());
  for (size_t i = size_t(levels); i < lv.size(); ++i) app(out, lv[i].second);
  return out;
}

Bytes dxt5_remap_alpha(const Bytes& d, AlphaMap map) {
  const DdsInfo info = dds_info(d);
  if (info.fourcc != "DXT5") throw Error("expected a DXT5 texture");
  Bytes out(d.begin(), d.begin() + 128);
  for (auto& [size, lv] : dds_levels(d)) {
    Bytes b = lv;
    for (size_t k = 0; k + 16 <= b.size(); k += 16) {
      b[k] = clip_rint(map(b[k]));
      b[k + 1] = clip_rint(map(b[k + 1]));
      if (b[k] == b[k + 1]) std::memset(&b[k + 2], 0, 6);
    }
    app(out, b);
  }
  return out;
}

Bytes encode_red_dxt5(const std::vector<double>& red, int w, int h, const Bytes& tmpl) {
  struct Level {
    int w, h;
    std::vector<double> v;
  };
  std::vector<Level> levels;
  Level cur{w, h, red};
  while (true) {
    levels.push_back(cur);
    if (cur.h < 2 || cur.w < 2 || std::min(cur.w, cur.h) <= 4) break;
    Level nxt{cur.w / 2, cur.h / 2, {}};
    nxt.v.resize(size_t(nxt.w) * nxt.h);
    for (int y = 0; y < nxt.h; ++y)
      for (int x = 0; x < nxt.w; ++x)
        nxt.v[size_t(y) * nxt.w + x] = (cur.v[size_t(2 * y) * cur.w + 2 * x] + cur.v[size_t(2 * y) * cur.w + 2 * x + 1] +
                                        cur.v[size_t(2 * y + 1) * cur.w + 2 * x] + cur.v[size_t(2 * y + 1) * cur.w + 2 * x + 1]) / 4;
    cur = std::move(nxt);
  }
  Bytes out = dds_header(tmpl, w, h, int(levels.size()), "DXT5");
  for (const Level& L : levels) {
    const int pw = std::max(4, (L.w + 3) / 4 * 4), ph = std::max(4, (L.h + 3) / 4 * 4);
    for (int by = 0; by < ph / 4; ++by) {
      for (int bx = 0; bx < pw / 4; ++bx) {
        double px[16], lo = 1e9, hi = -1e9;
        for (int k = 0; k < 16; ++k) {
          const int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
          px[k] = (x < L.w && y < L.h) ? L.v[size_t(y) * L.w + x] : 0.0;
          lo = std::min(lo, px[k]);
          hi = std::max(hi, px[k]);
        }
        const uint32_t r0 = uint32_t(std::clamp(std::nearbyint(hi * 31 / 255), 0.0, 31.0));
        const uint32_t r1 = uint32_t(std::clamp(std::nearbyint(lo * 31 / 255), 0.0, 31.0));
        const uint32_t c0 = r0 << 11, c1 = r1 << 11;
        const double v0 = r0 * 255.0 / 31, v1 = r1 * 255.0 / 31;
        const double pal[4] = {v0, v1, (2 * v0 + v1) / 3, (v0 + 2 * v1) / 3};
        uint32_t bits = 0;
        for (int k = 0; k < 16; ++k) {
          int best = 0;
          for (int i = 1; i < 4; ++i)
            if (std::abs(px[k] - pal[i]) < std::abs(px[k] - pal[best])) best = i;
          bits |= uint32_t(best) << (2 * k);
        }
        uint8_t blk[16] = {};
        blk[8] = uint8_t(c0), blk[9] = uint8_t(c0 >> 8), blk[10] = uint8_t(c1), blk[11] = uint8_t(c1 >> 8);
        put_le32(blk + 12, bits);
        app(out, blk, 16);
      }
    }
  }
  return out;
}

Bytes constant_dxt1(int w, int h, std::array<int, 3> rgb, const Bytes& tmpl) {
  const uint32_t c = ((rgb[0] * 31 / 255) << 11) | ((rgb[1] * 63 / 255) << 5) | (rgb[2] * 31 / 255);
  std::vector<size_t> sizes;
  int ww = w, hh = h;
  while (true) {
    sizes.push_back(size_t(std::max(1, (ww + 3) / 4)) * size_t(std::max(1, (hh + 3) / 4)));
    if (ww == 1 && hh == 1) break;
    ww = std::max(1, ww / 2), hh = std::max(1, hh / 2);
  }
  Bytes out = dds_header(tmpl, w, h, int(sizes.size()), "DXT1");
  const uint8_t block[8] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  for (size_t n : sizes)
    for (size_t i = 0; i < n; ++i) app(out, block, 8);
  return out;
}

Bytes encode_dxt5(const Image& img, const Bytes& tmpl) {
  Bytes out = dds_header(tmpl, img.w, img.h, 1, "DXT5");
  for (int by = 0; by < img.h / 4; ++by) {
    for (int bx = 0; bx < img.w / 4; ++bx) {
      float px[16][4];
      float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
      for (int k = 0; k < 16; ++k) {
        const uint8_t* p = img.at(bx * 4 + (k & 3), by * 4 + (k >> 2));
        for (int c = 0; c < 4; ++c) px[k][c] = p[c];
        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], px[k][c]), hi[c] = std::max(hi[c], px[k][c]);
      }
      auto to565 = [](const float* v, double* back) {
        const uint32_t r = uint32_t(std::clamp(std::nearbyint(v[0] * 31.0 / 255), 0.0, 31.0));
        const uint32_t g = uint32_t(std::clamp(std::nearbyint(v[1] * 63.0 / 255), 0.0, 63.0));
        const uint32_t b = uint32_t(std::clamp(std::nearbyint(v[2] * 31.0 / 255), 0.0, 31.0));
        back[0] = r * 255.0 / 31, back[1] = g * 255.0 / 63, back[2] = b * 255.0 / 31;
        return (r << 11) | (g << 5) | b;
      };
      double v0[3], v1[3];
      uint32_t c0 = to565(hi, v0), c1 = to565(lo, v1);
      if (c0 < c1) {
        std::swap(c0, c1);
        for (int c = 0; c < 3; ++c) std::swap(v0[c], v1[c]);
      }
      double pal[4][3];
      for (int c = 0; c < 3; ++c) pal[0][c] = v0[c], pal[1][c] = v1[c], pal[2][c] = (2 * v0[c] + v1[c]) / 3, pal[3][c] = (v0[c] + 2 * v1[c]) / 3;
      uint32_t cbits = 0;
      for (int k = 0; k < 16; ++k) {
        int best = 0;
        double bd = 1e30;
        for (int i = 0; i < 4; ++i) {
          double dd = 0;
          for (int c = 0; c < 3; ++c) dd += (px[k][c] - pal[i][c]) * (px[k][c] - pal[i][c]);
          if (dd < bd) bd = dd, best = i;
        }
        if (c0 == c1) best = 0;
        cbits |= uint32_t(best) << (2 * k);
      }
      float amax = 0, amin = 255;
      for (int k = 0; k < 16; ++k) amax = std::max(amax, px[k][3]), amin = std::min(amin, px[k][3]);
      const uint32_t a0 = uint32_t(std::nearbyint(amax)), a1 = uint32_t(std::nearbyint(amin));
      double apal[8] = {double(a0), double(a1)};
      for (int i = 1; i <= 6; ++i) apal[1 + i] = ((7 - i) * double(a0) + i * double(a1)) / 7;
      uint64_t abits = 0;
      for (int k = 0; k < 16; ++k) {
        int best = 0;
        for (int i = 1; i < 8; ++i)
          if (std::abs(px[k][3] - apal[i]) < std::abs(px[k][3] - apal[best])) best = i;
        if (a0 == a1) best = 0;
        abits |= uint64_t(best) << (3 * k);
      }
      uint8_t blk[16];
      blk[0] = uint8_t(a0), blk[1] = uint8_t(a1);
      for (int k = 0; k < 6; ++k) blk[2 + k] = uint8_t(abits >> (8 * k));
      blk[8] = uint8_t(c0), blk[9] = uint8_t(c0 >> 8), blk[10] = uint8_t(c1), blk[11] = uint8_t(c1 >> 8);
      put_le32(blk + 12, cbits);
      app(out, blk, 16);
    }
  }
  return out;
}

}  // namespace rs::primport
