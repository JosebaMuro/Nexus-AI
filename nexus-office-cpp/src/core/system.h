// Utilidades del sistema que usa la interfaz: selector CPU/GPU/NPU (hardware/accel_choice.py), descargas de la
// oficina (downloads.py), drivers de la GPU (hardware/drivers.py) y abrir enlaces en el navegador.
#pragma once
#include <optional>
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::sys2 {

json accel_options(Services& s);
// remember: recordar la eleccion (no preguntar al arrancar); nullopt = no tocarlo
// capas: las de la GPU/NPU (hardware.capas) si se pasan; null = las guardadas
// runtime: variante de llama.cpp (cuda, rocm, vulkan...) o acelerador del motor (npu, qnn, cuda...); "auto" = el del hardware
json accel_apply(Services& s, const std::string& engine, const std::string& accel, std::optional<bool> remember,
                 const json& capas = nullptr, const std::string& runtime = "auto");

json downloads_listing();
std::optional<fs::path> downloads_resolve(const std::string& rel);

json drivers_report(const std::string& accelerator, Services& s);
json drivers_launch(const std::string& option_id);

void open_url(const std::string& url);

}  // namespace nx::sys2
