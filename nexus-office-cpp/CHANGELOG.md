# Cambios

## Nexus-Office 2.7.0-cpp

### IA especializada para la bolsa
- Tres modelos financieros GGUF en el catálogo de la oficina de bolsa, elegidos según el equipo: Finanzas 1.5B
  (móvil), Fin-R1 7B (PC) y Qwen3.8 27B Finanzas con visión (GPU de 24 GB).
- Los agentes de la mesa usan `model: especialidad:finanzas`: la IA financiera instalada que mejor va o, si no hay,
  la general (`ia_especializada.finanzas`: auto, un modelo o no).
- El agente autónomo pide a esa IA que revise cada compra (comprar o esperar, con su motivo en el diario); se apaga
  con «La IA financiera revisa cada compra» o `bolsa.agente.ia = false`.

## Nexus-Office 2.6.0-cpp

### Velocidad con GPU dedicada y con NPU
- llama-server arranca con `-ngl auto`: llama.cpp pone en la VRAM todas las capas que caben con el contexto. Con 999
  fijo, un modelo que no cabía hacía que CUDA fallase y todo fuera a la CPU (Linux) o que la VRAM se desbordase a
  la RAM compartida (Windows). Con un llama-server antiguo se vuelve a 999 solo.
- El autoajuste ya no deja el modelo en 28 o 24 capas para siempre tras un pico de VRAM, descarta ese valor si lo
  guardó una versión anterior y no agranda el contexto si la VRAM va llena.
- Si la GPU no arranca, el paso a la CPU se recuerda en la sesión (antes se recargaba el modelo dos veces en cada
  respuesta).
- Móvil: un solo slot de llama-server con GPU o NPU (todo el contexto para una conversación) y NPU Hexagon usable
  aunque la sonda no dé nombre de GPU.
- ARM: se cuentan todos los núcleos (un Snapdragon X de 12 núcleos se veía como de 4).

### Capas en la GPU / NPU
- Nueva opción en *MOTOR Y CPU/GPU/NPU* (y en *Ajustes → Administración*): automático, todas, ninguna o un número de
  capas. Se guarda en `hardware.capas` y la respeta también el autoajuste del móvil.

## Nexus-Office 2.5.0-cpp

### Motores para cada acelerador
- **LiteRT-LM** (Google): modelos `.litertlm` en CPU, GPU y NPU (Google Tensor, Snapdragon 8 Elite, Dimensity, Intel).
  Va también en la APK autónoma.
- **ONNX Runtime GenAI** (Microsoft): modelos ONNX con DirectML (cualquier GPU en Windows), QNN (NPU de los
  Snapdragon X en Windows ARM), OpenVINO, CUDA y CPU.
- Cada motor corre en un proceso aparte y prueba NPU → GPU → CPU; se elige solo por el formato del modelo.

### Runtimes por fabricante
- llama.cpp con **CUDA 13** para las RTX 50, **ROCm** (AMD), **SYCL** (Intel Arc), **OpenCL Adreno** (Snapdragon X
  en Windows) y el paquete **Snapdragon** de Linux ARM (CPU, Adreno y Hexagon). Los nombres de las Releases oficiales
  de 2026 ya se reconocen al descargar.
- Las variantes pesadas van en paquetes aparte de la Release (`Nexus-Office-runtime-*.zip`) que se importan sin
  conexión.
- Las APU de AMD con Radeon 680M/780M/890M o Strix Halo usan Vulkan con 16 GB o más.

### Imagen con GGUF
- Los GGUF de **Flux, Z-Image, SD3, Qwen-Image, Chroma**... se reconocen como modelos de imagen. Si son solo el
  modelo de difusión, Nexus-Office busca sus piezas (VAE, clip_l, clip_g, T5-XXL o el LLM codificador) y dice cuál
  falta. En *Modelos* hay **Z-Image-Turbo** y **FLUX.1-schnell** en GGUF que se descargan con todas sus piezas.
- **sd-server** mantiene el modelo cargado entre imágenes y stable-diffusion.cpp se actualiza en los instaladores.
- Ajustes según la VRAM: atención rápida, codificadores en la CPU, pesos por partes y VAE por teselas.
- Si la GPU no consigue generar la imagen (por ejemplo, Metal en una máquina virtual), se reintenta en la CPU
  antes de recurrir a la imagen compuesta sin IA.

### Rendimiento
- Caché KV cuantizada (q8_0) cuando el modelo va justo de memoria, con vuelta atrás si la build no lo admite.
- Hilos de lectura del prompt aparte de los de generación.

## Nexus-Office 2.4.0-cpp

### Conversaciones largas sin perder el hilo
- Cada conversación tiene un hilo: resumen progresivo de lo antiguo, notas fijas del proyecto y recuperación de los
  mensajes anteriores que vienen al caso. Antes solo se veían los 12 últimos mensajes y lo demás se perdía.
- El contexto se reparte en tokens según el `n_ctx` real del modelo y del equipo, no con un número fijo de mensajes.
- La petición actual ya no se puede descartar al ajustar el contexto (antes, con resultados de herramientas largos,
  podía quedarse fuera la propia pregunta).
- Con GPU, llama-server reparte el contexto entre dos conversaciones a la vez: el reparto usa ya lo que le toca a cada
  una (antes se contaba el doble y el servidor rechazaba la petición). Las definiciones de herramientas también cuentan,
  y si aun así el servidor dice que no cabe, se reajusta con sus cifras y se reintenta.
- Panel **🧵 HILO** en el chat y API `/api/sessions/{id}/hilo`. Las conversaciones se abren enteras (hasta 1000 mensajes).

## Nexus-Office 2.3.0-cpp

### Todo el proyecto en C++
- La edición C++ ya no depende de la carpeta de la edición Python: la interfaz web, los perfiles de oficina, los
  agentes, los paquetes de conocimiento y las tablas de datos están en `recursos/`, y la app Android en `android/`.
- **nexus-construir**, la herramienta de construcción, está en C++ y sustituye a todos los scripts de Python:
  instaladores (.exe, .run, .deb, AppImage, Flatpak y macOS), código fuente y sumas, APK autónoma, manuales en PDF,
  prueba de humo con modelos reales, grafo de ejemplo de la Wikipedia, tablas de datos y el avatar 3D a partir de su
  hoja de personaje. Ninguna Action de la edición C++ usa ya Python.
- El firmware del Pico W (NexusPico) está en C++ con el Pico SDK y la herramienta `flash.py` desaparece: todas las
  placas se graban con `nexus-office firmware` o desde *Dispositivos*.
- La edición Python queda congelada: no recibe cambios nuevos.

### Documentación
- La documentación técnica y el manual describen la edición C++: órdenes `nexus-office`, carpetas e identificadores
  propios (`Nexus-Office-C`, `nexus-office-c`), instaladores sin Python, servicio con `nexus-office-w.exe` en Windows,
  arquitectura por módulos de `src/` y empaquetado con `nexus-construir`.

### Correcciones
- Los paquetes ya no llevan las cabeceras ni las bibliotecas de mbedTLS (unos 4 MB menos por instalador).
- Las peticiones HTTP que responden 204 (sin contenido) ya no se toman por un fallo de conexión.
- La APK autónoma lleva un versionCode que sube con cada versión, así Android la instala encima de la anterior.
- Prueba nueva: los 55 ejemplos de formatos de grafo se leen en todas las plataformas.

## Nexus-Office 2.2.0-cpp

### Todo en C++
- Desde esta versión todo lo nuevo de Nexus-Office se hace en la edición C++.

### Comprobación y autorreparación al arrancar
- En cada arranque se revisan la configuración, la base de datos, los modelos, el motor de IA, el puerto, las carpetas,
  el disco, los registros y los drivers de las placas, y se arregla solo lo que se pueda: configuración desde la última
  buena, base de datos rescatada tabla a tabla con la copia diaria (`data/copias`, las tres últimas), GGUF dañados
  apartados, llama-server con permiso de ejecución, sin cuarentena en macOS y de la GPU a la CPU si la GPU no arranca, y
  el siguiente puerto libre si el 8775 está ocupado.
- Panel en *Sistema → Comprobación y autorreparación*, `/api/salud` y orden `nexus-office comprobar`.

### Más rápido y más pequeño
- Optimización en el enlace (LTO) en Windows, Linux y macOS: el ejecutable ocupa un 19 % menos.
- La revisión completa de la base de datos solo tras un cierre brusco y una vez al día; el resto de arranques, la rápida.

### Toda la familia ESP y drivers USB-serie
- *Dispositivos > Grabar firmware* reconoce y graba toda la familia ESP con Wi-Fi: ESP8266 (ESP-01, NodeMCU, Wemos
  D1 mini), ESP32, S2, S3, C3, C5 y C6. El C2 se reconoce y graba cualquier .bin, pero aún no hay NexusLED para él;
  el H2 y el P4 se reconocen, aunque no tienen Wi-Fi propio.
- Detecta los puentes CH340/CH9102, CP210x y FTDI y si les falta driver; **INSTALAR DRIVERS** (o
  `nexus-office firmware drivers --instalar`) lo instala sin preguntar: pnputil en Windows con los drivers del paquete,
  el instalador en macOS y, en Linux, módulo del kernel, regla udev, grupo `dialout` y `brltty` apartado.
- Orden nueva `nexus-office firmware` para ver placas, puertos y drivers y grabar desde la terminal.
- Grabador nativo **nexus-flash** dentro de Nexus-Office: habla directamente con la ROM de Espressif, verifica por MD5
  y reinicia la placa. Ya no hace falta esptool ni Python (`NEXUS_GRABADOR=esptool` lo sigue usando si se quiere).
- ESP8266 con imagen por tamaño de flash: 1 MB (ESP-01S y mayores), 4 MB y 512 KB (ESP-01 antiguo).

## Nexus-Office 2.1.0-cpp

### La edición C++, completa
- **Antes**: la 2.0 beta C dejaba fuera la mesa de bolsa, la red COOP, la VPN, el acceso remoto, la importación de
  grafos en otros formatos, el rastreo web, guardar una URL como PDF, las herramientas de GIMP, varias órdenes de la
  terminal y la GPU y la NPU en la APK.
- **Ahora** todo eso está en C++ con las mismas respuestas que la edición Python: 108 flujos más de la API comparados
  (bolsa, grafos, web, red, VPN y GIMP), 55 formatos de grafo leídos igual y 73 ediciones de imagen comparadas píxel a
  píxel. `/api/tools` lista las mismas herramientas en las dos ediciones.

### Agente Profesor con IA online
- Nuevo agente **Profesor** que enseña y resuelve dudas con una IA online: Google Gemini, ChatGPT, Perplexity, Claude,
  Mistral, DeepSeek, Groq, OpenRouter u otra compatible con OpenAI. Con Claude usa `claude-opus-5-5` con pensamiento
  adaptativo; con Perplexity añade las fuentes.
- Un administrador pone la clave en *Ajustes → Profesor* (o `PUT /api/nube`) y la prueba con `POST /api/nube/probar`.
  Las claves son secretas: nunca se devuelven.
- Sin clave responde con el modelo local. La IA online nunca se usa como respaldo automático de los demás agentes.
- Los agentes le preguntan con `consultar_profesor` y guardan la lección en su memoria: lo aprendido se queda en local.

### Android: GPU, NPU y autoajuste en vivo
- La APK autónoma C++ usa la **NPU Hexagon** (Snapdragon 8 Gen 2 o posteriores), la GPU (OpenCL en Adreno, Vulkan en
  el resto) o la CPU. Si la NPU no arranca se pasa a la GPU, y si falla la GPU, a la CPU.
- Autoajuste en vivo: mide la velocidad real con GPU y con CPU y se queda con lo más rápido, prueba varios números de
  hilos, usa menos con el móvil caliente o con poca batería, reduce el contexto si falta memoria y propone otro modelo
  si va lento o sobra velocidad. Tras cada arranque limpia temporales, optimiza la base de datos y precarga el modelo.

### Bolsa con datos reales y agente autónomo
- **Antes**: los precios de la mesa eran ficticios.
- **Ahora**, con conexión, son cotizaciones reales (Yahoo Finance, con CNBC, Google Finance, Stooq, Coinbase, Kraken y el BCE
  de respaldo) y el cambio de divisa es el real. Sin conexión la mesa sigue simulada desde el último precio real y con
  el histórico diario guardado. Sigue siendo siempre una simulación: nunca hay dinero ni órdenes reales.
- Nuevo **agente de bolsa autónomo** con su propia cartera simulada: analiza, arriesga como mucho el 1 % por
  operación, pone stops vivos que suben con el precio, vende en el objetivo y aprende de sus resultados. Diario, curva
  de patrimonio y gráficos de cada orden en vivo.

### Dispositivos
- Luces NexusLED, reloj NexusWatch y avatar NexusPico para el Pico W: estado de Nexus en directo, efectos, brillo y
  modos preconfigurados que se cambian desde el ordenador, el móvil o el reloj, que además muestra qué hace cada
  agente. Las imágenes de tinta del Pico W se generan en C++.
- Grabar el firmware por USB sin internet desde *Dispositivos > Grabar firmware*: los binarios y esptool vienen en el
  paquete, con puertos serie nativos en Windows, Linux y macOS.

### Perfiles de hardware y elección de GPU
- Perfiles con nombre (Automático, Rendimiento, Equilibrado, Modelo grande, Ahorro, Solo CPU y los tuyos) que juntan
  motor, acelerador, GPU, modelo, capas en la VRAM, caché de contexto en VRAM o RAM, `--mlock`, contexto, hilos y lote.
  Se eligen en la barra superior, en Sistema o con los modos de los dispositivos (`/api/hw/perfiles`).
- Con varias GPU, llama-server usa solo una: la elegida (`POST /api/hw/gpu`) o la dedicada con más memoria. Se leen
  sus dispositivos con `--list-devices` y nunca se usa un renderizador por software (llvmpipe). Si la GPU elegida no
  arranca, el modelo sigue en la CPU y la causa se ve en `/api/accel`.
- Bolsa: velas de CNBC (y de Coinbase o Kraken para cripto) cuando Yahoo limita, y cierre anterior deducido de la variación si falta.

### Avatares 3D gratuitos
- Biblioteca de avatares en *Ajustes > Avatar*: busca y descarga avatares VRM de Open Source Avatars, modelos de
  referencia (VRM Consortium, Khronos, three.js) y modelos CC de Sketchfab. Si una pasarela IPFS falla prueba otra, solo
  descarga URLs del catálogo y lo descargado sirve sin conexión (`/api/avatares`).

### RAG que lee páginas como un navegador

- Las webs que se pintan con JavaScript (React, Vue, Angular, tiendas...) se leen con Chrome, Edge, Chromium o Brave sin
  interfaz si hay uno en el equipo; si no, todo sigue con la descarga normal. Lo usan el rastreador, «URL a PDF» y la
  herramienta web de los agentes (`rag.browser`: auto, siempre o nunca; `rag.browser_path` para elegir uno).
- El navegador sale por un proxy local que solo deja direcciones públicas (salvo `rag.allow_private_urls`), con un
  perfil propio limpio en cada lectura. API nueva `GET /api/rag/navegador`.

### Personajes con sexo elegible

- Cada agente (`sexo` en su ficha) y el propio usuario (`user.sexo`) pueden ser mujer u hombre; despachos y sala de
  descanso lo respetan.

### Ejemplos para importar
- **Grafo multicapa de la Wikipedia en español** (`ejemplos/grafo-wikipedia-es`): 2.251 nodos y 7.007 relaciones en
  cuatro capas (temas, artículos, enlaces entre artículos y hechos de Wikidata), en GraphML y JSON. Se importa con
  `nexus-office grafo importar ejemplos/grafo-wikipedia-es/grafo.graphml --en auto`.
- **Resumen del proyecto** (`ejemplos/resumen-proyecto/Nexus-Office-resumen.md`) para subirlo a la biblioteca: los
  agentes responden preguntas sobre el propio Nexus-Office.

### Terminal
- Órdenes nuevas en C++: `bolsa`, `grafo`, `setup`, `wheels` y `cleanup` (quita del paquete offline las variantes de
  runtime que el equipo no usa, con `--dry-run` y `--yes`).

## Nexus-Office 2.0.0-beta-cpp (2.0 beta C)

### Nexus-Office reescrito en C++
- **Antes**: Nexus-Office era una aplicación Python que llevaba dentro su propio intérprete y unos 60 paquetes; cada
  instalador ocupaba entre 0,5 y 1,7 GB y tardaba en arrancar.
- **Ahora** hay una edición C++ con la misma interfaz, la misma API, los mismos perfiles, agentes y datos: un único
  programa nativo de unos 15 MB que arranca en 0,13 s y usa 68 MB de memoria en reposo (la edición Python, 1,2 s y
  119 MB en el mismo equipo). Los modelos de IA y los motores (llama-server y stable-diffusion.cpp) son los mismos.
- Las dos ediciones se comparan automáticamente: 39 rutas de la API y 41 flujos completos (administrador, memoria,
  agentes, biblioteca, multimedia, grafo, oficina y chat) dan la misma respuesta.
- Instaladores 100 % offline para Windows (x64 y ARM64), Linux (x86_64 y ARM64: .run, .deb, AppImage y Flatpak),
  macOS (Apple Silicon e Intel) y Android, con servicio del sistema y elección del modelo de IA según el hardware. Se
  instala junto a la edición Python sin pisarla.
- Es una beta: la mesa de bolsa, la red COOP, la VPN, el acceso remoto, importar grafos en otros formatos, el rastreo
  web y las herramientas de GIMP siguen solo en la edición Python. Ver [docs/CPP.md](docs/CPP.md).
