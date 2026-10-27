# Edición C++ (Nexus-Office 2.1)

Nexus-Office reescrito en C++20, con todas las funciones de la edición Python. Es la misma aplicación: la misma interfaz web (los ficheros de
`nexus-office/nexus/web` se usan tal cual), la misma API HTTP y WebSocket, los mismos perfiles de oficina, agentes,
paquetes de conocimiento, base de datos y configuración. Cambia el motor: en vez de Python con sus dependencias es un
único ejecutable nativo, `nexus-office` (`nexus-office.exe` en Windows).

| | Edición Python | Edición C++ |
|---|---|---|
| Programa | CPython + ~60 paquetes en `venv/` | un ejecutable de unos 15 MB |
| Arranque hasta responder (mismo equipo, datos nuevos) | 1,2 s | 0,13 s |
| Memoria en reposo (mismo equipo) | 119 MB | 68 MB |
| Modelos de IA y motores (llama-server, sd-cli) | los mismos | los mismos |

Medido en un servidor Linux x86_64 con las dos ediciones recién instaladas y sin modelo cargado; con el modelo de IA en
marcha, lo que más ocupa es llama-server, que es el mismo en las dos.

## Qué está igual

- **Interfaz**: todas las vistas (Chat, Salón, Núcleo, Estudio, Agentes, Modelos, Herramientas, Sistema, Ajustes),
  modo usuario y administrador con PIN, temas, avatar y modo en vivo por voz.
- **API**: todas las rutas HTTP y el WebSocket de la edición Python, con las mismas respuestas (también los errores 422 al estilo de
  FastAPI). Las pruebas `tests/compare_api.py` y `tests/compare_flows.py` arrancan las dos ediciones con datos nuevos y
  comparan 39 rutas y 41 flujos completos (administrador, memoria, agentes, biblioteca, multimedia, grafo, oficina y
  chat por WebSocket), y `tests/compare_full.py` otros 108 (bolsa, grafos, web, red COOP, VPN y GIMP): coinciden
  todos salvo la clave `edition` que añade esta edición y un código de error (404 frente a 405) en una ruta inexistente.
- **Motor de IA**: llama-server (llama.cpp) con el selector de motor y acelerador (CPU, GPU, NPU) al arrancar, modelos
  recomendados según el hardware (Qwen3.5), descarga de Hugging Face, Ollama, ModelScope, Civitai y Kaggle, y el motor
  integrado sin modelo para las operaciones sencillas (la calculadora del chat responde aunque no haya modelo).
- **Memoria, grafo y aprendizaje**: memoria semántica, grafo general y por agente, gestor de conocimiento (crear,
  editar, mover, fusionar, reorganizar y buscar duplicados), embeddings, retroalimentación y consolidación.
- **Biblioteca (RAG)**: PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, Markdown, texto, código y
  archivos ZIP, TAR, 7z y RAR; búsqueda híbrida con citas.
- **Multimedia**: imagen con stable-diffusion.cpp (o arte procedural sin modelo), música con el compositor procedural
  (WAV + MIDI), voz con Piper o la voz del sistema (SAPI, `say`, espeak-ng) y dictado con Whisper (whisper.cpp).
- **Agentes y herramientas**: agentes YAML, enrutado y delegación, salón y reuniones, herramientas supervisadas con
  aprobaciones, sandbox y auditoría, y propuestas de herramientas nuevas.
- **Servicio**: `nexus-office servicio instalar [--red]` con systemd, launchd, el Programador de tareas de Windows (con
  `nexus-office-w.exe`, sin ventana) o Termux.
- **Línea de órdenes**: `run`, `detect`, `pull`, `modelos`, `servicio`, `chat`, `oficina`, `bolsa`, `grafo`, `setup`,
  `wheels` y `cleanup`, con las mismas opciones.

## Funciones que llegaron en la 2.1

La 2.0 beta C dejaba fuera varias funciones. Desde la 2.1 están todas, con las mismas respuestas que la edición Python:

| Función | Cómo está hecha en C++ |
|---|---|
| Mesa de bolsa (herramientas `bolsa_*`, vista y orden `bolsa`) | `src/office/bolsa.cpp`; mismos mercados, costes, órdenes e informe fiscal |
| Red COOP, VPN WireGuard y acceso remoto por internet | `src/coop`, `src/remote`; cifrado propio (scrypt, X25519) y TLS 1.3 con mbedTLS |
| Importar y exportar grafos de más de 50 formatos y paquetes de dominio | `src/memory/kg_formats.cpp`, con lectores propios de XML, CSV, TOML, bzip2 y xz |
| Rastrear sitios web y guardar una URL como PDF maquetado | `src/rag/crawler.cpp` y `url_pdf.cpp`, con un escritor de PDF propio (`pdfgen.cpp`) |
| Edición de imágenes como en GIMP (`imagen_editar`, `imagen_info`, `gimp_estado`, `gimp_script`) | `src/media/gimp.cpp` y un motor de imagen propio (`raster.cpp`) cuando no hay GIMP |
| Agente Profesor con IA online (`/api/nube`, `consultar_profesor`) | `src/llm/cloud.cpp`: API compatible con OpenAI y Messages API de Anthropic en streaming |
| APK autónoma con GPU (Vulkan, OpenCL) y NPU Hexagon | `src/hardware/android_accel.cpp`: sonda de llama-server-gpu y respaldo NPU → GPU → CPU |
| Autoajuste en vivo en Android (`/api/android/autoajuste`) | `src/core/android_tune.cpp`: hilos medidos, temperatura, batería, GPU frente a CPU y consejo de modelo |
| Bolsa con cotizaciones reales (Yahoo Finance, CNBC, Google Finance, Stooq, Coinbase, Kraken y BCE) | `src/office/bolsa_datos.cpp`: caché, fuentes apartadas un rato si fallan y simulación sin conexión |
| Agente de bolsa autónomo que opera siempre en simulación (`/api/bolsa/agente`) | `src/office/bolsa_agente.cpp`: medias, RSI, momento, ATR, pesos aprendidos, stops vivos y diario |
| Gráficos en vivo de las órdenes (`/api/bolsa/serie`, `/api/bolsa/orden/{id}/grafico`) | `src/office/bolsa.cpp`: velas reales y histórico guardado para trabajar sin red |
| Lo que hace cada agente (despachos, reloj y salón: `/api/agentes/actividad`) | `src/agents/activity.cpp` |
| Luces NexusLED, modos, reloj NexusWatch y Pico W (`/api/dispositivos`) | `src/devices/devices.cpp`: UDP y HTTP del firmware e imágenes de tinta hechas en C++ |
| Grabar el firmware de las luces, el reloj y el Pico W (`/api/firmware`) | `src/devices/firmware.cpp`: puertos serie nativos, esptool para los ESP32 y copia del .uf2 al Pico |
| Órdenes `grafo`, `bolsa`, `setup`, `wheels` y `cleanup` | `src/main.cpp` |

`/api/tools` lista las mismas herramientas en las dos ediciones. Quedan dos diferencias que no aplican a C++:

| Función de la edición Python | En C++ |
|---|---|
| Motor «llama.cpp integrado» (llama-cpp-python) | se usa llama-server, que es el mismo llama.cpp |
| Backend `transformers` y MusicGen (PyTorch) | el compositor procedural de música |

## Ejemplos incluidos

- `ejemplos/grafo-wikipedia-es/`: grafo multicapa de la Wikipedia en español para importar (capas `1-temas`,
  `2-articulos`, `3-enlaces` y `4-wikidata`). Lo genera `tools/wikipedia_grafo.py` con las API públicas de
  es.wikipedia.org y wikidata.org, desde la GitHub Action `wikipedia-grafo.yml`.
- `ejemplos/resumen-proyecto/Nexus-Office-resumen.md`: resumen de todo el proyecto para subirlo a la biblioteca.

## Convivencia con la edición Python

Se instalan por separado y no se pisan: Windows en `%LOCALAPPDATA%\Nexus-Office-C`, Linux en
`~/.local/share/nexus-office-c` (o `/opt/nexus-office-c` con el `.deb`), con su propia entrada de menú. Los datos tienen
el mismo formato: para pasar los de la edición Python basta copiar su carpeta `data/` (o
`~/.local/share/nexus-office`) a la de la edición C++ con las dos cerradas. Las dos usan el puerto 8775 y el mismo nombre
de servicio, así que solo una puede estar en marcha a la vez.

## Cómo está hecha

```
nexus-office-cpp/
  CMakeLists.txt        compila nexus-office y las pruebas; copia los recursos compartidos junto al ejecutable
  src/main.cpp          línea de órdenes
  src/server/           servidor HTTP y WebSocket (cpp-httplib) y las rutas de la API (routes.cpp)
  src/core/             configuración, base de datos (SQLite), servicios y servicio del sistema
  src/llm/              gestor de motores, llama-server, Ollama, OpenAI compatible y motor integrado
  src/hardware/         detección de CPU/GPU/NPU, perfiles, monitor y selector de acelerador
  src/models/           catálogo recomendado, descargas, lector GGUF y runtimes incluidos
  src/memory/           memoria, grafo, embeddings, organizador y aprendizaje
  src/rag/              lectores de documentos, PDF, archivos comprimidos y extracción web
  src/media/            imagen, música, voz y mejorador de prompts
  src/agents/ src/tools/ src/office/   agentes, salón, herramientas supervisadas y perfiles de oficina
  src/util/             ficheros, procesos, HTTP, expresiones regulares (PCRE2), YAML, cifrado, texto
  src/gen/data.cpp      datos de la edición Python (catálogos, tablas) generados por tools/gen_data.py
  third_party/          cpp-httplib, nlohmann/json, SQLite, miniz, PCRE2, rapidyaml, stb y certificados raíz
  tests/                pruebas unitarias y de integración, comparación con Python y prueba de la interfaz
  packaging/            instaladores (build.py, NSIS, guiones de instalación)
```

Dependencias externas: ninguna en tiempo de ejecución. Para compilar, un compilador C++20 (GCC 12, Clang 15, MSVC
2022 o Apple Clang 15) y CMake 3.20. El TLS para HTTPS es mbedTLS (se descarga y compila solo) u OpenSSL
(`-DNEXUS_TLS=openssl`).

```
cmake -S nexus-office-cpp -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/nexus-office run
```

## Pruebas

- `nexus-tests`: unitarias (texto, expresiones regulares, YAML, JSON, configuración, base de datos, memoria, RAG...) y
  una de integración que levanta el servidor en un puerto libre y recorre API, administrador, memoria, biblioteca,
  multimedia y chat por WebSocket.
- `tests/compare_api.py`, `tests/compare_flows.py` y `tests/compare_full.py`: comparación con la edición Python, ruta
  por ruta y flujo por flujo; `tests/compare_kg.py` lee un ejemplo de cada formato de grafo en las dos ediciones y
  `tests/compare_gimp.py` compara 73 ediciones de imagen píxel a píxel.
- `nexus-tests nube` y `nexus-tests android`: el Profesor contra un servidor falso de OpenAI y de Anthropic, y la sonda,
  el respaldo NPU → GPU → CPU y el autoajuste de Android con un llama-server falso.
- `nexus-tests bolsa`: cotizaciones con fuentes falsas (caché, peniques a libras, sin conexión) y el agente autónomo
  comprando, poniendo el stop, vendiendo en el objetivo y aprendiendo. Con `NEXUS_ONLINE_TESTS=1` (en `cpp.yml`)
  pide precios reales del catálogo con la pila TLS de cada sistema.
- `nexus-tests dispositivos`: luces contra el emulador del firmware NexusLED (`firmware/emulator`, se compila con las
  pruebas), modos, reloj, imágenes del Pico W y grabado del reloj con un esptool falso y un puerto serie simulado.
  El código real del Pico W (`firmware/nexuspico/nexuspico.py`) descarga las imágenes del servidor C++ y su giro
  coincide byte a byte.
- `tests/ui_smoke.js`: abre la interfaz en Chromium, pregunta «¿Cuánto es 12*7?», espera «84», recorre las vistas y
  falla si hay errores de JavaScript o respuestas 5xx.
- Revisión de memoria con AddressSanitizer y UndefinedBehaviorSanitizer recorriendo todas las pruebas anteriores: sin
  errores.
- `.github/workflows/cpp.yml` compila y prueba en Windows x64 y ARM64, Linux x86_64 y ARM64 y macOS Apple Silicon e
  Intel; `.github/workflows/cpp-entrega.yml` construye los instaladores y los prueba instalados con un modelo real
  (Qwen3.5 0.8B) en cada sistema.

Resultado de la entrega 2.0.0-beta-cpp: los seis instaladores de escritorio se instalaron y pasaron la prueba de
humo con modelos reales (Windows x64 y ARM64, Linux x86_64 y ARM64 con .run, .deb, AppImage y Flatpak, macOS Apple
Silicon e Intel): chat por WebSocket, herramienta de calculadora, voz con Piper, dictado con Whisper, imagen con
stable-diffusion.cpp y servicio del sistema. La APK autónoma arranca en un emulador Android y responde a `/api/ping`,
estado y prueba del acelerador.

- En Android ARM64 el servidor se compila sin AdvSIMD (`NEXUS_ANDROID_NOSIMD`, activado por defecto): la traducción
  ARM de los emuladores x86_64 no implementa algunas instrucciones escalares de AdvSIMD. Con `-DNEXUS_ANDROID_NOSIMD=OFF`
  se compila para móviles reales con NEON.
- En la máquina virtual macOS ARM de GitHub la GPU paravirtual devuelve texto sin sentido con Metal, así que allí el chat
  se prueba en CPU; el chat con Metal en un Mac real está pendiente de probar.
- Con `-ngl 0` (todo en CPU) llama-server usa un solo hueco de conversación para no partir el contexto en dos.

Sin probar en hardware real (como en la edición Python): GPU dedicada, NPU Intel, y la GPU y la NPU Hexagon de un
móvil. La lógica de Android (sonda, respaldo NPU → GPU → CPU y autoajuste) se prueba en `tests/test_android.cpp` con un
llama-server falso que falla con el dispositivo que se le pide.
