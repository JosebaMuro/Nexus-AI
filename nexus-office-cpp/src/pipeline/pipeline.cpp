// Pipelines desacoplados: percepcion, difusion asincrona y buffer de pantalla (ver pipeline.h).
#include "pipeline/pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

#include "core/db.h"
#include "media/media.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

#include "stb_image.h"

namespace nx::pipeline {

static auto L = log::get("pipeline");

namespace {

const char* ESQUEMA = R"(
CREATE TABLE IF NOT EXISTS percepcion (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts REAL NOT NULL,
  fuente TEXT,
  descripcion TEXT,
  motor TEXT,
  ancho INTEGER,
  alto INTEGER,
  url TEXT,
  emb_id TEXT,
  embedding BLOB,
  huella BLOB
);
CREATE INDEX IF NOT EXISTS percepcion_ts ON percepcion(ts);
)";

std::string extension(std::string_view b) {
  auto u = [&](size_t i) { return i < b.size() ? (unsigned char)b[i] : 0; };
  if (u(0) == 0xFF && u(1) == 0xD8) return ".jpg";
  if (u(0) == 0x89 && u(1) == 'P') return ".png";
  if (b.size() > 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") return ".webp";
  if (u(0) == 'B' && u(1) == 'M') return ".bmp";
  if (u(0) == 'G' && u(1) == 'I') return ".gif";
  return ".img";
}

// lee un fichero directamente en un bloque del asignador (sin pasar por un std::string)
hal::Buffer leer_en_bloque(const fs::path& p) {
  std::error_code ec;
  auto n = fs::file_size(p, ec);
  if (ec || n == 0) return {};
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  hal::Buffer b = hal::pool_imagenes().pedir((size_t)n);
  f.read((char*)b.data(), (std::streamsize)n);
  if (f.gcount() != (std::streamsize)n) return {};
  return b;
}

}  // namespace

mem::Vec huella(const uint8_t* rgb, int w, int h) {
  constexpr int G = 8;
  mem::Vec v(G * G * 3, 0.f);
  if (!rgb || w <= 0 || h <= 0) return v;
  std::vector<int> cuenta(G * G, 0);
  // muestreo con paso: basta para una huella y no recorre millones de pixeles en frames grandes
  const int paso = std::max(1, std::min(w, h) / 64);
  for (int y = 0; y < h; y += paso) {
    const int gy = std::min(G - 1, y * G / h);
    for (int x = 0; x < w; x += paso) {
      const int gx = std::min(G - 1, x * G / w), c = gy * G + gx;
      const uint8_t* p = rgb + ((size_t)y * w + x) * 3;
      v[c * 3] += p[0];
      v[c * 3 + 1] += p[1];
      v[c * 3 + 2] += p[2];
      ++cuenta[c];
    }
  }
  double media = 0;
  for (int c = 0; c < G * G; ++c)
    for (int k = 0; k < 3; ++k) {
      v[c * 3 + k] = cuenta[c] ? v[c * 3 + k] / (float)cuenta[c] / 255.f : 0.f;
      media += v[c * 3 + k];
    }
  media /= (double)v.size();
  double norma = 0;
  for (auto& x : v) {
    x = (float)(x - media);
    norma += (double)x * x;
  }
  norma = std::sqrt(norma);
  if (norma > 1e-9)
    for (auto& x : v) x = (float)(x / norma);
  return v;
}

Pipeline::Pipeline(Dependencias d, Opciones o)
    : dep_(std::move(d)), opt_(o), frames_(o.cola_frames), cola_(o.cola_trabajos) {
  if (dep_.db) dep_.db->script(ESQUEMA);
}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start() {
  if (started_.exchange(true)) return;
  quit_ = false;
  t_percepcion_ = std::thread([this] { bucle_percepcion(); });
  t_difusion_ = std::thread([this] { bucle_difusion(); });
  t_presentador_ = std::thread([this] { bucle_presentador(); });
}

void Pipeline::stop() {
  if (!started_.exchange(false)) return;
  quit_ = true;
  senal_frames_.notificar_todos();
  senal_cola_.notificar_todos();
  senal_pantalla_.notificar_todos();
  for (auto* t : {&t_percepcion_, &t_difusion_, &t_presentador_})
    if (t->joinable()) t->join();
  // los trabajos que quedaban en cola no se van a hacer: quien los espere recibe el error
  std::lock_guard<std::mutex> lk(mu_trabajos_);
  for (auto& [id, t] : trabajos_)
    if (t->estado == "en cola" || t->estado == "generando") {
      t->estado = "cancelado";
      try {
        t->promesa.set_exception(std::make_exception_ptr(std::runtime_error("Agente IA se esta cerrando")));
      } catch (...) {
      }
    }
}

void Pipeline::emitir(const json& e) {
  if (!dep_.emitir) return;
  try {
    dep_.emitir(e);
  } catch (const std::exception& ex) {
    L.debug("aviso del pipeline sin enviar: ", ex.what());
  }
}

// ================================================================== percepcion
bool Pipeline::enviar_frame(std::string_view bytes, const std::string& fuente) {
  if (bytes.empty()) return false;
  hal::Buffer b = hal::pool_frames().pedir(bytes.size());
  std::memcpy(b.data(), bytes.data(), bytes.size());   // la unica copia: de la peticion al bloque
  return enviar_frame(std::move(b), fuente);
}

bool Pipeline::enviar_frame(hal::Buffer datos, const std::string& fuente) {
  recibidos_.fetch_add(1, std::memory_order_relaxed);
  en_proceso_.fetch_add(1, std::memory_order_acq_rel);
  if (!frames_.push(Frame{std::move(datos), fuente, now()})) {
    en_proceso_.fetch_sub(1, std::memory_order_acq_rel);
    descartados_.fetch_add(1, std::memory_order_relaxed);   // la percepcion va atrasada: se descarta, no se espera
    return false;
  }
  senal_frames_.notificar();
  return true;
}

void Pipeline::bucle_percepcion() {
  while (!quit_) {
    const uint32_t g = senal_frames_.generacion();
    if (auto f = frames_.pop()) {
      try {
        percibir(*f);
      } catch (const std::exception& e) {
        L.warning("percepcion: frame descartado (", e.what(), ")");
        descartados_.fetch_add(1, std::memory_order_relaxed);
      }
      en_proceso_.fetch_sub(1, std::memory_order_acq_rel);
      senal_vacia_.notificar_todos();
      continue;
    }
    if (quit_) break;
    senal_frames_.esperar(g);
  }
}

bool Pipeline::esperar_percepcion(double timeout_s) {
  const double fin = now() + timeout_s;
  while (en_proceso_.load(std::memory_order_acquire) > 0) {
    if (now() > fin) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

void Pipeline::percibir(Frame& f) {
  int w = 0, h = 0, n = 0;
  unsigned char* raw = stbi_load_from_memory(f.datos.data(), (int)f.datos.size(), &w, &h, &n, 3);
  if (!raw) throw std::runtime_error(std::string("imagen no valida: ") + stbi_failure_reason());
  mem::Vec hv = huella(raw, w, h);
  json stats;
  {
    std::vector<uint8_t> rgb(raw, raw + (size_t)w * h * 3);
    stbi_image_free(raw);
    // igual que el anterior hace poco: la escena no ha cambiado
    if (!ultima_huella_.empty() && mem::dot(hv, ultima_huella_) >= opt_.umbral_repetido && f.ts - ultimo_guardado_ < 60) {
      repetidos_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    stats = media::ImageEngine::describe_stats(rgb, w, h);
  }
  ultima_huella_ = hv;
  ultimo_guardado_ = f.ts;

  // descripcion: la red de vision en el acelerador (llama-server con mmproj) si la hay y toca; si no, estadisticas
  std::string descripcion = jstr(stats, "summary"), motor = "estadisticas";
  if (dep_.analizar && dep_.vision_disponible && f.ts - ultima_vision_ >= opt_.vision_cada_s && dep_.vision_disponible()) {
    ultima_vision_ = f.ts;
    try {
      json a = dep_.analizar(std::string(f.datos.vista()));
      if (!trim(jstr(a, "description")).empty()) {
        descripcion = trim(jstr(a, "description"));
        motor = jstr(a, "engine", "vision");
      }
    } catch (const std::exception& e) {
      L.warning("percepcion: la red de vision fallo (", e.what(), "); se usan las estadisticas");
    }
  }

  std::string url;
  if (opt_.guardar_frames) {
    std::string nombre = std::to_string((long long)(f.ts * 1000)) + extension(f.datos.vista());
    fs::path dir = paths::sub({"outputs", "percepcion"});
    if (write_file(dir / P(nombre), std::string(f.datos.vista()))) url = "/outputs/percepcion/" + nombre;
  }

  json emb_blob = nullptr;
  std::string emb_id;
  if (dep_.embedder) {
    try {
      if (auto e = dep_.embedder()) {
        mem::Mat m = e->embed({descripcion});
        if (!m.empty()) {
          emb_blob = blob(m[0]);
          emb_id = e->id;
        }
      }
    } catch (const std::exception& e) {
      L.warning("percepcion: sin embedding (", e.what(), ")");
    }
  }
  long long id = 0;
  if (dep_.db) {
    dep_.db->execute(
        "INSERT INTO percepcion(ts, fuente, descripcion, motor, ancho, alto, url, emb_id, embedding, huella) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)",
        {f.ts, f.fuente, descripcion, motor, w, h, url, emb_id, emb_blob, blob(hv)});
    id = dep_.db->last_insert_id();
  }
  guardados_.fetch_add(1, std::memory_order_relaxed);
  emitir({{"type", "percepcion"}, {"id", id}, {"descripcion", descripcion}, {"motor", motor}, {"url", url},
          {"fuente", f.fuente}});
}

json Pipeline::fila(const json& r, double score) {
  json o = {{"id", r["id"]},         {"ts", r["ts"]},   {"fuente", r["fuente"]}, {"descripcion", r["descripcion"]},
            {"motor", r["motor"]},   {"ancho", r["ancho"]}, {"alto", r["alto"]}, {"url", r["url"]}};
  if (score >= 0) o["score"] = std::round(score * 1000) / 1000;
  return o;
}

json Pipeline::recientes(int n) {
  json out = json::array();
  if (!dep_.db) return out;
  for (auto& r : dep_.db->query("SELECT id, ts, fuente, descripcion, motor, ancho, alto, url FROM percepcion "
                                "ORDER BY id DESC LIMIT ?",
                                {std::clamp(n, 1, 500)}))
    out.push_back(fila(r));
  return out;
}

json Pipeline::buscar(const std::string& q, int k) {
  if (trim(q).empty()) return recientes(k);
  json out = json::array();
  if (!dep_.db) return out;
  k = std::clamp(k, 1, 50);
  mem::EmbedderPtr e = dep_.embedder ? dep_.embedder() : nullptr;
  if (e) {
    try {
      mem::Mat mq = e->embed_query({q});
      if (!mq.empty()) {
        std::vector<std::pair<float, json>> todos;
        for (auto& r : dep_.db->query("SELECT * FROM percepcion WHERE emb_id = ? ORDER BY id DESC LIMIT 5000", {e->id})) {
          mem::Vec v = floats(r["embedding"]);
          if (v.size() == mq[0].size()) todos.emplace_back(mem::dot(v, mq[0]), r);
        }
        std::stable_sort(todos.begin(), todos.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < todos.size() && (int)i < k; ++i) out.push_back(fila(todos[i].second, todos[i].first));
        if (!out.empty()) return out;
      }
    } catch (const std::exception& ex) {
      L.warning("percepcion: busqueda semantica no disponible (", ex.what(), ")");
    }
  }
  for (auto& r : dep_.db->query("SELECT id, ts, fuente, descripcion, motor, ancho, alto, url FROM percepcion "
                                "WHERE descripcion LIKE ? ORDER BY id DESC LIMIT ?",
                                {"%" + q + "%", k}))
    out.push_back(fila(r));
  return out;
}

// ================================================================== difusion
uint64_t Pipeline::difundir(DescriptorTrabajo d) {
  auto t = std::make_shared<Trabajo>();
  d.id = siguiente_.fetch_add(1);
  d.creado = now();
  t->d = std::move(d);
  t->futuro = t->promesa.get_future().share();
  const uint64_t id = t->d.id;
  {
    std::lock_guard<std::mutex> lk(mu_trabajos_);
    trabajos_[id] = t;
    // solo se recuerdan los ultimos 200
    while (trabajos_.size() > 200 && trabajos_.begin()->second->fin > 0) trabajos_.erase(trabajos_.begin());
  }
  if (!cola_.push(id)) {
    std::lock_guard<std::mutex> lk(mu_trabajos_);
    trabajos_.erase(id);
    throw std::runtime_error("la cola de difusion esta llena; prueba dentro de un rato");
  }
  senal_cola_.notificar();
  emitir({{"type", "difusion"}, {"id", id}, {"estado", "en cola"}, {"prompt", t->d.idea.empty() ? t->d.prompt : t->d.idea}});
  return id;
}

std::shared_future<json> Pipeline::resultado(uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_trabajos_);
  auto it = trabajos_.find(id);
  if (it == trabajos_.end()) throw std::out_of_range("trabajo de difusion desconocido");
  return it->second->futuro;
}

namespace {
json a_json(const DescriptorTrabajo& d, const std::string& estado, int paso, double inicio, double fin,
            const json& res, const std::string& error) {
  json o = {{"id", d.id},       {"estado", estado}, {"prompt", d.idea.empty() ? d.prompt : d.idea},
            {"origen", d.origen}, {"paso", paso},     {"creado", d.creado}};
  if (inicio > 0) o["inicio"] = inicio;
  if (fin > 0) o["fin"] = fin;
  if (res.is_object()) {
    json r = res;
    r.erase("path");
    o["resultado"] = r;
  }
  if (!error.empty()) o["error"] = error;
  return o;
}
}  // namespace

json Pipeline::trabajo(uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_trabajos_);
  auto it = trabajos_.find(id);
  if (it == trabajos_.end()) return nullptr;
  auto& t = *it->second;
  return a_json(t.d, t.estado, t.paso, t.inicio, t.fin, t.resultado, t.error);
}

json Pipeline::trabajos(int n) {
  json out = json::array();
  std::lock_guard<std::mutex> lk(mu_trabajos_);
  for (auto it = trabajos_.rbegin(); it != trabajos_.rend() && (int)out.size() < n; ++it) {
    auto& t = *it->second;
    out.push_back(a_json(t.d, t.estado, t.paso, t.inicio, t.fin, t.resultado, t.error));
  }
  return out;
}

void Pipeline::bucle_difusion() {
  while (!quit_) {
    const uint32_t g = senal_cola_.generacion();
    auto id = cola_.pop();
    if (!id) {
      if (quit_) break;
      senal_cola_.esperar(g);
      continue;
    }
    std::shared_ptr<Trabajo> t;
    {
      std::lock_guard<std::mutex> lk(mu_trabajos_);
      auto it = trabajos_.find(*id);
      if (it == trabajos_.end()) continue;
      t = it->second;
      t->estado = "generando";
      t->inicio = now();
    }
    emitir({{"type", "difusion"}, {"id", *id}, {"estado", "generando"}});
    try {
      if (!dep_.generar) throw std::runtime_error("no hay motor de difusion");
      // vista previa de cada paso: llega desde el hilo que vigila a sd.cpp, que termina antes de que generar vuelva;
      // asi el buffer de pantalla solo tiene un escritor a la vez
      VistaPrevia vp = [this, t](const fs::path& p, int paso) {
        {
          std::lock_guard<std::mutex> lk(mu_trabajos_);
          t->paso = paso;
        }
        publicar(t->d.id, paso, false, p);
      };
      json res = dep_.generar(t->d, vp);
      fs::path out = P(jstr(res, "path"));
      int paso_final = (int)jnum(res.value("steps", json(0)), 0);
      if (!out.empty()) publicar(t->d.id, paso_final, true, out);
      {
        std::lock_guard<std::mutex> lk(mu_trabajos_);
        t->estado = "hecho";
        t->fin = now();
        t->paso = std::max(t->paso, paso_final);
        t->resultado = res;
      }
      t->promesa.set_value(res);
      emitir({{"type", "difusion"}, {"id", *id}, {"estado", "hecho"}, {"url", res.value("url", json(""))},
              {"engine", res.value("engine", json(""))}});
    } catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lk(mu_trabajos_);
        t->estado = "error";
        t->fin = now();
        t->error = e.what();
      }
      try {
        t->promesa.set_exception(std::current_exception());
      } catch (...) {
      }
      emitir({{"type", "difusion"}, {"id", *id}, {"estado", "error"}, {"error", e.what()}});
    }
  }
}

// ================================================================== buffer de pantalla
void Pipeline::publicar(uint64_t trabajo, int paso, bool final, const fs::path& fichero) {
  hal::Buffer b = leer_en_bloque(fichero);
  if (!b) return;
  FramePantalla f;
  f.imagen = std::move(b);
  f.trabajo = trabajo;
  f.paso = paso;
  f.final = final;
  f.version = version_.fetch_add(1) + 1;
  pantalla_buf_.publicar(std::move(f));
  senal_pantalla_.notificar();
}

void Pipeline::bucle_presentador() {
  while (!quit_) {
    const uint32_t g = senal_pantalla_.generacion();
    if (pantalla_buf_.recoger()) {
      const FramePantalla& f = pantalla_buf_.actual();
      {
        std::lock_guard<std::mutex> lk(mu_pantalla_);
        actual_ = f;   // comparte el bloque: no se copia la imagen
      }
      emitir({{"type", "pantalla"},
              {"trabajo", f.trabajo},
              {"paso", f.paso},
              {"final", f.final},
              {"version", f.version},
              {"bytes", f.imagen.size()},
              {"url", "/api/pipeline/pantalla?v=" + std::to_string(f.version)}});
      continue;
    }
    if (quit_) break;
    senal_pantalla_.esperar(g);
  }
}

FramePantalla Pipeline::pantalla() {
  std::lock_guard<std::mutex> lk(mu_pantalla_);
  return actual_;
}

json Pipeline::estado() {
  json cola = json::object();
  {
    std::lock_guard<std::mutex> lk(mu_trabajos_);
    int en_cola = 0, generando = 0, hechos = 0, errores = 0;
    for (auto& [id, t] : trabajos_) {
      if (t->estado == "en cola") ++en_cola;
      else if (t->estado == "generando") ++generando;
      else if (t->estado == "hecho") ++hechos;
      else if (t->estado == "error") ++errores;
    }
    cola = {{"en_cola", en_cola}, {"generando", generando}, {"hechos", hechos}, {"errores", errores},
            {"capacidad", cola_.capacity()}};
  }
  long long guardados_db = 0;
  if (dep_.db) guardados_db = (long long)jnum(dep_.db->scalar("SELECT COUNT(*) FROM percepcion"), 0);
  FramePantalla p = pantalla();
  return {{"activo", started_.load()},
          {"percepcion",
           {{"recibidos", recibidos_.load()},
            {"descartados", descartados_.load()},
            {"repetidos", repetidos_.load()},
            {"guardados", guardados_.load()},
            {"en_cola", frames_.size()},
            {"capacidad", frames_.capacity()},
            {"base_vectorial", guardados_db},
            {"vision", dep_.vision_disponible ? dep_.vision_disponible() : false},
            {"vision_cada_s", opt_.vision_cada_s}}},
          {"difusion", cola},
          {"pantalla",
           {{"version", p.version}, {"trabajo", p.trabajo}, {"paso", p.paso}, {"final", p.final},
            {"bytes", p.imagen.size()}, {"memoria", p.imagen ? hal::nombre(p.imagen.memoria()) : ""}}}};
}

}  // namespace nx::pipeline
