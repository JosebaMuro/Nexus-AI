# NexusWatch — reloj para Nexus-Office (ESP32-2424S012C-Y / B)

Placa: ESP32-C3 con pantalla redonda GC9A01 de 1,28" (240×240) y táctil CST816S. Las variantes -Y y (B) usan los mismos pines.

- **Inicio**: el avatar de Nexus animado según lo que hace (en línea, escucha, piensa, usa una herramienta, habla, hecho, error), con el anillo del color del estado y el texto de lo que está haciendo.
- Desliza a izquierda o derecha para las *apps*:
  - **Agentes**: qué hace cada agente ahora (pensar, investigar, programar, reunión, café…). Toca uno para ver su escena y su tarea.
  - **Modos**: toca un modo (Normal, Manos libres, Silencio, Concentración, Rendimiento, Ahorro, Noche) y Nexus lo cambia en el escritorio y en el móvil a la vez.
  - **Info**: Wi-Fi, IP y Nexus al que está conectado. Mantén pulsado para volver al portal de configuración.
- La pantalla baja el brillo a los 30 s y se apaga a los 2 min; un toque la enciende.

## Grabar
Lo más fácil: *Nexus › Dispositivos › Firmware › Reloj*, o `nexus-flash reloj --wifi RED CLAVE`.

Binario: `firmware/bin/nexuswatch-esp32c3.bin` (dirección 0x0). Para compilarlo tú: Arduino IDE 2.x con el núcleo
**esp32 3.x**, placa **ESP32C3 Dev Module**, *USB CDC On Boot: Enabled*, *Flash Mode: DIO*. Sin librerías externas.

## Configuración
Por el puerto serie (115200), una línea JSON:
`{"wifi":{"ssid":"MiRed","pass":"MiClave"},"nexus":{"host":"192.168.1.10:8775","key":"CLAVE_DE_RED"}}`.
O desde el móvil: sin Wi-Fi el reloj crea la red `NexusWatch-XXXX` (clave `nexuswatch`) con una página en http://192.168.4.1.

## Archivos
| Archivo | Qué es |
|---|---|
| `nexuswatch.ino` | hardware: pantalla, táctil, Wi-Fi, portal y consultas a Nexus |
| `nw_core.h` | pantallas, gestos y animaciones (el mismo código corre en el emulador del PC) |
| `nw_gfx.h` | dibujo por franjas con bordes suavizados (sin necesitar 115 KB de RAM) |
| `nw_assets.h` | fuente y cara del avatar; se regenera con `firmware/tools/gen_assets.cpp` |
| `nl_json.h` | lector de JSON compartido con NexusLED |
| `emulator/` | `nw_emu.cpp` dibuja la pantalla en un `.ppm` a partir de la respuesta de `/api/dispositivos/reloj` |
