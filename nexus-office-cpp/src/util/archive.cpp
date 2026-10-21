#include <bzlib.h>
extern "C" {
#include <xz.h>
}
#include "util/archive.h"

#include <miniz.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "util/crypto.h"
#include "util/text.h"

namespace nx::archive {

static bool safe_target(const fs::path& dest, const std::string& name, fs::path& out) {
  if (name.empty() || name[0] == '/' || name[0] == '\\' || contains(name, ":")) return false;
  fs::path t = dest / P(name);
  if (!is_relative_to(t, dest)) return false;
  out = t;
  return true;
}

Zip::Zip(const fs::path& p) {
  auto* z = new mz_zip_archive();
  std::memset(z, 0, sizeof *z);
  z_ = z;
  // ruta UTF-8 en Windows: se lee a memoria (miniz usa fopen)
  auto data = read_file(p);
  if (!data) return;
  mem_ = std::move(*data);
  ok_ = mz_zip_reader_init_mem(z, mem_.data(), mem_.size(), 0);
}

Zip::Zip(std::string data, bool) : mem_(std::move(data)) {
  auto* z = new mz_zip_archive();
  std::memset(z, 0, sizeof *z);
  z_ = z;
  ok_ = mz_zip_reader_init_mem(z, mem_.data(), mem_.size(), 0);
}

Zip::~Zip() {
  auto* z = (mz_zip_archive*)z_;
  if (ok_) mz_zip_reader_end(z);
  delete z;
}

std::vector<Entry> Zip::entries() const {
  std::vector<Entry> out;
  if (!ok_) return out;
  auto* z = (mz_zip_archive*)z_;
  mz_uint n = mz_zip_reader_get_num_files(z);
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z, i, &st)) continue;
    Entry e{st.m_filename, (uint64_t)st.m_uncomp_size, (bool)mz_zip_reader_is_file_a_directory(z, i)};
    e.comp_size = (uint64_t)st.m_comp_size;
    e.encrypted = (st.m_bit_flag & 1) != 0;
    e.symlink = ((st.m_external_attr >> 16) & 0170000) == 0120000;
    out.push_back(e);
  }
  return out;
}

bool Zip::has(const std::string& name) const {
  return ok_ && mz_zip_reader_locate_file((mz_zip_archive*)z_, name.c_str(), nullptr, 0) >= 0;
}

std::optional<std::string> Zip::read(const std::string& name, size_t max_bytes) const {
  if (!ok_) return std::nullopt;
  auto* z = (mz_zip_archive*)z_;
  int idx = mz_zip_reader_locate_file(z, name.c_str(), nullptr, 0);
  if (idx < 0) return std::nullopt;
  mz_zip_archive_file_stat st;
  if (!mz_zip_reader_file_stat(z, (mz_uint)idx, &st) || st.m_uncomp_size > max_bytes) return std::nullopt;
  std::string out((size_t)st.m_uncomp_size, '\0');
  if (!mz_zip_reader_extract_to_mem(z, (mz_uint)idx, out.data(), out.size(), 0)) return std::nullopt;
  return out;
}

bool Zip::extract_all(const fs::path& dest) const {
  if (!ok_) return false;
  mkdirs(dest);
  auto* z = (mz_zip_archive*)z_;
  mz_uint n = mz_zip_reader_get_num_files(z);
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z, i, &st)) continue;
    fs::path t;
    if (!safe_target(dest, st.m_filename, t)) continue;
    if (mz_zip_reader_is_file_a_directory(z, i)) {
      mkdirs(t);
      continue;
    }
    std::string buf((size_t)st.m_uncomp_size, '\0');
    if (!mz_zip_reader_extract_to_mem(z, i, buf.data(), buf.size(), 0)) return false;
    if (!write_file(t, buf)) return false;
#ifndef _WIN32
    // permisos unix guardados en el zip (ejecutables de llama.cpp)
    unsigned mode = (st.m_external_attr >> 16) & 0777;
    if (mode) fs::permissions(t, (fs::perms)mode, fs::perm_options::replace);
#endif
  }
  return true;
}

std::optional<std::string> inflate(const std::string& data, bool zlib_header) {
  size_t out_len = 0;
  void* p = tinfl_decompress_mem_to_heap(data.data(), data.size(), &out_len,
                                         zlib_header ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
  if (!p) return std::nullopt;
  std::string out((const char*)p, out_len);
  mz_free(p);
  return out;
}

std::optional<std::string> bunzip2(const std::string& d, size_t max_bytes) {
  bz_stream bz{};
  if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) return std::nullopt;
  std::string out;
  std::vector<char> buf(1 << 16);
  size_t pos = 0;
  int rc = BZ_OK;
  while (rc == BZ_OK || (rc == BZ_STREAM_END && pos < d.size())) {
    if (rc == BZ_STREAM_END) {   // varios flujos concatenados (pbzip2)
      BZ2_bzDecompressEnd(&bz);
      bz = bz_stream{};
      if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK) return std::nullopt;
    }
    bz.next_in = const_cast<char*>(d.data() + pos);
    bz.avail_in = (unsigned)std::min<size_t>(d.size() - pos, 1u << 30);
    bz.next_out = buf.data();
    bz.avail_out = (unsigned)buf.size();
    rc = BZ2_bzDecompress(&bz);
    pos = d.size() - bz.avail_in;
    out.append(buf.data(), buf.size() - bz.avail_out);
    if (out.size() > max_bytes) rc = BZ_MEM_ERROR;
    if (rc == BZ_OK && bz.avail_in == 0 && bz.avail_out != 0) rc = BZ_UNEXPECTED_EOF;
  }
  BZ2_bzDecompressEnd(&bz);
  if (rc != BZ_STREAM_END) return std::nullopt;
  return out;
}

std::optional<std::string> unxz(const std::string& d, size_t max_bytes) {
  static bool crc_ready = [] {
    xz_crc32_init();
    xz_crc64_init();
    return true;
  }();
  (void)crc_ready;
  xz_dec* dec = xz_dec_init(XZ_DYNALLOC, 1u << 26);
  if (!dec) return std::nullopt;
  std::string out;
  std::vector<uint8_t> buf(1 << 16);
  xz_buf b{};
  b.in = (const uint8_t*)d.data();
  b.in_size = d.size();
  xz_ret rc;
  do {
    b.out = buf.data();
    b.out_pos = 0;
    b.out_size = buf.size();
    rc = xz_dec_run(dec, &b);
    out.append((const char*)buf.data(), b.out_pos);
    if (out.size() > max_bytes) rc = XZ_MEM_ERROR;
  } while (rc == XZ_OK && !(b.in_pos == b.in_size && b.out_pos == 0));
  xz_dec_end(dec);
  if (rc != XZ_STREAM_END) return std::nullopt;
  return out;
}

std::optional<std::string> gunzip(const std::string& d) {
  if (d.size() < 18 || (unsigned char)d[0] != 0x1f || (unsigned char)d[1] != 0x8b || d[2] != 8) return std::nullopt;
  unsigned char flg = (unsigned char)d[3];
  size_t pos = 10;
  if (flg & 4) {
    if (pos + 2 > d.size()) return std::nullopt;
    pos += 2 + ((unsigned char)d[pos] | ((unsigned char)d[pos + 1] << 8));
  }
  if (flg & 8) while (pos < d.size() && d[pos++]) {}
  if (flg & 16) while (pos < d.size() && d[pos++]) {}
  if (flg & 2) pos += 2;
  if (pos >= d.size()) return std::nullopt;
  // los .tar.gz grandes (runtimes de llama.cpp) se descomprimen por bloques
  tinfl_decompressor inf;
  tinfl_init(&inf);
  std::string out;
  std::vector<unsigned char> dict(TINFL_LZ_DICT_SIZE);
  size_t in_pos = pos, dict_ofs = 0;
  for (;;) {
    size_t in_bytes = d.size() - in_pos, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
    tinfl_status st = tinfl_decompress(&inf, (const mz_uint8*)d.data() + in_pos, &in_bytes, dict.data(),
                                       dict.data() + dict_ofs, &out_bytes, TINFL_FLAG_HAS_MORE_INPUT * 0);
    in_pos += in_bytes;
    out.append((const char*)dict.data() + dict_ofs, out_bytes);
    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    if (st == TINFL_STATUS_DONE) break;
    if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT)) return std::nullopt;
  }
  return out;
}

bool write_zip(const fs::path& dest, const std::vector<std::pair<std::string, std::string>>& files) {
  mz_zip_archive z;
  std::memset(&z, 0, sizeof z);
  if (!mz_zip_writer_init_heap(&z, 0, 1 << 16)) return false;
  for (auto& [name, data] : files)
    if (!mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION)) {
      mz_zip_writer_end(&z);
      return false;
    }
  void* buf = nullptr;
  size_t size = 0;
  bool ok = mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
  if (ok) ok = write_file(dest, std::string((const char*)buf, size));
  mz_zip_writer_end(&z);
  return ok;
}

std::string write_targz(const std::vector<TarFile>& files) {
  std::string tar;
  auto field = [](char* dst, size_t n, uint64_t v) { std::snprintf(dst, n, "%0*llo", (int)n - 1, (unsigned long long)v); };
  long long mtime = (long long)now();
  for (auto& f : files) {
    char h[512] = {0};
    std::string name = f.name, prefix;
    if (name.size() > 100) {   // ustar: prefijo de hasta 155 bytes cortado en una barra
      size_t cut = name.rfind('/', 155);
      if (cut == std::string::npos || name.size() - cut - 1 > 100) continue;
      prefix = name.substr(0, cut);
      name = name.substr(cut + 1);
    }
    std::memcpy(h, name.data(), name.size());
    field(h + 100, 8, (uint64_t)f.mode);
    field(h + 108, 8, 0);
    field(h + 116, 8, 0);
    field(h + 124, 12, f.data.size());
    field(h + 136, 12, (uint64_t)mtime);
    std::memset(h + 148, ' ', 8);
    h[156] = '0';
    std::memcpy(h + 257, "ustar\0" "00", 8);
    std::memcpy(h + 345, prefix.data(), prefix.size());
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(h + 148, 8, "%06o", sum);
    tar.append(h, 512);
    tar += f.data;
    tar.append((512 - f.data.size() % 512) % 512, '\0');
  }
  tar.append(1024, '\0');
  size_t clen = 0;
  void* comp = tdefl_compress_mem_to_heap(tar.data(), tar.size(), &clen, tdefl_create_comp_flags_from_zip_params(9, -15, 0));
  std::string out("\x1f\x8b\x08\x00\0\0\0\0\x02\xff", 10);
  if (comp) {
    out.append((const char*)comp, clen);
    mz_free(comp);
  }
  uint32_t crc = crc32(tar), isize = (uint32_t)tar.size();
  for (int i = 0; i < 4; ++i) out += (char)((crc >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; ++i) out += (char)((isize >> (8 * i)) & 0xFF);
  return out;
}

static uint64_t octal(const char* p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n && p[i]; ++i)
    if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (uint64_t)(p[i] - '0');
  return v;
}

bool extract_tar(const fs::path& archive, const fs::path& dest) {
  auto raw = read_file(archive);
  if (!raw) return false;
  std::string data;
  if (raw->size() > 2 && (unsigned char)(*raw)[0] == 0x1f && (unsigned char)(*raw)[1] == 0x8b) {
    auto g = gunzip(*raw);
    if (!g) return false;
    data = std::move(*g);
  } else {
    data = std::move(*raw);
  }
  mkdirs(dest);
  std::string long_name;
  std::vector<std::pair<fs::path, std::string>> links;
  for (size_t pos = 0; pos + 512 <= data.size();) {
    const char* h = data.data() + pos;
    if (!h[0]) break;
    std::string name(h, strnlen(h, 100));
    std::string prefix(h + 345, strnlen(h + 345, 155));
    if (!prefix.empty() && std::memcmp(h + 257, "ustar", 5) == 0) name = prefix + "/" + name;
    uint64_t size = octal(h + 124, 12);
    unsigned mode = (unsigned)octal(h + 100, 8);
    char type = h[156];
    std::string linkname(h + 157, strnlen(h + 157, 100));
    pos += 512;
    std::string body = data.substr(pos, (size_t)size);
    pos += (size_t)((size + 511) / 512 * 512);
    if (type == 'L') {   // nombre largo GNU
      long_name = std::string(body.c_str());
      continue;
    }
    if (type == 'x' || type == 'g') continue;   // cabeceras pax
    if (!long_name.empty()) {
      name = long_name;
      long_name.clear();
    }
    while (starts_with(name, "./")) name = name.substr(2);
    fs::path t;
    if (name.empty() || !safe_target(dest, name, t)) continue;
    if (type == '5') {
      mkdirs(t);
    } else if (type == '0' || type == '\0') {
      if (!write_file(t, body)) return false;
#ifndef _WIN32
      if (mode) fs::permissions(t, (fs::perms)(mode & 0777), fs::perm_options::replace);
#endif
    } else if (type == '2' || type == '1') {
      links.push_back({t, linkname});   // libggml.so -> libggml.so.0 en los paquetes de llama.cpp
    }
  }
  for (auto& [t, target] : links) {
    std::error_code ec;
    fs::path src = t.parent_path() / P(target);
    if (!is_relative_to(src, dest)) continue;
#ifdef _WIN32
    fs::copy_file(src, t, fs::copy_options::overwrite_existing, ec);
#else
    fs::remove(t, ec);
    fs::create_symlink(P(target), t, ec);
    if (ec) fs::copy_file(src, t, fs::copy_options::overwrite_existing, ec);
#endif
  }
  return true;
}

bool extract(const fs::path& archive, const fs::path& dest) {
  std::string n = lower(S(archive.filename()));
  if (ends_with(n, ".zip")) return Zip(archive).extract_all(dest);
  return extract_tar(archive, dest);
}

}  // namespace nx::archive

extern "C" void bz_internal_error(int code) {
  throw std::runtime_error("bzip2: error interno " + std::to_string(code));
}
