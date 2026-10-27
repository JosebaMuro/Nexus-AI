# Dispositivos: luces, reloj y avatar de bolsillo

Nexus-Office reparte su estado (en línea, escuchando, pensando, usando una herramienta, hablando, hecho, error) y lo
que hace cada agente a tres dispositivos baratos que se conectan por Wi‑Fi a tu ordenador. Todo se graba y configura
**sin internet**: los binarios, el script de grabado y `esptool` vienen en el paquete.

| Dispositivo | Placa | Qué hace | Binario (`firmware/bin/`) |
|---|---|---|---|
| **NexusLED** | Cualquier ESP32 (ESP32, S2, S3, C3, C6) con tiras WS2812, WS2812B o SK6812 (RGB y RGBW) | Luces que muestran el estado de Nexus (vúmetro con su voz al hablar), 23 efectos, 11 paletas, hasta 4 tiras y 1500 LED | `nexusled-esp32.bin`, `-esp32s2`, `-esp32s3`, `-esp32c3`, `-esp32c6` |
| **NexusWatch** | ESP32-2424S012C-Y / (B): ESP32‑C3 con pantalla redonda GC9A01 de 1,28" y táctil CST816S | Reloj con el avatar animado, qué hace cada agente y los modos | `nexuswatch-esp32c3.bin` |
| **NexusPico** | Raspberry Pi Pico W + pantalla de tinta Waveshare 2,13" V3 o V4 (la del pwnagotchi) | Avatar de bolsillo con caras al estilo pwnagotchi según el estado | `nexuspico-picow.uf2` |

## Grabar el firmware

**Desde Nexus-Office**: *Dispositivos → Grabar firmware*. Conecta la placa por USB, elige el dispositivo y el modelo
de ESP32, escribe la Wi‑Fi y pulsa **GRABAR**. Nexus graba el binario, le pone la Wi‑Fi, su propia dirección y la
clave de red, y el dispositivo aparece en *Conectados ahora*. Para el Pico W, conéctalo con **BOOTSEL** pulsado.

**Con el script** (`firmware/flash.py` en la carpeta de instalación):

```bash
python firmware/flash.py puertos
python firmware/flash.py luces --wifi MiRed MiClave --leds 60 --pin 16
python firmware/flash.py reloj --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py pico  --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py config reloj --wifi OtraRed OtraClave      # solo cambiar la configuración
```

El script reconoce el modelo de ESP32 y elige su `.bin`. La *clave de red* es la de *Red COOP → Conectar
dispositivos*, y Nexus-Office tiene que escuchar en la red local.

**A mano**: `esptool --chip esp32s3 write_flash 0x0 nexusled-esp32s3.bin` (los `.bin` son imágenes completas y van en
la dirección `0x0`) o la web <https://espressif.github.io/esptool-js/>. Para el Pico W, copia el `.uf2` a la unidad
`RPI-RP2`. `firmware/bin/firmware.json` trae la versión y la huella SHA-256 de cada binario.

Sin el script, cada dispositivo sin Wi‑Fi crea su propia red (`NexusLED-XXXX`, `NexusWatch-XXXX` con clave
`nexuswatch`, `NexusPico-XXXX` con clave `nexuspico`) con una página de configuración en http://192.168.4.1.

## Luces NexusLED
- *Dispositivos → Luces*: **BUSCAR** encuentra las luces de tu red (mDNS y UDP) o se añaden por IP. Para cada una:
  encender, brillo, efecto, paleta, color y si sigue el estado de Nexus.
- Conexión: DIN de la tira al GPIO 16 (en el ESP32‑C3, por ejemplo el 8 o el 10) con una resistencia de 330 Ω, y GND
  común entre el ESP32 y la fuente. Más detalles en `firmware/nexusled/README.md`.

## Reloj NexusWatch
- **Inicio**: el avatar de Nexus animado según lo que hace, con un anillo del color del estado.
- Desliza a los lados: **Agentes** (qué hace cada uno ahora; toca uno para ver su escena y su tarea), **Modos** y
  **Info** (Wi‑Fi, IP y Nexus al que está conectado; mantén pulsado para volver al portal de configuración).
- Baja el brillo a los 30 s y se apaga a los 2 min; un toque lo enciende.

## Avatar NexusPico
Nexus genera una imagen por estado con el avatar y una cara al estilo pwnagotchi. El Pico las descarga una vez, las
guarda y después solo pregunta el estado cada 2 s (refresco parcial, sin parpadeo). Si cambias el nombre del
asistente o el estilo (`dispositivos.pico.estilo`: `avatar` o `caras`), las vuelve a descargar. Las imágenes se ven
en *Dispositivos*. Pines (Pico-ePaper-2.13): RST GP12, DC GP8, CS GP9, CLK GP10, DIN GP11, BUSY GP13.

## Modos
Un modo cambia de una vez la voz, las reuniones, el estilo de respuesta, el perfil de hardware y el brillo de las
luces. Se elige en *Dispositivos*, desde el móvil o tocando en el reloj, y se aplica en todos a la vez.

| Modo | Qué hace |
|---|---|
| **Normal** | Voz y luces como siempre |
| **Manos libres** | Responde siempre en voz alta |
| **Silencio** | Sin voz y luces tenues |
| **Concentración** | Sin reuniones automáticas ni voz; respuestas breves |
| **Rendimiento** | Perfil de hardware *Rendimiento* |
| **Ahorro** | Perfil de hardware *Ahorro* |
| **Noche** | Luces apagadas y sin voz |

Un modo nunca cambia la seguridad ni la red. Se pueden añadir modos propios en `dispositivos.modos`.

## API
`GET /api/dispositivos`, `GET` y `POST /api/dispositivos/estado`, `GET /api/dispositivos/reloj`,
`POST /api/dispositivos/modo` `{modo}`, `GET /api/dispositivos/pico/manifiesto` y `/api/dispositivos/pico/{estado}.bin|.png`,
`GET /api/dispositivos/leds/buscar`, `POST`, `PUT` y `DELETE /api/dispositivos/leds/...`, `GET /api/firmware`,
`POST /api/firmware/grabar` y `GET /api/firmware/trabajo/{id}`.

## Qué no se ha podido probar sin el hardware
Todo compila en CI y la lógica está probada en el PC (emulador de NexusLED, pantalla del reloj dibujada en un fichero
y el código del Pico contra el servidor), pero falta probarlo en las placas reales: los tiempos de la pantalla GC9A01
y el táctil CST816S del reloj, el refresco de la tinta (V3 y V4) del Pico W y la salida de las tiras en cada modelo
de ESP32.
