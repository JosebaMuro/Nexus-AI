// Grabar el firmware de las luces, el reloj y el Pico W desde Nexus (nexus/devices/firmware.py y firmware/flash.py).
//
// Los binarios van en firmware/bin (sin internet). El Pico W se graba copiando el .uf2 a la unidad RPI-RP2 y su
// config.json se escribe por el REPL de MicroPython; los ESP8266/ESP32 se graban con nexus-flash (firmware/flasher, el
// grabador nativo en C++ que habla con la ROM de Espressif, sin esptool ni Python; NEXUS_GRABADOR=esptool usa esptool)
// y se configuran con una linea JSON por el puerto serie.
// Los puertos serie se listan y se abren en C++ en cada sistema. Solo desde el propio equipo: toca los puertos USB.
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::devices::firmware {

extern const std::vector<std::string> MODELOS_LUCES;
extern const json VARIANTES_ESP8266;   // [{modelo, nombre}] imagenes del ESP8266 por tamano de flash
extern const json FAMILIA;   // toda la familia ESP: [{chip, nombre, wifi, usb_nativo, placas, nota?}]
std::string chip_de_texto(const std::string& salida_esptool);   // "esp32c3", "esp8266"...; "" si no se reconoce
using Log = std::function<void(const std::string&)>;

fs::path carpeta();   // firmware/ (con bin/); vacio si no esta
json disponible();    // {carpeta, falta: [...], grabador}
json binarios();      // [{archivo, bytes, version, sha256...}]
json puertos();       // [{puerto, descripcion, tipo, vid, pid}] + unidades RPI-RP2
std::vector<fs::path> unidades_pico();
std::vector<std::string> esptool_cmd();   // vacio si no hay esptool (solo con NEXUS_GRABADOR=esptool)

json flashear_esp32(const std::string& tipo, std::string puerto, std::string modelo, const Log& log);
json configurar_esp32(const std::string& tipo, std::string puerto, const json& cfg, const Log& log, double espera_s = 8);
json flashear_pico(const std::string& unidad, const Log& log, double espera_s = 30);
json configurar_pico(std::string puerto, const json& cfg, const Log& log);

class Trabajos {
 public:
  json lanzar(const std::string& tipo, const std::string& puerto = "", const std::string& modelo = "",
              const json& config = nullptr, bool solo_config = false);
  json get(const std::string& tid);

 private:
  std::mutex mu_, ocupado_;
  std::map<std::string, json> t_;
};

Trabajos& trabajos();

}  // namespace nx::devices::firmware
