// Escritor minimo de modelos ONNX (protobuf a mano) para las pruebas: grafos diminutos con la forma de las redes de
// difusion (codificador de texto, UNet, VAE) y de vision, sin descargar modelos ni usar herramientas externas.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace onnxmin {

// ---------------------------------------------------------------- protobuf
inline void varint(std::string& o, uint64_t v) {
  while (v >= 0x80) {
    o += (char)(v | 0x80);
    v >>= 7;
  }
  o += (char)v;
}
inline void clave(std::string& o, int campo, int tipo) { varint(o, ((uint64_t)campo << 3) | tipo); }
inline void entero(std::string& o, int campo, int64_t v) {
  clave(o, campo, 0);
  varint(o, (uint64_t)v);
}
inline void bytes(std::string& o, int campo, const std::string& b) {
  clave(o, campo, 2);
  varint(o, b.size());
  o += b;
}

// tipos de ONNX
enum { FLOAT = 1, INT32 = 6, INT64 = 7, FLOAT16 = 10 };

struct Dim {
  int64_t valor = -1;
  std::string nombre;   // dimension dinamica
};

inline std::string valor_info(const std::string& nombre, int tipo, const std::vector<Dim>& forma) {
  std::string shape;
  for (auto& d : forma) {
    std::string dim;
    if (d.valor >= 0) entero(dim, 1, d.valor);
    else bytes(dim, 2, d.nombre.empty() ? "n" : d.nombre);
    bytes(shape, 1, dim);
  }
  std::string tensor;
  entero(tensor, 1, tipo);
  bytes(tensor, 2, shape);
  std::string type;
  bytes(type, 1, tensor);
  std::string vi;
  bytes(vi, 1, nombre);
  bytes(vi, 2, type);
  return vi;
}

inline std::string inicial_f(const std::string& nombre, const std::vector<int64_t>& forma, const std::vector<float>& v) {
  std::string t;
  for (auto d : forma) entero(t, 1, d);
  entero(t, 2, FLOAT);
  bytes(t, 8, nombre);
  bytes(t, 9, std::string((const char*)v.data(), v.size() * 4));
  return t;
}
inline std::string inicial_i64(const std::string& nombre, const std::vector<int64_t>& forma, const std::vector<int64_t>& v) {
  std::string t;
  for (auto d : forma) entero(t, 1, d);
  entero(t, 2, INT64);
  bytes(t, 8, nombre);
  bytes(t, 9, std::string((const char*)v.data(), v.size() * 8));
  return t;
}

struct Atributo {
  std::string nombre;
  int64_t i = 0;
  std::string s;
  bool es_texto = false;
};

inline std::string nodo(const std::string& op, const std::vector<std::string>& in, const std::vector<std::string>& out,
                        const std::vector<Atributo>& at = {}) {
  std::string n;
  for (auto& x : in) bytes(n, 1, x);
  for (auto& x : out) bytes(n, 2, x);
  bytes(n, 4, op);
  for (auto& a : at) {
    std::string ap;
    bytes(ap, 1, a.nombre);
    if (a.es_texto) {
      bytes(ap, 4, a.s);
      entero(ap, 20, 3);   // STRING
    } else {
      entero(ap, 3, a.i);
      entero(ap, 20, 2);   // INT
    }
    bytes(n, 5, ap);
  }
  return n;
}

struct Grafo {
  std::vector<std::string> nodos, iniciales, entradas, salidas;
  std::string modelo(int opset = 17) const {
    std::string g;
    for (auto& x : nodos) bytes(g, 1, x);
    bytes(g, 2, "g");
    for (auto& x : iniciales) bytes(g, 5, x);
    for (auto& x : entradas) bytes(g, 11, x);
    for (auto& x : salidas) bytes(g, 12, x);
    std::string m;
    entero(m, 1, 8);   // ir_version
    std::string op;
    bytes(op, 1, "");
    entero(op, 2, opset);
    bytes(m, 8, op);
    bytes(m, 2, "nexus-pruebas");
    bytes(m, 7, g);
    return m;
  }
};

}  // namespace onnxmin
