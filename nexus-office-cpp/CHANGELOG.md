# Cambios

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
- **Ahora**, con conexión, son cotizaciones reales (Yahoo Finance, con Google Finance, Stooq, Coinbase, Kraken y el BCE
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
