// Fighter import from other Yuke's Xbox 360 games (currently Pacific Rim):
// shared pieces - bytes, 3D maths, Yuke's archives (EPAC / EPK8 / PACH /
// BPE), JBOY models, DDS / DXT textures. Standalone C++20, no emulator code.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rs::primport {

using Bytes = std::vector<uint8_t>;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// --- bytes -----------------------------------------------------------------
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
float bef(const uint8_t* p);
void put_be32(uint8_t* p, uint32_t v);
void put_be16(uint8_t* p, uint32_t v);
void put_le32(uint8_t* p, uint32_t v);
void put_bef(uint8_t* p, double v);
void app_be32(Bytes& b, uint32_t v);
void app_be16(Bytes& b, uint32_t v);
void app_le32(Bytes& b, uint32_t v);
void app_bef(Bytes& b, double v);
void app(Bytes& b, const Bytes& o);
void app(Bytes& b, const uint8_t* p, size_t n);
Bytes read_file(const std::filesystem::path& p);
void write_file(const std::filesystem::path& p, const Bytes& b);

// --- maths (row-major 3x3, column vectors: world = parent * local) ---------
using Vec3 = std::array<double, 3>;
struct Mat3 {
  std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
  double& operator()(int r, int c) { return m[r * 3 + c]; }
  double operator()(int r, int c) const { return m[r * 3 + c]; }
  static Mat3 identity() { return Mat3{}; }
};
Mat3 operator*(const Mat3& a, const Mat3& b);
Vec3 operator*(const Mat3& a, const Vec3& v);
Mat3 transpose(const Mat3& a);
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
Mat3 euler(double rx, double ry, double rz);      // Rz * Ry * Rx (the JBOY bone rotation)
Mat3 rot_y(double a);
Mat3 rot_x(double a);
Mat3 rotation_between(Vec3 a, Vec3 b);           // turns direction a onto b
using Quat = std::array<double, 4>;               // x, y, z, w
Mat3 quat_mat(const Quat& q);
Quat mat_quat(const Mat3& m);                     // w >= 0
Quat slerp(Quat a, Quat b, double t);

// --- archives ----------------------------------------------------------------
Bytes bpe_decode(const uint8_t* data, size_t size);
Bytes bpe_store(const Bytes& data);               // a BPE block without compression
bool is_bpe(const Bytes& b);

// PACH container: items (id, raw bytes as stored)
struct PachItem {
  uint32_t id;
  Bytes raw;
  Bytes data() const;    // decoded
};
std::vector<PachItem> pach_items(const Bytes& d);
Bytes pach_build(const std::vector<PachItem>& items);   // 16-byte aligned items, padded to 0x100

// EPAC (4-character entry names) or EPK8 (8-character) archive, read only.
// find("/MENU/SSFD/0000/0064") walks entries and nested PACH items (%04X).
class Archive {
 public:
  explicit Archive(const std::filesystem::path& path);
  explicit Archive(Bytes data);
  struct Entry {
    std::string group, name;
    Bytes raw;
  };
  const std::vector<Entry>& entries() const { return entries_; }
  // decoded data of a path; empty optional-like: returns false if missing
  bool find(const std::string& path, Bytes* out) const;
  // the decoded children (PACH items) of a path
  bool children(const std::string& path, std::vector<std::pair<std::string, Bytes>>* out) const;

 private:
  void parse();
  Bytes data_;
  std::vector<Entry> entries_;
};

// --- JBOY models -----------------------------------------------------------
struct Bone {
  std::string name;
  Vec3 pos;        // local
  Vec3 rot;        // euler
  int parent = -1;
  std::array<uint8_t, 0x50> raw{};
};
struct Mesh {
  uint32_t offset = 0;             // record offset in the data
  uint32_t vertex_count = 0, weights = 0;
  std::vector<uint32_t> palette;   // bone index + 1
  uint32_t influences = 0;         // +0x5C
  uint32_t group = 0;              // +0x60
  uint32_t vertex_block = 0, weight_block = 0, uv_block = 0, extra_block = 0;
  uint32_t group74 = 0;            // Real Steel: +0x74
  uint32_t param_count = 0, param_table = 0, strip_table = 0;
};
struct Model {
  explicit Model(Bytes data);
  Bytes data;
  bool pr = false;                 // Pacific Rim layout (0xB8-byte meshes)
  uint32_t bone_table = 0, bone_count = 0, texture_table = 0, texture_count = 0, group_table = 0, group_count = 0;
  std::vector<Mesh> meshes;
  std::vector<Bone> bones;
  std::vector<std::string> group_names;
  // per vertex (x, y, z, nx, ny, nz) + colour
  void vertices(const Mesh& m, std::vector<std::array<double, 6>>* v, std::vector<uint32_t>* colour) const;
  std::vector<std::array<double, 2>> uvs(const Mesh& m) const;
  std::vector<std::vector<std::pair<int, double>>> skin(const Mesh& m) const;   // (bone, weight)
  std::vector<std::array<int, 3>> triangles(const Mesh& m) const;               // first strip
};
std::vector<std::array<int, 3>> strip_triangles(const std::vector<uint16_t>& s);

// --- textures --------------------------------------------------------------
struct Image {             // RGBA8
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};
struct DdsInfo {
  int w = 0, h = 0, mips = 1;
  std::string fourcc;
};
DdsInfo dds_info(const Bytes& d);
Image dds_decode(const Bytes& d);                   // top level
Bytes dds_header(const Bytes& tmpl, int w, int h, int mips, const char* fourcc);
std::vector<std::pair<std::array<int, 2>, Bytes>> dds_levels(const Bytes& d);
// texture pack items (character packs' 000A / 000C)
std::vector<std::pair<std::string, Bytes>> texpack_read(const Bytes& d);
Bytes texpack_write(const std::vector<std::pair<std::string, Bytes>>& t);
using AlphaMap = double (*)(double);
Bytes dxt5_from(const Bytes& colour_dxt1, const Bytes& alpha_dxt5, AlphaMap map);
Bytes dds_drop_top(const Bytes& d, int levels);
Bytes dxt5_remap_alpha(const Bytes& d, AlphaMap map);
Bytes encode_red_dxt5(const std::vector<double>& red, int w, int h, const Bytes& tmpl);
Bytes constant_dxt1(int w, int h, std::array<int, 3> rgb, const Bytes& tmpl);
Bytes encode_dxt5(const Image& img, const Bytes& tmpl);   // one level

}  // namespace rs::primport
