# Instaladores sin conexión

Desde la 4.3.0 Agente IA guarda los instaladores de **todas las plataformas** para pasarlos a otros equipos **sin
internet**. Está en la pestaña **INSTALADORES** (icono de la flecha hacia abajo), tanto en la APK autónoma como en el
escritorio.

## Qué viene dentro y qué se guarda

| Qué | Dónde está | Tamaño |
|---|---|---|
| APK autónoma (la propia app instalada) | dentro de la app: Android la tiene siempre | ≈ 230 MB |
| APK cliente, código fuente, script de la Ubuntu mínima, LEEME y manuales en PDF | dentro de la APK (`app/maletin/`) | ≈ 12 MB |
| Windows x64 y ARM64, Linux x86_64 y ARM64 (`.run`, `.deb`, AppImage, Flatpak), macOS | se bajan **una vez** y quedan guardados | 0,4 a 1,9 GB cada uno, ≈ 14 GB todos |

Los instaladores de escritorio no pueden ir dentro de la APK: juntos pasan de 14 GB y Android no instala APK de más
de 4 GB (el de Windows x64 solo ya pesa 1,9 GB). Por eso se bajan **una sola vez** de la Release de GitHub de la misma
versión, con la wifi, y desde entonces están disponibles sin conexión. Se comprueban con el `SHA256SUMS.txt` de la
Release; si un fichero no coincide se borra y hay que volver a bajarlo. Las descargas se pueden parar y se reanudan
donde iban.

Botones (modo administrador):

- **BAJAR TODOS LOS QUE FALTAN** o **BAJAR LOS DE ARM**: solo los de móvil, Raspberry Pi, Snapdragon y Mac con chip
  Apple (unos 3,3 GB), para no llenar el móvil.
- **BAJAR** en una fila: solo ese instalador. Antes de empezar se mira que haya espacio (se deja 300 MB libres).
- **IMPORTAR DE CARPETA O USB**: sin internet en ningún momento. En el móvil se eligen los ficheros (USB por OTG,
  Descargas, tarjeta); en el escritorio se escribe la carpeta. Si junto a ellos está el `SHA256SUMS.txt` de la
  Release, también se comprueban.
- **GUARDAR**: copia el instalador donde elijas (Descargas, un USB...) para pasarlo por cable.

## Repartir por la red local

**COMPARTIR POR WIFI** abre un pequeño servidor de **solo lectura** (puerto 8776) con una página de descarga y un
código QR. Los otros equipos tienen que estar en la misma wifi o conectados al **punto de acceso del móvil** (no hace
falta que el punto de acceso tenga datos). Abren la dirección en el navegador, eligen su instalador y lo bajan; la
página explica cómo instalarlo en cada sistema. Ese servidor solo sirve los instaladores: no da acceso a Agente IA ni a
sus datos. Se cierra con **DEJAR DE COMPARTIR** o al cerrar la app.

## API

| Ruta | Para qué |
|---|---|
| `GET /api/instaladores` | lista, estado, espacio libre, descarga en curso y reparto |
| `POST /api/instaladores/release` | consulta la Release (nombres, tamaños y SHA256) y la guarda para usarla sin conexión |
| `POST /api/instaladores/descargar` `{"ids": [...]}` | baja los indicados, uno detrás de otro, en segundo plano |
| `POST /api/instaladores/cancelar` | para la descarga (el `.part` se queda para reanudar) |
| `POST /api/instaladores/importar` `{"carpeta": "/media/usb"}` | copia y comprueba los instaladores de una carpeta |
| `POST /api/instaladores/revisar` | comprueba los copiados a mano a `datos/instaladores/` |
| `DELETE /api/instaladores/{id}` | borra uno guardado en el equipo |
| `POST /api/instaladores/compartir` `{"activo": true, "puerto": 8776}` | abre o cierra el servidor de la red local |
| `GET /api/instaladores/fichero/{id}` | descarga un instalador desde este equipo |

Los ficheros guardados están en `NEXUS_DATA_DIR/instaladores/` (en el móvil, en los datos de la app). El código está en
`nexus/instaladores.py`; la APK los copia con `MainActivity.guardarInstalador` e `importarInstaladores` (por bloques,
sin cargarlos en memoria).
