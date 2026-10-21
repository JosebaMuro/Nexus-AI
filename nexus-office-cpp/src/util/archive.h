// ZIP (miniz) y tar/tar.gz: lectura de entradas (docx, xlsx, epub...) y extraccion segura.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "util/fs.h"

namespace nx::archive {

struct Entry {
  std::string name;
  uint64_t size = 0;
  bool dir = false;
  uint64_t comp_size = 0;
  bool encrypted = false;
  bool symlink = false;
};

class Zip {
 public:
  explicit Zip(const fs::path& p);
  explicit Zip(std::string data, bool in_memory);
  ~Zip();
  Zip(const Zip&) = delete;
  Zip& operator=(const Zip&) = delete;
  bool ok() const { return ok_; }
  std::vector<Entry> entries() const;
  bool has(const std::string& name) const;
  std::optional<std::string> read(const std::string& name, size_t max_bytes = 512u << 20) const;
  bool extract_all(const fs::path& dest) const;   // ignora rutas fuera de dest

 private:
  void* z_ = nullptr;
  std::string mem_;
  bool ok_ = false;
};

// gzip -> bytes (zlib.decompress con cabecera gzip)
std::optional<std::string> gunzip(const std::string& data);
// deflate crudo o con cabecera zlib
std::optional<std::string> inflate(const std::string& data, bool zlib_header);
// crea un zip (para exportaciones); files: (nombre en el zip, bytes)
bool write_zip(const fs::path& dest, const std::vector<std::pair<std::string, std::string>>& files);
// .tar, .tar.gz, .tgz
bool extract_tar(const fs::path& archive, const fs::path& dest);
bool extract(const fs::path& archive, const fs::path& dest);   // por extension

}  // namespace nx::archive
