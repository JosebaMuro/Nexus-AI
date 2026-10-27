# NexusLED — firmware para ESP32 y tiras WS2812

Firmware propio de NEXUS, al estilo de WLED: maneja tiras **WS2812 / WS2812B / SK6812 (RGB y RGBW)** desde un
**ESP32** y se controla desde NEXUS (*Ajustes → Luces LED*), desde su propia página web o por su API.

- 23 efectos (fijo, respiración, arcoíris, fuego, cometa, escáner, centelleo, plasma, relámpagos…) y 11 paletas.
- Hasta 4 tiras y 1500 LED; segmentos con efecto propio; transiciones suaves; 16 presets.
- **Estados de NEXUS**: la tira muestra cuándo NEXUS escucha, piensa, usa una herramienta o habla (vúmetro con su voz).
- Límite de corriente estimado, corrección gamma, relé de alimentación y botón opcionales.
- Wi‑Fi con portal de configuración, mDNS (`nexusled-xxxx.local`), descubrimiento UDP y actualización por la web.
- **Sin librerías externas**: solo el núcleo *esp32* de Espressif 3.x (los LED van por el periférico RMT).

## Grabarlo en el ESP32

### Arduino IDE 2.x (lo más sencillo)
1. *Archivo → Preferencias → URLs adicionales del gestor de placas*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. *Herramientas → Placa → Gestor de placas*: instala **esp32 de Espressif Systems** (versión 3.x).
3. Abre `firmware/nexusled/nexusled.ino`.
4. *Herramientas → Placa*: tu modelo (p. ej. **ESP32 Dev Module**, **ESP32S3 Dev Module**, **ESP32C3 Dev Module**).
   *Partition Scheme*: **Minimal SPIFFS (1.9MB APP with OTA)** para poder actualizar por la web.
5. Conecta el ESP32 por USB, elige el puerto y pulsa **Subir**.

### arduino-cli
```bash
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs firmware/nexusled
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/nexusled
```

### PlatformIO
```bash
cd firmware && pio run -e esp32 -t upload     # o esp32s3 / esp32c3
```

## Conexión de la tira
| Tira | ESP32 |
|------|-------|
| DIN (datos) | GPIO 16 por defecto (en ESP32‑C3, usa p. ej. GPIO 8 o 10), con una resistencia de 330 Ω en serie |
| GND | GND del ESP32 **y** de la fuente |
| +5V | fuente de 5 V (no desde el ESP32): ~60 mA por LED en blanco a tope |

Con tiras largas pon un condensador de 1000 µF entre +5V y GND. El ESP32 da 3,3 V en datos: casi todas las WS2812B
lo aceptan; si parpadea, usa un adaptador de nivel (74AHCT125) o alimenta la tira a 4,7 V.

## Primer arranque
1. El ESP32 crea la Wi‑Fi **NexusLED-XXXXXX** (clave `nexusled`). Conéctate desde el móvil o el PC: se abre su página
   (si no, ve a `http://192.168.4.1`).
2. *Ajustes del dispositivo*: **BUSCAR** tu Wi‑Fi, escribe la clave, pon el GPIO y el número de LED y **GUARDAR AJUSTES**.
   Se reinicia y se conecta a tu red.
3. En NEXUS: *Ajustes → Luces LED → BUSCAR EN LA RED* → **AÑADIR**.

Si no conecta a la Wi‑Fi en 20 s, vuelve a abrir el punto de acceso para corregirla. Con un botón en `button_pin`:
pulsación corta = encender/apagar; 1 s = siguiente preset; 10 s = olvidar la Wi‑Fi.

## Probar sin hardware: emulador
El mismo núcleo del firmware corre en el PC (Linux, macOS o WSL) y NEXUS lo trata como un ESP32 de verdad:
```bash
make -C firmware/emulator
./firmware/emulator/nexusled-emu --http 8080 --udp 4210 --leds 60 --ansi
```
`--ansi` dibuja la tira en la terminal. En NEXUS, añádelo con la IP `127.0.0.1:8080`. Su página: `http://127.0.0.1:8080`.

## Pruebas del núcleo
```bash
g++ -std=c++17 -fsanitize=address,undefined firmware/tests/core_test.cpp -o /tmp/core_test && /tmp/core_test
```

El protocolo completo (HTTP, UDP y puerto serie) está en [docs/LEDS.md](../docs/LEDS.md).
