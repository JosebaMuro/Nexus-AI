# Nexus-Office

**Agente de inteligencia artificial multimodal, portable y local**: conversa, ve imágenes, genera imágenes y música, habla y escucha, recuerda y aprende, coordina un equipo de agentes con herramientas supervisadas y se adapta automáticamente al hardware en el que se ejecuta. Todo en Python, con su propio intérprete completo dentro de la carpeta del proyecto.

```
┌─────────────── webui futurista (FastAPI + WebSocket + PWA) ───────────────┐
│ Chat · Salón de agentes · Núcleo (grafo 3D) · Estudio · Agentes · Modelos │
│ Herramientas · Red cooperativa · Sistema · Ajustes  — modo Usuario/Admin  │
└───────────────────────────────────────────────────────────────────────────┘
```

## Características

| Área | Qué hace |
|---|---|
| **Multimodal** | Texto + visión con razonamiento (Qwen3‑VL Thinking vía `llama-server`), generación de imagen (stable-diffusion.cpp), música (compositor procedural WAV+MIDI o MusicGen en GPU), voz (Piper / SAPI / navegador) y reconocimiento de voz (faster‑whisper / navegador). |
| **Memoria y aprendizaje** | Memoria semántica a largo plazo con **grafo general común** (tu perfil, gustos, pautas de comportamiento, relaciones e interacciones) y **memoria propia de cada agente** con el saber hacer de su especialidad. **Gestor de conocimiento** para importar como conocimiento grafos de **cualquier formato**, reconocido solo (JSON, CSV, Excel, GraphML, GEXF, GML, DOT, Pajek, RDF/OWL, JSON-LD, Neo4j/Cypher, GraphSON, Mermaid, mapas mentales, notas con `[[enlaces]]`, SQLite, ZIP…) y fusionarlos con lo que ya sabe, leer y razonar un ZIP, editar, mover y fusionar nodos. Embeddings **Qwen3-Embedding** en CPU, especializados en documentación, extracción de hechos y preferencias, retroalimentación 👍/👎 que genera ejemplos y lecciones, preferencia de longitud aprendida, consolidación y olvido gradual. |
| **RAG y URL→PDF** | Biblioteca de documentos con búsqueda híbrida y citas. Formatos: PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, Markdown, texto y código. También **archivos ZIP, RAR, 7z y TAR** con proyectos o documentación. **Importación en lote** que usa todos los núcleos y **rastreo web paralelo** de sitios de documentación. Convierte cualquier URL en un **PDF estructurado** (portada, índice, secciones, tablas, imágenes, enlaces), lo publica, lo archiva en el RAG y **asimila** su contenido en memoria y grafo. Las webs hechas con JavaScript se **leen como un navegador**. |
| **Avatar** | Seleccionable: **Geisha del eclipse 3D** construida a partir de una hoja de personaje (casco visual de las siluetas de frente/perfil/espalda con las fotos proyectadas por shader; busto o cuerpo entero con doble clic; parpadeo, mirada, labios sincronizados con la voz, eclipse que cambia con el estado), **grafo neuronal** 2D o **tu propio modelo GLB/GLTF/VRM**, también de la **biblioteca de avatares 3D gratuitos** (Open Source Avatars, VRM Consortium, Khronos, three.js, Sketchfab). |
| **Mejorador de prompts** | Botón ✨ en chat, imagen y música: reescribe con el LLM (o por reglas) añadiendo estilo, iluminación, composición, calidad, negativo y traducción al inglés; los prompts de imagen de los agentes se mejoran solos. |
| **Editor de temas** | Cambia por completo la estética: 7 plantillas (Neón, Geisha carmesí, Sakura claro, Matrix, Ámbar retro, Minimal, Claro profesional), colores, tipografías, redondeo, brillo, desenfoque y efectos de fondo; exportar/importar JSON. |
| **Drivers** | Detecta versión y antigüedad del driver de la GPU frente a lo que necesitan los runtimes y abre el actualizador **oficial** del fabricante (siempre confirmado por el usuario). |
| **Oficina para pymes** | **59 perfiles** (oficios, comercio, hostelería, salud, despachos, bolsa e inversiones, agro, transporte, industria…) con agentes de oficina y de sector, cada uno con su grafo, y modelos de IA sugeridos para el sector. Ver [docs/OFICINA.md](docs/OFICINA.md). |
| **Móvil y red local** | **Una app Android** con Nexus-Office completo dentro del móvil (NPU, GPU o CPU), que además se conecta con tu PC (Windows, Linux o macOS) por **VPN WireGuard cifrada**, HTTPS anclado o Wi‑Fi. La app VPN ligera sigue para móviles modestos. Acceso desde móvil, tablet o consola protegido con **clave de red**. El PC de la oficina reparte los instaladores a la red (*Sistema → Descargas*). |
| **Bolsa** | Mesa **siempre simulada** con **cotizaciones reales** (Yahoo, CNBC, Google, Stooq, Coinbase, Kraken) y un **agente de bolsa autónomo** con su cartera simulada, diario y gráficos en vivo de sus órdenes. Ver [docs/BOLSA.md](docs/BOLSA.md). |
| **Dispositivos** | Luces **NexusLED** (cualquier ESP32), reloj **NexusWatch** (ESP32-2424S012C) y avatar de bolsillo **NexusPico** (Pico W con la pantalla del pwnagotchi), con modos y grabado del firmware sin internet. Ver [docs/DISPOSITIVOS.md](docs/DISPOSITIVOS.md). |
| **Agentes** | Agentes en YAML totalmente configurables (prompt, modelo, herramientas, memoria, delegación). Enrutado automático y delegación entre agentes. |
| **Salón virtual** | Sala gráfica y **despachos animados** con lo que hace cada agente (personajes mujer u hombre, tú incluido) donde los agentes se reúnen, aprenden qué se le da bien a cada uno, deciden en quién delegar y **proponen nuevos agentes** cuando detectan carencias. |
| **Herramientas** | Sistema modular y **supervisado** (políticas por riesgo, aprobaciones humanas, sandbox, auditoría). Los agentes pueden **proponer herramientas nuevas**, que solo se instalan tras revisión de un administrador. |
| **Hardware** | Detecta CPU, GPU NVIDIA/AMD/Intel/Apple y **NPU** (Intel AI Boost, AMD Ryzen AI, Qualcomm Hexagon, Apple ANE), elige el perfil óptimo o el **perfil de hardware** que tú definas (motor, modelo, capas en GPU, caché en VRAM o RAM), deja **elegir la GPU** cuando hay varias, monitoriza el rendimiento y **reajusta la configuración en cada arranque**. |
| **Modelos** | Descarga **cualquier modelo** de **Hugging Face, Ollama, ModelScope, Civitai y Kaggle** (también repositorios completos, GGUF multiparte y proyector de visión automático; con Ollama instalado usa `ollama pull`), con cada fichero anotado: cuantización, motor compatible, si cabe y si irá lento en tu equipo, y ★ en la opción recomendada. Importación local y lector GGUF. |
| **VPN cifrada** | Túnel WireGuard entre el PC y la app Nexus-Office VPN, con una clave precompartida por móvil. Solo pasa por él el tráfico de la app. El PC se configura solo, o con los comandos de Windows, Linux, macOS o WSL. Ver [VPN](docs/VPN.md). |
| **Acceso remoto** | Desde cualquier lugar por internet con **TLS 1.3** (intercambio post‑cuántico X25519MLKEM768, AES‑GCM o ChaCha20), certificado anclado en la app, dispositivos emparejados con código de un solo uso y revocables, UPnP, detección de CGNAT y **asistente del router** que abre su configuración y da la regla exacta. |
| **Red cooperativa** | **LAN virtual** cifrada (AES‑256‑GCM) entre instancias a través de internet: IPs virtuales `10.77.x.y`, enrutado en malla, chat, inferencia y agentes remotos. |
| **Portabilidad** | Instaladores 100 % portables que se instalan sin internet: CPython completo y autónomo en `venv/`, dependencias y runtimes nativos precompilados (CPU, Vulkan, CUDA, Metal); mover la carpeta = mover la instalación. |
| **Servicio** | Arranque automático al encender el equipo en Windows, Linux, macOS y Android, sin ventana y con reinicio si falla. Ver [Servicio](docs/SERVICIO.md). |

## Instalación

Todos los instaladores llevan dentro Python, las dependencias y el motor de IA (llama-server y sd-cli: CPU, Vulkan y,
según la plataforma, CUDA o Metal) y **se instalan sin internet**. Al instalar, Nexus-Office evalúa el hardware y elige el
modelo de IA adecuado; lo descarga si hay conexión y, si no, lo deja recomendado para *Modelos → Recomendados*.

| Equipo | Fichero |
|---|---|
| Windows 10/11 x64 | `Nexus-Office-Setup-2.2.0-windows-x64.exe` |
| Windows 11 ARM (Snapdragon X) | `Nexus-Office-Setup-2.2.0-windows-arm64.exe` |
| Linux x86_64 | `Nexus-Office-2.2.0-linux-x86_64.run` o `nexus-office_2.2.0_amd64.deb` |
| Linux ARM64, Raspberry Pi 4/5 (sistema de 64 bits) | `Nexus-Office-2.2.0-linux-aarch64.run` o `nexus-office_2.2.0_arm64.deb` |
| macOS Apple Silicon / Intel | `Nexus-Office-2.2.0-macos-arm64.tar.gz` / `Nexus-Office-2.2.0-macos-x86_64.tar.gz` (abre `Nexus-Office.command`) |
| Android: Nexus-Office dentro del móvil y conexión al PC | `Nexus-Office-2.2.0-android-autonomo.apk` |
| Android, cliente del PC de la oficina | `Nexus-Office-2.2.0-android-vpn.apk` |
| Código fuente | `Nexus-Office-2.2.0-codigo-fuente.zip` |

```bash
Nexus-Office-Setup-2.2.0-windows-x64.exe /S /OFICINA=herreria /SERVICIO /RED       # Windows, desatendido
sh Nexus-Office-2.2.0-linux-x86_64.run --oficina=herreria --servicio --red          # Linux y Raspberry Pi
sudo apt install ./nexus-office_2.2.0_amd64.deb && nexus-office                         # Debian, Ubuntu, Raspberry Pi OS
```

El instalador de Windows es gráfico (perfil de oficina, modelo de IA, servicio al encender, acceso desde la red y
acceso directo). No está firmado: si Windows avisa, *Más información → Ejecutar de todas formas*. Paso a paso para
cada sistema en la [Guía de usuario](docs/USUARIO.md#instalación); arranque automático en [Servicio](docs/SERVICIO.md);
detalles técnicos y empaquetado en [Plataformas](docs/PLATAFORMAS.md).

**Desde el código fuente**, `install.bat` (Windows) o `bash install.sh` (Linux, WSL, macOS) descargan el Python
portable y las dependencias como wheels binarias, sin compilar nada; después, `start.bat` o `./start.sh` abren
`http://127.0.0.1:8775`. Admiten las mismas opciones (`--oficina=`, `--servicio`, `--red`, `--modelos=auto|todos|no`;
en Windows `-Oficina`, `-Servicio`, `-Red`, `-Modelos`). En Android con Termux: `bash install-termux.sh`.

Para elegir o descargar el modelo más tarde:
```bash
venv/python.exe -m nexus modelos --descargar       # Windows
venv/bin/python3 -m nexus modelos --descargar      # Linux / macOS
```
Sin modelos Nexus-Office ya funciona con su **núcleo integrado** (herramientas, memoria, imagen compuesta a partir del prompt, música, voz del navegador).

## Rendimiento medido en el equipo de desarrollo

Intel Core i3‑1115G4 (2 núcleos), Intel UHD integrada, 7,8 GB RAM, sin GPU dedicada ni NPU → perfil `cpu-intel-igpu` (ligero):

| Prueba | Resultado |
|---|---|
| Qwen3‑VL‑2B Q4_K_M, generación | ~10–15 tokens/s |
| Visión: describir una imagen | ~4 s |
| SDXS‑512, imagen 512×512 (1 paso) | ~14 s |
| Arranque de `llama-server` | 5–10 s |

Cualquier hardware superior solo mejora: con GPU se activan descarga completa de capas, modelos 4B/8B, SDXL‑Turbo y MusicGen.

## Dependencias y pesos de IA

[`requirements.txt`](requirements.txt) recoge en un solo sitio todo lo que necesita Nexus-Office:

- **Paquetes Python**, con la versión mínima y la probada. pip solo lee estas líneas.
- **Software nativo como referencia comentada:** Python portable (python-build-standalone, CPython 3.11), llama.cpp, stable-diffusion.cpp, los extras de voz y NPU, y las herramientas para compilar la APK.
- **Pesos de IA sugeridos para 11 perfiles de hardware:** CPU básica o potente, NVIDIA de 6, 12 y 24 GB, AMD, Intel Arc, Apple Silicon y Android/Raspberry Pi.

El fichero se genera desde el propio catálogo y la lógica de perfiles. Regenéralo con `venv/python.exe packaging/requirements_ref.py`.

## Documentación

- [Oficina para pymes](docs/OFICINA.md) — perfiles por tipo de empresa (herrería, calderería, reformas…) con agentes de oficina y de gremio, cada uno con su propio grafo.
- [Bolsa e inversiones](docs/BOLSA.md) — oficina de bolsa con mesa simulada: cartera ficticia, órdenes, comisiones, deslizamiento y horarios reales; la cartera real nunca ejecuta nada sin tu confirmación.
- [Guía de usuario](docs/USUARIO.md) — instalación en cada sistema, modelo de IA según tu equipo, arranque automático, descargas para el móvil y uso diario.
- [Servicio](docs/SERVICIO.md) — arranque automático al encender el equipo en cada sistema, comandos, registro y problemas frecuentes.
- [Guía de administrador](docs/ADMINISTRADOR.md) — PIN, configuración avanzada, agentes, herramientas, modelos, autoajuste.
- [Arquitectura](docs/ARQUITECTURA.md) — módulos, flujo de un mensaje, perfiles de hardware.
- [Plataformas e instaladores](docs/PLATAFORMAS.md) — PC, ARM64, móvil, consolas, Docker, empaquetado.
- [Herramientas y supervisión](docs/HERRAMIENTAS.md) — crear herramientas, políticas, propuestas de agentes.
- [Salón virtual](docs/SALON.md) — habilidades aprendidas, delegación y nuevos agentes.
- [Memoria y gestor de conocimiento](docs/MEMORIA.md) — grafo común y memoria de cada agente, importar grafos, leer un ZIP, embeddings.
- [Importar grafos como conocimiento](docs/GRAFOS.md) — más de 50 formatos, desde la app, la terminal o el chat.
- [Biblioteca RAG y URL→PDF](docs/RAG.md) — documentos, búsqueda, PDF estructurado y asimilación.
- [Avatares](docs/AVATAR.md) — geisha 3D, grafo, modelos propios y referencias para mejorarlo.
- [VPN cifrada](docs/VPN.md) — app Nexus-Office VPN y túnel WireGuard en Windows, Linux y macOS.
- [App Android autónoma](android-autonomo/README.md) — Nexus-Office dentro del móvil y cómo compilar su APK.
- [Acceso remoto](docs/ACCESO_REMOTO.md) — usar Nexus-Office desde fuera de casa: cifrado, emparejamiento, router y túneles.
- [Red cooperativa](docs/RED_COOPERATIVA.md) — LAN virtual por internet, seguridad, NAT.
- [API](docs/API.md) — REST y WebSocket.

## Pruebas
```bash
venv/python.exe -m pytest -q        # 203 pruebas: hardware, memoria, grafo común y memoria por agente, gestor de conocimiento, embeddings, RAG/PDF, importación (ZIP/RAR/7z/TAR, formatos), rastreo paralelo, herramientas, multimedia, modelos, drivers, red local, acceso remoto TLS, VPN, apps Android (DNS, CPU, modelos por RAM), agentes, API, interfaz adaptativa, empaquetado
```

## Licencia
MIT (ver [LICENSE](LICENSE)). Los modelos descargados tienen sus propias licencias: revísalas en su repositorio de origen.
