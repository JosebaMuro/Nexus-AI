# Novedades de la 4.1

La 4.1 trae a la edición Python lo que solo tenía la edición C++ 3.x. Todo funciona sin internet salvo donde se
indica, y nada de ello envía datos fuera del equipo.

## Conversaciones largas: el hilo

En una charla larga el modelo local no puede leerlo todo en cada turno. Ahora cada conversación guarda:

- **Notas fijas**: lo que escribas ahí (el cliente, el plazo, cómo quieres que responda) entra siempre en el prompt.
- **Resumen progresivo**: los mensajes antiguos se van plegando en un resumen, con el modelo si hay uno cargado y, si
  no, con la primera frase de cada mensaje.
- **Fragmentos que vienen al caso**: de lo que ya no entra literal, se recuperan los trozos relacionados con tu
  pregunta.

El espacio se reparte según el contexto del modelo (`n_ctx`): por defecto el 45 % es para la conversación
(`hilo.pct_contexto`). En el chat, el botón **HILO** abre las notas y el resumen; puedes corregirlo o pulsar
**RESUMIR AHORA**. Se desactiva con `hilo.activo: false`.

## Ubicación en tiempo real

Los agentes tienen la herramienta `ubicacion_actual`: «¿dónde estoy?», «¿a cuánto estoy de Madrid?». En
**Ajustes → Ubicación** eliges de dónde sale la posición:

| Fuente | Dónde |
|---|---|
| GPS por gpsd | Linux, Raspberry Pi, macOS |
| Receptor GPS por puerto serie o USB (NMEA) | todos; indica el puerto (`COM3`, `/dev/ttyACM0`) |
| Servicio de ubicación de Windows | Windows, con la ubicación activada |
| GeoClue (`where-am-i`) | Linux de escritorio |
| Ubicación del navegador | cualquier equipo o móvil, tras dar permiso |
| Posición fija | la que pongas («43.26, -2.93» o una ciudad) |

Da la ciudad más cercana con una tabla propia de 145 ciudades y la distancia en línea recta hasta un lugar guardado,
una ciudad o unas coordenadas. La posición no se envía a ningún servicio ni se comparte por la red COOP: si la pide
otro nodo, la herramienta se niega.

En la app Android la fuente es la del navegador integrado; el GPS nativo en segundo plano que tenía la edición C++
no está portado.

## MCP (Model Context Protocol)

- **Servidor**: otros programas con MCP pueden usar las herramientas de Agente IA. Por HTTP en
  `http://127.0.0.1:8775/mcp` o por stdio con la orden `nexus mcp`. Cada llamada pasa por el supervisor: las
  herramientas de riesgo medio o alto piden aprobación en la interfaz y todo queda en la auditoría. Desde otro equipo
  hace falta el token que se crea en **Ajustes → MCP** (cabecera `Authorization: Bearer …`).
- **Cliente**: añade servidores MCP externos (una orden que se arranca con stdio o una URL) y sus herramientas
  aparecen para los agentes como `mcp_<servidor>_<herramienta>`, con el riesgo que elijas.

## Rastreador web con lista de URLs

En **Núcleo → Biblioteca RAG**, **LISTA DE URLS…** admite direcciones pegadas (una por línea o separadas por comas)
y **SUBIR .TXT** un fichero. Se crea un rastreo por dirección y se procesan de dos en dos (`rag.crawl_parallel`); el
resto espera en cola y se puede pausar. Por la API: `POST /api/rag/crawl` con `urls`.

## La luz del grafo y las voces de los agentes

- Cuando un agente consulta el grafo de conocimiento para responder, una luz recorre en el avatar los nodos y las
  relaciones que ha leído.
- Cada agente puede tener **voz propia** y su velocidad (editor de agentes): al leer su respuesta se usa esa voz del
  equipo o del navegador.

## Laboratorio de bolsa

En **Bolsa → Laboratorio**, antes de mover nada en la cartera:

- **Señales**: cinco estrategias técnicas (tendencia, reversión, momento, volatilidad y estadística) con su confianza.
- **Backtest**: reproduce la estrategia con el histórico, con comisiones y deslizamiento, la compara con comprar y
  mantener y separa un tramo fuera de muestra. La orden se ejecuta al cierre siguiente, sin mirar al futuro.
- **Mesa de analistas**: técnico, debate alcista frente a bajista, riesgo y decisión con el peso sugerido. Con un
  modelo cargado lo razona la IA; sin él, lo hacen reglas. Solo informa: nunca da órdenes.
- **Montecarlo** y **reparto de cartera** (paridad de riesgo, varianza mínima, inversa de la volatilidad).

Hace falta histórico: con conexión se descarga y queda guardado para usarlo después sin internet. Los agentes
tienen las herramientas `bolsa_senales`, `bolsa_backtest` y `bolsa_analistas`. No se han portado las etapas hasta
operar con un bróker (Alpaca en papel) ni las noticias por RSS de la edición C++.

## Vídeo con tus imágenes

En **Estudio**, **CREAR VÍDEO** monta un AVI con las imágenes elegidas (Ctrl + clic en la galería) o con las últimas
generadas: zoom lento y fundido entre ellas, en 16:9, vertical, cuadrado o 4:3. No necesita ffmpeg. Los agentes
tienen la herramienta `crear_video`.

## Laboratorio beta

En **Sistema → Laboratorio beta**: **MEDIR ESTE EQUIPO** (base de datos, embeddings, memoria, biblioteca, grafo y
modelo), **TIEMPOS DE LA API** (media, p95 y máximo por ruta; cada respuesta lleva la cabecera `Server-Timing`),
opiniones y un informe en Markdown para enviar. Desde la terminal: `nexus bench`.

## Rutas nuevas de la API

| Ruta | Para qué |
|---|---|
| `GET/PUT /api/sessions/{id}/hilo`, `POST …/hilo/resumir` | notas fijas y resumen de la conversación |
| `GET/PUT /api/ubicacion`, `POST /api/ubicacion`, `POST /api/ubicacion/consultar` | estado, ajustes, posición del navegador y consulta |
| `POST /mcp` | servidor MCP (JSON-RPC 2.0) |
| `GET/PUT /api/mcp`, `PUT /api/mcp/servidores`, `POST /api/mcp/reconectar` | ajustes de MCP y servidores externos |
| `GET /api/beta`, `/api/beta/http`, `/api/beta/bench`, `/api/beta/opiniones`, `/api/beta/informe` | laboratorio beta |
| `GET /api/bolsa/lab/senales/{valor}`, `POST /api/bolsa/lab/backtest`, `…/mesa`, `…/montecarlo`, `…/optimizar` | laboratorio de bolsa |
| `POST /api/video/rapido`, `POST /api/video/guardar`, `GET /api/video` | vídeo |
| `GET /api/voice/voces` | voces del equipo para los agentes |

## Lo que sigue solo en la edición C++

La capa de hardware sin copias (`hal/`), los pipelines de percepción y difusión y el motor de difusión ONNX propio
son piezas nativas de la edición C++ y no tienen equivalente en Python; el editor de vídeo en el navegador con
títulos y música tampoco está portado.
