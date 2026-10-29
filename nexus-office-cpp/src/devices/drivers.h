// Drivers USB-serie para grabar y configurar las placas ESP (luces, reloj y demas familia ESP) y el Pico W.
//
// Los ESP8266/ESP32 llegan por un puente USB-serie: WCH CH340/CH341/CH9102/CH343, Silicon Labs CP210x o FTDI; los
// ESP32-S2/S3/C3/C5/C6/H2/P4 con USB nativo y el Pico W usan el CDC estandar, que no necesita driver.
// dispositivos() mira lo que hay enchufado y si el sistema ya lo maneja; instalar() pone lo que falte sin preguntar:
//   * Windows: pnputil con los .inf de firmware/drivers/windows (se pide elevacion una vez, con UAC).
//   * macOS: el paquete de firmware/drivers/macos (CH34x, CP210x, FTDI vienen en los macOS recientes).
//   * Linux: carga el modulo del kernel (ch341, cp210x, ftdi_sio, cdc_acm), da permiso al usuario sobre el puerto
//     (regla udev y grupo dialout) y aparta brltty, que en Ubuntu se queda con los CH340.
// Todo sale de lo que va en el paquete: sin internet.
#pragma once
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::devices::drivers {

struct Puente {
  int vid, pid;
  const char* chip;     // nombre comercial
  const char* driver;   // ch34x, ch343, cp210x, ftdi o cdc (el del sistema)
};

const Puente* puente(int vid, int pid);   // nullptr si no es un puente USB-serie conocido
const std::vector<Puente>& puentes();

fs::path carpeta();   // firmware/drivers/<sistema>; vacio si el paquete no trae drivers

// USB-serie enchufados: [{vid, pid, chip, driver, puerto, instalado, problema}]
json dispositivos();
// {sistema, dispositivos, faltan: [driver...], paquetes: {driver: carpeta}, avisos: [...]}
json estado();
// Instala los drivers que faltan (o los pedidos). simular: solo dice que haria.
// {ok, hecho: [...], pendiente: [...], comandos: [...], mensaje}
json instalar(std::vector<std::string> drivers = {}, bool simular = false);

namespace detail {   // para las pruebas: la parte de Linux lee /sys de esta raiz
void set_sysfs(const fs::path& root);
std::string modulo_linux(const std::string& driver);
}

}  // namespace nx::devices::drivers
