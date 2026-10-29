// Avatares 3D gratuitos desde Internet (nexus/media/avatares.py): buscar, descargar y usar en Nexus-Office, y
// despues sin conexion.
//
// Fuentes: «osa» (Open Source Avatars, cientos de VRM casi todos CC0; indice en GitHub y modelos en Arweave/IPFS),
// «muestras» (modelos de referencia de VRM Consortium, Khronos glTF y three.js) y «sketchfab» (modelos CC
// descargables; descargar pide la clave de API gratuita, avatares.sketchfab_token). Lo descargado queda en
// outputs/avatars con su miniatura y sus datos en biblioteca.json. Solo se descargan URLs del propio catalogo.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "util/fs.h"
#include "util/http.h"
#include "util/json.h"

namespace nx::avatares {

extern const json MUESTRAS;
extern const json FUENTES;

struct LookupError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct PermissionError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct ValueError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

fs::path dir();   // data/outputs/avatars

class Avatares : public std::enable_shared_from_this<Avatares> {
 public:
  using Fetch = std::function<http::Response(const http::Request&)>;   // respeta Request::on_data
  explicit Avatares(std::function<std::string()> token = {}, Fetch fetch = {});

  json biblioteca();
  bool borrar(const std::string& aid);
  json buscar(const std::string& fuente = "osa", const std::string& q = "", long long pagina = 1,
              long long por_pagina = 24);
  json item(const std::string& cid);                 // del catalogo; {} si no esta
  json descargar(const std::string& cid);            // trabajo en segundo plano
  json descargar_ya(const std::string& cid);         // lanza LookupError/ValueError/PermissionError/runtime_error
  json trabajo(const std::string& tid);              // null si no existe

  std::function<std::string()> token;
  Fetch fetch;
  void olvidar_indice();   // el indice de Open Source Avatars en memoria (las pruebas simulan reiniciar)

 private:
  std::recursive_mutex mu_;
  std::map<std::string, json> trabajos_;
  json osa_;
  double osa_t_ = 0;

  json osa_indice();
  json sketchfab(const std::string& q, long long pagina, long long por_pagina);
  http::Response get(const std::string& url, const http::Headers& h = {});
  uint64_t bajar(const std::string& url, const fs::path& dest, const std::string& tid, uint64_t limite);
  void guardar(const json& items);
};

Avatares& get(std::function<std::string()> token = {});
void reset();

}  // namespace nx::avatares
