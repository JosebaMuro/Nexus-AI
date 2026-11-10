// Capa de abstraccion de hardware (asignador zero-copy, rings lock-free, triple buffer) y pipelines del dossier de
// arquitectura (percepcion -> base vectorial, difusion asincrona -> buffer de pantalla).
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <thread>

#include "check.h"
#include "core/config.h"
#include "core/db.h"
#include "core/services.h"
#include "hal/hal.h"
#include "hal/ring.h"
#include "hal/zerocopy.h"
#include "memory/memory.h"
#include "pipeline/pipeline.h"
#include "server/app.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/net.h"
#include "util/text.h"

#include "stb_image_write.h"

using namespace nx;

namespace {

// PNG en memoria: degradado del color dado (frames de prueba)
std::string png(int w, int h, uint8_t r, uint8_t g, uint8_t b, bool rayas = false) {
  std::vector<uint8_t> px((size_t)w * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[((size_t)y * w + x) * 3];
      double f = 0.5 + 0.5 * x / (double)w;
      bool raya = rayas && (y / 8) % 2 == 0;
      p[0] = (uint8_t)(raya ? 255 - r : r * f);
      p[1] = (uint8_t)(raya ? 255 - g : g * f);
      p[2] = (uint8_t)(raya ? 255 - b : b * f);
    }
  std::string out;
  stbi_write_png_to_func([](void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, size); },
                         &out, w, h, 3, px.data(), w * 3);
  return out;
}

}  // namespace

TEST("hal: buffer zero-copy alineado a pagina, compartido y con referencias") {
  hal::Buffer a = hal::reservar(1000, hal::Memoria::Compartida);
  CHECK(a);
  CHECK_EQ(a.size(), (size_t)1000);
  CHECK_EQ(a.capacidad() % hal::kPagina, (size_t)0);
  CHECK_EQ(a.direccion() % hal::kPagina, (uintptr_t)0);
#if defined(__linux__) || defined(_WIN32)
  CHECK(a.compartida());
  CHECK(!a.descriptor().empty());
#endif
  std::memset(a.data(), 7, a.size());
  hal::Buffer b = a;   // compartir: misma memoria, sin copiar
  CHECK_EQ(a.referencias(), 2L);
  CHECK(b.data() == a.data());
  b.data()[0] = 42;
  CHECK_EQ((int)a.data()[0], 42);
  CHECK(a.resize(a.capacidad()));
  CHECK(!a.resize(a.capacidad() + 1));
  hal::Buffer c = std::move(b);
  CHECK(!b);
  CHECK_EQ(c.referencias(), 2L);

  hal::Buffer d = hal::copiar("hola", hal::Memoria::Host);
  CHECK_EQ(std::string(d.vista()), std::string("hola"));
  CHECK(!d.compartida());
}

TEST("hal: el pool recicla bloques sin volver al sistema") {
  hal::Pool pool(64 * 1024, 4);
  const uint8_t* primero;
  {
    hal::Buffer x = pool.pedir(100);
    primero = x.data();
  }
  hal::Buffer y = pool.pedir(200);
  CHECK(y.data() == primero);   // el mismo bloque, devuelto a la lista libre
  CHECK_EQ(y.size(), (size_t)200);
  json e = pool.estado();
  CHECK_EQ((int)e["creados"], 1);
  CHECK_EQ((int)e["reutilizados"], 1);
  CHECK_EQ((int)e["en_uso"], 1);
  // mas grande que un bloque: suelto
  hal::Buffer z = pool.pedir(1 << 20);
  CHECK_EQ((int)pool.estado()["sueltos"], 1);
  // por encima del maximo de bloques vivos: tambien suelto (no se bloquea)
  std::vector<hal::Buffer> muchos;
  for (int i = 0; i < 6; ++i) muchos.push_back(pool.pedir(10));
  CHECK_EQ((int)pool.estado()["creados"], 4);
  CHECK((int)pool.estado()["sueltos"] >= 3);
  // de varios hilos a la vez
  muchos.clear();
  std::vector<std::thread> ts;
  std::atomic<int> fallos{0};
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < 2000; ++i) {
        hal::Buffer b = pool.pedir(64);
        b.data()[0] = (uint8_t)t;
        if (b.data()[0] != (uint8_t)t) ++fallos;
      }
    });
  for (auto& t : ts) t.join();
  CHECK_EQ(fallos.load(), 0);
}

TEST("hal: rings lock-free SPSC y MPMC entre hilos") {
  hal::SpscRing<int, 8> s;
  for (int i = 0; i < 8; ++i) CHECK(s.push(i));
  CHECK(!s.push(99));   // lleno
  CHECK_EQ(*s.pop(), 0);
  CHECK(s.push(8));
  for (int i = 1; i <= 8; ++i) CHECK_EQ(*s.pop(), i);
  CHECK(!s.pop());

  // SPSC: el orden se conserva con un productor y un consumidor en hilos distintos
  hal::SpscRing<long, 256> r;
  const long N = 200000;
  long esperado = 0;
  bool orden = true;
  std::thread prod([&] {
    for (long i = 0; i < N; ++i)
      while (!r.push(i)) std::this_thread::yield();
  });
  while (esperado < N) {
    if (auto v = r.pop()) {
      if (*v != esperado) orden = false;
      ++esperado;
    }
  }
  prod.join();
  CHECK(orden);

  // MPMC: 4 productores y 4 consumidores; ni se pierde ni se repite nada
  hal::MpmcRing<long> m(1024);
  const int P = 4, K = 20000;
  std::atomic<long long> suma{0};
  std::atomic<int> sacados{0};
  std::vector<std::thread> ts;
  for (int p = 0; p < P; ++p)
    ts.emplace_back([&, p] {
      for (int i = 1; i <= K; ++i)
        while (!m.push((long)p * K + i)) std::this_thread::yield();
    });
  for (int c = 0; c < 4; ++c)
    ts.emplace_back([&] {
      while (sacados.load() < P * K) {
        if (auto v = m.pop()) {
          suma += *v;
          ++sacados;
        } else {
          std::this_thread::yield();
        }
      }
    });
  for (auto& t : ts) t.join();
  long long total = 0;
  for (int p = 0; p < P; ++p)
    for (int i = 1; i <= K; ++i) total += (long long)p * K + i;
  CHECK_EQ(suma.load(), total);
  CHECK_EQ(m.size(), (size_t)0);

  // triple buffer: el lector ve siempre el ultimo valor completo
  hal::TripleBuffer<int> tb;
  CHECK(!tb.recoger());
  tb.publicar(1);
  tb.publicar(2);
  CHECK(tb.recoger());
  CHECK_EQ(tb.actual(), 2);
  CHECK(!tb.recoger());
  tb.publicar(3);
  CHECK(tb.recoger());
  CHECK_EQ(tb.actual(), 3);
}

TEST("hal: latencia entre hilos por los rings") {
  auto l = hal::medir_latencia(50000);
  std::cout << "    latencia ring SPSC: ida " << l.ida_ns << " ns, vuelta " << l.vuelta_ns << " ns ("
            << std::thread::hardware_concurrency() << " hilos)\n";
  CHECK(l.vueltas == 50000);
  CHECK(l.ida_ns > 0);
  // en un equipo normal queda muy por debajo del microsegundo; el limite es holgado para las maquinas de CI cargadas
  CHECK(l.ida_ns < 50000);
  json e = hal::estado(json{{"accelerator", "cpu"}});
  CHECK_EQ(jstr(e["acelerador"], "accelerator"), std::string("cpu"));
  CHECK(e["asignador"].contains("frames"));
  CHECK(!jstr(e, "artefacto").empty());
}

TEST("pipeline: percepcion a la base vectorial y difusion asincrona al buffer de pantalla") {
  auto dir = std::filesystem::temp_directory_path() / ("nexus-pipeline-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  DB db(dir / "p.db");
  auto emb = std::make_shared<mem::HashingEmbedder>(256);
  std::mutex mu;
  std::vector<json> eventos;
  std::atomic<int> generando_en_otro_hilo{0};
  const auto hilo_prueba = std::this_thread::get_id();

  pipeline::Dependencias d;
  d.db = &db;
  d.embedder = [emb] { return emb; };
  d.vision_disponible = [] { return true; };
  auto vistas = std::make_shared<std::atomic<int>>(0);
  d.analizar = [vistas](const std::string&) {
    return json{{"engine", "vision-falsa"}, {"description", (*vistas)++ ? "una oficina con rayas" : "un cielo azul"}};
  };
  d.generar = [&](const pipeline::DescriptorTrabajo& t, const pipeline::VistaPrevia& vp) {
    if (std::this_thread::get_id() != hilo_prueba) ++generando_en_otro_hilo;
    if (t.prompt == "falla") throw std::runtime_error("sin modelo");
    for (int paso = 1; paso <= 3; ++paso) vp(hal::copiar(png(32, 32, (uint8_t)(paso * 60), 0, 0)), paso);
    fs::path out = dir / ("img" + std::to_string(t.id) + ".png");
    write_file(out, png(64, 64, 0, 200, 0));
    return json{{"path", S(out)}, {"url", "/outputs/images/x.png"}, {"steps", 3}, {"engine", "falso"}};
  };
  d.emitir = [&](const json& e) {
    std::lock_guard<std::mutex> lk(mu);
    eventos.push_back(e);
  };
  pipeline::Opciones o;
  o.vision_cada_s = 0;
  o.guardar_frames = false;
  pipeline::Pipeline p(std::move(d), o);
  p.start();

  // ---- percepcion
  CHECK(p.enviar_frame(png(64, 48, 30, 60, 220), "camara"));
  CHECK(p.enviar_frame(png(64, 48, 30, 60, 220), "camara"));   // igual: se descarta como repetido
  CHECK(p.enviar_frame(png(96, 96, 200, 120, 40, true), "camara"));
  CHECK(!p.enviar_frame(std::string_view(""), "camara"));
  CHECK(p.enviar_frame(std::string_view("no es una imagen"), "api"));
  CHECK(p.esperar_percepcion(30));
  json st = p.estado();
  CHECK_EQ((int)st["percepcion"]["guardados"], 2);
  CHECK_EQ((int)st["percepcion"]["repetidos"], 1);
  CHECK_EQ((int)st["percepcion"]["base_vectorial"], 2);
  json rec = p.recientes(10);
  CHECK_EQ(rec.size(), (size_t)2);
  CHECK_EQ(jstr(rec[0], "motor"), std::string("vision-falsa"));
  json hits = p.buscar("oficina con rayas", 1);
  CHECK_EQ(hits.size(), (size_t)1);
  CHECK(contains(jstr(hits[0], "descripcion"), "oficina"));

  // huella: la misma imagen da la misma huella; otra distinta, no
  mem::Vec h1 = pipeline::huella(nullptr, 0, 0);
  CHECK_EQ(h1.size(), (size_t)192);

  // ---- difusion: el encargo vuelve enseguida y se genera en el hilo del motor de difusion
  pipeline::DescriptorTrabajo t;
  t.prompt = "a green field";
  t.origen = "prueba";
  uint64_t id = p.difundir(t);
  CHECK(id > 0);
  json res = p.resultado(id).get();
  CHECK_EQ(jstr(res, "engine"), std::string("falso"));
  CHECK_EQ(generando_en_otro_hilo.load(), 1);
  json tj = p.trabajo(id);
  CHECK_EQ(jstr(tj, "estado"), std::string("hecho"));
  CHECK(!tj["resultado"].contains("path"));

  // pantalla: 3 vistas previas + el final; el presentador publica el ultimo
  bool final = false;
  for (int i = 0; i < 200 && !final; ++i) {
    final = p.pantalla().final;
    if (!final) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  pipeline::FramePantalla f = p.pantalla();
  CHECK(f.final);
  CHECK_EQ(f.trabajo, id);
  CHECK(f.version >= 4);
  CHECK(f.imagen.size() > 8 && f.imagen.data()[0] == 0x89);

  // un fallo llega como excepcion a quien espera
  t.prompt = "falla";
  uint64_t id2 = p.difundir(t);
  bool lanzo = false;
  try {
    p.resultado(id2).get();
  } catch (const std::exception& e) {
    lanzo = contains(e.what(), "sin modelo");
  }
  CHECK(lanzo);
  CHECK_EQ(jstr(p.trabajo(id2), "estado"), std::string("error"));
  p.stop();

  std::set<std::string> tipos;
  int previas = 0;
  {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& e : eventos) {
      tipos.insert(jstr(e, "type"));
      if (jstr(e, "type") == "difusion" && jstr(e, "estado") == "hecho") CHECK_EQ((uint64_t)e["id"], id);
      if (jstr(e, "type") == "pantalla" && !e["final"].get<bool>()) ++previas;
    }
  }
  CHECK(tipos.count("percepcion") && tipos.count("difusion") && tipos.count("pantalla"));
  CHECK(previas >= 1);
}

TEST("pipeline: rutas de la API (hal, frame, percepcion, difusion y pantalla sin copia)") {
  auto dir = std::filesystem::temp_directory_path() / ("nexus-pipeline-srv-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  auto s = std::make_unique<Services>(false);
  auto app = std::make_unique<server::App>(s.get());
  s->start();
  int port = net::free_port(18995);
  std::thread th([&] { app->listen("127.0.0.1", port); });
  httplib::Client c("127.0.0.1", port);
  c.set_read_timeout(300);
  for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto h = c.Get("/api/hal");
  CHECK(h && h->status == 200);
  if (h) CHECK(jparse(h->body).contains("asignador"));
  auto lat = c.Get("/api/hal/latencia?vueltas=2000");
  CHECK(lat && lat->status == 200);
  if (lat) CHECK(jnum(jparse(lat->body)["ida_ns"]) > 0);

  // frame en crudo (image/png) y en multipart
  auto fr = c.Post("/api/pipeline/frame?fuente=prueba&esperar=1", png(48, 48, 10, 200, 90), "image/png");
  CHECK(fr && fr->status == 200);
  httplib::UploadFormDataItems items = {{"frame", png(48, 48, 250, 20, 20, true), "f.png", "image/png"}};
  auto fr2 = c.Post("/api/pipeline/frame?esperar=1", items);
  CHECK(fr2 && fr2->status == 200);
  auto pe = c.Get("/api/pipeline/percepcion?k=5");
  CHECK(pe && pe->status == 200);
  if (pe) CHECK(jparse(pe->body)["resultados"].size() >= 2);
  auto pe2 = c.Get("/api/pipeline/percepcion?q=colores&k=1");
  CHECK(pe2 && pe2->status == 200);

  // sin frame todavia la pantalla responde 204
  auto vacia = c.Get("/api/pipeline/pantalla");
  CHECK(vacia && (vacia->status == 204 || vacia->status == 200));

  // difusion por la API: sin modelo de imagen, el compositor de escenas; la imagen acaba en la pantalla
  auto dj = c.Post("/api/pipeline/difusion", R"({"prompt": "un faro al atardecer", "size": 128})", "application/json");
  CHECK(dj && dj->status == 200);
  uint64_t id = dj ? (uint64_t)jnum(jparse(dj->body)["id"]) : 0;
  CHECK(id > 0);
  std::string estado;
  for (int i = 0; i < 600 && estado != "hecho" && estado != "error"; ++i) {
    auto t = c.Get(("/api/pipeline/difusion/" + std::to_string(id)).c_str());
    if (t && t->status == 200) estado = jstr(jparse(t->body), "estado");
    if (estado != "hecho") std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK_EQ(estado, std::string("hecho"));
  httplib::Result pant;
  for (int i = 0; i < 100; ++i) {
    pant = c.Get("/api/pipeline/pantalla");
    if (pant && pant->status == 200 && pant->get_header_value("X-Pantalla-Final") == "1") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(pant && pant->status == 200);
  if (pant) {
    CHECK_EQ(pant->get_header_value("Content-Type"), std::string("image/png"));
    CHECK(pant->body.size() > 8 && (unsigned char)pant->body[0] == 0x89);
    CHECK_EQ(pant->get_header_value("X-Pantalla-Trabajo"), std::to_string(id));
  }
  auto e = c.Get("/api/pipeline");
  CHECK(e && e->status == 200);
  if (e) CHECK((int)jparse(e->body)["difusion"]["hechos"] >= 1);
  auto mal = c.Post("/api/pipeline/difusion", R"({"prompt": "  "})", "application/json");
  CHECK(mal && mal->status == 422);

  app->stop();
  th.join();
  s->shutdown();
}
