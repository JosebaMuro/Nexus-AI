# Firmware de los dispositivos de Nexus-Office

| Carpeta | Dispositivo | Binario listo para grabar (`bin/`) |
|---|---|---|
| `nexusled/` | Luces: cualquier ESP32 con tiras WS2812/SK6812 | `nexusled-esp32.bin`, `-esp32s2`, `-esp32s3`, `-esp32c3`, `-esp32c6` |
| `nexuswatch/` | Reloj ESP32-2424S012C-Y / (B) (ESP32-C3, pantalla redonda táctil) | `nexuswatch-esp32c3.bin` |
| `nexuspico/` | Avatar de bolsillo: Pico W + pantalla de tinta 2,13" del pwnagotchi | `nexuspico-picow.uf2` |

Los `.bin` son imágenes completas (cargador + particiones + programa): se graban en la dirección `0x0`.
`bin/firmware.json` lista la versión y la huella SHA-256 de cada uno. Los compila
`.github/workflows/firmware.yml` en cada cambio de esta carpeta, con el núcleo esp32 de Arduino 3.1.1 y MicroPython 1.24.1.

## Grabarlos

**Desde Nexus**: *Dispositivos › Firmware*: elige el dispositivo, la Wi-Fi y pulsa **GRABAR**. Nexus graba el
binario, le pone la Wi-Fi y su propia dirección y clave de red, y el dispositivo aparece en *Conectados ahora*.

**Con el script** (sin Internet; usa `esptool` y `pyserial`, que vienen con Nexus-Office):

```bash
python firmware/flash.py puertos
python firmware/flash.py luces --wifi MiRed MiClave --leds 60 --pin 16
python firmware/flash.py reloj --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py pico  --wifi MiRed MiClave --nexus 192.168.1.10:8775 CLAVE_DE_RED
python firmware/flash.py config reloj --wifi OtraRed OtraClave      # solo cambiar la configuracion
```

La *clave de red* es la de *Red COOP › Conectar dispositivos*. Nexus tiene que escuchar en la red local (mismo sitio).

**A mano**: `esptool --chip esp32s3 write_flash 0x0 bin/nexusled-esp32s3.bin`, o la web
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
```

Las pruebas de Nexus (`nexus-office/tests/test_dispositivos.py`) usan el emulador de NexusLED y ejecutan el código
del Pico contra el servidor.

## Qué no se ha podido probar sin el hardware

Todo compila y la lógica está probada en el PC, pero falta probarlo en las placas reales: los tiempos de la
pantalla GC9A01 y el táctil CST816S del reloj, el refresco de la tinta (V3 y V4) del Pico W y la salida RMT de las
tiras en cada modelo de ESP32.
