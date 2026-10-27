# nexus-flash — grabador nativo para la familia ESP y el Pico W

Una cabecera C++17 sin dependencias (`nexus_flash.h`) y una orden (`nexus-flash`) que graban el firmware de Nexus
sin esptool ni Python. Hablan directamente con el cargador de la ROM de Espressif por el puerto serie (SLIP).

| Chip | Detección | Velocidad al grabar | Verificación |
|---|---|---|---|
| ESP8266 (ESP-01/01S, ESP-12, NodeMCU, Wemos D1 mini) | registro 0x40001000 | 115200 (la ROM no cambia de velocidad) | suma de cada bloque |
| ESP32, S2, S3, C3, C6, H2 | registro 0x40001000 | 460800 | MD5 de lo grabado |
| ESP32-C2 | registro | 115200 (la ROM con cristal de 26 MHz cambiaría a otra velocidad) | MD5 |
| ESP32-C5, C61, P4 | GET_SECURITY_INFO | 460800 | MD5 |

Por USB-JTAG integrado (S3, C3, C6... con VID 303A:1001) usa su secuencia de reinicio y no cambia la velocidad.
Placas sin reinicio automático (ESP-01): durante `--espera` segundos (20) sigue intentando; une GPIO0 a GND y dale
corriente. No instala drivers: eso lo hace Nexus-Office (`src/devices/drivers.cpp`).

## Compilar

```bash
c++ -std=c++17 -O2 -o nexus-flash nexus_flash_cli.cpp                    # Linux, macOS, Raspberry Pi
cl /std:c++17 /EHsc /O2 nexus_flash_cli.cpp advapi32.lib                 # Windows (MSVC)
aarch64-linux-android24-clang++ -std=c++17 -O2 -static-libstdc++ -o nexus-flash nexus_flash_cli.cpp   # Android
```

`.github/workflows/firmware.yml` (trabajo *grabador*) lo compila en los cuatro y deja los ejecutables como artefactos.

## Orden

```
nexus-flash puertos | chips | chip [--puerto P]
nexus-flash grabar ARCHIVO.bin [--puerto P] [--direccion 0x0] [--baudios 460800] [--chip esp32c3]
nexus-flash luces [--modelo M] [--wifi RED CLAVE] [--leds N --pin GPIO] [--nombre N]
nexus-flash reloj | pico [--wifi RED CLAVE] [--nexus HOST:PUERTO CLAVE]
nexus-flash config luces|reloj|pico [...]
  --bin CARPETA  --espera S  --sin-verificar  --sin-reiniciar  --json
```

Sin `--puerto` usa el primer adaptador conocido (Espressif, CP210x, CH340/CH9102, FTDI). Con `--json` el avance sale
en líneas `{"fase":..., "pct":..., "texto":...}` y un fallo como `{"error": ...}`.

## API

```cpp
#include "nexus_flash.h"
auto progreso = [](const std::string& fase, int pct, const std::string& texto) { /* ... */ };
std::string chip = nexusflash::detectar(puerto, progreso);                     // "esp8266", "esp32c3"...
nexusflash::grabar(puerto, chip, "firmware/bin/nexusled-" + chip + ".bin", 0x0, progreso);
std::string resp = nexusflash::wifi(puerto, R"({"config":{"wifi":{"ssid":"Casa","pass":"..."}}})");
```

Fases: `conectar`, `detectar`, `baudios`, `borrar`, `grabar`, `verificar`, `reiniciar`, `configurar`, `copiar`.
Todo lanza `nexusflash::Error` (un `std::runtime_error`) con el motivo. Para más control: `nexusflash::Grabador`
(`conectar()`, `grabar(datos, direccion, opciones)`, `reiniciar()`, `orden()`), `puertos()`, `unidades_pico()`,
`copiar_uf2()` y `configurar_pico()`.

## Pruebas

`sh tests/prueba.sh` compila la orden y `tests/rom_falsa.cpp`, una ROM falsa sobre un pseudoterminal que contesta como
la de cada chip (2 o 4 bytes de estado, el campo de cifrado de FLASH_BEGIN, sin MD5 ni cambio de velocidad en el
ESP8266, el tamaño de borrado corregido del ESP8266) y comprueba sumas y secuencia de cada bloque. La prueba detecta,
graba, compara la flash y manda la configuración en ESP8266, ESP32, S2, S3, C2, C3, C5 y C6.

Falta probarlo con placas reales: el protocolo sigue el de esptool, pero los tiempos de reinicio de cada adaptador
y la velocidad alta dependen del hardware.
