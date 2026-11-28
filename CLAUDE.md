# CLAUDE.md · Agente IA (antes Nexus-AI / Nexus-Office)

Guía corta para trabajar en el repositorio sin leerlo entero. Primero mira aquí; abre solo los ficheros que nombra la
tarea. Detalle de cada módulo en `nexus-office/docs/ARQUITECTURA.md`.

> **Nombre**: el producto se llama **Agente IA**. Solo cambió el nombre visible (interfaz, instaladores, manuales, web,
> APK con identificador `ai.agente.ia`); carpetas, paquete (`nexus`), espacios de nombres y nombres de fichero de los
> instaladores siguen con «nexus». Los textos nuevos para el usuario dicen «Agente IA».

## Qué hay en el repositorio

| Carpeta | Qué es |
|---|---|
| `nexus-office/` | **Agente IA, edición Python** (la única desde la 4.2.0). Paquete `nexus`, servidor FastAPI, interfaz web, empaquetado, apps Android y pruebas. |
| `firmware/` | NexusLED (ESP8266/ESP32), NexusWatch, NexusPico (Pico W), grabador `nexus-flash` (C++), `flash.py`. |
| `.github/workflows/` | Pruebas, instaladores, APK, motores y Releases (ver abajo). |
| `index.html`, `README.md`, `LEEME.txt` | Página de descargas (GitHub Pages, lee la última Release) y presentación. |

La edición C++ (3.x) salió de `main` en la 4.2.0: su código está en el tag `v3.2.3-cpp`, completo con los últimos
cambios en el commit `59fa50d` de `main`, y como zip en la Release v4.2.0 (`Agente-IA-cpp-referencia-codigo-fuente.zip`).

## Normas del proyecto (de Joseba, obligatorias)

- **Python es la base** desde 2026-11-10 (Agente IA 4.0.0), por los motores de inferencia por hardware.
- **Entrega**: solo versión portable 100 % offline (los motores y runtimes van dentro), instalable como servicio.
  Formatos: `.exe` (Windows x64 y ARM64), `.run`, `.deb`, AppImage y Flatpak (Linux x86_64 y aarch64/Raspberry Pi),
  macOS `tar.gz`, APK autónoma y APK cliente, zip del código fuente limpio, más manuales (técnico y de usuario) y el
  script de la Ubuntu mínima (`packaging/ubuntu/`). Hay que probar que todo funciona.
- **Espacio**: se pueden borrar binarios o instaladores de versiones viejas, pero el código fuente de cada versión se
  conserva siempre (tag, rama o zip).
- **Releases**: las sesiones no crean Releases ni usan LFS directamente; se construye en Actions y `publicar.yml`
  publica. En el hilo, cuando se sube a GitHub, se da **solo el enlace a la Release**.
- Textos de la interfaz, comentarios, commits y documentación **en español**, sin tildes en los comentarios del código
  (como el código existente). No poner identificadores de modelos de Claude en el repositorio.
- Herramientas y agentes **sin sandbox** (lo pidió Joseba en la 4.0.3): no volver a aislar `python_exec` ni el terminal.

## Probar y arrancar

```bash
cd nexus-office
python -m pytest -q                     # pruebas (NEXUS_SIN_MUNDO=1 ya lo pone tests/conftest.py)
python -m nexus run                     # interfaz en http://127.0.0.1:8775
python -m nexus terminal                # Terminal IA en la consola (-p "pregunta" para una sola orden)
python -m nexus mundo probar --todas    # prueba en vivo las fuentes del mundo (necesita red)
NEXUS_CHROMIUM=/ruta/chromium python packaging/build_docs_pdf.py <carpeta>   # manuales en PDF
```

- Versión: `nexus/__init__.py` (`__version__`) y su sección `## Agente IA x.y.z` en `CHANGELOG.md` (la usa
  `publicar.yml` para las notas de la Release).
- `NEXUS_DATA_DIR` redirige los datos; `NEXUS_SIN_MUNDO=1` no arranca el servicio Mundo; `NEXUS_BOLSA_OFFLINE=1` sin red.

## Dónde está cada cosa (nexus-office/nexus/)

| Para tocar... | Mira |
|---|---|
| Rutas de la API | `server/app.py` (grande: grep `@app.get("/api/...`), `server/rutas_extra.py` (4.1), `server/rutas_42.py` (mundo, terminal de bolsa, Terminal IA) |
| Interfaz web | `web/index.html` (vistas `<section class="view" id="view-...">`), `web/js/app.js` (`api()`, `toast()`, `guard()`, `loaders.<vista>`; token de administrador en `sessionStorage`), `web/js/extras.js` (4.1), `web/js/v42.js` (Mundo, Terminal IA, terminal de bolsa), `web/sw.js` (sube `CACHE` al cambiar ficheros) |
| Servicios y arranque | `core.py` (`Services`), `config.py` (`DEFAULTS`, `SECRET_KEYS`), `__main__.py` (línea de órdenes) |
| Hardware, perfil y autoajuste | `hardware/` (`detectar` con caché y huella, `detect.py`, `profiles.py`, `tuner.py`, `android_accel.py`) |
| Motores de inferencia por chip | `motores/` (`selector.py`, `chips.py`, `banco.py`, un módulo por motor), `llm/llamaserver.py` (llama.cpp de respaldo), `runtimes.py`, `wheelhouse.py` (qué va en cada instalador) |
| Agentes | `agents/manager.py`, `agents/defaults/*.yaml` (incluye `terminal.yaml`), oficina en `office/agentes/*.yaml` y `office/perfiles/*.yaml` |
| Herramientas de los agentes | `tools/builtin/*.py` (`TOOLS = [(definicion, funcion)]`; riesgo low/medium/high lo decide `Supervisor`) |
| Mundo (fuentes públicas por categoría y país) | `mundo/` (`fuentes.py`, `servicio.py`, `paises.py`, `probar.py`) |
| Bolsa | `office/bolsa.py` (mesa simulada), `bolsa_agente.py` (agente autónomo), `mercados.py` (datos reales), `bolsa_analisis.py` (opciones, riesgo, DCF, cribas, alertas, expedientes), `bolsa_informes.py` (SEC), `bolsa_terminal.py` (recetas y escritorio), `bolsa_lab.py` |
| Terminal IA | `terminal/nucleo.py` (sesiones, consola, ficheros, procesos), `terminal/interprete.py`, `terminal/cli.py`, herramientas en `tools/builtin/terminal_tools.py` |
| Instaladores sin conexión (pestaña INSTALADORES) | `instaladores.py` (catálogo, descarga única con SHA256, importar, servidor de la red local en el puerto 8776), `server/rutas_instaladores.py`, `web/js/instaladores.js`; lo pequeño entra en la APK por `build_apk_autonomo.maletin()` |
| FreeLLMAPI (endpoint de planes gratis, del proyecto «9000 tokens») | `freellmapi.py` (arranca la incluida, copia la clave unica), `server/rutas_freellmapi.py`, `web/js/freellmapi.js`, motor `freellmapi` en `llm/manager.py`; se empaqueta con `packaging/fetch_freellmapi.py` |
| Memoria, RAG, media, red, remoto, dispositivos, MCP | `memory/`, `rag/`, `media/`, `coop/`, `remote/`, `devices/`, `mcp.py` |
| Empaquetado | `packaging/build.py` (instaladores), `build_apk*.py`, `build_linux_bundles.py`, `build_docs_pdf.py` (capítulos de los PDF), `windows/nexus.nsi`, `ubuntu/` (Ubuntu mínima con el demonio) |
| Android | `android/` (APK cliente), `android-autonomo/` (APK con todo dentro), `android_main.py`, `android_tune.py` |
| Documentación | `docs/` (`USUARIO.md` manual, `ARQUITECTURA.md`, `MOTORES.md`, `API.md`, `NOVEDADES-4.x.md`...) |

## Workflows de GitHub Actions

| Workflow | Para qué |
|---|---|
| `python.yml` | Pruebas en Linux, Windows y macOS en cada push. |
| `entrega.yml` | Instaladores de escritorio (Windows, Linux, macOS) y ISO de Ubuntu mínima (manual). |
| `apk.yml` | APK autónoma y cliente (trozos en la rama `apk-sin-firmar`). |
| `publicar.yml` | Publica la Release desde la rama temporal `entrega-<versión>` con `entrega/entrega.json`. |
| `motores-python.yml`, `motores-npu.yml`, `android-npu.yml`, `selector-runtimes.yml`, `gimp-runtimes.yml` | Banco de motores con modelos reales y runtimes que se compilan aparte. |
| `firmware.yml` | Firmware de las placas y `nexus-flash`. |

## Firma y entorno

- **Firma de la APK**: clave «Agente IA» (alias `agente-ia`), que guarda Joseba fuera del repositorio. Actions firma
  con una clave temporal. Nunca subas la clave al repositorio.
- `dl.google.com` está bloqueado en las sesiones y el proxy corta muchas APIs públicas: lo que necesite el SDK de
  Android, o probar fuentes en vivo, se hace en Actions.
- Ramas de trabajo `claude/...`; PR contra `main`.
