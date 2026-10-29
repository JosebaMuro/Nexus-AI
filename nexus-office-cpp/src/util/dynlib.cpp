#include "util/dynlib.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdio>
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
  // un fichero que no es una DLL abre la ventana «Imagen incorrecta» y deja el proceso esperando: se comprueba antes
  // la cabecera y se quitan los dialogos de error mientras se carga
  {
    std::string head;
    if (FILE* f = _wfopen(path.wstring().c_str(), L"rb")) {
      char b[2] = {0, 0};
      if (std::fread(b, 1, 2, f) == 2) head.assign(b, 2);
      std::fclose(f);
    }
    if (head != "MZ") {
      err_ = S(path) + ": no es una biblioteca de Windows";
      return false;
    }
  }
  DWORD old_mode = 0;
  SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
  h_ = (void*)LoadLibraryExW(fs::absolute(path).wstring().c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  DWORD code = GetLastError();
  SetThreadErrorMode(old_mode, nullptr);
  if (!h_) err_ = "LoadLibrary " + S(path) + ": error " + std::to_string(code);
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
