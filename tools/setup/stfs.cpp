#include "stfs.h"

#include <cstring>
#include <fstream>

namespace rs::stfs {
namespace {

constexpr uint32_t kBlock = 0x1000;
constexpr uint32_t kPerL0 = 0xAA;     // data blocks per level-0 hash table
constexpr uint32_t kPerL1 = 0x70E4;   // data blocks per level-1 hash table
constexpr uint32_t kHeaderBytes = 0xA000;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t Be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
uint32_t Le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// UTF-16BE, NUL-terminated, to UTF-8 (names are plain text; anything odd
// becomes '?').
std::string Utf16BeToUtf8(const uint8_t* p, size_t bytes) {
  std::string out;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    const uint32_t c = (uint32_t(p[i]) << 8) | p[i + 1];
    if (c == 0) break;
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else if (c >= 0xD800 && c < 0xE000) {
      out.push_back('?');
    } else {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

Identity ParseIdentity(const uint8_t* head, size_t size) {
  Identity id;
  if (size < 0x491) return id;
  const std::string magic(reinterpret_cast<const char*>(head), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") return id;
  if (Be32(head + 0x3A9) != 0) return id;  // SVOD volume (disc image), not STFS
  id.stfs = true;
  id.magic = magic;
  id.content_type = Be32(head + 0x344);
  id.title_id = Be32(head + 0x360);
  id.display_name = Utf16BeToUtf8(head + 0x411, 0x80);
  return id;
}

}  // namespace

Identity Identify(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return Identity{};
  uint8_t head[0x491];
  f.read(reinterpret_cast<char*>(head), sizeof(head));
  if (f.gcount() != std::streamsize(sizeof(head))) return Identity{};
  return ParseIdentity(head, sizeof(head));
}

Package::~Package() {
  if (file_ != nullptr) std::fclose(file_);
}

bool Package::ReadAt(uint64_t offset, void* out, size_t size) {
#ifdef _WIN32
  if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
  if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
  return std::fread(out, 1, size, file_) == size;
}

bool Package::Open(const std::filesystem::path& file, std::string* error) {
  path_ = file;
#ifdef _WIN32
  file_ = _wfopen(file.c_str(), L"rb");
#else
  file_ = std::fopen(file.c_str(), "rb");
#endif
  if (file_ == nullptr) {
    if (error) *error = "cannot open " + file.string();
    return false;
  }
  std::vector<uint8_t> head(kHeaderBytes);
  if (!ReadAt(0, head.data(), head.size())) {
    if (error) *error = file.filename().string() + ": too short to be a package";
    return false;
  }
  identity_ = ParseIdentity(head.data(), head.size());
  if (!identity_.stfs) {
    if (error) *error = file.filename().string() + ": not an Xbox 360 package";
    return false;
  }
  header_size_ = Be32(head.data() + 0x340);
  read_only_ = (head[0x37B] & 1) != 0;
  table_blocks_ = Le16(head.data() + 0x37C);
  table_start_ = Le24(head.data() + 0x37E);
  total_blocks_ = Be32(head.data() + 0x395);
  data_start_ = (uint64_t(header_size_) + 0xFFF) & ~uint64_t(0xFFF);
  shift_ = read_only_ ? 0 : 1;
  return ReadTable(error);
}

uint64_t Package::DataOffset(uint32_t block) const {
  uint64_t n = block;
  const uint64_t pers[3] = {kPerL0, uint64_t(kPerL0) * kPerL0, uint64_t(kPerL0) * kPerL0 * kPerL0};
  for (uint64_t per : pers) {
    n += ((block + per) / per) << shift_;
    if (block < per) break;
  }
  return data_start_ + n * kBlock;
}

bool Package::NextBlock(uint32_t block, uint32_t* next) {
  // Level-0 hash table covering |block|; its entry's last three bytes chain
  // to the next block of the file.
  const uint32_t step0 = read_only_ ? 0xAB : 0xAC;
  uint64_t table = 0;
  if (block >= kPerL0) {
    table = uint64_t(block / kPerL0) * step0;
    table += uint64_t((block / kPerL1) + 1) << shift_;
    if (block >= kPerL1) table += uint64_t(1) << shift_;
  }
  const uint64_t offset = data_start_ + table * kBlock + uint64_t(block % kPerL0) * 0x18;
  uint8_t entry[0x18];
  if (!ReadAt(offset, entry, sizeof(entry))) return false;
  *next = Be24(entry + 0x15);
  return true;
}

bool Package::ReadBlocks(uint32_t start, uint32_t count, std::vector<uint8_t>& out,
                         std::string* error) {
  out.clear();
  out.reserve(size_t(count) * kBlock);
  uint32_t block = start;
  std::vector<uint8_t> buf(kBlock);
  for (uint32_t i = 0; i < count && block != 0xFFFFFF; ++i) {
    if (block >= total_blocks_) {
      if (error) *error = path_.filename().string() + ": block out of range (damaged package?)";
      return false;
    }
    if (!ReadAt(DataOffset(block), buf.data(), kBlock)) {
      if (error) *error = path_.filename().string() + ": read error";
      return false;
    }
    out.insert(out.end(), buf.begin(), buf.end());
    if (!NextBlock(block, &block)) {
      if (error) *error = path_.filename().string() + ": hash table read error";
      return false;
    }
  }
  return true;
}

bool Package::ReadTable(std::string* error) {
  std::vector<uint8_t> raw;
  if (!ReadBlocks(table_start_, table_blocks_, raw, error)) return false;
  entries_.clear();
  for (size_t i = 0; i + 0x40 <= raw.size(); i += 0x40) {
    const uint8_t* e = raw.data() + i;
    if (e[0] == 0) break;
    const uint8_t flags = e[0x28];
    Entry entry;
    entry.name.assign(reinterpret_cast<const char*>(e), flags & 0x3F);
    entry.directory = (flags & 0x80) != 0;
    entry.blocks = Le24(e + 0x29);
    entry.start = Le24(e + 0x2F);
    entry.parent = uint16_t(Be16(e + 0x32));
    entry.size = Be32(e + 0x34);
    entries_.push_back(std::move(entry));
  }
  for (Entry& e : entries_) {
    std::string path = e.name;
    uint16_t p = e.parent;
    size_t guard = 0;
    while (p != 0xFFFF && p < entries_.size() && guard++ < 64) {
      path = entries_[p].name + "/" + path;
      p = entries_[p].parent;
    }
    e.path = path;
  }
  return true;
}

uint64_t Package::total_bytes() const {
  uint64_t total = 0;
  for (const Entry& e : entries_) {
    if (!e.directory) total += e.size;
  }
  return total;
}

const Entry* Package::Find(const std::string& path) const {
  for (const Entry& e : entries_) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

bool Package::Read(const Entry& entry, std::vector<uint8_t>& out, std::string* error) {
  const uint32_t blocks = (entry.size + kBlock - 1) / kBlock;
  if (!ReadBlocks(entry.start, blocks, out, error)) return false;
  if (out.size() < entry.size) {
    if (error) *error = path_.filename().string() + ": " + entry.path + " is short";
    return false;
  }
  out.resize(entry.size);
  return true;
}

bool Package::Extract(const std::filesystem::path& out_dir, std::string* error,
                      const std::function<void(uint64_t, uint64_t)>& progress) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  const uint64_t total = total_bytes();
  uint64_t done = 0;
  std::vector<uint8_t> data;
  for (const Entry& e : entries_) {
    std::filesystem::path target = out_dir;
    size_t start = 0;
    while (start <= e.path.size()) {
      const size_t slash = e.path.find('/', start);
      const std::string part = e.path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
      if (part == ".." || part.empty()) {
        if (error) *error = "bad path in package: " + e.path;
        return false;
      }
      target /= part;
      if (slash == std::string::npos) break;
      start = slash + 1;
    }
    if (e.directory) {
      std::filesystem::create_directories(target, ec);
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (!Read(e, data, error)) return false;
    std::ofstream f(target, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
      if (error) *error = "cannot write " + target.string();
      return false;
    }
    done += e.size;
    if (progress) progress(done, total);
  }
  return true;
}

}  // namespace rs::stfs
