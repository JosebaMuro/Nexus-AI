#include "util/fs.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace nx {

fs::path P(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string S(const fs::path& p) {
  auto u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string env(const char* name, const std::string& def) {
#ifdef _WIN32
  std::wstring wname = P(name).wstring();
  DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (!n) return def;
  std::wstring buf(n, L'\0');
  GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
  buf.resize(n - 1);
  return S(fs::path(buf));
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : def;
#endif
}

void set_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
  SetEnvironmentVariableW(P(name).wstring().c_str(), P(value).wstring().c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

namespace paths {
static std::mutex mu;
static fs::path root_override;

fs::path exe_path() {
#ifdef _WIN32
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  return fs::path(std::wstring(buf, n));
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0) return fs::weakly_canonical(buf);
  return fs::current_path();
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::current_path() : p;
#endif
}

static bool has_resources(const fs::path& d) { return fs::exists(d / "nexus" / "web" / "index.html"); }

fs::path root() {
  std::lock_guard<std::mutex> lk(mu);
  if (!root_override.empty()) return root_override;
  std::string e = env("NEXUS_ROOT");
  if (!e.empty()) return root_override = P(e);
  fs::path d = exe_path().parent_path();
  for (int i = 0; i < 3 && !d.empty(); ++i) {
    if (has_resources(d)) return root_override = d;
    d = d.parent_path();
  }
  return root_override = exe_path().parent_path();
}

void set_root(const fs::path& p) {
  std::lock_guard<std::mutex> lk(mu);
  root_override = p;
}

fs::path package() { return root() / "nexus"; }
fs::path web() { return package() / "web"; }

fs::path data_dir() {
  std::string e = env("NEXUS_DATA_DIR");
  fs::path d = e.empty() ? root() / "data" : P(e);
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

fs::path sub(std::initializer_list<std::string> parts) {
  fs::path p = data_dir();
  for (auto& s : parts) p /= P(s);
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

fs::path models_dir() { return sub({"models"}); }
fs::path outputs_dir() { return sub({"outputs"}); }
fs::path logs_dir() { return sub({"logs"}); }
fs::path agents_dir() { return sub({"agents"}); }
fs::path sandbox_dir() { return sub({"sandbox"}); }
fs::path config_file() { return data_dir() / "config.yaml"; }
fs::path hardware_file() { return data_dir() / "hardware.json"; }
fs::path tuning_file() { return data_dir() / "tuning.json"; }
fs::path db_file() { return data_dir() / "nexus.db"; }
}  // namespace paths

std::optional<std::string> read_file(const fs::path& p, size_t max_bytes) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  std::string out;
  if (max_bytes == (size_t)-1) {
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
  out.resize(max_bytes);
  f.read(out.data(), (std::streamsize)max_bytes);
  out.resize((size_t)f.gcount());
  return out;
}

std::string read_text(const fs::path& p) { return read_file(p).value_or(""); }

bool write_file(const fs::path& p, const std::string& data) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  static std::mt19937_64 rng(std::random_device{}());
  fs::path tmp = p;
  tmp += ".tmp" + std::to_string(rng() % 1000000);
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {   // Windows: el destino puede estar abierto; se reintenta copiando
    fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp);
    return !ec;
  }
  return true;
}

bool append_file(const fs::path& p, const std::string& data) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::app);
  if (!f) return false;
  f.write(data.data(), (std::streamsize)data.size());
  return (bool)f;
}

bool path_exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}
void mkdirs(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
}
void rm_rf(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
}
bool remove_file(const fs::path& p) {
  std::error_code ec;
  return fs::remove(p, ec);
}
uintmax_t fsize(const fs::path& p) {
  std::error_code ec;
  auto n = fs::file_size(p, ec);
  return ec ? 0 : n;
}

double mtime(const fs::path& p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  // portable (libc++ no siempre trae clock_cast): desfase entre el reloj de ficheros y el del sistema
  auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return std::chrono::duration<double>(sys.time_since_epoch()).count();
}

bool fnmatch(const std::string& pat, const std::string& name) {
  // '*' y '?' (sin clases): suficiente para "*.yaml", "slide*.xml"...
  size_t p = 0, n = 0, star = std::string::npos, mark = 0;
  while (n < name.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) { ++p; ++n; }
    else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
    else if (star != std::string::npos) { p = star + 1; n = ++mark; }
    else return false;
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

std::vector<fs::path> glob(const fs::path& dir, const std::string& pattern, bool recursive) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  if (recursive) {
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (fnmatch(pattern, S(it->path().filename()))) out.push_back(it->path());
    }
  } else {
    for (auto& e : fs::directory_iterator(dir, ec))
      if (fnmatch(pattern, S(e.path().filename()))) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

fs::path resolve(const fs::path& p) {
  std::error_code ec;
  auto r = fs::weakly_canonical(fs::absolute(p, ec), ec);
  return ec ? fs::absolute(p) : r;
}

bool is_relative_to(const fs::path& p, const fs::path& base) {
  auto a = resolve(p), b = resolve(base);
  auto ai = a.begin(), bi = b.begin();
  for (; bi != b.end(); ++ai, ++bi) {
    if (ai == a.end()) return false;
    if (bi->empty() && std::next(bi) == b.end()) break;   // barra final
    if (*ai != *bi) return false;
  }
  return true;
}

std::string stem(const fs::path& p) { return S(p.stem()); }
std::string suffix_lower(const fs::path& p) { return lower(S(p.extension())); }

double disk_free_gb(const fs::path& p) {
  std::error_code ec;
  auto sp = fs::space(p, ec);
  return ec ? 0.0 : (double)sp.available / (1024.0 * 1024 * 1024);
}

}  // namespace nx

namespace nx {
fs::path home_dir() {
#ifdef _WIN32
  std::string h = env("USERPROFILE");
#else
  std::string h = env("HOME");
#endif
  return h.empty() ? fs::current_path() : P(h);
}
}  // namespace nx
