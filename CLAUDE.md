# CLAUDE.md · Nexus-AI / Nexus-Office

Guía corta para trabajar en el repositorio sin leerlo entero. Primero mira aquí; abre solo los ficheros que nombra la
tarea. Detalle de cada módulo en `nexus-office-cpp/docs/ARQUITECTURA.md` y `docs/CPP.md`.

## Qué hay en el repositorio

| Carpeta | Qué es | Estado |
|---|---|---|
| `nexus-office-cpp/` | **Nexus-Office, edición C++** (oficina de IA local para pymes). Aquí va todo el trabajo nuevo. | activa |
| `nexus-office/` | Edición Python original (y NexusAI particular). | **congelada**: no tocar salvo que se pida |
| `firmware/` | NexusLED (ESP8266/ESP32), NexusWatch, NexusPico (Pico W, C++ con Pico SDK), grabador `nexus-flash`. | activa |
| `.github/workflows/` | Compilación, instaladores y Releases (ver abajo). | |
| `index.html`, `README.md`, `LEEME.txt` | Página de descargas (GitHub Pages) y presentación. | |

## Normas del proyecto (de Joseba, obligatorias)

- **Todo en C++** (desde 2026-10-27). Nada nuevo en Python; las herramientas de construcción también son C++
  (`tools/construir/`). Llamar a herramientas del sistema (osascript, zenity...) está bien; escribir scripts no.
- **Entrega**: solo versión portable 100 % offline (los motores y runtimes van dentro), instalable como servicio.
  Formatos: `.exe` (Windows x64 y ARM64), `.run`, `.deb`, AppImage y Flatpak (Linux x86_64 y aarch64/Raspberry Pi),
  macOS `tar.gz`, APK autónoma y APK cliente, zip del código fuente limpio, más manuales (técnico y de usuario).
  Hay que probar que todo funciona.
- **Espacio**: se pueden borrar binarios o instaladores de versiones viejas, pero el código fuente de cada versión se
  conserva siempre (tag, rama o zip).
- **Releases**: las sesiones no crean Releases ni usan LFS directamente; se construye en Actions y `cpp-publicar.yml`
  publica. En el hilo, cuando se sube a GitHub, se da **solo el enlace a la Release**.
- Textos de la interfaz, comentarios, commits y documentación **en español**, sin tildes en los comentarios del código
  (como el código existente). No poner identificadores de modelos de Claude en el repositorio.

## Compilar y probar (edición C++)

```bash
cd nexus-office-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # C++20, CMake 3.20+
cmake --build build --parallel
build/nexus-tests                                  # o: ctest --test-dir build --output-on-failure
build/nexus-office run                             # interfaz en http://127.0.0.1:8775
build/nexus-construir datos                        # tras tocar recursos/datos/*.json: regenera src/gen/data.cpp
```

- Los `src/**/*.cpp` y `tests/*.cpp` entran solos (GLOB en `CMakeLists.txt`); no hace falta registrar ficheros nuevos.
- Versión: `project(... VERSION x.y.z)` y `NEXUS_VERSION "x.y.z-cpp"` en `nexus-office-cpp/CMakeLists.txt`, y su
  sección en `CHANGELOG.md`.
- Pruebas: framework propio (`tests/check.h`), un fichero por área (`test_motores.cpp`, `test_server.cpp`...).

## Dónde está cada cosa (nexus-office-cpp/)

| Para tocar... | Mira |
|---|---|
| Rutas de la API REST/WebSocket | `src/server/routes.cpp` (grande: busca con grep `app.post("/api/...`), `src/server/app.h` (`Ctx`: `c.bs()`, `c.q()`, `c.local()`, `c.remote`; el último argumento `true` de `app.get/post` = solo Admin) |
| Interfaz web | `recursos/web/index.html` (vistas `<section class="view" id="view-...">`, diálogos `<dialog>`), `recursos/web/js/app.js` (`api()`, `toast()`, `guard()`, `$`/`$$`, `loaders.<vista>`), `recursos/web/css/` |
| Línea de órdenes | `src/main.cpp` |
| Modelos (catálogo, descargas, importar, recomendados) | `src/models/` (`manager.cpp`, `sources.cpp`, `recommended.cpp`, `runtimes.cpp`), datos en `recursos/datos/recommended.json` |
| Motores de IA | `src/llm/`: `llamaserver.cpp` (llama.cpp), `motores.cpp` + `motor_litert.cpp` / `motor_onnx.cpp` / `motor_genie.cpp` (LiteRT-LM, ONNX GenAI, Qualcomm Genie, en proceso aparte con dlopen), `manager.cpp` (elección de motor) |
| Hardware y selector CPU/GPU/NPU | `src/hardware/` (`detect`, `profile`, `android_accel`), `src/core/system.cpp` (`accel_options`, `accel_apply`) |
| Configuración, base de datos, servicio, autorreparación | `src/core/` (`config`, `db`, `service`, `salud`) |
| Utilidades (rutas UTF-8 `P()`/`S()`, ficheros, procesos, HTTP, texto, JSON) | `src/util/` (`fs.h`, `proc.h`, `text.h`, `sysinfo.h`) |
| Agentes, memoria/grafos, RAG, media, oficina y bolsa, red COOP, acceso remoto, dispositivos | `src/agents/`, `src/memory/`, `src/rag/`, `src/media/`, `src/office/`, `src/coop/`, `src/remote/`, `src/devices/` |
| Instaladores, APK, PDF, prueba de humo | `tools/construir/` (`nexus-construir paquetes|apk|pdf|humo|datos`), `packaging/`, `android/` |
| Documentación | `docs/` (`USUARIO.md` manual, `ACELERADOR.md` motores y aceleradores, `API.md`, `PLATAFORMAS.md`...) |

## Workflows de GitHub Actions

| Workflow | Para qué |
|---|---|
| `cpp.yml` | Compilar y probar la edición C++ en cada push. |
| `cpp-entrega.yml` | Instaladores de todas las plataformas, APK y pruebas con modelo real (manual). Toma bibliotecas de NPU de `motores-npu.yml`. |
| `motores-npu.yml` | Compila lo que no viene hecho: LiteRT-LM con dispatch de NPU (Tensor, Qualcomm, MediaTek), QAIRT/Genie, llama.cpp CUDA ARM64. Artefactos 90 días. |
| `cpp-publicar.yml` | Publica la Release a partir de una rama `publicar-cpp-<versión>` con `entrega/entrega.json`. |
| `android-npu.yml`, `selector-runtimes.yml`, `gimp-runtimes.yml`, `firmware.yml`, `wikipedia-grafo.yml` | Runtimes y piezas aparte. |
| `entrega.yml`, `apk.yml`, `publicar.yml`, `python.yml`, `release.yml` | Edición Python (congelada). |

## Entorno de las sesiones

- `dl.google.com` está bloqueado y Maven Central limita peticiones: lo que necesite el SDK de Android o bazel se
  compila en Actions, no en la sesión.
- Ramas de trabajo `claude/...`; PR contra `main`. `main` es la base de la edición C++.
