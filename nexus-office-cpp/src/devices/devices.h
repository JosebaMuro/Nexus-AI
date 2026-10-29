// Dispositivos (nexus/devices): luces NexusLED, reloj y avatar de bolsillo Pico W.
//
// * Leds: firmware NexusLED para ESP32 y tiras WS2812/SK6812. Descubrimiento por UDP 4210 ("NEXUSLED?"), estado de
//   Nexus en directo por UDP ("NXL" 2 | estado | nivel | R G B | ttl) y control HTTP JSON (/api/info, /api/state,
//   /api/effects, /api/palettes, /api/presets) con la clave en X-Nexus-Token. Se guardan en data/dispositivos/leds.json.
// * Hub: reparte el estado de Nexus (escuchando, pensando, hablando...) a las luces, al reloj y al Pico W, y aplica los
//   modos preconfigurados desde el escritorio, el movil o el reloj indistintamente. Genera las imagenes de tinta
//   electronica de cada estado para el Pico W.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
struct Services;
}

namespace nx::devices {

extern const int UDP_PORT;
extern const std::vector<std::string> ESTADOS;   // mismo orden que NEXUS_STATES del firmware
std::string nombre_estado(const std::string& estado);

class Leds {
 public:
  explicit Leds(const fs::path& path = {});
  void load();
  void save();
  json list();
  json get(const std::string& did);   // lanza std::out_of_range si no existe (KeyError)
  json add(std::string host, int port = 80, const std::string& name = "", const std::string& token = "",
           std::string did = "");
  json update(const std::string& did, const json& kw);
  void remove(const std::string& did);
  json discover(double timeout = 1.5, const std::vector<std::string>& hosts = {});
  json call(const std::string& did, const std::string& method, const std::string& path, const json& body = nullptr);
  int send_state(const std::string& state, double level = -1, int r = 0, int g = 0, int b = 0, int ttl = 0);
  json devices();   // copia con las claves (para los modos)
  void set_udp_port(const std::string& did, int port);

 private:
  fs::path path_;
  std::recursive_mutex mu_;
  json devices_ = json::array();
  json http(const json& d, const std::string& method, const std::string& path, const json& body = nullptr,
            double timeout = 3.0);
};

class Hub {
 public:
  explicit Hub(Services* s = nullptr);
  Services* s;
  Leds leds;

  json set_state(std::string state, const json& level = nullptr, const json& texto = nullptr, const std::string& origen = "");
  json snapshot();
  void visto(const std::string& cid, const std::string& tipo, const json& extra = json::object());
  json clientes_recientes(double max_age = 120);
  json modos();
  std::string modo_actual();
  json aplicar_modo(const std::string& mid, const std::string& origen = "");
  json resumen_reloj();
  // Pico W con pantalla de tinta Waveshare 2.13": imagen 250x122 en blanco y negro por estado
  std::string pico_version();
  std::vector<uint8_t> pico_imagen(const std::string& state, int ancho = 250, int alto = 122);   // 1 byte/pixel: 0 o 255
  std::string pico_buffer(const std::string& state);   // MONO_HLSB 122x250 vertical, 1 = blanco
  std::string pico_png(const std::string& state);
  json pico_manifiesto();

 private:
  std::mutex mu_;
  std::string state_ = "idle", texto_;
  double level_ = 0, since_;
  long long seq_ = 0;
  std::map<std::string, json> clientes_;
};

Hub& hub(Services* s = nullptr);
void reset();

}  // namespace nx::devices
