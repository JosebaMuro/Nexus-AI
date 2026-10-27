// Avatares 3D gratuitos (nexus/media/avatares.py).
#include "media/avatares.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::avatares {

namespace {

auto L = log::get("avatares");
const std::string OSA = "https://raw.githubusercontent.com/ToxSam/open-source-avatars/main/data/";
const uint64_t MAX_BYTES = 300'000'000;
const std::vector<std::string> EXT_OK = {".glb", ".vrm", ".gltf"};
const std::vector<std::string> IPFS_GATEWAYS = {"https://dweb.link/ipfs/", "https://ipfs.io/ipfs/",
                                                "https://gateway.pinata.cloud/ipfs/", "https://w3s.link/ipfs/"};
const http::Headers UA = {{"User-Agent", "Mozilla/5.0 (compatible; Nexus-Office avatares)"}};

std::string safe(const std::string& name) {
  std::string out;
  bool sep = false;
  for (unsigned char ch : name) {
    if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-') {
      out += char(ch);
      sep = false;
    } else if (!sep) {
      out += '_';
      sep = true;
    }
  }
  size_t a = out.find_first_not_of("._"), b = out.find_last_not_of("._");
  out = a == std::string::npos ? "" : out.substr(a, b - a + 1).substr(0, 80);
  return out.empty() ? "avatar" : out;
}

// s[:n] de Python: n caracteres, no bytes
std::string utf8_prefix(const std::string& s, size_t n) {
  size_t i = 0;
  for (size_t k = 0; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && k++ == n) break;
  return s.substr(0, i);
}

std::string sin_query(const std::string& url) { return lower(url.substr(0, url.find('?'))); }

// la miniatura mas cercana a 256 px de ancho
std::string miniatura(const json& m) {
  const json& imgs = m.value("thumbnails", json::object()).value("images", json::array());
  std::string best;
  double d = 1e18;
  for (auto& t : imgs) {
    double w = t.value("width", json(0)).is_number() ? t["width"].get<double>() : 0;
    if (std::fabs(w - 256) < d) d = std::fabs(w - 256), best = jstr(t, "url");
  }
  return best;
}

std::string licencia(const json& m) {
  json l = m.value("license", json::object());
  if (!l.is_object()) return "";
  std::string s = jstr(l, "label");
  return s.empty() ? jstr(l, "slug") : s;
}

}  // namespace

const json MUESTRAS = [] {
  const std::string GH = "https://raw.githubusercontent.com/";
  const std::string KH = GH + "KhronosGroup/glTF-Sample-Assets/main/Models/";
  const std::string VRMC = GH + "vrm-c/vrm-specification/master/samples/";
  const std::string THREE = GH + "mrdoob/three.js/r160/examples/models/gltf/";
  auto m = [](const char* id, const char* nombre, const char* formato, const char* autor, const char* lic,
              const std::string& url, const std::string& mini, const char* desc) {
    return json{{"id", id}, {"nombre", nombre}, {"formato", formato}, {"autor", autor}, {"licencia", lic},
                {"url", url}, {"miniatura", mini}, {"descripcion", desc}};
  };
  return json::array({
      m("seed-san", "Seed-san", "VRM", "VirtualCast (VRM Consortium)", "VRM Public License 1.0",
        VRMC + "Seed-san/vrm/Seed-san.vrm", VRMC + "Seed-san/screenshot/screenshot.png", "Avatar de referencia de VRM 1.0"),
      m("vrm1-twist", "VRM1 Constraint Twist", "VRM", "VRM Consortium", "VRM Public License 1.0",
        VRMC + "VRM1_Constraint_Twist_Sample/vrm/VRM1_Constraint_Twist_Sample.vrm",
        VRMC + "VRM1_Constraint_Twist_Sample/screenshot/screenshot.jpg", "Chica anime (VRM 1.0)"),
      m("robot-expressive", "Robot expresivo", "GLB", "Tomás Laulhé (three.js)", "CC0",
        THREE + "RobotExpressive/RobotExpressive.glb", "", "Robot con 14 animaciones (saludar, bailar, andar...)"),
      m("xbot", "X Bot", "GLB", "Mixamo (three.js)", "Mixamo (uso libre)", THREE + "Xbot.glb", "",
        "Maniquí humano con animaciones"),
      m("michelle", "Michelle", "GLB", "Mixamo (three.js)", "Mixamo (uso libre)", THREE + "Michelle.glb", "",
        "Personaje bailando"),
      m("cesium-man", "Cesium Man", "GLB", "Cesium (Khronos)", "CC-BY 4.0", KH + "CesiumMan/glTF-Binary/CesiumMan.glb",
        KH + "CesiumMan/screenshot/screenshot.gif", "Hombre caminando (muestra de glTF)"),
      m("brainstem", "BrainStem", "GLB", "Keith Hunter (Khronos)", "CC-BY 4.0", KH + "BrainStem/glTF-Binary/BrainStem.glb",
        KH + "BrainStem/screenshot/screenshot.gif", "Robot bailando"),
      m("rigged-figure", "Rigged Figure", "GLB", "Cesium (Khronos)", "CC-BY 4.0",
        KH + "RiggedFigure/glTF-Binary/RiggedFigure.glb", KH + "RiggedFigure/screenshot/screenshot.gif",
        "Figura articulada sencilla"),
      m("fox", "Zorro", "GLB", "PixelMannen, tomkranis (Khronos)", "CC-BY 4.0", KH + "Fox/glTF-Binary/Fox.glb",
        KH + "Fox/screenshot/screenshot.jpg", "Zorro low-poly con animaciones"),
  });
}();

const json FUENTES = {
    {"osa", {{"nombre", "Open Source Avatars"}, {"web", "https://opensourceavatars.com"},
             {"descripcion", "Cientos de avatares VRM gratuitos, casi todos CC0"}}},
    {"muestras", {{"nombre", "Modelos de referencia"}, {"web", "https://github.com/KhronosGroup/glTF-Sample-Assets"},
                  {"descripcion", "VRM Consortium, Khronos glTF y three.js con licencia abierta"}}},
    {"sketchfab", {{"nombre", "Sketchfab (Creative Commons)"}, {"web", "https://sketchfab.com"},
                   {"descripcion", "Buscar modelos descargables con licencia CC; descargar necesita tu clave de API gratuita"}}},
};

fs::path dir() { return paths::sub({"outputs", "avatars"}); }

Avatares::Avatares(std::function<std::string()> tok, Fetch f)
    : token(std::move(tok)), fetch(f ? std::move(f) : Fetch([](const http::Request& r) { return http::send(r); })) {}

http::Response Avatares::get(const std::string& url, const http::Headers& h) {
  http::Request r;
  r.url = url;
  r.headers = UA;
  r.headers.insert(r.headers.end(), h.begin(), h.end());
  r.timeout = 30;
  return fetch(r);
}

// ------------------------------------------------------------------ biblioteca local
json Avatares::biblioteca() {
  json items = json::array();
  if (auto t = read_file(dir() / "biblioteca.json")) {
    try {
      items = jparse(*t);
    } catch (...) {
      items = json::array();
    }
  }
  json out = json::array();
  if (!items.is_array()) return out;
  for (auto& i : items)
    if (i.is_object() && std::filesystem::exists(dir() / P(jstr(i, "archivo")))) out.push_back(i);
  return out;
}

void Avatares::guardar(const json& items) {
  fs::path bib = dir() / "biblioteca.json", tmp = dir() / "biblioteca.tmp";
  write_file(tmp, items.dump(1, ' ', false));
  std::filesystem::rename(tmp, bib);
}

bool Avatares::borrar(const std::string& aid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json items = biblioteca(), resto = json::array();
  bool hay = false;
  for (auto& i : items) {
    if (jstr(i, "id") != aid) {
      resto.push_back(i);
      continue;
    }
    hay = true;
    std::error_code ec;
    for (auto* k : {"archivo", "miniatura_local"})
      if (!jstr(i, k).empty()) std::filesystem::remove(dir() / P(jstr(i, k)), ec);
  }
  if (hay) guardar(resto);
  return hay;
}

// ------------------------------------------------------------------ catalogos
void Avatares::olvidar_indice() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  osa_ = nullptr;
}

json Avatares::osa_indice() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  fs::path cache = dir() / "osa-indice.json";
  if (!osa_.is_null() && now() - osa_t_ < 86400) return osa_;
  std::string err;
  try {
    http::Response r = get(OSA + "projects.json");
    if (!r.ok()) throw std::runtime_error(r.status ? "HTTP " + std::to_string(r.status) : r.error);
    json projects = r.json_body();
    json out = json::array();
    for (auto& p : projects) {
      if (!p.is_object() || !truthy(p.value("is_public", json(true))) || jstr(p, "avatar_data_file").empty()) continue;
      json avs;
      try {
        http::Response ra = get(OSA + jstr(p, "avatar_data_file"));
        if (!ra.ok()) throw std::runtime_error("HTTP " + std::to_string(ra.status));
        avs = ra.json_body();
      } catch (const std::exception& e) {
        L.warning("osa ", jstr(p, "id"), ": ", e.what());
        continue;
      }
      if (!avs.is_array()) continue;
      for (auto& a : avs) {
        if (!a.is_object() || a.value("is_public", json(true)) == false || truthy(a.value("is_draft", json(false))) ||
            jstr(a, "model_file_url").empty())
          continue;
        std::vector<std::string> etiquetas;
        json meta = a.value("metadata", json::object());
        json attrs = meta.is_object() ? meta.value("attributes", json::array()) : json::array();
        if (attrs.is_array())
          for (auto& t : attrs)
            if (t.is_object()) {
              json v = t.value("value", json(""));
              etiquetas.push_back(v.is_string() ? v.get<std::string>() : v.dump());
            }
        std::string nombre = jstr(a, "name"), formato = upper(jstr(a, "format"));
        std::string id = a["id"].is_string() ? a["id"].get<std::string>() : a["id"].dump();
        out.push_back({{"id", "osa:" + id}, {"nombre", nombre.empty() ? "Avatar" : nombre},
                       {"descripcion", jstr(a, "description")}, {"formato", formato.empty() ? "VRM" : formato},
                       {"url", jstr(a, "model_file_url")}, {"miniatura", jstr(a, "thumbnail_url")},
                       {"coleccion", jstr(p, "name").empty() ? jstr(p, "id") : jstr(p, "name")},
                       {"autor", jstr(p, "creator_id")}, {"licencia", jstr(p, "license")}, {"fuente", "osa"},
                       {"etiquetas", join(etiquetas, " ")}});
      }
    }
    if (!out.empty()) {
      write_file(cache, out.dump(-1, ' ', false));
      osa_ = out;
      osa_t_ = now();
      return osa_;
    }
  } catch (const std::exception& e) {
    L.info("Open Source Avatars sin conexion (", e.what(), "): uso el indice guardado");
  }
  osa_ = json::array();
  if (auto t = read_file(cache)) {
    try {
      osa_ = jparse(*t);
      osa_t_ = now() - 86400 + 600;   // reintentar en 10 min
    } catch (...) {
      osa_ = json::array();
    }
  }
  return osa_;
}

json Avatares::sketchfab(const std::string& q, long long pagina, long long por_pagina) {
  std::map<std::string, std::string> params = {{"type", "models"},
                                               {"downloadable", "true"},
                                               {"q", q.empty() ? "avatar character" : q},
                                               {"count", std::to_string(por_pagina)},
                                               {"cursor", std::to_string((pagina - 1) * por_pagina)},
                                               {"sort_by", "-likeCount"}};
  http::Response r = get("https://api.sketchfab.com/v3/search?" + http::query_string(params));
  if (!r.ok()) throw http::HttpError(r.status, r.status ? "HTTP " + std::to_string(r.status) : r.error);
  json d = r.json_body(), items = json::array();
  for (auto& m : d.value("results", json::array())) {
    json user = m.value("user", json::object());
    std::string nombre = jstr(m, "name");
    items.push_back({{"id", "sketchfab:" + jstr(m, "uid")}, {"nombre", nombre.empty() ? "Modelo" : nombre},
                     {"formato", "GLB"}, {"descripcion", utf8_prefix(jstr(m, "description"), 200)},
                     {"autor", user.is_object() ? jstr(user, "displayName") : ""}, {"licencia", licencia(m)},
                     {"miniatura", miniatura(m)}, {"url", jstr(m, "viewerUrl")}, {"fuente", "sketchfab"},
                     {"coleccion", "Sketchfab"}});
  }
  json total = d.value("totalCount", json(nullptr));
  return {{"resultados", items}, {"total", truthy(total) ? total : json(items.size())}, {"pagina", pagina},
          {"necesita_clave", (token ? token() : "").empty()}};
}

json Avatares::buscar(const std::string& fuente, const std::string& q, long long pagina, long long por_pagina) {
  pagina = std::max(1LL, pagina);
  por_pagina = std::max(1LL, std::min(60LL, por_pagina));
  if (fuente == "sketchfab") return sketchfab(q, pagina, por_pagina);
  json base = fuente == "muestras" ? MUESTRAS : osa_indice();
  auto words = split_ws(lower(q));
  json items = json::array();
  std::set<std::string> colecciones;
  for (auto i : base) {
    std::string id = jstr(i, "id");
    i["fuente"] = fuente;
    i["id"] = id.find(':') != std::string::npos ? id : fuente + ":" + id;
    if (!words.empty()) {
      std::string texto;
      for (auto* k : {"nombre", "descripcion", "coleccion", "autor", "licencia", "etiquetas"})
        texto += (texto.empty() ? "" : " ") + jstr(i, k);
      texto = lower(texto);
      if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return contains(texto, w); })) continue;
    }
    if (!jstr(i, "coleccion").empty()) colecciones.insert(jstr(i, "coleccion"));
    items.push_back(i);
  }
  std::set<std::string> tengo;
  for (auto& i : biblioteca()) tengo.insert(jstr(i, "origen_id"));
  json page = json::array();
  for (size_t k = size_t((pagina - 1) * por_pagina); k < items.size() && page.size() < size_t(por_pagina); ++k) {
    json it = items[k];
    it["descargado"] = tengo.count(jstr(it, "id")) > 0;
    page.push_back(it);
  }
  return {{"resultados", page}, {"total", items.size()}, {"pagina", pagina}, {"colecciones", colecciones}};
}

json Avatares::item(const std::string& cid) {
  size_t c = cid.find(':');
  std::string fuente = cid.substr(0, c), rid = c == std::string::npos ? "" : cid.substr(c + 1);
  if (c == std::string::npos) return json::object();
  if (fuente == "muestras") {
    for (auto m : MUESTRAS)
      if (jstr(m, "id") == rid) {
        m["id"] = cid;
        m["fuente"] = fuente;
        return m;
      }
    return json::object();
  }
  if (fuente == "osa") {
    for (auto& i : osa_indice())
      if (jstr(i, "id") == cid) return i;
    return json::object();
  }
  if (fuente == "sketchfab") {
    std::string tok = token ? token() : "";
    if (tok.empty()) throw PermissionError("Sketchfab pide la clave de API de tu cuenta (gratis): ponla en Ajustes > Avatar");
    http::Response ri = get("https://api.sketchfab.com/v3/models/" + rid);
    json info = ri.ok() ? ri.json_body() : json::object();
    http::Response r = get("https://api.sketchfab.com/v3/models/" + rid + "/download", {{"Authorization", "Token " + tok}});
    if (r.status == 401 || r.status == 403) throw PermissionError("Sketchfab ha rechazado la clave de API");
    if (!r.ok()) throw http::HttpError(r.status, r.status ? "HTTP " + std::to_string(r.status) : r.error);
    json d = r.json_body();
    std::string url = d.contains("glb") && d["glb"].is_object() ? jstr(d["glb"], "url") : "";
    if (url.empty()) throw LookupError("Sketchfab no ofrece este modelo en GLB");
    json user = info.value("user", json::object());
    std::string nombre = jstr(info, "name");
    return {{"id", cid}, {"nombre", nombre.empty() ? rid : nombre}, {"formato", "GLB"}, {"url", url},
            {"fuente", fuente}, {"autor", user.is_object() ? jstr(user, "displayName") : ""},
            {"licencia", info.contains("license") && info["license"].is_object() ? jstr(info["license"], "label") : ""},
            {"miniatura", miniatura(info)}, {"web", jstr(info, "viewerUrl")}, {"ext", ".glb"}};
  }
  return json::object();
}

// ------------------------------------------------------------------ descarga
uint64_t Avatares::bajar(const std::string& url, const fs::path& dest, const std::string& tid, uint64_t limite) {
  // la URL y, si es de IPFS, la misma por otras pasarelas (alguna suele estar caida)
  std::vector<std::string> urls = {url};
  size_t p = url.find("/ipfs/");
  if (p != std::string::npos)
    for (auto& g : IPFS_GATEWAYS)
      if (!starts_with(url, g)) urls.push_back(g + url.substr(p + 6));
  std::string err;
  for (auto& u : urls) {
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    uint64_t n = 0;
    bool grande = false;
    http::Request r;
    r.url = u;
    r.headers = UA;
    r.timeout = 30;
    r.read_timeout = 120;
    r.on_headers = [&](const http::Response& h) {
      if (h.ok() && !tid.empty()) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        std::string cl = h.header("content-length");
        trabajos_[tid]["total"] = cl.empty() ? 0ULL : std::stoull(cl);
      }
      return true;
    };
    r.on_data = [&](const char* d, size_t len) {
      n += len;
      if (n > limite) {
        grande = true;
        return false;
      }
      f.write(d, std::streamsize(len));
      if (!tid.empty()) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        trabajos_[tid]["bytes"] = n;
      }
      return true;
    };
    http::Response res = fetch(r);
    f.close();
    if (grande)
      err = "el archivo supera el limite de tamano";
    else if (!res.ok())
      err = res.status ? "HTTP " + std::to_string(res.status) : res.error;
    else if (!f)
      err = "no se pudo escribir " + S(dest);
    else
      return n;
    L.info("avatar: ", u, " fallo (", err, ")");
  }
  throw std::runtime_error("no se pudo descargar: " + err);
}

json Avatares::descargar_ya(const std::string& cid) {
  json it = item(cid);
  if (it.empty() || jstr(it, "url").empty()) throw LookupError("avatar desconocido en el catalogo");
  for (auto& i : biblioteca())
    if (jstr(i, "origen_id") == cid) return i;
  std::string url = jstr(it, "url"), ext = jstr(it, "ext");
  if (ext.empty())
    for (auto& e : EXT_OK)
      if (ends_with(sin_query(url), e)) {
        ext = e;
        break;
      }
  if (ext.empty()) ext = jstr(it, "formato") == "VRM" ? ".vrm" : ".glb";
  std::string aid = uuid_hex().substr(0, 10), base = aid + "_" + safe(jstr(it, "nombre"));
  fs::path dest = dir() / P(base + ext), part = dir() / P(base + ext + ".part");
  std::string tid;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (auto& [k, j] : trabajos_)
      if (jstr(j, "origen_id") == cid && j["estado"] == "en_curso") tid = k;
  }
  std::error_code ec;
  try {
    bajar(url, part, tid, MAX_BYTES);
    std::string head;
    {
      std::ifstream f(part, std::ios::binary);
      head.resize(4);
      f.read(head.data(), 4);
      head.resize(size_t(f.gcount()));
    }
    if (head != "glTF" && ext != ".gltf") throw ValueError("lo descargado no es un modelo glTF/VRM");
    std::filesystem::rename(part, dest);
  } catch (...) {
    std::filesystem::remove(part, ec);
    throw;
  }
  std::string mini;
  if (!jstr(it, "miniatura").empty()) {
    std::string mext = lower(fs::path(sin_query(jstr(it, "miniatura"))).extension().string());
    if (mext != ".png" && mext != ".jpg" && mext != ".jpeg" && mext != ".gif" && mext != ".webp") mext = ".png";
    try {
      bajar(jstr(it, "miniatura"), dir() / P(base + "_mini" + mext), "", 20'000'000);
      mini = base + "_mini" + mext;
    } catch (const std::runtime_error&) {
      std::filesystem::remove(dir() / P(base + "_mini" + mext), ec);
    }
  }
  std::string web = jstr(it, "web");
  if (web.empty() && FUENTES.contains(jstr(it, "fuente"))) web = jstr(FUENTES[jstr(it, "fuente")], "web");
  std::string archivo = S(dest.filename());
  json nuevo = {{"id", aid}, {"origen_id", cid}, {"nombre", jstr(it, "nombre")}, {"formato", jstr(it, "formato")},
                {"autor", jstr(it, "autor")}, {"licencia", jstr(it, "licencia")}, {"coleccion", jstr(it, "coleccion")},
                {"fuente", jstr(it, "fuente")}, {"archivo", archivo}, {"miniatura_local", mini},
                {"url", "/outputs/avatars/" + archivo}, {"miniatura_url", mini.empty() ? "" : "/outputs/avatars/" + mini},
                {"web", web}, {"bytes", std::filesystem::file_size(dest)}, {"fecha", now()}};
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json items = biblioteca();
  items.push_back(nuevo);
  guardar(items);
  return nuevo;
}

json Avatares::descargar(const std::string& cid) {
  std::string tid = uuid_hex().substr(0, 8);
  json job = {{"id", tid}, {"origen_id", cid}, {"estado", "en_curso"}, {"bytes", 0}, {"total", 0},
              {"inicio", now()}, {"error", ""}, {"avatar", nullptr}};
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    trabajos_[tid] = job;
  }
  std::shared_ptr<Avatares> self;   // el hilo mantiene vivo el objeto aunque se haga reset()
  try {
    self = shared_from_this();
  } catch (const std::bad_weak_ptr&) {
  }
  std::thread([this, self, cid, tid] {
    try {
      json av = descargar_ya(cid);
      std::lock_guard<std::recursive_mutex> lk(mu_);
      trabajos_[tid]["avatar"] = av;
      trabajos_[tid]["estado"] = "hecho";
    } catch (const std::exception& e) {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      trabajos_[tid]["estado"] = "error";
      trabajos_[tid]["error"] = e.what();
      L.warning("avatar ", cid, ": ", e.what());
    }
  }).detach();
  return job;
}

json Avatares::trabajo(const std::string& tid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto it = trabajos_.find(tid);
  return it == trabajos_.end() ? json(nullptr) : it->second;
}

namespace {
std::mutex g_mu;
std::shared_ptr<Avatares> g_a;
}  // namespace

Avatares& get(std::function<std::string()> token) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_a)
    g_a = std::make_shared<Avatares>(std::move(token));
  else if (token)
    g_a->token = std::move(token);
  return *g_a;
}

void reset() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_a.reset();
}

}  // namespace nx::avatares
