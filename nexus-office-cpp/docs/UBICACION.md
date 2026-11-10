# Ubicación en tiempo real

Los agentes saben dónde está el equipo (o el móvil) con la herramienta **`ubicacion_actual`**: «¿dónde estoy?»,
«¿a cuánto estoy de Vitoria?», «¿a qué velocidad voy?». Todo funciona **sin internet**: la posición se lee en el propio
equipo y **no se envía a ningún servicio externo**. El lugar se da con una tabla propia de ciudades de referencia
(capitales de provincia, ciudades grandes de España y del mundo) y con los **lugares guardados** del usuario
(Oficina, Casa, Almacén…).

## Fuentes

En automático se usa la mejor que haya en este orden; en *Ajustes > Ubicación > Fuente* se puede fijar una.

| Fuente | Dónde | Cómo |
|---|---|---|
| `app` | Android (app autónoma) | GPS del móvil mientras la app está abierta. Se activa con **USAR EL GPS DEL MÓVIL** (pide el permiso de Android). Usa el GPS (sin internet) y, sin pedir nada más, las posiciones que ya piden otras apps. En segundo plano queda la última posición, con su edad. |
| `gpsd` | Linux, Raspberry Pi, macOS | Receptor GPS servido por gpsd (`ubicacion.gpsd`, por defecto `127.0.0.1:2947`; solo equipo propio o red local). |
| `nmea` | Todas | Receptor GPS USB o serie que habla NMEA 0183 (`ubicacion.serie`: `/dev/ttyACM0`, `COM3`…; `ubicacion.baudios`, 9600). Sin puerto configurado solo se abren los que en `/dev/serial/by-id` se llaman *gps*, *gnss* o *u-blox*: abrir el puerto de un ESP32 o un Arduino lo reinicia, así que nunca se prueban puertos al azar. |
| `windows` | Windows 10/11 (x64 y ARM64) | Servicio de ubicación de Windows (`Windows.Devices.Geolocation`): GPS, wifi o red móvil según el equipo. Hay que activar *Configuración > Privacidad y seguridad > Ubicación* y «Permitir que las aplicaciones de escritorio accedan a tu ubicación». |
| `geoclue` | Linux con escritorio | GeoClue2 por el D-Bus del sistema (libdbus se carga al ejecutar; si no está, no pasa nada). El escritorio pide permiso la primera vez. En un equipo sin escritorio GeoClue espera a un «agente»: añade `[nexus-office]` con `allowed=true`, `system=true` y `users=` a `/etc/geoclue/geoclue.conf`, o usa gpsd o NMEA. Si falla, en automático no se vuelve a probar en 5 minutos. |
| `navegador` | Cualquier navegador | Si se activa *Usar la ubicación del navegador*, la interfaz manda la posición mientras la página está abierta (el navegador pide permiso). |
| `manual` | Todas | Posición fija de *Ajustes > Ubicación* para equipos sin GPS (una oficina, un local). Se avisa de que no es una medida en tiempo real. |

Las fuentes de wifi o red móvil de Windows, GeoClue y el navegador dependen de cómo tenga configurado el sistema su
propio servicio de ubicación; Agente IA no llama a ninguno por su cuenta. La de Android no pide la ubicación por red.

## La herramienta

`ubicacion_actual` (riesgo bajo, permiso `location`). Argumentos opcionales: `hasta` (lugar guardado, ciudad de la
tabla o `"lat, lon"`) y `fuente`. Devuelve `lat`, `lon`, `precision_m`, `altitud_m`, `velocidad_kmh`, `rumbo_grados`
y `hacia`, `satelites`, `fuente` y `detalle`, `hora`, `edad_s`, `en_tiempo_real`, `geo` (URI `geo:`), `lugar` (si está
dentro del radio de un lugar guardado), `cerca_de` (ciudad de referencia, distancia y dirección) y, con `hasta`, la
distancia en línea recta y el rumbo. Sin modelo de lenguaje también responde a «dónde estoy».

Privacidad: no se responde a otros nodos de la red COOP (salvo `ubicacion.remota`) ni a usuarios que no son el titular
(perfiles y dispositivos emparejados, salvo `ubicacion.usuarios`). La API solo responde desde el propio equipo o en
modo administrador.

## Configuración (`ubicacion.*`)

`activada` (true), `fuente` (`auto`), `manual` `{lat, lon, nombre}`, `lugares` `[{nombre, lat, lon, radio}]` (radio en
metros, 150), `serie`, `baudios`, `gpsd`, `navegador` (false), `remota` (false), `usuarios` (false), `frescura_s` (120:
hasta esa edad una posición cuenta como «en tiempo real») y `timeout_s` (8).

## API

| Ruta | Qué hace |
|---|---|
| `GET /api/ubicacion` | Fuentes de este equipo, configuración y última posición. |
| `POST /api/ubicacion/consultar` | `{fuente?, hasta?, timeout_s?}`: la posición ahora (lo mismo que la herramienta). |
| `POST /api/ubicacion` | `{lat, lon, precision?, altitud?, velocidad?, rumbo?, t?}`: posición del navegador (con `ubicacion.navegador`). |

La app Android escribe la posición en `ubicacion.json` junto a la carpeta de datos (`NEXUS_UBICACION`).
