# NexusPico — avatar de bolsillo con el hardware del pwnagotchi

Pico W + pantalla de tinta **Waveshare 2,13" V3 o V4** (módulo Pico-ePaper-2.13, la del pwnagotchi).

Nexus genera una imagen por estado (en línea, escuchando, pensando, usando herramienta, hablando, hecho, error,
aviso) con el avatar y una cara al estilo pwnagotchi. El Pico las descarga una vez, las guarda en su memoria y
después solo pregunta el estado cada 2 s: cuando cambia, pinta la imagen guardada (refresco parcial, sin parpadeo;
uno completo cada 20). Si cambias el nombre del asistente o el estilo (`dispositivos.pico.estilo`: `avatar` o
`caras`), las vuelve a descargar solo. Puedes ver las imágenes en *Nexus › Dispositivos*.

Desde la 2.0 es C++17 con el Pico SDK (sin MicroPython): arranca en menos de un segundo y no hay intérprete.

## Grabar
1. Conecta el Pico W manteniendo pulsado **BOOTSEL**: aparece la unidad `RPI-RP2`.
2. *Nexus › Dispositivos › Firmware › Avatar Pico W › GRABAR*, o `nexus-flash pico --wifi RED CLAVE`.
   A mano: copia `firmware/bin/nexuspico-picow.uf2` a la unidad.

## Configuración
Se guarda en los últimos 64 KB de la flash del Pico. Tres formas de darla:
- `nexus-flash pico --wifi RED CLAVE --nexus 192.168.1.10:8775 CLAVE_DE_RED` (o *Nexus › Dispositivos*): la manda
  por el USB como una línea JSON y el Pico contesta `{"ok":true,"guardado":true}` y se reinicia.
- La página del propio Pico: sin Wi-Fi crea la red `NexusPico-XXXX`, clave `nexuspico`, con una página en
  http://192.168.4.1.
- Los Nexus que todavía escriben `/config.json` por el REPL de MicroPython también funcionan: el firmware entiende
  esa secuencia y contesta `NEXUS-OK`.

```json
{"wifi": {"ssid": "MiRed", "pass": "MiClave"}, "nexus": {"host": "192.168.1.10:8775", "key": "CLAVE_DE_RED"},
 "cada_s": 2, "completo_cada": 20, "pines": {"rst": 12, "dc": 8, "cs": 9, "clk": 10, "din": 11, "busy": 13}}
```
Cualquier otra línea JSON (por ejemplo `{"info":true}`) devuelve la versión, la MAC y la configuración sin claves.

## Pines (Pico-ePaper-2.13)
RST GP12 · DC GP8 · CS GP9 · CLK GP10 · DIN GP11 · BUSY GP13. Si usas la pantalla del pwnagotchi (HAT de la
Raspberry) con cables, conéctala a esos pines o cámbialos en `pines`.

## Compilar
```sh
export PICO_SDK_PATH=/ruta/al/pico-sdk     # 2.1.0 con lib/cyw43-driver, lwip, tinyusb y btstack
cmake -S firmware/nexuspico -B build-pico && cmake --build build-pico -j
# -> build-pico/nexuspico.uf2
```
Necesita `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi` y `libstdc++-arm-none-eabi-newlib`.

| Archivo | Qué es |
|---|---|
| `np_core.h` | lógica sin hardware: configuración, almacén en flash, pantallas de texto, bucle, consola USB, portal |
| `nexuspico.cpp` | lo del Pico: pantalla SSD1680, flash, Wi-Fi y cliente HTTP con lwIP, portal en modo AP |
| `tests/np_test.cpp` | pruebas en el PC: `g++ -std=c++17 -fsanitize=address,undefined -o np_test tests/np_test.cpp && ./np_test` |
| `third_party/dhcpserver` | servidor DHCP de los ejemplos del Pico SDK (MIT), para el portal |
