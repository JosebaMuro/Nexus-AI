// Prueba de humo de un Nexus-Office ya instalado desde su instalador: con modelos reales descargados de internet,
// comprueba el selector de motor y acelerador, el chat por WebSocket, la voz (Piper y Whisper), la imagen
// (stable-diffusion.cpp) y el servicio del sistema. Sale con error si algo falla.
//
//   nexus-construir humo <carpeta instalada> [--exe ruta/al/nexus-office] [--modelo qwen3.5-0.8b]
//                        [--sin-imagen] [--sin-servicio] [--acelerador auto|cpu|gpu|npu]
#include <chrono>
#include <iostream>
#include <thread>

#include <httplib.h>

#include "comun.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {

int PORT = 8799;

std::string url(const std::string& path) { return cat("http://127.0.0.1:", PORT, path); }

http::Response call_raw(const std::string& path, const json* body = nullptr, double timeout = 900) {
  http::Request r;
  r.method = body ? "POST" : "GET";
  r.url = url(path);
  r.timeout = timeout;
  if (body) {
    r.body = body->dump();
    r.content_type = "application/json";
  }
  return http::send(r);
}

json call(const std::string& path, const json* body = nullptr, double timeout = 900) {
  auto r = call_raw(path, body, timeout);
  if (r.status == 0) throw http::HttpError(0, r.error);
  if (!r.ok()) throw http::HttpError(r.status, cat(path, ": HTTP ", r.status, " ", uleft(r.body, 300)));
  return r.json_body();
}

json post(const std::string& path, const json& body, double timeout = 900) { return call(path, &body, timeout); }

json upload(const std::string& path, const std::string& field, const std::string& name, const std::string& content) {
  std::string b = uuid_hex();
  http::Request r;
  r.method = "POST";
  r.url = url(path);
  r.timeout = 600;
  r.content_type = "multipart/form-data; boundary=" + b;
  r.body = cat("--", b, "\r\nContent-Disposition: form-data; name=\"", field, "\"; filename=\"", name,
               "\"\r\nContent-Type: application/octet-stream\r\n\r\n", content, "\r\n--", b, "--\r\n");
  auto res = http::send(r);
  if (!res.ok()) throw http::HttpError(res.status, cat(path, ": HTTP ", res.status, " ", res.error));
  return res.json_body();
}

std::string ws_chat(const std::string& text) {
  httplib::ws::WebSocketClient ws(cat("ws://127.0.0.1:", PORT, "/ws"));
  ws.set_connection_timeout(10);
  ws.set_read_timeout(600);
  if (!ws.connect()) throw Fallo("no conecta el WebSocket /ws");
  std::string msg;
  ws.read(msg);   // saludo
  ws.send(json{{"type", "chat"}, {"text", text}, {"agent", "auto"}}.dump());
  std::string out;
  while (true) {
    auto r = ws.read(msg);
    if (r == httplib::ws::Fail || r == httplib::ws::Timeout) throw Fallo("el chat por WebSocket se ha cortado");
    if (r != httplib::ws::Text) continue;
    json e = jparse(msg);
    std::string type = jstr(e, "type");
    if (type == "token") out += jstr(e, "text");
    if (type == "done" || type == "error") {
      ws.close();
      return !out.empty() ? out : !jstr(e, "text").empty() ? jstr(e, "text") : jstr(e, "error");
    }
  }
}

}  // namespace

int humo(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {"sin-imagen", "sin-servicio"});
  if (a.pos.empty()) throw Fallo("uso: nexus-construir humo <carpeta instalada> [--sin-imagen] [--sin-servicio]");
  fs::path home = fs::absolute(P(a.pos[0]));
  std::string modelo = a.val("modelo", "qwen3.5-0.8b"), acelerador = a.val("acelerador", "auto");
  bool sin_imagen = a.tiene("sin-imagen"), sin_servicio = a.tiene("sin-servicio");
  std::string exe = a.val("exe");
  if (exe.empty())
    for (auto& c : {home / "nexus-office.exe", home / "nexus-office"})
      if (is_file(c)) { exe = S(c); break; }
  if (exe.empty()) throw Fallo("no encuentro nexus-office en " + S(home));
  fs::path data = env("NEXUS_DATA_DIR").empty() ? fs::temp_directory_path() / ("nexus-humo-" + token_hex(4))
                                                : P(env("NEXUS_DATA_DIR"));
  fs::create_directories(data);
  std::vector<std::string> fails;

  auto check = [&](bool ok, const std::string& what) {
    std::cout << (ok ? "OK    " : "FALLO ") << what << std::endl;
    if (!ok) fails.push_back(what);
    return ok;
  };
  proc::Options opt;
  opt.cwd = home;
  opt.env = {{"NEXUS_DATA_DIR", S(data)}};
  auto cli = [&](std::vector<std::string> args, double timeout = 3600) {
    proc::Options o = opt;
    o.capture = true;
    std::vector<std::string> cmd = {exe};
    cmd.insert(cmd.end(), args.begin(), args.end());
    auto r = proc::run(cmd, timeout, o);
    std::cout << "$ nexus-office " << join(args, " ") << " -> " << r.code << "\n"
              << uright(r.out, 1500) << uright(r.err, 1500) << std::endl;
    return r;
  };

  check(cli({"--version"}).ok(), "el programa arranca (--version)");
  check(cli({"detect"}).ok(), "deteccion del hardware");
  auto r = cli({"pull", modelo});
  check(r.ok() && contains(r.out, "completado"), "modelo " + modelo + " descargado");
  check(cli({"pull", "piper-es-davefx"}).ok(), "voz de Piper descargada");
  if (!sin_imagen) check(cli({"pull", "sdxs-512"}).ok(), "modelo de imagen SDXS-512 descargado");

  fs::path log_path = data / "servidor.log";
  proc::Options so = opt;
  so.output_file = log_path;
  proc::Process srv;
  srv.start({exe, "run", "--no-browser", "--port", std::to_string(PORT)}, so);
  try {
    json opts;
    for (int i = 0; i < 120; ++i) {
      try {
        opts = call("/api/accel", nullptr, 5);
        break;
      } catch (const std::exception&) {
        if (!srv.running()) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    if (!check(opts.is_object(), "servidor arrancado")) throw Fallo("el servidor no arranca");
    json rt = call("/api/runtimes");
    std::cout << rt["bundled"].dump() << std::endl;
    auto bundled = [&](const char* k) {   // bool() de Python: ni null, ni false, ni vacio
      if (!rt["bundled"].contains(k)) return false;
      const json& v = rt["bundled"][k];
      if (v.is_null() || v == false) return false;
      if (v.is_string() || v.is_array() || v.is_object()) return !v.empty() && v != "";
      return true;
    };
    check(bundled("llama"), "llama-server incluido: " + jstr(rt["bundled"], "llama"));
    check(bundled("sd") || sin_imagen, "sd-cli incluido: " + jstr(rt["bundled"], "sd"));
    json llama;
    for (auto& e : opts["engines"])
      if (jstr(e, "id") == "llamaserver") llama = e;
    check(jget(llama, "available", false), "selector: llama-server disponible (" + jstr(llama, "note") + ")");
    json res = post("/api/accel", {{"engine", "llamaserver"}, {"accel", "cpu"}, {"remember", true}});
    json t = post("/api/accel/test", json::object());
    json resumen;
    for (const char* k : {"ok", "backend", "text", "seconds", "tokens_s", "error", "effective"})
      resumen[k] = t.contains(k) ? t[k] : json();
    std::cout << jdump(resumen) << std::endl;
    check(jget(t, "ok", false) && jstr(t, "backend") == "llamaserver" && !jstr(t, "text").empty(),
          cat("llama-server en CPU genera texto (", jstr(t, "tokens_s"), " tok/s): ",
              jstr(res, "note").empty() ? "sin avisos" : jstr(res, "note")));
    for (std::string acc : {"gpu", "npu"}) {   // sin GPU ni NPU en el equipo de pruebas: debe decirlo y seguir
      res = post("/api/accel", {{"engine", "auto"}, {"accel", acc}, {"remember", true}});
      t = post("/api/accel/test", json::object());
      check(jget(t, "ok", false), cat(acc, " pedida en un equipo sin ", upper(acc), ": responde igual (", jstr(res, "note"),
                                      "; ", jstr(t, "backend"), ": ",
                                      jstr(t, "error").empty() ? jstr(t, "text") : jstr(t, "error"), ")"));
    }
    post("/api/accel", {{"engine", "auto"}, {"accel", acelerador}, {"remember", true}});

    std::string answer = ws_chat("Responde con una sola palabra: ¿cual es la capital de Francia?");
    // Un modelo de 0.8B puede equivocarse de capital: lo que se comprueba es que genera texto coherente (palabras
    // reales, no simbolos sueltos como los que salen con un acelerador roto).
    static const Rx punct(R"([*_.,:;¡!¿?"'()\-])");
    static const Rx word(R"([a-záéíóúñ]{3,})");
    static const Rx paris(R"(par[ií]s)");
    std::string limpio = trim(punct.sub(answer, ""));
    size_t letras = 0, total = 0;
    for (char32_t c : u32(limpio)) {
      ++total;
      if (is_alpha_cp(c) || is_space_cp(c)) ++letras;
    }
    std::string low = lower(limpio);
    bool coherente = word.test(low) && letras >= 0.8 * total;
    if (coherente && !paris.test(lower(answer)))
      std::cout << "AVISO el modelo responde con texto coherente pero no acierta: " << uleft(answer, 120) << std::endl;
    check(coherente, "chat por WebSocket con el modelo: " + uleft(answer, 120));
    std::string calc = ws_chat("Cuanto es 12*7?");
    check(contains(calc, "84"), "chat con herramienta (calculadora): " + uleft(calc, 120));

    json tts = {{"text", "Hola, soy Nexus-Office."}};
    auto audio = call_raw("/api/voice/tts", &tts);
    std::string engine = audio.header("x-tts-engine");
    bool riff = audio.body.compare(0, 4, "RIFF") == 0;
    check(audio.status == 200 && engine == "piper" && riff,
          cat("voz con Piper: ", audio.status, " ", engine, " ", audio.body.size(), " bytes"));
    if (audio.status == 200 && riff) {
      json stt = upload("/api/voice/stt", "file", "voz.wav", audio.body);
      std::cout << uleft(jdump(stt), 300) << std::endl;
      check(!trim(jstr(stt, "text")).empty() && jstr(stt, "error").empty(),
            "dictado con Whisper: " + (jstr(stt, "text").empty() ? jstr(stt, "error") : jstr(stt, "text")));
    }
    if (!sin_imagen) {
      json img = post("/api/image/generate", {{"prompt", "un faro al atardecer"}, {"seed", 3}, {"steps", 1}}, 1800);
      json show;
      for (const char* k : {"engine", "url", "note", "error"}) show[k] = img.contains(k) ? img[k] : json();
      std::cout << jdump(show) << std::endl;
      std::string eng = jstr(img, "engine");
      check(!eng.empty() && eng != "procedural" && !jstr(img, "url").empty(),
            "imagen con stable-diffusion.cpp: " + eng + " " + jstr(img, "note"));
    }
  } catch (const std::exception& e) {
    check(false, std::string("error durante la prueba: ") + e.what());
  }
  srv.terminate();
  if (!srv.wait(30)) srv.kill();
  std::cout << uright(read_text(log_path), 4000) << std::endl;

  if (!sin_servicio) {
    auto s = cli({"servicio", "instalar", "--puerto", "8798"});
    if (check(s.ok(), "servicio del sistema instalado")) {
      bool up = false;
      for (int i = 0; i < 60 && !up; ++i) {
        up = http::get("http://127.0.0.1:8798/api/ping", {}, 3).status == 200;
        if (!up) std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      std::cout << (up ? "servicio respondiendo en 8798"
                       : "el servicio no responde (el sistema de pruebas puede no arrancarlo sin sesion)")
                << std::endl;
      cli({"servicio", "estado"});
      check(cli({"servicio", "quitar"}).ok(), "servicio quitado");
    }
  }
  std::cout << "\n" << fails.size() << " fallos" << (fails.empty() ? "" : ": " + join(fails, "; ")) << std::endl;
  return fails.empty() ? 0 : 1;
}

}  // namespace nc
