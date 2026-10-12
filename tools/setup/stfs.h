// Xbox 360 STFS packages (LIVE / PIRS / CON): the containers Real Steel, its
// title update and its add-ons come in. Read-only; the C++ form of
// tools/pac/stfs_extract.py.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rs::stfs {

struct Entry {
  std::string path;   // "pac/misc.pac" (forward slashes)
  std::string name;
  bool directory = false;
  uint32_t blocks = 0;
  uint32_t start = 0;
  uint32_t size = 0;
  uint16_t parent = 0xFFFF;
};

// Quick look at a file: is it an STFS package, and whose?
struct Identity {
  bool stfs = false;
  std::string magic;        // "LIVE", "PIRS", "CON "
  uint32_t content_type = 0;
  uint32_t title_id = 0;
  std::string display_name;
};
Identity Identify(const std::filesystem::path& file);

class Package {
 public:
  Package() = default;
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;

  bool Open(const std::filesystem::path& file, std::string* error);
  const Identity& identity() const { return identity_; }
  const std::vector<Entry>& entries() const { return entries_; }
  uint64_t total_bytes() const;
  const Entry* Find(const std::string& path) const;

  bool Read(const Entry& entry, std::vector<uint8_t>& out, std::string* error);
  // Writes every file under |out_dir|; |progress| gets (bytes done, total).
  bool Extract(const std::filesystem::path& out_dir, std::string* error,
               const std::function<void(uint64_t, uint64_t)>& progress = {});

 private:
  bool ReadAt(uint64_t offset, void* out, size_t size);
  uint64_t DataOffset(uint32_t block) const;
  bool NextBlock(uint32_t block, uint32_t* next);
  bool ReadBlocks(uint32_t start, uint32_t count, std::vector<uint8_t>& out, std::string* error);
  bool ReadTable(std::string* error);

  std::FILE* file_ = nullptr;
  std::filesystem::path path_;
  Identity identity_;
  std::vector<Entry> entries_;
  uint32_t header_size_ = 0;
  bool read_only_ = true;
  uint32_t table_blocks_ = 0;
  uint32_t table_start_ = 0;
  uint32_t total_blocks_ = 0;
  uint64_t data_start_ = 0;
  uint32_t shift_ = 0;
};

}  // namespace rs::stfs
