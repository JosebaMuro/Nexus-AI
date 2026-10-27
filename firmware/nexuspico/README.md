# NexusPico — avatar de bolsillo con el hardware del pwnagotchi

Pico W + pantalla de tinta **Waveshare 2,13" V3 o V4** (módulo Pico-ePaper-2.13, la del pwnagotchi).

Nexus genera una imagen por estado (en línea, escuchando, pensando, usando herramienta, hablando, hecho, error,
aviso) con el avatar y una cara al estilo pwnagotchi. El Pico las descarga una vez, las guarda en su memoria y
después solo pregunta el estado cada 2 s: cuando cambia, pinta la imagen guardada (refresco parcial, sin parpadeo;
uno completo cada 20). Si cambias el nombre del asistente o el estilo (`dispositivos.pico.estilo`: `avatar` o
`caras`), las vuelve a descargar solo. Puedes ver las imágenes en *Nexus › Dispositivos*.

## Grabar
1. Conecta el Pico W manteniendo pulsado **BOOTSEL**: aparece la unidad `RPI-RP2`.
2. *Nexus › Dispositivos › Firmware › Avatar Pico W › GRABAR*, o `python firmware/flash.py pico --wifi RED CLAVE`.
   A mano: copia `firmware/bin/nexuspico-picow.uf2` a la unidad.

El `.uf2` es MicroPython 1.24.1 con este código congelado. También vale un MicroPython normal copiando
`nexuspico.py`, `epd2in13.py` y `main.py` (por ejemplo con `mpremote cp`).

## Configuración
`/config.json` en el Pico (lo escribe `flash.py`, o la página del propio Pico: sin Wi-Fi crea la red
`NexusPico-XXXX`, clave `nexuspico`, con una página en http://192.168.4.1):
```json
{"wifi": {"ssid": "MiRed", "pass": "MiClave"}, "nexus": {"host": "192.168.1.10:8775", "key": "CLAVE_DE_RED"},
 "cada_s": 2, "completo_cada": 20, "pines": {"rst": 12, "dc": 8, "cs": 9, "clk": 10, "din": 11, "busy": 13}}
```

## Pines (Pico-ePaper-2.13)
RST GP12 · DC GP8 · CS GP9 · CLK GP10 · DIN GP11 · BUSY GP13. Si usas la pantalla del pwnagotchi (HAT de la
Raspberry) con cables, conéctala a esos pines o cámbialos en `pines`.
