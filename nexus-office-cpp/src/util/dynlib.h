// Bibliotecas que se cargan al ejecutar (dlopen / LoadLibrary): motores de inferencia que no se enlazan al compilar
// (LiteRT-LM, ONNX Runtime GenAI). Si la biblioteca no esta o le falta una funcion, el motor no se ofrece.
#pragma once
#include <string>

#include "util/fs.h"

namespace nx {

class DynLib {
 public:
  DynLib() = default;
  ~DynLib();
  DynLib(const DynLib&) = delete;
  DynLib& operator=(const DynLib&) = delete;

  // carga la biblioteca; en Windows sus DLL se buscan tambien en su propia carpeta
  bool open(const fs::path& path);
  bool loaded() const { return h_ != nullptr; }
  void* sym(const char* name) const;
  template <class F>
  bool get(const char* name, F& out) const {
    out = reinterpret_cast<F>(sym(name));
    return out != nullptr;
  }
  const std::string& error() const { return err_; }

 private:
  void* h_ = nullptr;
  std::string err_;
};

// nombre de la biblioteca en esta plataforma: «litert-lm» -> liblitert-lm.so / litert-lm.dll / liblitert-lm.dylib
std::string dynlib_name(const std::string& base);

}  // namespace nx
