# Cambios

## Nexus-Office 2.2.0-cpp

### Todo en C++
- Desde esta versión todo lo nuevo de Nexus-Office se hace en la edición C++.

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
