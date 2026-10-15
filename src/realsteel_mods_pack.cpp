// realsteel - mod robots loaded by the game itself. See the header.
//
// Port of tools/mods/build_part_package.py and the parts of tools/pac/paclib.py
// it needs (Yuke's EPAC archives, PACH containers, BPE blocks, the archive
// index files). The Python stays the reference: the package written here is
// byte for byte what the Python writes (checked), apart from the catalog's
// time stamp.

#include "realsteel_mods_pack.h"

#include "realsteel_mods_build.h"
#include "realsteel_netplay.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

REXCVAR_DEFINE_STRING(realsteel_mods_dir, "", "Real Steel",
                      "Folder with robot mods (default: mods next to the executable)");

namespace realsteel::mods {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr const char* kTitle = "584111E0";
constexpr const char* kPackageName = "4D4F44000000000000000000000000000000005253";  // 42 chars
constexpr uint32_t kPackageNumber = 0x16;
constexpr uint32_t kDataVersion = 7;
constexpr const char* kSourcePrefix = "947F";
constexpr const char* kGroup = "0003";          // the 3000-series part group
constexpr const char* kFilePrefix = "DLC04";
constexpr uint32_t kSourcePart = 0xBBF;
constexpr int kIndexSpace = 960;                 // robot indexes 0..959: part ids 3008..3967+          // Basenja: the set every mod robot is cloned from
const char* const kSlots[] = {"BODY", "LEG", "HEAD", "L_ARM", "R_ARM", "L_GLOVE", "R_GLOVE",
                              "GENERATOR", "OIL", "MOTHERBOARD"};

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// What the player is told about robots that were not loaded (shown on screen
// after start-up by the overlay, and logged).
std::mutex g_notice_mutex;
std::vector<std::string> g_notices;

void Notice(const std::string& text) {
  REXLOG_WARN("[mods] {}", text);
  std::lock_guard<std::mutex> lock(g_notice_mutex);
  g_notices.push_back(text);
}

// --- little helpers --------------------------------------------------------

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void put_le32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void put_le16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void app_le32(Bytes& b, uint32_t v) { uint8_t t[4]; put_le32(t, v); b.insert(b.end(), t, t + 4); }
void app_le16(Bytes& b, uint32_t v) { uint8_t t[2]; put_le16(t, v); b.insert(b.end(), t, t + 2); }
void app_be32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }
void app_be16(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 8), uint8_t(v)}); }
void app(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }
void app(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
void pad_to(Bytes& b, size_t align) { while (b.size() % align) b.push_back(0); }

Bytes read_file(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Failure("cannot read " + path.string());
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const Bytes& data) {
  fs::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  if (!f) throw Failure("cannot write " + path.string());
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

std::string hex4(uint32_t v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%04X", v);
  return b;
}

std::string hex3(uint32_t v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%03X", v);
  return b;
}

// --- BPE blocks ------------------------------------------------------------

bool is_bpe(const Bytes& d) { return d.size() >= 16 && std::memcmp(d.data(), "BPE ", 4) == 0; }

// Philip Gage's byte-pair encoding as the game stores it (tools/pac/yukes_bpe.py).
Bytes bpe_decode(const Bytes& d) {
  const uint32_t csize = le32(&d[8]), usize = le32(&d[12]);
  const uint8_t* src = d.data() + 16;
  const size_t n = std::min<size_t>(csize, d.size() - 16);
  Bytes out;
  out.reserve(usize);
  size_t p = 0;
  while (p < n && out.size() < usize) {
    uint8_t left[256], right[256] = {0};
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i);
    int count = src[p++];
    int c = 0;
    while (true) {
      if (count > 127) {
        c += count - 127;
        count = 0;
      }
      if (c == 256) break;
      for (int k = 0; k <= count; ++k) {
        if (p >= n) throw Failure("bad BPE block");
        left[c] = src[p++];
        if (c != left[c]) right[c] = src[p++];
        ++c;
      }
      if (c == 256) break;
      count = src[p++];
    }
    const size_t size = src[p] | (src[p + 1] << 8);
    p += 2;
    const size_t end = p + size;
    std::vector<uint8_t> stack;
    while (true) {
      int ch;
      if (!stack.empty()) {
        ch = stack.back();
        stack.pop_back();
      } else if (p < end) {
        ch = src[p++];
      } else {
        break;
      }
      if (ch == left[ch]) {
        out.push_back(uint8_t(ch));
      } else {
        stack.push_back(right[ch]);
        stack.push_back(left[ch]);
      }
    }
  }
  out.resize(std::min<size_t>(out.size(), usize));
  return out;
}

// A BPE block with identity tables: no compression, but the game's reader
// accepts it (paclib.bpe_store).
Bytes bpe_store(const Bytes& data) {
  Bytes body;
  for (size_t i = 0; i < data.size(); i += 0x1000) {
    const size_t n = std::min<size_t>(0x1000, data.size() - i);
    body.insert(body.end(), {0xFF, 0x80, 0xFE});
    app_le16(body, uint32_t(n));
    body.insert(body.end(), data.begin() + i, data.begin() + i + n);
  }
  Bytes out;
  app(out, std::string("BPE "));
  app_le32(out, 0x100);
  app_le32(out, uint32_t(body.size()));
  app_le32(out, uint32_t(data.size()));
  app(out, body);
  return out;
}

// --- PACH trees ------------------------------------------------------------

struct Node {
  std::string name;              // entry name (EPAC) or %04X item id (PACH)
  Bytes raw;                     // as stored in the parent
  bool packed = false;           // raw is a BPE block
  Bytes data;                    // decoded
  bool has_children = false;
  std::vector<std::unique_ptr<Node>> children;
  size_t align = 16;             // item alignment inside the container
  bool dirty = false;
  Node* parent = nullptr;
  uint32_t sector = 0, units = 0;  // where Archive::save put it

  explicit Node(std::string n, Bytes r) : name(std::move(n)), raw(std::move(r)) {
    packed = is_bpe(raw);
    data = packed ? bpe_decode(raw) : raw;
    parse_children();
  }

  void parse_children() {
    has_children = false;
    children.clear();
    if (data.size() < 8 || std::memcmp(data.data(), "PACH", 4) != 0) return;
    const uint32_t count = le32(&data[4]);
    const size_t base = 8 + size_t(count) * 12;
    if (count > 0x4000 || base > data.size()) return;
    std::vector<std::unique_ptr<Node>> items;
    size_t a = 16;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t id = le32(&data[8 + i * 12]), off = le32(&data[12 + i * 12]), size = le32(&data[16 + i * 12]);
      if (base + size_t(off) + size > data.size()) return;
      if ((base + off) % 16) a = 4;
      items.push_back(std::make_unique<Node>(hex4(id), Bytes(data.begin() + base + off, data.begin() + base + off + size)));
    }
    has_children = true;
    align = a;
    children = std::move(items);
    for (auto& c : children) c->parent = this;
  }

  void touch() {
    for (Node* n = this; n != nullptr; n = n->parent) n->dirty = true;
  }

  void set_data(Bytes d) {
    data = std::move(d);
    has_children = false;
    children.clear();
    touch();
  }

  Node* child(const std::string& n) {
    for (auto& c : children)
      if (c->name == n) return c.get();
    return nullptr;
  }

  Bytes build_data() {
    if (!has_children) return data;
    Bytes table, body;
    const size_t base = 8 + children.size() * 12;
    for (auto& c : children) {
      Bytes r = c->build();
      while ((base + body.size()) % align) body.push_back(0);
      app_le32(table, uint32_t(std::strtoul(c->name.c_str(), nullptr, 16)));
      app_le32(table, uint32_t(body.size()));
      app_le32(table, uint32_t(r.size()));
      app(body, r);
    }
    Bytes out;
    app(out, std::string("PACH"));
    app_le32(out, uint32_t(children.size()));
    app(out, table);
    app(out, body);
    return out;
  }

  Bytes build() {
    if (!dirty) return raw;
    Bytes d = build_data();
    return packed ? bpe_store(d) : d;
  }

  template <typename F>
  void walk(F&& f) {
    f(*this);
    for (auto& c : children) c->walk(f);
  }
};

// --- EPAC archives ---------------------------------------------------------

constexpr size_t kDataStart = 0x4000;

struct Group {
  std::string name;  // 4 characters
  std::vector<std::unique_ptr<Node>> nodes;
};

struct Archive {
  std::vector<Group> groups;
  uint32_t kind = 7;
  Bytes trailer;

  explicit Archive(const fs::path& path) {
    Bytes d = read_file(path);
    if (d.size() < kDataStart || std::memcmp(d.data(), "EPAC", 4) != 0) throw Failure("not an EPAC archive: " + path.string());
    const uint32_t dir_size = le32(&d[4]), data_size = le32(&d[8]);
    kind = le32(&d[12]);
    trailer.assign(d.begin() + std::min(d.size(), kDataStart + size_t(data_size)), d.end());
    size_t off = 0x800;
    const size_t end = 0x800 + dir_size;
    while (off < end) {
      Group g;
      g.name.assign(reinterpret_cast<const char*>(&d[off]), 4);
      const uint32_t words = le32(&d[off + 4]);
      off += 12;
      for (uint32_t i = 0; i < words / 3; ++i) {
        std::string name(reinterpret_cast<const char*>(&d[off]), 4);
        const uint32_t sector = le32(&d[off + 4]), size = le32(&d[off + 8]);
        const size_t start = kDataStart + size_t(sector) * 0x800;
        if (start + size_t(size) * 0x100 > d.size()) throw Failure("truncated archive: " + path.string());
        g.nodes.push_back(std::make_unique<Node>(name, Bytes(d.begin() + start, d.begin() + start + size_t(size) * 0x100)));
        off += 12;
      }
      groups.push_back(std::move(g));
    }
  }

  Group* group(const std::string& name) {
    for (auto& g : groups) {
      std::string n = g.name;
      while (!n.empty() && n.back() == ' ') n.pop_back();
      if (n == name) return &g;
    }
    return nullptr;
  }

  // "/BATS/INIT/001D/0003/0001"
  Node* find(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/'))
      if (!item.empty()) parts.push_back(item);
    if (parts.size() < 2) return nullptr;
    Group* g = group(parts[0]);
    if (g == nullptr) return nullptr;
    Node* node = nullptr;
    for (auto& n : g->nodes) {
      std::string name = n->name;
      while (!name.empty() && name.back() == ' ') name.pop_back();
      if (name == parts[1]) node = n.get();
    }
    for (size_t i = 2; node != nullptr && i < parts.size(); ++i) node = node->child(parts[i]);
    return node;
  }

  Bytes save(const fs::path& path) {
    Bytes directory, body;
    for (auto& g : groups) {
      app(directory, g.name);
      app_le32(directory, uint32_t(g.nodes.size() * 3));
      app_le32(directory, 0);
      for (auto& n : g.nodes) {
        Bytes raw = n->build();
        pad_to(raw, 0x100);
        n->sector = uint32_t(body.size() / 0x800);
        n->units = uint32_t(raw.size() / 0x100);
        app(directory, n->name);
        app_le32(directory, n->sector);
        app_le32(directory, n->units);
        app(body, raw);
        pad_to(body, 0x800);
      }
    }
    if (directory.size() > kDataStart - 0x800) throw Failure("archive directory too large");
    Bytes out;
    app(out, std::string("EPAC"));
    app_le32(out, uint32_t(directory.size()));
    app_le32(out, uint32_t(body.size()));
    app_le32(out, kind);
    out.resize(0x800, 0);
    app(out, directory);
    out.resize(kDataStart, 0);
    app(out, body);
    app(out, trailer);
    write_file(path, out);
    return out;
  }

  // Its section of an index file (after save).
  Bytes index_section() const {
    Bytes out;
    for (auto& g : groups) {
      app(out, g.name);
      app_be32(out, uint32_t(g.nodes.size() * 3) << 20);
      app_be32(out, 0);
      for (auto& n : g.nodes) {
        app(out, n->name);
        app_be32(out, n->sector);
        app_be32(out, n->units);
      }
    }
    return out;
  }
};

// --- index files (catalog_hd.arc / catalog_sd.arc) --------------------------

std::vector<Bytes> arc_sections(const Bytes& d) {
  if (d.size() < 4 || d[0] != 0xFF || d[1] != 0 || d[2] != 0 || d[3] != 0) throw Failure("bad index file");
  std::vector<Bytes> sections;
  size_t pos = 4, start = 4;
  while (pos + 4 <= d.size()) {
    if (d[pos] == 0xFF && d[pos + 1] == 0xFF) {
      sections.emplace_back(d.begin() + start, d.begin() + pos);
      if (d[pos + 2] == 0xFF && d[pos + 3] == 0xFF) return sections;
      pos += 4;
      start = pos;
      continue;
    }
    const uint32_t head = be32(&d[pos + 4]);
    const size_t name_len = (head & 0x1000) ? 8 : 4;
    pos += 12 + size_t(head >> 20) / (2 + name_len / 4) * (name_len + 8);
  }
  sections.emplace_back(d.begin() + start, d.begin() + std::min(pos, d.size()));
  return sections;
}

Bytes arc_join(const std::vector<Bytes>& sections) {
  Bytes out = {0xFF, 0, 0, 0};
  for (size_t i = 0; i < sections.size(); ++i) {
    if (i) {
      app_be16(out, 0xFFFF);
      app_be16(out, uint32_t(i));
    }
    app(out, sections[i]);
  }
  out.insert(out.end(), {0xFF, 0xFF, 0xFF, 0xFF});
  return out;
}

// --- part tables -----------------------------------------------------------

// Appends a copy of record src_index to a counted table.
template <typename Patch>
void append_record(Node* node, size_t head_size, bool count_u16, size_t count_off, size_t src_index, Patch patch) {
  Bytes d = node->data;
  const uint32_t count = count_u16 ? le16(&d[count_off]) : le32(&d[count_off]);
  if (count == 0) throw Failure("empty table " + node->name);
  const size_t size = (d.size() - head_size) / count;
  if (head_size + count * size != d.size()) throw Failure("odd table size " + node->name);
  Bytes rec(d.begin() + head_size + src_index * size, d.begin() + head_size + (src_index + 1) * size);
  patch(rec);
  if (count_u16) put_le16(&d[count_off], count + 1); else put_le32(&d[count_off], count + 1);
  app(d, rec);
  node->set_data(std::move(d));
}

size_t part_index(Node* menu, uint32_t part_id) {
  const uint32_t count = le32(&menu->data[0]);
  for (uint32_t i = 0; i < count; ++i)
    if (le16(&menu->data[8 + i * 60 + 12]) == part_id) return i;
  throw Failure("part " + hex3(part_id) + " not in table " + menu->name);
}

uint32_t next_part_id(Node* menu) {
  const uint32_t count = le32(&menu->data[0]);
  uint32_t best = 0;
  for (uint32_t i = 0; i < count; ++i) best = std::max<uint32_t>(best, le16(&menu->data[8 + i * 60 + 12]));
  return best + 1;
}

using Ids = std::map<int, uint32_t>;                      // slot -> part id
using Strings = std::map<int, std::pair<uint32_t, uint32_t>>;  // slot -> (name, description)

// The part set a robot is based on: the group (0 base game, 1..3 the DLC
// groups) and the id its parts share in every slot table.
struct Base {
  int group = 3;
  uint32_t id = kSourcePart;
};

// Appends to table dst a copy of record src_index of table src (same layout).
// Stat overrides from robot.txt: slot -> (16-bit field index of the part's
// stats record -> value); see StatField and docs/modding.md "Stats".
using StatOverrides = std::map<int, std::map<int, int>>;
constexpr int kPriceField = 18;  // the price is 32-bit (fields 18 + 19)

template <typename Patch>
void append_record_from(Node* dst, Node* src, size_t head_size, bool count_u16, size_t count_off, size_t src_index, Patch patch) {
  const Bytes& sd = src->data;
  const uint32_t src_count = count_u16 ? le16(&sd[count_off]) : le32(&sd[count_off]);
  const size_t size = (sd.size() - head_size) / src_count;
  Bytes rec(sd.begin() + head_size + src_index * size, sd.begin() + head_size + (src_index + 1) * size);
  patch(rec);
  Bytes d = dst->data;
  const uint32_t count = count_u16 ? le16(&d[count_off]) : le32(&d[count_off]);
  if (head_size + count * size != d.size()) throw Failure("odd table size " + dst->name);
  if (count_u16) put_le16(&d[count_off], count + 1); else put_le32(&d[count_off], count + 1);
  app(d, rec);
  dst->set_data(std::move(d));
}

// Menu records (60 bytes), paint records (40), stat records (196): copies
// of the base set's records (from its own group) appended to group 3.
void add_part_tables(Node* dst_group, Node* dst_stats, Node* src_group, Node* src_stats, uint32_t src_id,
                     const Ids& new_ids, const Strings* strings, const uint8_t* paint,
                     const StatOverrides* stats = nullptr) {
  for (int slot = 1; slot <= 10; ++slot) {
    Node* src_menu = src_group->child(hex4(slot));
    Node* dst_menu = dst_group->child(hex4(slot));
    if (src_menu == nullptr || dst_menu == nullptr) throw Failure("menu table for slot missing");
    const size_t index = part_index(src_menu, src_id);
    const uint32_t new_id = new_ids.at(slot);
    append_record_from(dst_menu, src_menu, 8, false, 0, index, [&](Bytes& rec) {
      for (size_t off : {12u, 14u, 16u})
        if (le16(&rec[off]) == src_id) put_le16(&rec[off], new_id);
      if (strings) {
        put_le32(&rec[52], strings->at(slot).first);
        put_le32(&rec[56], strings->at(slot).second);
      }
    });
    append_record_from(dst_group->child(hex4(0x64 + slot)), src_group->child(hex4(0x64 + slot)), 4, false, 0, index,
                       [&](Bytes& rec) {
                         if (paint)
                           for (int zone = 0; zone < 4; ++zone) std::memcpy(&rec[13 + zone * 4], paint, 4);
                       });
    if (dst_stats) {
      append_record_from(dst_stats->child(hex4(slot)), src_stats->child(hex4(slot)), 8, true, 4, index, [&](Bytes& rec) {
        // The stats record: 98 little-endian 16-bit fields (196 bytes).
        // robot.txt may replace any of them; the rest stay the base set's.
        if (stats == nullptr) return;
        const auto it = stats->find(slot);
        if (it == stats->end()) return;
        for (const auto& [field, value] : it->second) {
          if (field == kPriceField) {
            if (rec.size() >= size_t(kPriceField) * 2 + 4) put_le32(&rec[size_t(kPriceField) * 2], uint32_t(std::max(0, value)));
          } else if (field >= 0 && size_t(field) * 2 + 2 <= rec.size()) {
            put_le16(&rec[size_t(field) * 2], uint16_t(std::clamp(value, 0, 65535)));
          }
        }
      });
    }
  }
}


// String tables that hold the part names: u32 0, u32 count, count x (offset,
// length incl. NUL, id, 0) sorted by id, then the texts.
std::map<uint32_t, Bytes> read_strings(const Node& node) {
  std::map<uint32_t, Bytes> out;
  const uint32_t count = le32(&node.data[4]);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t off = le32(&node.data[8 + i * 16]), len = le32(&node.data[12 + i * 16]), id = le32(&node.data[16 + i * 16]);
    out[id] = Bytes(node.data.begin() + off, node.data.begin() + off + len);
  }
  return out;
}

void write_strings(Node* node, const std::map<uint32_t, Bytes>& strings) {
  Bytes table, body;
  const size_t base = 8 + strings.size() * 16;
  for (auto& [id, text] : strings) {
    app_le32(table, uint32_t(base + body.size()));
    app_le32(table, uint32_t(text.size()));
    app_le32(table, id);
    app_le32(table, 0);
    app(body, text);
  }
  Bytes d;
  app_le32(d, 0);
  app_le32(d, uint32_t(strings.size()));
  app(d, table);
  app(d, body);
  node->set_data(std::move(d));
}

Strings add_strings(Archive& text, Node* menu_group, uint32_t src_id, const std::string& name) {
  Strings result;
  for (auto& g : text.groups)
    for (auto& entry : g.nodes)
      entry->walk([&](Node& node) {
        if (node.has_children || node.data.size() <= 24 || le32(&node.data[16]) != 10000) return;
        auto strings = read_strings(node);
        uint32_t next = strings.rbegin()->first + 1;
        result.clear();
        for (int slot = 1; slot <= 10; ++slot) {
          Node* menu = menu_group->child(hex4(slot));
          const uint8_t* rec = &menu->data[8 + part_index(menu, src_id) * 60];
          const uint32_t name_id = le32(rec + 52);
          std::string old(strings[name_id].begin(), strings[name_id].end());
          while (!old.empty() && old.back() == 0) old.pop_back();
          const size_t space = old.find(' ');
          const std::string text = space == std::string::npos ? name : name + old.substr(space);
          Bytes a(text.begin(), text.end());
          a.push_back(0);
          const std::string desc = "Mod part: " + text + " (based on " + old + ")";
          Bytes b(desc.begin(), desc.end());
          b.push_back(0);
          strings[next] = a;
          strings[next + 1] = b;
          result[slot] = {next, next + 1};
          next += 2;
        }
        write_strings(&node, strings);
      });
  if (result.empty()) throw Failure("no part-name string table found");
  return result;
}

// Finds the set whose body is called "<name> ..." in the update's tables
// (groups 0..3) and that has a part with that id in every slot.
Base resolve_base(const std::string& name, Archive& tu, Archive& text) {
  std::map<uint32_t, Bytes> strings;
  for (auto& g : text.groups)
    for (auto& entry : g.nodes)
      entry->walk([&](Node& node) {
        if (strings.empty() && !node.has_children && node.data.size() > 24 && le32(&node.data[16]) == 10000)
          strings = read_strings(node);
      });
  for (int group = 0; group <= 3; ++group) {
    Node* body = tu.find("/BATS/INIT/001D/" + hex4(group) + "/0001");
    if (body == nullptr) continue;
    const uint32_t count = le32(&body->data[0]);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* rec = &body->data[8 + i * 60];
      const uint32_t id = le16(rec + 12), name_id = le32(rec + 52);
      if (!strings.count(name_id)) continue;
      std::string text_name(strings[name_id].begin(), strings[name_id].end());
      std::string first = text_name.substr(0, text_name.find(' '));
      for (char& c : first) c = char(std::toupper(uint8_t(c)));
      if (first != name) continue;
      bool complete = true;
      for (int slot = 1; slot <= 10 && complete; ++slot) {
        Node* table = tu.find("/BATS/INIT/001D/" + hex4(group) + "/" + hex4(slot));
        complete = false;
        if (table == nullptr) break;
        const uint32_t n = le32(&table->data[0]);
        for (uint32_t k = 0; k < n; ++k)
          if (le16(&table->data[8 + k * 60 + 12]) == id) complete = true;
      }
      if (complete) return {group, id};
    }
  }
  throw Failure("no complete part set called " + name + " (see docs/modding.md for the choices)");
}

// The menu says "Xbox LIVE" where this port offers plain multiplayer: the
// main-menu item, that menu's title and the session items are renamed in
// every language table (string ids are the same in all of them).
void rename_menu_strings(Archive& text) {
  static const std::map<uint32_t, const char*> kNames = {
      {41, "MULTIPLAYER"}, {502, "MULTIPLAYER"},
      {16054, "FIND MATCHES"}, {16055, "HOST MATCH"}, {16056, "HOST PRIVATE\nMATCH"},
      {47, "QUIT GAME"}, {56, "Close Real Steel"},
      // Xbox LIVE leftovers: help pages, sign-in errors, the story ending
      {22002, "3. Multiplayer"},
      {22060, "-Online: set a lobby server in the settings (F1)."},
      {22062, " etiquette and be good to each other."},
      {22305, "-Both players need the same downloadable content "},
      {22306, " and mods to fight each other online."},
      {30200, "You have been disconnected."},
      {30541, "You have been disconnected."},
      {30315, "But this ain't the end.\nThere's even stronger opponents, all around the world.\n"
              "Search for your next rival in MULTIPLAYER, and stay sharp.\n\nGuess I'll see ya around."},
      {25115, "Party (not available)"}, {25123, "Invite party (not available)"},
      {30511, "Online play is off.\nTurn it on in the settings (F1 or L3+R3)."},
      {30512, "This profile is unable to play online.\nCheck Online play in the settings (F1 or L3+R3)."}};
  for (auto& g : text.groups)
    for (auto& entry : g.nodes)
      entry->walk([&](Node& node) {
        if (node.has_children || node.data.size() <= 24) return;
        auto strings = read_strings(node);
        bool changed = false;
        for (auto& [id, name] : kNames) {
          auto it = strings.find(id);
          if (it == strings.end()) continue;
          Bytes value(name, name + std::strlen(name));
          value.push_back(0);
          if (it->second != value) {
            it->second = value;
            changed = true;
          }
        }
        if (changed) write_strings(&node, strings);
      });
}

// --- the package -----------------------------------------------------------


struct Robot {
  std::string name;
  fs::path models;  // built/ folder, or the robot folder itself when built from source; empty: placeholder
  int index = 0;    // stable slot from the registry (ids = stock max + 1 + index)
  std::string base = "BASENJA";   // the stock set whose skeleton and stats it takes
  StatOverrides stats;            // fields of the copied stats records to change
  int wanted_index = -1;          // robot.txt "id = N": the index to use (0..959)
  bool from_source = false;       // models are made here from the folder's OBJ / PNG files
  std::string sig;                // this robot's part of the install signature
};

// The archives a base set's models come from: the base game's, or the
// official add-on content's (by part group).
const char* const kStockPrefix[] = {"ed", "DLC01_ed", "DLC03_ed", "DLC04_ed"};

fs::path stock_archive_path(int group, int n, const fs::path& game, const fs::path& src) {
  char f[32];
  std::snprintf(f, sizeof(f), "%s%02d.pac", kStockPrefix[group], n);
  return (group == 0 ? game / "pac" / "edit" : src / "pac" / "edit") / f;
}

// The newest official add-on package (the mod package is built on top of it).
fs::path source_package(const fs::path& root) {
  const fs::path packages = root / "0000000000000000" / kTitle / "00000002";
  fs::path src;
  std::error_code ec;
  if (fs::is_directory(packages, ec))
    for (auto& e : fs::directory_iterator(packages, ec))
      if (e.is_directory() && e.path().filename().string().rfind(kSourcePrefix, 0) == 0) src = e.path();
  return src;
}

// REALSTEEL_MODS_DUMP=<folder>: part files made from source are also written
// there (<robot>/ed<slot>_<kind>_<item>.bin), to compare with robot_folder.py.
void dump_built(const std::string& robot, int slot, char kind, const std::string& item, const Bytes& data) {
  const char* dir = std::getenv("REALSTEEL_MODS_DUMP");
  if (dir == nullptr || *dir == 0) return;
  char f[48];
  std::snprintf(f, sizeof(f), "ed%02d_%c_%s.bin", slot, kind, item.c_str());
  write_file(fs::path(dir) / robot / f, data);
}

void copy_or_link(const fs::path& from, const fs::path& to) {
  fs::create_directories(to.parent_path());
  std::error_code ec;
  fs::create_hard_link(from, to, ec);
  if (ec) fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

uint64_t filetime_now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  return (uint64_t(secs) + 11644473600ull) * 10000000ull;
}

void build_package(const fs::path& root, const fs::path& game, const fs::path& update_in, const fs::path& update_out,
                   const std::vector<Robot>& robots, const std::string& signature) {
  const fs::path base = root / "0000000000000000" / kTitle;
  const fs::path packages = base / "00000002";
  const fs::path src = source_package(root);
  if (src.empty() && !robots.empty())
    throw Failure("the official DLC is not installed (package " + std::string(kSourcePrefix) + "... missing in " + packages.string() + ")");
  const fs::path out = packages / kPackageName;
  const fs::path header_path = base / "Headers" / "00000002" / (std::string(kPackageName) + ".header");
  fs::remove_all(out);
  if (robots.empty()) fs::remove(header_path);

  Archive tu(update_in / "pac" / "misc.pac");
  Archive text(update_in / "pac" / "string.pac");
  rename_menu_strings(text);
  if (!robots.empty()) {
    Archive misc(src / "pac" / "misc.pac");
    Archive edit(src / "pac" / "edit" / (std::string(kFilePrefix) + "_edit.pac"));
    std::map<int, std::unique_ptr<Archive>> eds;
    for (int n = 1; n <= 7; ++n) {
      char f[32];
      std::snprintf(f, sizeof(f), "%s_ed%02d.pac", kFilePrefix, n);
      eds[n] = std::make_unique<Archive>(src / "pac" / "edit" / f);
    }
    Archive dlc(src / "pac" / "DLC.pac");
    Node* bind = dlc.find("/DLCS/BIND/0000");
    if (bind == nullptr) throw Failure("DLC.pac has no bind table");
    Bytes bind_data = bind->data;
    uint32_t rows = le32(&bind_data[0]);
    Bytes entries;
    const uint8_t paint[4] = {255, 255, 255, 255};  // own textures show at full brightness
    Ids stock_max;   // slot -> highest part id in the untouched tables
    for (int slot = 1; slot <= 10; ++slot) {
      Node* menu = misc.find(std::string("/BATS/INIT/001D/") + kGroup + "/" + hex4(slot));
      if (menu == nullptr) throw Failure("misc.pac part tables missing");
      stock_max[slot] = next_part_id(menu) - 1;
    }

    // the stock archives the base sets' models come from, loaded when needed
    std::map<std::string, std::unique_ptr<Archive>> stock;
    auto stock_archive = [&](int group, int n) -> Archive* {
      const fs::path path = stock_archive_path(group, n, game, src);
      if (group == 3) return eds[n].get();
      if (!stock.count(path.string())) stock[path.string()] = std::make_unique<Archive>(path);
      return stock[path.string()].get();
    };

    // Every robot's part ids by slot, for the clean-up of removed mods at the
    // next start (mods_ids.txt: "<index> <id slot 1> ... <id slot 10> <name>").
    std::string ids_text;
    for (const Robot& robot : robots) {
      const int number = robot.index + 1;
      // the catalog's set number indexes a 20-entry table in the game (any
      // larger value corrupts memory), so robots share set numbers 1..19
      const uint32_t set_no = uint32_t(robot.index % 19) + 1;
      const Base base = resolve_base(robot.base, tu, text);
      const std::string src_group = hex4(base.group);
      Ids ids;
      for (int slot = 1; slot <= 10; ++slot) ids[slot] = stock_max[slot] + 1 + uint32_t(robot.index);
      for (const char* section : {"BATS", "BATH"}) {
        const std::string p = std::string("/") + section + "/INIT/";
        Node* menu = misc.find(p + "001D/" + kGroup);
        Node* stats = misc.find(p + "001E/" + kGroup);
        if (menu == nullptr || stats == nullptr) throw Failure("misc.pac part tables missing");
        add_part_tables(menu, stats, misc.find(p + "001D/" + src_group), misc.find(p + "001E/" + src_group), base.id, ids, nullptr, paint,
                        &robot.stats);
      }
      add_part_tables(edit.find("/EDIT/MENU/0000"), nullptr, misc.find("/BATS/INIT/001D/" + src_group), nullptr, base.id, ids, nullptr, paint);
      const Strings strings = add_strings(text, tu.find("/BATS/INIT/001D/" + src_group), base.id, robot.name);
      for (const char* section : {"BATS", "BATH"}) {
        const std::string p = std::string("/") + section + "/INIT/";
        add_part_tables(tu.find(p + "001D/" + kGroup), tu.find(p + "001E/" + kGroup), tu.find(p + "001D/" + src_group),
                        tu.find(p + "001E/" + src_group), base.id, ids, &strings, paint, &robot.stats);
      }

      // models and textures: a copy of the base set's entries (from its own
      // archives) with the robot's files dropped in - made here from its OBJ /
      // PNG sources, each from the base set's entry it replaces, or (older
      // mods) read from its built/ folder (ed<slot>_<kind>_<item>.bin)
      std::unique_ptr<RobotSource> source;
      if (robot.from_source && !robot.models.empty()) {
        source = std::make_unique<RobotSource>(robot.models);
        const std::vector<std::string> problems = source->Check();
        if (!problems.empty()) throw Failure(robot.name + ": " + problems.front());
      }
      for (int n = 1; n <= 7; ++n) {
        char gname[8];
        std::snprintf(gname, sizeof(gname), "ED%02d", n);
        Group* g = eds[n]->group(gname);
        Group* sg = stock_archive(base.group, n)->group(gname);
        if (g == nullptr || sg == nullptr) throw Failure("part archive without its group");
        for (char kind : {'M', 'T'}) {
          const std::string src_name = kind + hex3(base.id), new_name = kind + hex3(ids[n]);
          Node* original = nullptr;
          for (auto& x : sg->nodes)
            if (x->name == src_name) original = x.get();
          if (original == nullptr) throw Failure("source part " + src_name + " missing");
          auto copy_node = std::make_unique<Node>(new_name, original->raw);
          for (auto& child : copy_node->children) {
            if (robot.models.empty()) break;             // placeholder: the stock models stay
            if (source) {
              const uint32_t item = uint32_t(std::strtoul(child->name.c_str(), nullptr, 16));
              std::optional<Bytes> made = source->Build(n, kind, item, child->data);
              if (made) {
                dump_built(robot.name, n, kind, child->name, *made);
                child->set_data(std::move(*made));
              }
              continue;
            }
            char f[48], legacy[48];
            std::snprintf(f, sizeof(f), "ed%02d_%c_%s.bin", n, kind, child->name.c_str());
            std::snprintf(legacy, sizeof(legacy), "ed%02d_%cBBF_%s.bin", n, kind, child->name.c_str());
            for (const char* candidate : {f, legacy}) {
              const fs::path file = robot.models / candidate;
              if (fs::is_regular_file(file)) {
                child->set_data(read_file(file));
                break;
              }
            }
          }
          auto pos = g->nodes.begin();
          while (pos != g->nodes.end() && (*pos)->name < new_name) ++pos;
          g->nodes.insert(pos, std::move(copy_node));
        }
      }

      // bind rows (one per part plus the set) and catalog entries; licence bit
      // 0 for every entry, so any number of robots works
      const uint32_t first_row = rows;
      for (int slot = 0; slot < 10; ++slot) {
        app_le32(bind_data, 0x7E000000u | (uint32_t(number) << 16) | (uint32_t(slot) << 8));
        app_le16(bind_data, 0x0B + slot);
        app_le16(bind_data, ids[slot + 1]);
        bind_data.resize(bind_data.size() + 22, 0);
        app_le16(bind_data, 9999);
      }
      app_le32(bind_data, 0x7E000A00u | (uint32_t(number) << 16));
      app_le16(bind_data, 0x15);
      for (int n = 1; n <= 10; ++n) app_le16(bind_data, ids[n]);
      bind_data.resize(bind_data.size() + 4, 0);
      app_le16(bind_data, 9999);
      rows += 11;
      auto entry = [&](uint32_t type, uint32_t row, uint32_t item, uint32_t x, const std::string& label) {
        app_le16(entries, 0x40);
        app_le16(entries, uint32_t(entries.size() / 0x40) + 1);
        app_le16(entries, type);
        app_le16(entries, kPackageNumber);
        app_le16(entries, 0);
        app_le16(entries, row);
        app_le16(entries, 0);
        app_le16(entries, 0);
        app_le32(entries, item);
        app_le32(entries, x);
        entries.resize(entries.size() + 8, 0);
        std::string l = label.substr(0, 31);
        l.resize(32, 0);
        app(entries, l);
      };
      for (int slot = 0; slot < 10; ++slot) {
        char label[48];
        std::snprintf(label, sizeof(label), "MOD_ASM_%03d_%s", number, kSlots[slot]);
        entry(5, first_row + slot, 1000000 + (slot + 1) * 10000 + ids[slot + 1], set_no * 100 + 0x0B + slot, label);
      }
      char label[48];
      std::snprintf(label, sizeof(label), "MOD_ASM_%03d_SET", number);
      entry(10, first_row + 10, set_no, 0x15, label);
      ids_text += std::to_string(robot.index);
      for (int slot = 1; slot <= 10; ++slot) ids_text += ' ' + std::to_string(ids[slot]);
      ids_text += ' ' + robot.name + '\n';
      REXLOG_INFO("[mods] robot {}{} (based on {}): body {:X} head {:X}", robot.name,
                  robot.models.empty() ? " (placeholder, folder removed)" : "", robot.base, ids[1], ids[3]);
    }
    {
      std::ofstream ids_file(root / "mods_ids.txt", std::ios::binary);
      ids_file << ids_text;
    }
    put_le32(&bind_data[0], rows);
    put_le16(&bind_data[4], kPackageNumber);
    put_le16(&bind_data[6], kDataVersion);
    bind->set_data(bind_data);

    // files: changed archives rebuilt, the rest linked or copied
    std::map<std::string, Archive*> changed = {{"pac/misc.pac", &misc},
                                               {std::string("pac/edit/") + kFilePrefix + "_edit.pac", &edit},
                                               {"pac/DLC.pac", &dlc}};
    for (int n = 1; n <= 7; ++n) {
      char f[32];
      std::snprintf(f, sizeof(f), "pac/edit/%s_ed%02d.pac", kFilePrefix, n);
      changed[f] = eds[n].get();
    }
    for (auto& e : fs::recursive_directory_iterator(src)) {
      if (!e.is_regular_file()) continue;
      std::string rel = fs::relative(e.path(), src).generic_string();
      const fs::path target = out / rel;
      if (changed.count(rel)) {
        changed[rel]->save(target);
      } else if (rel != "info/catalog.dlc" && rel != "info/catalog_hd.arc" && rel != "info/catalog_sd.arc") {
        copy_or_link(e.path(), target);
      }
    }

    // catalog: our entries + the source package's file table
    const Bytes cat = read_file(src / "info" / "catalog.dlc");
    const uint32_t n_entries = le16(&cat[12]), n_files = le16(&cat[14]);
    const Bytes file_table(cat.begin() + 24 + n_entries * 0x40, cat.end());
    Bytes catalog;
    app(catalog, std::string("DLCC"));
    app_le16(catalog, 2);
    app_le16(catalog, 0x18);
    catalog.push_back(uint8_t(kPackageNumber));
    catalog.push_back(uint8_t(kDataVersion));
    app_le16(catalog, 2);
    app_le16(catalog, uint32_t(entries.size() / 0x40));
    app_le16(catalog, n_files);
    const uint64_t ft = filetime_now();
    app_le32(catalog, uint32_t(ft));
    app_le32(catalog, uint32_t(ft >> 32));
    app(catalog, entries);
    app(catalog, file_table);
    write_file(out / "info" / "catalog.dlc", catalog);

    // index files: the game locates archive entries through these
    std::map<std::string, std::pair<int, int>> indices;
    for (size_t pos = 0, i = 0; i < n_files; ++i) {
      const uint32_t size = le16(&file_table[pos]);
      const int hd = int16_t(le16(&file_table[pos + 2])), sd = int16_t(le16(&file_table[pos + 4]));
      const uint32_t length = file_table[pos + 6];
      std::string path(reinterpret_cast<const char*>(&file_table[pos + 7]), length);
      while (!path.empty() && path.back() == 0) path.pop_back();
      for (char& c : path) c = c == '\\' ? '/' : char(std::tolower(uint8_t(c)));
      indices[path] = {hd, sd};
      pos += size;
    }
    for (int which = 0; which < 2; ++which) {
      const char* arc_name = which == 0 ? "catalog_hd.arc" : "catalog_sd.arc";
      std::vector<Bytes> sections = arc_sections(read_file(src / "info" / arc_name));
      for (auto& [rel, archive] : changed) {
        std::string key = rel;
        for (char& c : key) c = char(std::tolower(uint8_t(c)));
        const auto it = indices.find(key);
        if (it == indices.end()) throw Failure("file table has no entry for " + rel);
        const int index = which == 0 ? it->second.first : it->second.second;
        if (index >= 0 && size_t(index) < sections.size()) sections[index] = archive->index_section();
      }
      write_file(out / "info" / arc_name, arc_join(sections));
    }

    // the content manager's header file (XCONTENT_AGGREGATE_DATA + licence)
    Bytes header;
    app_be32(header, 1);
    app_be32(header, 2);
    const std::string display = "Real Steel Mods";
    Bytes utf16;
    for (char c : display) utf16.insert(utf16.end(), {0, uint8_t(c)});
    utf16.resize(256, 0);
    app(header, utf16);
    std::string pname = kPackageName;
    pname.resize(44, 0);
    app(header, pname);
    header.resize(0x138, 0);
    header.resize(0x140, 0);
    app_be32(header, uint32_t(std::strtoul(kTitle, nullptr, 16)));
    app_be32(header, 0);
    app_be32(header, 0xFFFFFFFF);
    write_file(header_path, header);

  }
  // the patched title update
  fs::remove_all(update_out);
  for (auto& e : fs::recursive_directory_iterator(update_in)) {
    if (!e.is_regular_file()) continue;
    const std::string rel = fs::relative(e.path(), update_in).generic_string();
    if (rel == "pac/misc.pac" || rel == "pac/string.pac") continue;
    copy_or_link(e.path(), update_out / rel);
  }
  tu.save(update_out / "pac" / "misc.pac");
  text.save(update_out / "pac" / "string.pac");
  if (!robots.empty()) write_file(out / "mods_signature.txt", Bytes(signature.begin(), signature.end()));
  write_file(update_out / "mods_signature.txt", Bytes(signature.begin(), signature.end()));
}

// --- the mods folder -------------------------------------------------------

fs::path exe_dir() {
#ifdef __ANDROID__
  // /proc/self/exe is the system's app_process; the app runs in its data
  // folder (RealsteelApp::AppRoot), so mods/ is found from there.
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (!ec) return cwd;
#endif
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  if (n > 0 && n < std::size(buf)) return fs::path(buf).parent_path();
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) return fs::path(std::string(buf, size_t(n))).parent_path();
#endif
  return fs::current_path();
}

// Stat keys of robot.txt sections ([head], [body], [arms], [arm_l], [arm_r],
// [fists], [fist_l], [fist_r], [legs], [all]) -> 16-bit field of the part's
// stats record. Checked against the Customize screen (2026-10-10): sub_hp,
// attack, defense, durability, energy and weight are shown as given;
// power_charge and weight_capacity feed the robot totals; the rest of the
// record (punch speed, mobility, resilience... as robot-wide figures) is
// reachable as field<N>.
int StatField(const std::string& key) {
  static const std::map<std::string, int> kFields = {
      {"sub_hp", 0},          {"hp", 0},
      {"power_charge", 1},    {"attack", 2},        {"defense", 3},
      {"durability", 6},      {"energy", 9},        {"weight_capacity", 13},
      {"weight", 15},         {"price", kPriceField},
  };
  const auto it = kFields.find(key);
  if (it != kFields.end()) return it->second;
  if (key.rfind("field", 0) == 0 && key.size() > 5) {
    const int n = std::atoi(key.c_str() + 5);
    if (n >= 0 && n < 98 && std::to_string(n) == key.substr(5)) return n;
  }
  return -1;
}

// Section name -> part slots (1 body, 2 legs, 3 head, 4/5 arms, 6/7 fists).
std::vector<int> StatSlots(const std::string& section) {
  static const std::map<std::string, std::vector<int>> kSections = {
      {"body", {1}},   {"legs", {2}},   {"head", {3}},
      {"arm_l", {4}},  {"arm_r", {5}},  {"arms", {4, 5}},
      {"fist_l", {6}}, {"fist_r", {7}}, {"fists", {6, 7}},
      {"all", {1, 2, 3, 4, 5, 6, 7}},
  };
  const auto it = kSections.find(section);
  return it == kSections.end() ? std::vector<int>() : it->second;
}

std::string read_name(const fs::path& robot_txt, std::string* base = nullptr, StatOverrides* stats = nullptr,
                      int* wanted_index = nullptr) {
  std::ifstream f(robot_txt);
  std::string line, name, section;
  while (std::getline(f, line)) {
    auto trim = [](std::string& s) {
      while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
      while (!s.empty() && std::isspace(uint8_t(s.front()))) s.erase(s.begin());
    };
    std::string head = line.substr(0, line.find('#'));
    trim(head);
    if (head.size() >= 2 && head.front() == '[' && head.back() == ']') {
      section = head.substr(1, head.size() - 2);
      trim(section);
      for (char& c : section) c = char(std::tolower(uint8_t(c)));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.find('#') < eq) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    trim(key);
    trim(value);
    for (char& c : key) c = char(std::tolower(uint8_t(c)));
    if (!section.empty() && key != "name" && key != "based_on" && key != "id") {
      const int field = StatField(key);
      const std::vector<int> slots = StatSlots(section);
      if (field < 0 || slots.empty()) {
        REXLOG_WARN("[mods] {}: [{}] {} is not a stat the game has (ignored)", robot_txt.string(), section, key);
      } else if (stats != nullptr) {
        for (int slot : slots) (*stats)[slot][field] = std::atoi(value.c_str());
      }
      continue;
    }
    if (key == "name") name = value;
    if (key == "id" && wanted_index != nullptr) {
      const int id = std::atoi(value.c_str());
      if (id >= 0 && id < kIndexSpace && std::to_string(id) == value) {
        *wanted_index = id;
      } else {
        REXLOG_WARN("[mods] {}: id must be a number from 0 to {} (ignored: {})", robot_txt.string(), kIndexSpace - 1, value);
      }
    }
    if (key == "based_on" && base != nullptr) {
      base->clear();
      for (char c : value)
        if (c > 32 && c < 127) base->push_back(char(std::toupper(uint8_t(c))));
    }
  }
  std::string clean;
  for (char c : name)
    if (c >= 32 && c < 127) clean.push_back(char(std::toupper(uint8_t(c))));
  return clean.substr(0, 14);
}

std::vector<Robot> scan(const fs::path& mods, std::string& signature) {
  std::vector<Robot> robots;
  if (!fs::is_directory(mods)) return robots;
  std::vector<fs::path> folders;
  for (auto& e : fs::directory_iterator(mods))
    if (e.is_directory() && e.path().filename().string()[0] != '.') folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  for (const fs::path& folder : folders) {
    const fs::path robot_txt = folder / "robot.txt";
    if (!fs::is_regular_file(robot_txt)) {
      REXLOG_WARN("[mods] {}: no robot.txt, skipped", folder.filename().string());
      continue;
    }
    std::string base = "BASENJA";
    StatOverrides stats;
    int wanted_index = -1;
    const std::string name = read_name(robot_txt, &base, &stats, &wanted_index);
    if (base.empty()) base = "BASENJA";
    // The robot's own files (OBJ / PNG, made into game files at install time),
    // or the built/ folder older mods came with.
    const fs::path built = folder / "built";
    const bool from_source = RobotSource::HasSource(folder);
    const bool has_built = fs::is_regular_file(built / "ed07_M_07D7.bin") || fs::is_regular_file(built / "ed07_MBBF_07D7.bin");
    if (name.empty()) {
      Notice(folder.filename().string() + " not loaded: its robot.txt needs a name (name = ...)");
      continue;
    }
    if (!from_source && !has_built) {
      Notice(name + " not loaded: its folder has no models (body.obj and the other parts)");
      continue;
    }
    std::string sig = name + " based on " + base + (from_source ? " from source" : "") + "\n";
    for (const auto& [slot, fields] : stats)
      for (const auto& [field, value] : fields)
        sig += "  stat " + std::to_string(slot) + ":" + std::to_string(field) + "=" + std::to_string(value) + "\n";
    std::vector<fs::path> files;
    if (from_source) {
      files = RobotSource::SourceFiles(folder);
    } else {
      for (auto& e : fs::directory_iterator(built))
        if (e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
      // (libc++ on Android keeps file times as __int128: narrow for to_string)
      const auto t = static_cast<long long>(fs::last_write_time(f).time_since_epoch().count());
      sig += f.filename().string() + " " + std::to_string(fs::file_size(f)) + " " + std::to_string(t) + "\n";
    }
    signature += sig;
    Robot robot{name, from_source ? folder : built, 0, base, stats};
    robot.wanted_index = wanted_index;
    robot.from_source = from_source;
    robot.sig = sig;
    robots.push_back(std::move(robot));
  }
  return robots;
}

// Each robot name keeps the same index for ever (mods_registry.txt in the
// user data folder), so its part ids never change when other mods come and
// go and a saved robot keeps finding its parts.
std::map<std::string, int> assign_indexes(const fs::path& registry, std::vector<Robot>& robots) {
  std::map<std::string, int> known;
  std::ifstream in(registry);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    int index;
    std::string name;
    if (ss >> index && std::getline(ss, name)) {
      while (!name.empty() && std::isspace(uint8_t(name.front()))) name.erase(name.begin());
      while (!name.empty() && std::isspace(uint8_t(name.back()))) name.pop_back();
      if (!name.empty()) known[name] = index;
    }
  }
  bool changed = false;
  // Explicit ids (robot.txt "id = N") come first and win: the same number on
  // every machine whatever else is installed. A clash between two folders
  // keeps the first (alphabetical) and hashes the other.
  for (size_t i = 0; i < robots.size(); ++i) {
    Robot& robot = robots[i];
    if (robot.wanted_index < 0) continue;
    for (size_t j = 0; j < i; ++j) {
      if (robots[j].wanted_index == robot.wanted_index) {
        REXLOG_WARN("[mods] {} and {} both claim id {}: {} keeps it, {} gets a number from its name",
                    robots[j].name, robot.name, robot.wanted_index, robots[j].name, robot.name);
        robot.wanted_index = -1;
        break;
      }
    }
    if (robot.wanted_index < 0) continue;
    for (auto it = known.begin(); it != known.end();) {
      if (it->second == robot.wanted_index && it->first != robot.name) {
        REXLOG_INFO("[mods] {} takes id {} from {} (which gets a number from its name)", robot.name,
                    robot.wanted_index, it->first);
        it = known.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    auto it = known.find(robot.name);
    if (it == known.end() || it->second != robot.wanted_index) {
      known[robot.name] = robot.wanted_index;
      changed = true;
    }
  }
  for (Robot& robot : robots) {
    auto it = known.find(robot.name);
    if (it == known.end()) {
      // The index comes from the name (FNV-1a), so two players who install
      // the same robot get the same part ids and see each other's robots
      // correctly online. Ids stay below 4000 (the next part group).
      uint32_t hash = 2166136261u;
      for (char c : robot.name) hash = (hash ^ uint8_t(c)) * 16777619u;
      int index = int(hash % kIndexSpace);
      bool used = true;
      while (used) {
        used = false;
        for (auto& [n, i] : known)
          if (i == index) used = true;
        if (used) index = (index + 1) % kIndexSpace;
      }
      known[robot.name] = index;
      it = known.find(robot.name);
      changed = true;
    }
    robot.index = it->second;
  }
  if (changed) {
    fs::create_directories(registry.parent_path());
    std::ofstream out(registry);
    for (auto& [name, index] : known) out << index << ' ' << name << '\n';
  }
  return known;
}

std::string read_text(const fs::path& p);

// Removed mods nobody uses any more (2026-10-12). A removed robot stays as a
// placeholder so saves wearing its parts keep loading (plan section 27); once
// no saved robot wears any of its parts the placeholder goes too. The saves
// (<user data>/<profile>/584111E0/.../SaveData.Dat, big-endian) hold part
// records "u16 slot, u16 part id, u16 colour, u16 0": garage robots are ten
// of them (slots 1..10) plus six extra records, and an ownership table lists
// every set as ten records of that set's own ids. A part counts as used
// wherever its record appears except in its set's own ownership entry.
std::map<std::string, std::vector<uint32_t>> read_mod_ids(const fs::path& file) {
  std::map<std::string, std::vector<uint32_t>> out;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    int index;
    std::vector<uint32_t> ids(11, 0);
    if (!(ss >> index)) continue;
    bool ok = true;
    for (int slot = 1; slot <= 10 && ok; ++slot) ok = bool(ss >> ids[slot]);
    std::string name;
    if (!ok || !std::getline(ss, name)) continue;
    while (!name.empty() && std::isspace(uint8_t(name.front()))) name.erase(name.begin());
    while (!name.empty() && std::isspace(uint8_t(name.back()))) name.pop_back();
    if (!name.empty()) out[name] = ids;
  }
  return out;
}

std::vector<std::string> read_saves(const fs::path& root) {
  std::vector<std::string> saves;
  std::error_code ec;
  for (const auto& profile : fs::directory_iterator(root, ec)) {
    const std::string name = profile.path().filename().string();
    if (!profile.is_directory() || name.size() != 16 || name == "0000000000000000") continue;
    const fs::path title = profile.path() / kTitle;
    if (!fs::is_directory(title, ec)) continue;
    for (const auto& f : fs::recursive_directory_iterator(title, ec)) {
      std::string file = f.path().filename().string();
      std::transform(file.begin(), file.end(), file.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
      if (f.is_regular_file() && file == "savedata.dat") saves.push_back(read_text(f.path()));
    }
  }
  return saves;
}

// Byte ranges of a save that only list what the player owns (not what a
// robot wears): the set table - the long chain of ten-record set entries
// (8-byte records, entries 80 bytes apart; garage robots form at most a
// short chain) - and the owned-parts list (4-byte records "slot, id",
// hundreds back to back). Found in a test profile, 2026-10-12.
std::vector<std::pair<size_t, size_t>> ownership_ranges(const std::string& d) {
  auto u16 = [&](size_t at) { return uint32_t(uint8_t(d[at])) << 8 | uint8_t(d[at + 1]); };
  std::vector<std::pair<size_t, size_t>> ranges;
  // 10-slot runs of 8-byte records, chained 80 bytes apart.
  std::vector<size_t> runs;
  for (size_t at = 0; at + 80 <= d.size(); at += 2) {
    bool run = true;
    for (uint32_t s = 1; s <= 10 && run; ++s) run = u16(at + (s - 1) * 8) == s;
    if (run) runs.push_back(at);
  }
  for (size_t i = 0; i < runs.size();) {
    size_t j = i;
    while (j + 1 < runs.size() && runs[j + 1] == runs[j] + 80) ++j;
    if (j - i + 1 >= 10) ranges.push_back({runs[i], runs[j] + 80});
    i = j + 1;
  }
  // 4-byte "slot, id" records back to back: at least 20 in a row.
  for (size_t at = 0; at + 4 <= d.size();) {
    size_t end = at;
    while (end + 4 <= d.size() && u16(end) >= 1 && u16(end) <= 10 && u16(end + 2) != 0) end += 4;
    if ((end - at) / 4 >= 20) {
      ranges.push_back({at, end});
      at = end;
    } else {
      at += 2;
    }
  }
  return ranges;
}

// True when a part of the set |ids| (slot -> part id) appears anywhere in a
// save outside its ownership lists - a garage robot wears it. Unknown layouts
// err on the side of "in use", which keeps the placeholder as before.
bool parts_in_use(const std::vector<std::string>& saves, const std::vector<uint32_t>& ids) {
  for (const std::string& d : saves) {
    auto u16 = [&](size_t at) { return uint32_t(uint8_t(d[at])) << 8 | uint8_t(d[at + 1]); };
    const auto owned = ownership_ranges(d);
    for (size_t at = 0; at + 4 <= d.size(); at += 2) {
      const uint32_t slot = u16(at);
      if (slot < 1 || slot > 10 || u16(at + 2) != ids[slot]) continue;
      bool listed = false;
      for (const auto& [b, e] : owned) listed = listed || (at >= b && at < e);
      if (!listed) return true;
    }
  }
  return false;
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return f ? std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()) : std::string();
}

// Keeps out the robots that cannot be installed, telling the player why: an
// unknown base set, a base set whose add-on files are missing, models with
// problems. Results are remembered per robot (mods_checks.txt in the user
// data folder) until its files, the add-ons or the update change, so a
// normal start does not read every model again.
void check_robots(std::vector<Robot>& robots, const fs::path& root, const fs::path& game, const fs::path& update_in,
                  const fs::path& src) {
  const fs::path checks_file = root / "mods_checks.txt";
  std::map<std::string, std::string> cached;
  {
    std::ifstream in(checks_file);
    std::string line;
    while (std::getline(in, line)) {
      const size_t space = line.find(' ');
      if (space != std::string::npos) cached[line.substr(0, space)] = line.substr(space + 1);
    }
  }
  std::string stamp = src.filename().string();
  for (const fs::path& p : {update_in / "pac" / "misc.pac", update_in / "pac" / "string.pac"}) {
    std::error_code ec;
    stamp += " " + std::to_string(static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count()));
  }
  std::unique_ptr<Archive> tu, text;
  std::map<std::string, std::string> results;
  std::vector<Robot> kept;
  for (Robot& robot : robots) {
    uint64_t hash = 1469598103934665603ull;
    for (char c : robot.sig + stamp + " checks 1") hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    char key[24];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    std::string result;
    const auto found = cached.find(key);
    if (found != cached.end()) {
      result = found->second;
    } else {
      try {
        if (!tu) {
          tu = std::make_unique<Archive>(update_in / "pac" / "misc.pac");
          text = std::make_unique<Archive>(update_in / "pac" / "string.pac");
        }
        const Base base = resolve_base(robot.base, *tu, *text);
        for (int n = 1; n <= 7; ++n) {
          if (!fs::is_regular_file(stock_archive_path(base.group, n, game, src)))
            throw Failure("its base set " + robot.base + " needs add-on content (DLC) that is not installed");
        }
        if (robot.from_source) {
          RobotSource source(robot.models);
          const std::vector<std::string> problems = source.Check();
          if (!problems.empty()) {
            result = problems.front();
            if (problems.size() > 1) result += " (and " + std::to_string(problems.size() - 1) + " more problems, see the log)";
            for (const std::string& p : problems) REXLOG_WARN("[mods] {}: {}", robot.name, p);
          }
        }
        if (result.empty()) result = "ok";
      } catch (const std::exception& e) {
        result = e.what();
      }
    }
    for (char& c : result)
      if (c == '\n' || c == '\r') c = ' ';
    results[key] = result;
    if (result == "ok") {
      kept.push_back(std::move(robot));
    } else {
      Notice(robot.name + " not loaded: " + result);
    }
  }
  robots = std::move(kept);
  std::ofstream out(checks_file, std::ios::binary);
  for (const auto& [key, result] : results) out << key << ' ' << result << '\n';
}

}  // namespace

std::vector<std::string> Notices() {
  std::lock_guard<std::mutex> lock(g_notice_mutex);
  return g_notices;
}

void InstallOnce(rex::PathConfig& paths);

void Install(rex::PathConfig& paths) {
  // The first build after an update to this loader writes mods_ids.txt; with
  // it the clean-up of unused removed mods can run, so the install is done
  // once more (the second pass usually finds nothing to change).
  const fs::path ids_file = fs::path(paths.user_data_root) / "mods_ids.txt";
  const bool had_ids = fs::is_regular_file(ids_file);
  const fs::path update_in = paths.update_data_root;
  InstallOnce(paths);
  if (!had_ids && fs::is_regular_file(ids_file)) {
    paths.update_data_root = update_in;
    InstallOnce(paths);
  }
}

void InstallOnce(rex::PathConfig& paths) {
  try {
    const std::string dir = REXCVAR_GET(realsteel_mods_dir);
    const fs::path mods = dir.empty() ? exe_dir() / "mods" : fs::path(dir);
    std::string signature = "loader 10 ";   // bump when the package layout changes
    {
      std::lock_guard<std::mutex> lock(g_notice_mutex);
      g_notices.clear();
    }
    std::vector<Robot> robots = scan(mods, signature);
    const fs::path root = paths.user_data_root;
    if (!robots.empty()) {
      // The mod package is made from the newest official add-on package: no
      // add-ons, no mods (said once for all of them).
      const fs::path src = source_package(root);
      if (src.empty()) {
        std::string names;
        for (const Robot& r : robots) names += (names.empty() ? "" : ", ") + r.name;
        Notice("Mods not loaded (" + names + "): they need the official add-on content (DLC), which is not installed");
        return;
      }
      if (!paths.update_data_root.empty() && fs::is_regular_file(paths.update_data_root / "pac" / "misc.pac"))
        check_robots(robots, root, paths.game_data_root, paths.update_data_root, src);
    }
    const fs::path update_out = root / "update_mods";
    // A robot that was installed once stays in the registry: when its folder
    // is gone it is built as a placeholder (same ids and names, the stock
    // Basenja models), so a saved robot wearing its parts still loads.
    if (!robots.empty() || fs::is_regular_file(root / "mods_registry.txt")) {
      auto known = assign_indexes(root / "mods_registry.txt", robots);
      // Placeholders for removed robots - unless no saved robot wears their
      // parts: then the robot leaves the registry and the game for good.
      const auto mod_ids = read_mod_ids(root / "mods_ids.txt");
      const std::vector<std::string> saves = read_saves(root);
      if (!mod_ids.empty()) {
        REXLOG_INFO("[mods] clean-up: {} save(s) found in {}, part ids known for {} robot(s)",
                    saves.size(), root.string(), mod_ids.size());
      }
      // Which removed robots no save uses. A save that has seen the mod
      // package refuses to load once the package is gone altogether ("your
      // current game data cannot be used", 2026-10-12), while dropping some
      // of its robots is fine - so the last one always stays.
      std::vector<std::string> drop;
      size_t staying = 0;
      for (const auto& [name, index] : known) {
        bool present = false;
        for (const Robot& r : robots) present = present || r.name == name;
        const auto ids = mod_ids.find(name);
        if (!present && !saves.empty() && ids != mod_ids.end() && !parts_in_use(saves, ids->second)) {
          drop.push_back(name);
        } else {
          ++staying;
        }
      }
      if (staying == 0 && !drop.empty()) {
        std::string keep = drop.front();
        for (const std::string& n : drop)
          if (known[n] < known[keep]) keep = n;
        drop.erase(std::find(drop.begin(), drop.end(), keep));
        REXLOG_INFO("[mods] {} stays as a placeholder: saves need the mod package to exist", keep);
      }
      const bool pruned = !drop.empty();
      for (const std::string& name : drop) {
        REXLOG_INFO("[mods] {} was removed and no saved robot uses its parts: dropped", name);
        known.erase(name);
      }
      for (const auto& [name, index] : known) {
        bool present = false;
        for (const Robot& r : robots) present = present || r.name == name;
        if (!present) robots.push_back({name, fs::path(), index, "BASENJA"});
      }
      if (pruned) {
        std::ofstream out(root / "mods_registry.txt");
        for (auto& [name, index] : known) out << index << ' ' << name << '\n';
      }
      std::sort(robots.begin(), robots.end(), [](const Robot& x, const Robot& y) { return x.index < y.index; });
      for (const Robot& robot : robots)
        signature += robot.name + " #" + std::to_string(robot.index) + (robot.models.empty() ? " placeholder" : "") + "\n";
    }
    {
      std::string names;
      for (const Robot& robot : robots)
        if (!robot.models.empty()) names += robot.name + "\n";
      realsteel::netplay::SetModsSignature(names);
    }
    const fs::path package = root / "0000000000000000" / kTitle / "00000002" / kPackageName;
    if (paths.update_data_root.empty() || !fs::is_regular_file(paths.update_data_root / "pac" / "misc.pac")) {
      REXLOG_ERROR("[mods] the title update folder is missing, mods not installed");
      return;
    }
    // the source data is part of the signature so a changed DLC or update
    // rebuilds too
    for (const fs::path& p : {paths.update_data_root / "pac" / "misc.pac", paths.update_data_root / "pac" / "string.pac"})
      signature += p.filename().string() + " " + std::to_string(static_cast<long long>(fs::last_write_time(p).time_since_epoch().count())) + "\n";
    const bool package_stale = !robots.empty() && read_text(package / "mods_signature.txt") != signature;
    if (robots.empty() && fs::is_directory(package)) {
      REXLOG_INFO("[mods] no robots in {}: removing the mod package", mods.string());
      fs::remove_all(package);
    }
    if (package_stale || read_text(update_out / "mods_signature.txt") != signature) {
      const auto t0 = std::chrono::steady_clock::now();
      build_package(root, paths.game_data_root, paths.update_data_root, update_out, robots, signature);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      REXLOG_INFO("[mods] {} robot(s) installed from {} in {} ms", robots.size(), mods.string(), ms);
    } else {
      REXLOG_INFO("[mods] {} robot(s) from {} already installed", robots.size(), mods.string());
    }
    paths.update_data_root = update_out;
  } catch (const std::exception& e) {
    REXLOG_ERROR("[mods] not installed: {}", e.what());
  }
}

}  // namespace realsteel::mods
