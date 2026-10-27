# Firmware de los dispositivos de Nexus-Office

| Carpeta | Dispositivo | Binario listo para grabar (`bin/`) |
|---|---|---|
| `nexusled/` | Luces: toda la familia ESP con Wi-Fi (ESP-01/01S, ESP-12, NodeMCU, Wemos D1 mini y demás ESP8266; ESP32, S2, S3, C3, C5 y C6) con tiras WS2812/SK6812 | `nexusled-esp8266.bin` (1 MB), `-esp8266-4m`, `-esp8266-512k`, `-esp32`, `-esp32s2`, `-esp32s3`, `-esp32c3`, `-esp32c5`, `-esp32c6` |
| `nexuswatch/` | Reloj ESP32-2424S012C-Y / (B) (ESP32-C3, pantalla redonda táctil) | `nexuswatch-esp32c3.bin` |
| `nexuspico/` | Avatar de bolsillo: Pico W + pantalla de tinta 2,13" del pwnagotchi | `nexuspico-picow.uf2` |
| `flasher/` | **nexus-flash**, el grabador nativo en C++ (sin esptool ni Python) | se compila en CI para Windows, macOS, Linux y Android |

Los `.bin` son imágenes completas (cargador + particiones + programa): se graban en la dirección `0x0`.
`bin/firmware.json` lista la versión y la huella SHA-256 de cada uno. Los compila
`.github/workflows/firmware.yml` en cada cambio de esta carpeta, con el núcleo esp32 de Arduino 3.1.1 (3.3.0 para el C5), el esp8266 3.1.2 y MicroPython 1.24.1.

## Grabarlos

**Desde Nexus**: *Dispositivos › Firmware*: elige el dispositivo, la Wi-Fi y pulsa **GRABAR**. Nexus graba el
binario, le pone la Wi-Fi y su propia dirección y clave de red, y el dispositivo aparece en *Conectados ahora*.

**Con nexus-flash** (C++, sin Internet ni Python; detecta el chip solo y elige su `.bin`):

```bash
nexus-flash puertos
nexus-flash luces --wifi MiRed MiClave --leds 60            # GPIO 16 en ESP32, GPIO 2 en ESP8266
nexus-flash luces --modelo esp8266-4m --wifi MiRed MiClave  # NodeMCU o Wemos con 4 MB
nexus-flash reloj --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
nexus-flash pico  --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
nexus-flash grabar otro.bin --direccion 0x0                 # cualquier imagen en cualquier chip
```

Detalles y API para otros programas en [`flasher/README.md`](flasher/README.md). Los drivers del adaptador USB
(CH340, CP210x, FTDI) los instala Nexus-Office desde *Dispositivos › Grabar firmware › INSTALAR DRIVERS*.

**ESP-01 y placas sin reinicio automático**: el ESP-01 no tiene USB; usa un adaptador USB-serie de 3,3 V (o el
programador ESP-01 con interruptor). Une GPIO0 a GND, dale corriente y lanza la orden: nexus-flash espera 20 s
(`--espera` para más) a que entre en modo de carga. Al terminar, suelta GPIO0 y vuelve a darle corriente.

**Con el script de Python** (usa `esptool` y `pyserial`, que vienen con la edición Python):

```bash
python firmware/flash.py puertos
python firmware/flash.py luces --wifi MiRed MiClave --leds 60 --pin 16
python firmware/flash.py reloj --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py pico  --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py config reloj --wifi OtraRed OtraClave      # solo cambiar la configuracion
```

La *clave de red* es la de *Red COOP › Conectar dispositivos*. Nexus tiene que escuchar en la red local (mismo sitio).

**A mano**: `nexus-flash grabar bin/nexusled-esp32s3.bin`, `esptool --chip esp32s3 write_flash 0x0 bin/nexusled-esp32s3.bin`, o la web
<https://espressif.github.io/esptool-js/> (dirección 0x0). Para el Pico W, mantén BOOTSEL al conectarlo y copia el
`.uf2` a la unidad `RPI-RP2`.

Sin el script, cada dispositivo se configura desde el móvil: si no tiene Wi-Fi crea su propia red
(`NexusLED-XXXX`, `NexusWatch-XXXX` clave `nexuswatch`, `NexusPico-XXXX` clave `nexuspico`) con una página en
http://192.168.4.1.

## Probar sin hardware

```bash
g++ -std=c++17 -O1 -fsanitize=address,undefined tests/core_test.cpp && ./a.out     # nucleo de NexusLED
make -C emulator && ./emulator/nexusled-emu --help                                   # NexusLED en el PC (HTTP + UDP)
g++ -std=c++17 -O2 -o nw-emu nexuswatch/emulator/nw_emu.cpp
./nw-emu nexuswatch/emulator/ejemplo.json reloj.ppm 1000 left                        # pantalla del reloj
sh flasher/tests/prueba.sh                                                           # nexus-flash contra una ROM falsa de 8 chips
```

Las pruebas de Nexus (`nexus-office/tests/test_dispositivos.py`) usan el emulador de NexusLED y ejecutan el código
del Pico contra el servidor.

## Qué no se ha podido probar sin el hardware

Todo compila y la lógica está probada en el PC, pero falta probarlo en las placas reales: los tiempos de la
pantalla GC9A01 y el táctil CST816S del reloj, el refresco de la tinta (V3 y V4) del Pico W, la salida RMT de las
tiras en cada modelo de ESP32, la salida temporizada y la memoria del ESP8266 con tiras largas, y nexus-flash con
placas y adaptadores de verdad (está probado contra una ROM falsa que sigue el protocolo de Espressif). El ESP32-C2
no tiene binario: el núcleo de Arduino (3.3.0) no trae sus librerías; nexus-flash sí lo detecta y lo graba.
