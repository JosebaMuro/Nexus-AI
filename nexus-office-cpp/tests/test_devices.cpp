// Dispositivos (tests/test_dispositivos.py): luces NexusLED contra el emulador del firmware real, modos, reloj,
// imagenes del Pico W y actividad de los agentes.
#include <httplib.h>

#include <thread>

#include "agents/activity.h"
#include "agents/agents.h"
#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "devices/devices.h"
#include "server/app.h"
#include "util/fs.h"
#include "util/net.h"
#include "util/proc.h"

using namespace nx;

namespace {

struct DevServer {
  std::unique_ptr<Services> s;
  std::unique_ptr<server::App> app;
  std::thread th;
  int port = 0;
  explicit DevServer(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("nexus-dev-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    set_env("NEXUS_DATA_DIR", S(dir));
    reset_config();
    devices::reset();
    s = std::make_unique<Services>(false);
    app = std::make_unique<server::App>(s.get());
    s->start();
    port = net::free_port(18990);
    th = std::thread([this] { app->listen("127.0.0.1", port); });
    httplib::Client c("127.0.0.1", port);
    for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ~DevServer() {
    app->stop();
    th.join();
    s->shutdown();
    devices::reset();
  }
};

json body(const httplib::Result& r) { return r ? jparse(r->body) : json(); }
json post(httplib::Client& c, const std::string& p, const json& b) { return body(c.Post(p, b.dump(), "application/json")); }

}  // namespace

TEST("dispositivos: modos, reloj con lo que hace cada agente y clientes vistos") {
  DevServer srv("modos");
  httplib::Client c("127.0.0.1", srv.port);
  CHECK(srv.s->config.get("user.voice_enabled") == true);
  post(c, "/api/dispositivos/modo", {{"modo", "silencio"}, {"origen", "reloj"}});
  CHECK(srv.s->config.get("user.voice_enabled") == false);
  auto bad = c.Post("/api/dispositivos/modo", R"({"modo": "nada"})", "application/json");
  CHECK(bad && bad->status == 400);
  json w = body(c.Get("/api/dispositivos/reloj?cliente=reloj-1"));
  CHECK_EQ(jstr(w, "modo"), std::string("silencio"));
  bool normal = false, nexus = false;
  for (auto& m : w["modos"]) normal |= m["id"] == "normal";
  for (auto& a : w["agentes"]) nexus |= a["id"] == "nexus";
  CHECK(normal);
  CHECK(nexus);
  agents::activity().set("nexus", "tool", std::string("Buscar precios"), "web_search");
  w = body(c.Get("/api/dispositivos/reloj"));
  json a;
  for (auto& x : w["agentes"])
    if (x["id"] == "nexus") a = x;
  CHECK_EQ(jstr(a, "estado"), std::string("tool"));
  CHECK_EQ(jstr(a, "escena"), std::string("browsing"));
  CHECK_EQ(jstr(a, "tarea"), std::string("Buscar precios"));
  CHECK(w["agentes"][0]["id"] == "nexus");   // los que trabajan van primero
  bool visto = false;
  json d = body(c.Get("/api/dispositivos"));
  for (auto& cl : d["clientes"]) visto |= cl["id"] == "reloj-1";
  CHECK(visto);
  json act = body(c.Get("/api/agentes/actividad"));
  CHECK_EQ(jstr(act["nexus"], "escena"), std::string("browsing"));
  agents::activity().set("nexus", "idle");
}

TEST("dispositivos: imagenes de tinta del Pico W por estado") {
  DevServer srv("pico");
  httplib::Client c("127.0.0.1", srv.port);
  json m = body(c.Get("/api/dispositivos/pico/manifiesto"));
  bool thinking = false;
  for (auto& e : m["estados"]) thinking |= e == "thinking";
  CHECK(thinking);
  CHECK(!jstr(m, "version").empty());
  auto b = c.Get("/api/dispositivos/pico/thinking.bin");
  CHECK(b && b->body.size() == (size_t)16 * 250);   // 122 px -> 16 bytes por fila, 250 filas
  size_t ones = 0;
  for (unsigned char ch : b->body)
    for (int k = 0; k < 8; ++k) ones += (ch >> k) & 1;
  CHECK(ones > 0 && ones < b->body.size() * 8);   // ni todo blanco ni todo negro
  auto sp = c.Get("/api/dispositivos/pico/speaking.bin");
  CHECK(sp && sp->body != b->body);
  auto png = c.Get("/api/dispositivos/pico/idle.png");
  CHECK(png && png->get_header_value("Content-Type").find("image/png") == 0 && png->body.substr(1, 3) == "PNG");
  json st = body(c.Get("/api/dispositivos/estado?cliente=pico&tipo=pico"));
  CHECK_EQ(jstr(st, "pico_version"), jstr(m, "version"));
}

TEST("dispositivos: la actividad del agente sigue al chat") {
  DevServer srv("actividad");
  srv.s->agents->run("nexus", "Hola", "s1", {}, [](const json&) {});
  json st = agents::activity().get("nexus");
  CHECK_EQ(jstr(st, "estado"), std::string("idle"));
  CHECK(st.value("tareas", 0) >= 1);
  CHECK_EQ(jstr(st, "tarea"), std::string("Hola"));
}

#ifdef NEXUSLED_EMU
TEST("dispositivos: luces NexusLED contra el emulador del firmware") {
  DevServer srv("leds");
  httplib::Client c("127.0.0.1", srv.port);
  int http = net::free_port(19470), udp = net::free_port(19480);
  auto data = std::filesystem::temp_directory_path() /
              ("nexusled-emu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(data);
  proc::Process emu;
  CHECK(emu.start({NEXUSLED_EMU, "--http", std::to_string(http), "--udp", std::to_string(udp), "--leds", "30", "--bind",
                   "127.0.0.1", "--data", S(data), "--name", "Tira prueba"}));
  httplib::Client e("127.0.0.1", http);
  for (int i = 0; i < 50 && !e.Get("/api/info"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  json found = body(c.Get("/api/dispositivos/leds/buscar?ip=127.0.0.1:" + std::to_string(udp)))["encontrados"];
  CHECK(!found.empty() && found[0]["name"] == "Tira prueba" && found[0]["leds"] == 30);
  json dev = post(c, "/api/dispositivos/leds", {{"host", "127.0.0.1:" + std::to_string(http)}});
  CHECK(dev["leds"] == 30 && dev["name"] == "Tira prueba");
  std::string did = jstr(dev, "id");
  // puerto UDP del emulador (en la red real es el 4210)
  devices::hub().leds.set_udp_port(did, udp);
  post(c, "/api/dispositivos/estado", {{"estado", "thinking"}, {"nivel", 0.5}});
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  json st = body(c.Get("/api/dispositivos/leds/" + did + "/api/state"));
  CHECK_EQ(jstr(st["nexus"], "status"), std::string("thinking"));
  json fx = body(c.Get("/api/dispositivos/leds/" + did + "/api/effects"));
  CHECK(fx.size() >= 10 || fx.value("effects", json::array()).size() >= 10);
  json r = post(c, "/api/dispositivos/leds/" + did + "/api/state", {{"on", true}, {"bri", 90}});
  CHECK(r["bri"] == 90);
  post(c, "/api/dispositivos/modo", {{"modo", "silencio"}});   // un modo baja el brillo de las tiras
  CHECK(body(c.Get("/api/dispositivos/leds/" + did + "/api/state"))["bri"] == 25);
  CHECK(body(c.Get("/api/dispositivos"))["modo"] == "silencio");
  c.Delete("/api/dispositivos/leds/" + did);
  CHECK(body(c.Get("/api/dispositivos"))["leds"].empty());
  emu.terminate();
  emu.wait(5);
}
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "devices/firmware.h"

TEST("dispositivos: grabar el firmware del reloj con esptool y configurarlo por el puerto serie") {
  // carpeta firmware/ falsa: un binario del reloj y un esptool que responde como un ESP32-C3
  auto fwdir = std::filesystem::temp_directory_path() /
               ("nexus-fw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(fwdir / "bin");
  std::filesystem::create_directories(fwdir / "tools");
  write_file(fwdir / "bin" / "firmware.json", R"({"archivos": {"nexuswatch-esp32c3.bin": {"version": "1.0.0"}}})");
  write_file(fwdir / "bin" / "nexuswatch-esp32c3.bin", std::string(64, '\x55'));
  write_file(fwdir / "tools" / "esptool", "#!/bin/sh\necho \"esptool v5 $*\"\necho \"Chip type: ESP32-C3 (QFN32)\"\nexit 0\n");
  std::filesystem::permissions(fwdir / "tools" / "esptool", std::filesystem::perms::owner_all);
  set_env("NEXUS_FIRMWARE", S(fwdir));
  // el reloj por el puerto serie: un pseudo-terminal que contesta a la linea JSON de configuracion
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
  std::string slave = ptsname(master);
  {
    termios t{};
    tcgetattr(master, &t);
    cfmakeraw(&t);
    tcsetattr(master, TCSANOW, &t);
  }
  std::string recibido;
  std::thread reloj([&] {
    std::string buf;
    char ch;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
      ssize_t n = ::read(master, &ch, 1);
      if (n <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      buf += ch;
      if (ch == '\n') {
        recibido = buf;
        std::string r = "{\"ok\": true, \"wifi\": \"conectado\"}\r\n";
        (void)!::write(master, r.data(), r.size());
        return;
      }
    }
  });
  int fl = fcntl(master, F_GETFL);
  fcntl(master, F_SETFL, fl | O_NONBLOCK);

  DevServer srv("firmware");
  httplib::Client c("127.0.0.1", srv.port);
  json info = body(c.Get("/api/firmware"));
  CHECK_EQ(jstr(info, "carpeta"), S(fwdir));
  CHECK(info["falta"].empty());
  bool s3 = false;
  for (auto& m : info["modelos_luces"]) s3 |= m == "esp32s3";
  CHECK(s3);
  CHECK(info["binarios"].size() == 1 && info["binarios"][0]["version"] == "1.0.0");
  CHECK(info["nexus"].contains("key"));
  json j = post(c, "/api/firmware/grabar",
                {{"tipo", "reloj"}, {"puerto", slave}, {"config", {{"wifi", {{"ssid", "casa"}, {"pass", "x"}}}}}});
  json r;
  for (int i = 0; i < 200; ++i) {
    r = body(c.Get("/api/firmware/trabajo/" + jstr(j, "id")));
    if (jstr(r, "estado") != "en_curso") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  reloj.join();
  ::close(master);
  CHECK_EQ(jstr(r, "estado"), std::string("hecho"));
  CHECK_EQ(jstr(r["resultado"], "chip"), std::string("esp32c3"));
  CHECK(r["resultado"]["config"]["ok"] == true);
  json enviado = jparse(recibido);
  CHECK(enviado["wifi"]["ssid"] == "casa");
  CHECK(enviado.contains("nexus") || !truthy(info["nexus"]["host"]));
  bool grabado = false;
  for (auto& l : r["log"]) grabado |= l.get<std::string>().find("write_flash 0x0") != std::string::npos;
  CHECK(grabado);
  auto bad = c.Post("/api/firmware/grabar", R"({"tipo": "tostadora"})", "application/json");
  CHECK(bad && bad->status == 400);
  set_env("NEXUS_FIRMWARE", "");
}
#endif
