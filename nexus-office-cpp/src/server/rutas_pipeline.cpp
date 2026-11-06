// Rutas de la arquitectura en tres capas: capa de hardware (asignador zero-copy y rings lock-free), motor de
// percepcion (frames -> base vectorial), difusion asincrona y buffer de pantalla. Ver docs/ARQUITECTURA.md.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

#include "core/services.h"
#include "hal/hal.h"
#include "pipeline/pipeline.h"
#include "server/app.h"
#include "util/crypto.h"
#include "llm/motores.h"
#include "media/difusion_onnx.h"
#include "httplib.h"
#include "util/text.h"

namespace nx::server {

namespace {

pipeline::Pipeline& P_(Ctx& c) {
  auto& p = c.app.S().pipeline;
  if (!p) throw HttpErr(503, "El pipeline no esta activo");
  return *p;
}

}  // namespace

void register_pipeline_routes(App& app) {
  // los avisos de los pipelines (percepcion, difusion, pantalla) van a todas las interfaces por WebSocket
  if (app.ready() && !app.remote()) app.S().emitir_pipeline = [&app](const json& e) { app.broadcast(e); };

  // ------------------------------------------------------------------ capa de abstraccion de hardware
  app.get("/api/hal", [](Ctx& c) -> Out {
    json e = hal::estado(c.app.S().profile);
    // ONNX Runtime que usa la difusion propia (version y proveedores compilados) y motores dentro del proceso
    auto ort = difusion::biblioteca_ort();
    e["onnxruntime"] = ort ? difusion::info_ort(*ort) : json();
    e["motores_en_proceso"] = {{"activo", llm::MotorBackend::en_proceso.load()}, {"caidas", llm::MotorBackend::caidas()}};
    return e;
  });
  // vuelve a probar dentro del proceso los motores que se cayeron (solo Admin)
  app.post("/api/hal/caidas/olvidar", [](Ctx&) -> Out {
    llm::MotorBackend::olvidar_caidas();
    return json{{"ok", true}};
  }, true);

  // paso de mensajes entre hilos por los rings lock-free (ida y vuelta medida en este equipo)
  app.get("/api/hal/latencia", [](Ctx& c) -> Out {
    long vueltas = (long)std::clamp<long long>(c.qi("vueltas", 100000), 1000, 2000000);
    auto l = hal::medir_latencia(vueltas);
    return json{{"ida_ns", std::round(l.ida_ns * 10) / 10},
                {"vuelta_ns", std::round(l.vuelta_ns * 10) / 10},
                {"vueltas", l.vueltas},
                {"submicrosegundo", l.ida_ns < 1000}};
  });

  // ------------------------------------------------------------------ pipelines
  app.get("/api/pipeline", [](Ctx& c) -> Out {
    json e = P_(c).estado();
    e["hal"] = hal::estado(c.app.S().profile);
    return e;
  });

  // frame para el motor de percepcion: multipart (campo «frame») o el cuerpo tal cual (image/jpeg, image/png...)
  app.post("/api/pipeline/frame", [](Ctx& c) -> Out {
    auto& p = P_(c);
    std::string fuente = c.q("fuente", "api");
    bool ok;
    if (auto f = c.file("frame")) {
      if (f->content.empty()) throw HttpErr(422, "Frame vacio");
      ok = p.enviar_frame(f->content, fuente);
    } else if (!c.req.body.empty()) {
      ok = p.enviar_frame(c.req.body, fuente);
    } else {
      throw missing_field("body", "frame");
    }
    if (!ok) {
      c.res.status = 429;   // la percepcion va atrasada: el frame se descarta y la camara no tiene que esperar
      return json{{"aceptado", false}, {"detail", "Cola de percepcion llena; frame descartado"}};
    }
    if (c.qb("esperar")) p.esperar_percepcion(c.qi("timeout", 60));
    return json{{"aceptado", true}};
  });

  // codificador de video HTML (Estudio): el WebM que hace el navegador con WebCodecs se guarda en outputs/videos
  app.post("/api/media/video/subir", [](Ctx& c) -> Out {
    const std::string& b = c.req.body;
    if (b.size() < 4 || (unsigned char)b[0] != 0x1A || (unsigned char)b[1] != 0x45 || (unsigned char)b[2] != 0xDF ||
        (unsigned char)b[3] != 0xA3)
      throw HttpErr(422, "No es un video WebM");
    std::string nombre = c.q("nombre", "video");
    std::string limpio;
    for (unsigned char ch : nombre)
      if (std::isalnum(ch) || ch == '-' || ch == '_') limpio += (char)ch;
    if (limpio.empty()) limpio = "video";
    fs::path f = paths::sub({"outputs", "videos"}) / P(std::to_string((long long)(now() * 1000)) + "_" + limpio.substr(0, 40) + ".webm");
    if (!write_file(f, b)) throw HttpErr(500, "No se pudo guardar el video");
    return json{{"url", "/outputs/videos/" + S(f.filename())}, {"bytes", b.size()}};
  });

  app.get("/api/pipeline/percepcion", [](Ctx& c) -> Out {
    auto& p = P_(c);
    int k = (int)c.qi("k", 10);
    return json{{"resultados", c.q("q").empty() ? p.recientes(k) : p.buscar(c.q("q"), k)}};
  });

  // orquestador -> difusion: encargo asincrono; devuelve el id y la imagen sale por el buffer de pantalla
  app.post("/api/pipeline/difusion", [](Ctx& c) -> Out {
    auto& p = P_(c);
    pipeline::DescriptorTrabajo d;
    d.prompt = c.bs("prompt");
    if (trim(d.prompt).empty()) throw HttpErr(422, "Falta el prompt");
    d.negativo = c.bs("negative", "");
    d.idea = c.bs("idea", "");
    d.tam = (int)c.bn("size", 0);
    d.pasos = (int)c.bn("steps", 0);
    if (c.has_b("seed") && c.body["seed"].is_number()) d.semilla = (long long)jnum(c.body["seed"]);
    d.origen = "api";
    uint64_t id;
    try {
      id = p.difundir(std::move(d));
    } catch (const std::runtime_error& e) {
      throw HttpErr(429, e.what());
    }
    return json{{"id", id}, {"estado", "en cola"}, {"pantalla", "/api/pipeline/pantalla"}};
  });

  app.get("/api/pipeline/difusion", [](Ctx& c) -> Out { return P_(c).trabajos((int)c.qi("n", 20)); });

  app.get("/api/pipeline/difusion/{id}", [](Ctx& c) -> Out {
    json t = P_(c).trabajo((uint64_t)c.pi("id"));
    if (t.is_null()) throw HttpErr(404, "Trabajo desconocido");
    return t;
  });

  // ultimo frame del buffer de pantalla: los bytes salen directamente del bloque zero-copy (sin copiarlos)
  app.get("/api/pipeline/pantalla", [](Ctx& c) -> Out {
    pipeline::FramePantalla f = P_(c).pantalla();
    if (!f.imagen) return Raw{204, "", "", {{"Cache-Control", "no-store"}}};
    auto keep = std::make_shared<hal::Buffer>(f.imagen);
    Raw r{200, "", "image/png",
          {{"Cache-Control", "no-store"},
           {"X-Pantalla-Version", std::to_string(f.version)},
           {"X-Pantalla-Trabajo", std::to_string(f.trabajo)},
           {"X-Pantalla-Paso", std::to_string(f.paso)},
           {"X-Pantalla-Final", f.final ? "1" : "0"}}};
    r.vista = (const char*)keep->data();
    r.vista_len = keep->size();
    r.retener = keep;
    return r;
  });
}

}  // namespace nx::server
