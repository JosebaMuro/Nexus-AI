#include "util/dynlib.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "util/sysinfo.h"

namespace nx {

DynLib::~DynLib() {
  // no se descarga: los motores dejan hilos propios vivos hasta el final del proceso
}

bool DynLib::open(const fs::path& path) {
  err_.clear();
#ifdef _WIN32
  h_ = (void*)LoadLibraryExW(path.wstring().c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!h_) err_ = "LoadLibrary " + S(path) + ": error " + std::to_string(GetLastError());
#else
  h_ = dlopen(S(path).c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h_) {
    const char* e = dlerror();
    err_ = e ? e : "dlopen " + S(path);
  }
#endif
  return h_ != nullptr;
}

void* DynLib::sym(const char* name) const {
  if (!h_) return nullptr;
#ifdef _WIN32
  return (void*)GetProcAddress((HMODULE)h_, name);
#else
  return dlsym(h_, name);
#endif
}

std::string dynlib_name(const std::string& base) {
  if (sys::is_windows()) return base + ".dll";
  if (sys::is_macos()) return "lib" + base + ".dylib";
  return "lib" + base + ".so";
}

}  // namespace nx
