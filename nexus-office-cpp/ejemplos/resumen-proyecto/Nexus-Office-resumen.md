# Nexus-Office: resumen del proyecto

Documento pensado para la **biblioteca (RAG)** de Nexus-Office: súbelo en *Biblioteca → Subir* y los agentes podrán
responder preguntas sobre el propio programa («¿cómo instalo el servicio?», «¿qué hace el agente Profesor?», «¿qué
formatos de grafo admite?»). Cada sección se entiende sola, para que cada fragmento que encuentre la búsqueda tenga
sentido por sí mismo.

## Qué es Nexus-Office

Nexus-Office es una **oficina de inteligencia artificial local para pymes y autónomos**. Funciona entera en el equipo
del usuario, sin internet salvo para descargar los modelos de IA, y sin enviar datos a ningún servidor. Reúne un equipo
de agentes especialistas (un asistente principal, un investigador, un programador, un artista, un forjador de
herramientas y un profesor), una memoria a largo plazo con grafo de conocimiento, una biblioteca de documentos con
búsqueda y citas, voz, imagen, música, una mesa de bolsa simulada y perfiles de oficina para más de 130 oficios.

Lo desarrolla Joseba Muro. El código está en GitHub, en el repositorio público JosebaMuro/Nexus-AI. La oficina para pymes
se llama Nexus-Office desde la versión 1.7.0; NexusAI es la IA para particulares del mismo autor.

## Las dos ediciones: Python y C++

Nexus-Office tiene dos ediciones con la misma interfaz, la misma API, los mismos agentes y el mismo formato de datos:

- **Edición Python** (`nexus-office/`): la original. Lleva dentro su propio intérprete de Python y unos 60 paquetes.
- **Edición C++** (`nexus-office-cpp/`): la misma aplicación reescrita en C++20. Es un único ejecutable nativo de unos
  15 MB que arranca en 0,13 s y usa 68 MB de memoria en reposo (la edición Python: 1,2 s y 119 MB en el mismo equipo).
  Desde la versión 2.1 tiene **todas las funciones** de la edición Python: mesa de bolsa, red COOP, VPN, acceso remoto,
  importar grafos de cualquier formato, rastreo web, guardar una URL como PDF, edición de imágenes con GIMP, el agente
  Profesor con IA online, la APK Android con GPU y NPU y el autoajuste en vivo.

Las dos se comparan automáticamente: más de 100 flujos completos de la API dan la misma respuesta en las dos ediciones.
Se instalan por separado y no se pisan; para pasar los datos de una a otra basta copiar la carpeta de datos con las dos
cerradas. Las dos usan el puerto 8775, así que solo una puede estar en marcha a la vez.

## Plataformas e instaladores

Todos los instaladores son **100 % offline**: llevan dentro el programa y los motores de IA (llama-server de llama.cpp,
stable-diffusion.cpp, Piper para la voz y Whisper para el dictado) en sus variantes de CPU, Vulkan, CUDA y Metal.

| Equipo | Instalador |
|---|---|
| Windows 10/11 de 64 bits | `.exe` (instalación normal o desatendida con `/S`) |
| Windows 11 ARM (Snapdragon X) | `.exe` para ARM64 |
| Linux x86_64 | `.run`, `.deb`, AppImage y Flatpak |
| Linux ARM64 y Raspberry Pi 4/5 (64 bits) | `.run`, `.deb`, AppImage y Flatpak para ARM64 |
| macOS Apple Silicon e Intel | `.tar.gz` con `Nexus-Office.command` |
| Android (Nexus-Office dentro del móvil) | APK autónoma, 100 % local |
| Android (cliente del PC de la oficina) | APK ligera (VPN) |
| Termux en Android | paquete para Termux, que se descarga desde *Sistema → Descargas* del PC |
| Código fuente | zip limpio |

Al instalar, Nexus-Office **evalúa el hardware** (RAM, tarjeta gráfica, procesador, NPU) y elige el modelo de IA que
va con fluidez en ese equipo. Si hay conexión lo descarga; si no, lo deja recomendado en *Modelos → Recomendados*.
Cualquier instalación se puede dejar como **servicio** del sistema: arranca al encender el equipo, sin ventana, y se
reinicia si falla (systemd en Linux, launchd en macOS, el Programador de tareas en Windows, Termux en Android).

Órdenes de instalación desatendida:

```
Nexus-Office-Setup-<versión>-windows-x64.exe /S /OFICINA=herreria /SERVICIO /RED
sh Nexus-Office-<versión>-linux-x86_64.run --oficina=herreria --servicio --red
sudo apt install ./nexus-office_<versión>_amd64.deb
```

## Modelos de IA según el equipo

Nexus-Office usa modelos abiertos que corren en local con llama.cpp:

- **Chat, visión y razonamiento**: la familia Qwen3.5 (0.8B, 2B, 4B y mayores según la memoria y la GPU). Ven imágenes
  y razonan paso a paso antes de responder.
- **Documentos y memoria**: Qwen3-Embedding 0.6B para la búsqueda semántica.
- **Voz**: Piper con voz española; dictado con Whisper.
- **Imagen**: SDXS-512 en CPU, SD-Turbo o SDXL-Turbo con más memoria o GPU.
- **Música**: compositor procedural propio (WAV y MIDI).

Sin modelo descargado, Nexus-Office funciona igualmente con su **núcleo integrado**: herramientas como la calculadora,
memoria, imagen compuesta, música y la voz del sistema.

Los modelos se descargan de Hugging Face, Ollama, ModelScope, Civitai o Kaggle, o se importan desde un fichero local.

## CPU, GPU y NPU

Al abrir Nexus-Office se puede elegir el motor y el acelerador: **CPU**, **GPU** o **NPU**. Nexus-Office prueba la
opción y muestra dónde ha quedado el modelo de verdad.

- **Escritorio**: CUDA en NVIDIA, Vulkan en AMD e Intel, Metal en Mac, y la NPU de Intel con OpenVINO.
- **Android**: la app trae llama.cpp con Vulkan, OpenCL y el backend Hexagon. En los Snapdragon 8 Gen 2 o posteriores
  el modelo puede ir en la **NPU Hexagon**; en Adreno se usa OpenCL; en Mali, Immortalis, Xclipse y PowerVR, Vulkan.
  Si la NPU falla al arrancar se pasa a la GPU, y si falla la GPU, a la CPU; el fallo queda apuntado hasta la siguiente
  versión de la app o de Android.
- **Autoajuste**: en cada arranque Nexus-Office revisa el rendimiento del arranque anterior y ajusta hilos, contexto y
  capas en la GPU. En Android, además, mide la velocidad con las respuestas reales: compara GPU y CPU, prueba varios
  números de hilos y se queda con lo más rápido, usa menos hilos si el móvil se calienta o tiene poca batería, reduce
  el contexto si falta memoria y propone un modelo más pequeño si va lento o uno mayor si sobra velocidad.

## Los agentes

| Agente | Qué hace |
|---|---|
| **Nexus-Office** | Asistente principal multimodal; coordina al resto de agentes y decide a quién encargar cada tarea. |
| **Investigador** | Busca y contrasta información en internet y en la memoria. |
| **Programador** | Escribe, ejecuta y depura código Python en un espacio aislado (sandbox). |
| **Artista** | Crea imágenes y música a partir de ideas; edita fotos como en GIMP. |
| **Forjador** | Diseña herramientas nuevas y las propone al repositorio; un administrador las revisa antes de activarlas. |
| **Profesor** | Enseña y resuelve dudas con una IA online (Gemini, ChatGPT, Perplexity, Claude...) o con el modelo local. |

En modo *Automático*, Nexus-Office elige el agente por el mensaje. Los agentes pueden delegar subtareas entre ellos, se
reúnen en el **Salón** (donde se ve cómo conversan y se reparten el trabajo) y cada uno tiene su propio grafo de
conocimiento de su especialidad además del grafo general.

## El agente Profesor y las IA online

El **Profesor** es el único agente que puede usar una **IA online**. Un administrador elige el proveedor y pone su
clave en *Ajustes → Profesor*:

- Google Gemini, ChatGPT (OpenAI), Perplexity, Claude (Anthropic), Mistral, DeepSeek, Groq, OpenRouter y cualquier
  servicio compatible con la API de OpenAI.
- Con Claude se usa el modelo `claude-opus-5-5` con pensamiento adaptativo; con Perplexity, sus respuestas traen las
  fuentes al final.
- Las claves son secretas: nunca se devuelven por la API ni se muestran (aparecen como `********`).
- Sin clave, el Profesor responde con el modelo local. La IA online **nunca** se usa como respaldo automático de los
  demás agentes: solo cuando se pregunta al Profesor.

Los demás agentes le consultan con la herramienta `consultar_profesor` cuando no saben resolver bien una duda de
conocimiento, y **guardan la lección** en su memoria para la próxima vez: así el aprendizaje se queda en local.

## Memoria, grafo y aprendizaje

- **Memoria a largo plazo**: hechos, preferencias y pautas del usuario («llámame Joseba», «no uses emojis»), que todos
  los agentes respetan. Se busca por significado (embeddings) y por palabras.
- **Grafo de conocimiento**: personas, lugares, empresas, conceptos y sus relaciones. Hay un grafo general y uno por
  agente. Se explora en 3D en la vista *Núcleo*.
- **Aprendizaje**: Nexus-Office aprende de cada conversación (las líneas verdes «⟡ aprendido»), de los 👍 y 👎 de cada
  respuesta (ejemplos, lecciones y longitud preferida) y de las lecciones del Profesor. Periódicamente consolida la
  memoria, une duplicados y deja caer lo que no se usa.
- **Gestor de conocimiento** (modo administrador): ver, editar, mover entre agentes, fusionar y reorganizar lo que sabe
  cada uno.

## Importar grafos de conocimiento

Nexus-Office importa como conocimiento un grafo de **cualquier formato**, reconocido por su contenido: JSON node-link
(D3, NetworkX, vis.js), Cytoscape, Neo4j (exportaciones, Cypher y CSV de `neo4j-admin`), JSON-LD, GraphSON, RDF y OWL
(Turtle, N-Triples, RDF/XML...), GraphML, GEXF, GML, Graphviz DOT, Pajek, GDF, UCINET DL, tablas CSV, TSV, Excel, ODS y
SQLite, listas de aristas, Mermaid, PlantUML, mapas mentales (FreeMind, XMind, OPML), notas de Obsidian, Logseq y Foam,
YAML, TOML y comprimidos con cualquiera de ellos. Lo importado se fusiona con lo que ya sabe: los nodos con el mismo
nombre se unen y las relaciones repetidas se refuerzan.

Se importa desde *Ajustes → Gestor de conocimiento → Importar / exportar*, pidiéndoselo a un agente en el chat
(herramienta `graph_import`) o desde la terminal: `nexus-office grafo importar fichero --en auto`.

Como ejemplo, los paquetes traen un **grafo multicapa de la Wikipedia en español** (más de 2.000 nodos y 7.000
relaciones en cuatro capas: temas, artículos, enlaces entre artículos y hechos de Wikidata).

## Biblioteca de documentos (RAG)

La biblioteca lee PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML, Markdown, texto, código y archivos
comprimidos (ZIP, TAR, 7z y RAR). Al preguntar, los agentes buscan en ella (búsqueda híbrida por significado y por
palabras) y citan la fuente. También puede **rastrear un sitio web de documentación** entero, guardar una página web
como **PDF maquetado** (portada, resumen, índice y secciones) y leer un ZIP completo, importando los grafos que traiga.

## Herramientas de los agentes

Los agentes usan herramientas **supervisadas**: las de riesgo piden aprobación con una tarjeta *APROBACIÓN REQUERIDA* y
todo queda en un registro de auditoría. Entre ellas:

- Cálculo y fecha: `calculator`, `datetime_now`.
- Memoria y conocimiento: `memory_search`, `memory_save`, `knowledge_graph`, `graph_import`.
- Biblioteca y web: `rag_search`, `rag_list`, `web_search`, `web_fetch`, `crawl_docs`, `url_to_pdf`.
- Ficheros y código: `file_list`, `file_read`, `file_write`, `python_exec` (en el sandbox).
- Multimedia: `generate_image`, `compose_music`, `enhance_prompt`, `imagen_editar`, `imagen_info`, `gimp_estado`,
  `gimp_script`.
- Agentes: `delegate`, `list_agents`, `consultar_profesor`, `propose_tool`.
- Bolsa: `bolsa_cotizacion`, `bolsa_orden`, `bolsa_cartera`, `bolsa_cancelar`, `bolsa_costes`, `bolsa_mercados`,
  `bolsa_informe_fiscal`.
- Sistema: `system_info`.

Las herramientas nuevas que propone el Forjador quedan pendientes hasta que un administrador las aprueba.

## Edición de imágenes con GIMP

Los agentes editan fotos de la carpeta de trabajo como en GIMP: recortar, redimensionar, rotar, ajustar brillo,
contraste y color, enfocar, desenfocar, quitar el fondo, añadir texto o marca de agua, convertir de formato y aplicar
recetas (foto de producto, perfil para redes, documento escaneado...). Si GIMP está instalado o incluido lo usa, con
scripts de Script-Fu o Python-Fu; si no, lo hace el motor de imágenes propio. El original nunca se pisa.

## Mesa de bolsa

El agente de bolsa trabaja **siempre en simulación**: ninguna orden llega a un bróker ni mueve dinero real. Opera con
una cartera simulada (10.000 € al empezar), respeta el horario y los festivos de cada bolsa, las órdenes a
mercado, limitadas y stop, las comisiones, el impuesto sobre transacciones financieras, el cambio de divisa y el
deslizamiento, y lleva el histórico de inversiones y resultados con el informe fiscal (FIFO y la regla de los dos
meses). Una cartera marcada como *real* solo sirve para llevar a mano el seguimiento de lo que el usuario hace en su
bróker: nada se ejecuta sin que escriba `CONFIRMO` y Nexus-Office no se conecta a ningún bróker. Nada de esto es
asesoramiento financiero.

## Perfiles de oficina

Al instalar, o después con `nexus-office oficina <perfil>`, se configura la oficina de una pyme concreta: más de 130
oficios (carpintería, herrería, clínica dental, farmacia, restaurante, inmobiliaria, asesoría y gestoría, construcción,
agricultura, bolsa e inversiones...). Cada perfil trae sus agentes especializados, su vocabulario, paquetes de
conocimiento del oficio y ejemplos.

## Red COOP, VPN y acceso remoto

- **Red local**: con el servicio *accesible desde la red local*, los móviles, tablets y otros PC abren
  `http://IP-DEL-PC:8775` e introducen la **clave de red**. La interfaz se instala como app (PWA).
- **Red COOP**: varias instalaciones de Nexus-Office forman una red cooperativa: se reparten el trabajo, comparten
  agentes y usan el modelo del equipo más potente. Funciona entre las ediciones Python y C++.
- **VPN WireGuard** y **acceso remoto por internet** con TLS 1.3, para usar la oficina desde fuera con la app
  Nexus-Office VPN.
- **Descargas**: el PC de la oficina reparte los instaladores a los demás equipos desde *Sistema → Descargas*.

## Interfaz

Vistas: **Chat** (con adjuntos, dictado, lectura en voz alta y modo en vivo por voz manos libres), **Salón** (los
agentes reunidos), **Núcleo** (el grafo en 3D), **Estudio** (imagen y música), **Bolsa**, **Agentes**, **Modelos**,
**Herramientas**, **Sistema** y **Ajustes**. Hay un modo usuario sencillo y un modo administrador con PIN. El avatar
cambia de color según su estado: cian en línea, violeta pensando, ámbar usando una herramienta, magenta hablando y verde
escuchando.

## Línea de órdenes

```
nexus-office run [--host 0.0.0.0] [--port 8775] [--acelerador auto|cpu|gpu|npu]
nexus-office detect                                  # hardware y perfil
nexus-office modelos [--descargar] [--todos]         # elige y descarga el modelo de cada tipo
nexus-office servicio instalar|quitar|estado [--red]
nexus-office oficina <perfil>                        # configura la oficina de un oficio
nexus-office grafo formatos|importar|exportar [ficheros] [--en auto|general|<agente>]
nexus-office bolsa cartera|mercados|cotizar|comprar|vender|fiscal ...
nexus-office setup                                   # comprueba los runtimes del paquete
nexus-office cleanup [--dry-run] [--yes]             # quita lo que este equipo no usa
nexus-office chat [--agent A]                        # chat en la terminal
```

En la edición Python, las mismas órdenes con `python -m nexus`.

## Privacidad y seguridad

- Todo se queda en el equipo: modelos, memoria, documentos y conversaciones.
- Solo salen a internet las búsquedas web que pida el usuario, la descarga de modelos y, si un administrador lo activa,
  las preguntas al Profesor con IA online.
- Las claves (IA online, red, VPN) se guardan como secretos y nunca se muestran.
- Las herramientas de riesgo piden aprobación y el código se ejecuta en un sandbox.

## Preguntas frecuentes

**¿Necesita internet?** No. Solo para descargar los modelos (una vez), para buscar en la web y para el Profesor online.

**¿Qué equipo necesita?** Cualquier PC con 4 GB de RAM sirve con el modelo pequeño. Con 8-16 GB o una GPU, modelos
mayores y respuestas más rápidas. En Android, un móvil de 64 bits con Android 9 o superior.

**¿Cómo uso la GPU o la NPU?** Elige el acelerador al abrir Nexus-Office o en *Sistema → Motor y acelerador*; con
*Automático* lo decide él. En el móvil lo decide el autoajuste midiendo la velocidad real.

**¿Cómo conecto ChatGPT, Gemini o Claude?** En modo administrador, *Ajustes → Profesor*: elige el proveedor, pega la clave
y pulsa *Probar*. Solo el agente Profesor la usa.

**¿Puedo invertir dinero real con la mesa de bolsa?** No. Es siempre una simulación con fines de práctica.

**¿Cómo paso mis datos de la edición Python a la C++?** Cierra las dos y copia la carpeta de datos de una a la otra: el
formato es el mismo.
