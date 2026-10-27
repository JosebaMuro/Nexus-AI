// nexus-flash: graba y configura los dispositivos de Nexus (luces NexusLED, reloj NexusWatch y Pico W) sin esptool.
//
//   nexus-flash puertos                                    que hay conectado
//   nexus-flash chips                                      chips que reconoce
//   nexus-flash chip   [--puerto P]                        detecta el chip
//   nexus-flash grabar ARCHIVO.bin [--puerto P] [--direccion 0x0] [--baudios 460800] [--chip esp32c3]
//   nexus-flash luces  [--puerto P] [--modelo esp8266-4m] [--wifi RED CLAVE] [--leds 60 --pin 16] [--nombre N]
//   nexus-flash reloj  [--puerto P] [--wifi RED CLAVE] [--nexus HOST:PUERTO CLAVE]
//   nexus-flash pico   [--unidad RPI-RP2] [--wifi RED CLAVE] [--nexus HOST:PUERTO CLAVE]
//   nexus-flash config luces|reloj|pico [--puerto P] ...   solo la configuracion, sin regrabar
//
// Opciones comunes: --bin CARPETA (binarios; por defecto firmware/bin), --espera S (segundos para entrar en modo de
// carga), --sin-verificar, --sin-reiniciar, --json (avance en lineas JSON para otros programas).
#include <cstdlib>
#include <iostream>
#include <map>

#include "nexus_flash.h"

using namespace nexusflash;

namespace {

bool g_json = false;

std::string jesc(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else o += (char)c;
  }
  return o + "\"";
}

void avance(const std::string& fase, int pct, const std::string& texto) {
  if (g_json) {
    std::cout << "{\"fase\":" << jesc(fase) << ",\"pct\":" << pct << ",\"texto\":" << jesc(texto) << "}" << std::endl;
    return;
  }
  if (fase == "grabar") {
    std::cout << "\r  grabando " << pct << "%  " << texto << "      " << std::flush;
    if (pct >= 100) std::cout << std::endl;
  } else if (!texto.empty()) {
    std::cout << "  " << texto << std::endl;
  }
}

struct Args {
  std::string accion, tipo, archivo;
  std::map<std::string, std::vector<std::string>> op;
  bool tiene(const std::string& k) const { return op.count(k) > 0; }
  std::string get(const std::string& k, const std::string& def = "", size_t i = 0) const {
    auto it = op.find(k);
    return it == op.end() || it->second.size() <= i ? def : it->second[i];
  }
};

Args parse(int argc, char** argv) {
  Args a;
  static const std::map<std::string, int> n_valores = {
      {"--puerto", 1}, {"--unidad", 1}, {"--direccion", 1}, {"--baudios", 1}, {"--chip", 1},   {"--modelo", 1},
      {"--bin", 1},    {"--espera", 1}, {"--leds", 1},      {"--pin", 1},     {"--nombre", 1}, {"--wifi", 2},
      {"--nexus", 2},  {"--sin-verificar", 0}, {"--sin-reiniciar", 0}, {"--json", 0}};
  std::vector<std::string> pos;
  for (int i = 1; i < argc; i++) {
    std::string s = argv[i];
    if (s.rfind("--", 0) == 0) {
      auto it = n_valores.find(s);
      if (it == n_valores.end()) throw Error("opcion desconocida: " + s);
      auto& v = a.op[s];
      for (int k = 0; k < it->second && i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0; k++) v.push_back(argv[++i]);
      if (it->second == 1 && v.empty()) throw Error(s + " necesita un valor");
    } else {
      pos.push_back(s);
    }
  }
  if (!pos.empty()) a.accion = pos[0];
  if (pos.size() > 1) (a.accion == "grabar" ? a.archivo : a.tipo) = pos[1];
  return a;
}

bool existe_archivo(const std::string& p) { return std::ifstream(p).good(); }

std::string carpeta_bin(const Args& a, const char* argv0) {
  std::vector<std::string> cand;
  if (a.tiene("--bin")) return a.get("--bin");
  if (const char* e = std::getenv("NEXUS_FIRMWARE_BIN")) cand.push_back(e);
  std::string exe = argv0, dir = exe.find_last_of("/\\") == std::string::npos ? "." : exe.substr(0, exe.find_last_of("/\\"));
  for (auto& c : {std::string("bin"), std::string("firmware/bin"), dir + "/bin", dir + "/../bin", dir + "/../firmware/bin",
                  dir + "/../../firmware/bin", dir + "/../share/nexus/firmware/bin"})
    cand.push_back(c);
  for (auto& c : cand)
    if (existe_archivo(c + "/firmware.json") || existe_archivo(c + "/nexusled-esp32.bin")) return c;
  return "firmware/bin";
}

// configuracion de flash.py: luces -> {"config": {...}}, reloj -> {"wifi":..., "nexus":...}, pico -> config.json
std::string montar_config(const Args& a, const std::string& tipo) {
  std::vector<std::string> campos;
  if (a.tiene("--wifi"))
    campos.push_back("\"wifi\":{\"ssid\":" + jesc(a.get("--wifi")) + ",\"pass\":" + jesc(a.get("--wifi", "", 1)) + "}");
  if (a.tiene("--nexus") && tipo != "luces")
    campos.push_back("\"nexus\":{\"host\":" + jesc(a.get("--nexus")) + ",\"key\":" + jesc(a.get("--nexus", "", 1)) + "}");
  if (tipo == "luces" && a.tiene("--leds"))
    campos.push_back("\"outputs\":[{\"pin\":" + std::to_string(std::atoi(a.get("--pin", "-1").c_str())) +
                     ",\"count\":" + std::to_string(std::atoi(a.get("--leds").c_str())) + "}]");
  if (tipo == "luces" && a.tiene("--nombre")) campos.push_back("\"name\":" + jesc(a.get("--nombre")));
  if (campos.empty()) return "";
  std::string j = "{";
  for (size_t i = 0; i < campos.size(); i++) j += (i ? "," : "") + campos[i];
  j += "}";
  return tipo == "luces" ? "{\"config\":" + j + "}" : j;
}

Opciones opciones(const Args& a) {
  Opciones o;
  if (a.tiene("--baudios")) o.baudios = std::atoi(a.get("--baudios").c_str());
  if (a.tiene("--espera")) o.espera = std::atof(a.get("--espera").c_str());
  o.verificar = !a.tiene("--sin-verificar");
  o.reiniciar = !a.tiene("--sin-reiniciar");
  o.chip = a.get("--chip");
  return o;
}

// el pin por defecto de las luces depende del chip (GPIO2 en el ESP8266, el unico libre en el ESP-01)
std::string con_pin(const Args& a, const std::string& cfg, const std::string& chip) {
  if (cfg.find("\"pin\":-1") == std::string::npos) return cfg;
  std::string c = cfg;
  c.replace(c.find("\"pin\":-1"), 8, std::string("\"pin\":") + (chip == "esp8266" ? "2" : "16"));
  (void)a;
  return c;
}

void grabar_esp(const Args& a, const std::string& tipo, const std::string& bin, std::string& puerto) {
  if (puerto.empty()) puerto = puerto_auto("esp");
  Opciones op = opciones(a);
  Grabador g(puerto, avance);
  Chip c = g.conectar(op.espera);
  std::string modelo = a.get("--modelo", c.nombre);
  if (tipo == "reloj" && c.nombre != "esp32c3")
    throw Error("el reloj ESP32-2424S012C lleva un ESP32-C3 y este es un " + c.descripcion);
  if (tipo == "luces" && !c.wifi) throw Error("el " + c.descripcion + " no tiene Wi-Fi: NexusLED necesita Wi-Fi");
  if (tipo == "luces" && modelo.rfind(c.nombre, 0) != 0)
    throw Error("el modelo " + modelo + " no es para el " + c.descripcion + " conectado");
  std::string archivo = bin + "/" + (tipo == "reloj" ? std::string("nexuswatch-esp32c3.bin") : binario_luces(modelo));
  if (!existe_archivo(archivo)) throw Error("falta " + archivo + " (no hay firmware compilado para " + modelo + ")");
  avance("preparar", 0, "firmware " + archivo);
  g.grabar(leer_archivo(archivo), 0, op);
  avance("reiniciar", 100, archivo.substr(archivo.find_last_of("/\\") + 1) + " grabado en " + puerto);
  if (tipo == "luces") {
    std::string cfg = con_pin(a, montar_config(a, "luces"), c.nombre);
    if (!cfg.empty()) std::cout << enviar_linea(puerto, cfg, 10, avance) << std::endl;
  } else {
    std::string cfg = montar_config(a, tipo);
    if (!cfg.empty()) std::cout << enviar_linea(puerto, cfg, 10, avance) << std::endl;
  }
}

int ayuda() {
  std::cout << "nexus-flash " NEXUSFLASH_VERSION " - graba la familia ESP (ESP8266/ESP-01 ... ESP32-C6) y el Pico W\n\n"
               "  nexus-flash puertos\n"
               "  nexus-flash chips\n"
               "  nexus-flash chip   [--puerto P]\n"
               "  nexus-flash grabar ARCHIVO.bin [--puerto P] [--direccion 0x0] [--baudios 460800] [--chip esp32c3]\n"
               "  nexus-flash luces  [--puerto P] [--modelo M] [--wifi RED CLAVE] [--leds N --pin GPIO] [--nombre N]\n"
               "  nexus-flash reloj  [--puerto P] [--wifi RED CLAVE] [--nexus HOST:PUERTO CLAVE]\n"
               "  nexus-flash pico   [--unidad RPI-RP2] [--wifi RED CLAVE] [--nexus HOST:PUERTO CLAVE]\n"
               "  nexus-flash config luces|reloj|pico [--puerto P] [...]\n\n"
               "  --bin CARPETA  binarios (firmware/bin)    --espera S  segundos para entrar en modo de carga (20)\n"
               "  --sin-verificar  --sin-reiniciar  --json (avance en lineas JSON)\n\n"
               "ESP-01 y placas sin reinicio automatico: une GPIO0 a GND y dale corriente mientras espera.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args a = parse(argc, argv);
    g_json = a.tiene("--json");
    if (a.accion.empty() || a.accion == "ayuda" || a.accion == "-h" || a.accion == "--help") return ayuda();
    if (a.accion == "version") {
      std::cout << NEXUSFLASH_VERSION << std::endl;
      return 0;
    }
    std::string bin = carpeta_bin(a, argv[0]);
    std::string puerto = a.get("--puerto");

    if (a.accion == "puertos") {
      for (auto& p : puertos()) {
        char vp[16] = "";
        if (p.vid) std::snprintf(vp, sizeof vp, "%04x:%04x", p.vid, p.pid);
        if (g_json)
          std::cout << "{\"puerto\":" << jesc(p.puerto) << ",\"tipo\":" << jesc(p.tipo) << ",\"descripcion\":"
                    << jesc(p.descripcion) << ",\"vid_pid\":" << jesc(vp) << "}" << std::endl;
        else
          std::printf("%-24s %-13s %-10s %s\n", p.puerto.c_str(), p.tipo.empty() ? "-" : p.tipo.c_str(), vp,
                      p.descripcion.c_str());
      }
      return 0;
    }
    if (a.accion == "chips") {
      for (auto& c : chips_conocidos())
        std::printf("%-10s %-10s %s\n", c.nombre.c_str(), c.descripcion.c_str(), c.wifi ? "" : "(sin Wi-Fi)");
      return 0;
    }
    if (a.accion == "chip") {
      if (puerto.empty()) puerto = puerto_auto("esp");
      Chip c = detectar_chip(puerto, avance, opciones(a).espera);
      if (g_json) std::cout << "{\"chip\":" << jesc(c.nombre) << ",\"descripcion\":" << jesc(c.descripcion) << "}" << std::endl;
      else std::cout << c.nombre << "  (" << c.descripcion << ")" << std::endl;
      return 0;
    }
    if (a.accion == "grabar") {
      if (a.archivo.empty()) throw Error("indica el archivo .bin");
      if (puerto.empty()) puerto = puerto_auto("esp");
      uint32_t dir = (uint32_t)std::strtoul(a.get("--direccion", "0").c_str(), nullptr, 0);
      Chip c = grabar_archivo(puerto, a.archivo, dir, opciones(a), avance);
      std::cout << a.archivo << " grabado en el " << c.descripcion << " de " << puerto << std::endl;
      return 0;
    }
    std::string tipo = a.accion == "config" ? a.tipo : a.accion;
    if (tipo != "luces" && tipo != "reloj" && tipo != "pico") throw Error("accion desconocida (mira nexus-flash ayuda)");
    if (tipo == "pico") {
      std::string cfg = montar_config(a, "pico");
      if (a.accion != "config") {
        std::string uf2 = bin + "/nexuspico-picow.uf2";
        if (!existe_archivo(uf2)) throw Error("falta " + uf2);
        std::string u = copiar_uf2(uf2, a.get("--unidad"), avance);
        avance("copiar", 100, "copiado a " + u + "; el Pico se reinicia");
        if (cfg.empty()) return 0;
        double fin = detail::ahora() + 30;   // el Pico tarda en reaparecer como puerto serie
        while (puerto.empty() && detail::ahora() < fin) {
          try {
            puerto = puerto_auto("pico");
          } catch (const Error&) {
            detail::dormir(1);
          }
        }
        detail::dormir(2);
      }
      if (!cfg.empty()) configurar_pico(puerto.empty() ? puerto_auto("pico") : puerto, cfg, avance);
    } else if (a.accion == "config") {
      std::string cfg = montar_config(a, tipo);
      if (cfg.empty()) throw Error("no hay nada que configurar (usa --wifi, --nexus, --leds...)");
      if (tipo == "luces") cfg = con_pin(a, cfg, a.get("--modelo").rfind("esp8266", 0) == 0 ? "esp8266" : "esp32");
      std::cout << enviar_linea(puerto.empty() ? puerto_auto("esp") : puerto, cfg, 10, avance) << std::endl;
    } else {
      grabar_esp(a, tipo, bin, puerto);
    }
    std::cout << "Listo." << std::endl;
    return 0;
  } catch (const std::exception& e) {
    if (g_json) std::cout << "{\"error\":" << jesc(e.what()) << "}" << std::endl;
    std::cerr << "\nError: " << e.what() << std::endl;
    return 1;
  }
}
