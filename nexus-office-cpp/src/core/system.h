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
json accel_apply(Services& s, const std::string& engine, const std::string& accel, bool remember);

json downloads_listing();
std::optional<fs::path> downloads_resolve(const std::string& rel);

json drivers_report(const std::string& accelerator, Services& s);
json drivers_launch(const std::string& option_id);

void open_url(const std::string& url);

}  // namespace nx::sys2
