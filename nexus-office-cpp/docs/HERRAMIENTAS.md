# Herramientas: sistema modular y supervisado

## Anatomía de una herramienta
```python
TOOL = {
    "name": "convertir_unidades",
    "description": "Convierte entre unidades de longitud",
    "parameters": {"type": "object",
                   "properties": {"valor": {"type": "number"}, "de": {"type": "string"}, "a": {"type": "string"}},
                   "required": ["valor", "de", "a"]},
    "risk": "low",                 # low | medium | high
    "permissions": [],             # network, filesystem, code_exec, system, memory, media, agents
    "timeout": 30,                 # opcional, segundos
    "version": "1.0",
}

def run(args, ctx):
    factores = {"m": 1, "km": 1000, "cm": 0.01, "mi": 1609.344}
    return {"resultado": args["valor"] * factores[args["de"]] / factores[args["a"]]}
```
- **Integradas**: en C++, `src/tools/builtin_*.cpp`, con los mismos nombres, descripciones y parámetros de siempre.
- **Comunidad**: `data/tools/community/*.py`, solo tras aprobación. Son el único sitio donde se usa Python: se ejecutan con el Python del sistema si lo hay (sin él, no están disponibles). Su manifiesto se lee por AST (sin ejecutar el código) y se ejecutan en un **subproceso aislado** (`python -I`, entorno mínimo, directorio sandbox, límite de tiempo).

## Supervisión

Desde la beta 2.8, si el turno ya leyó contenido de terceros (web, documentos, ficheros, memoria), las herramientas que escriben, gastan o ejecutan **suben un escalón**, y las aprobaciones sin respuesta quedan pendientes y se reanudan al aprobarlas. La lista `tools` de un agente admite exclusiones (`"!web_fetch"`, `"!riesgo:high"`). Ver [AGENTES.md](AGENTES.md).
| Política | Comportamiento |
|---|---|
| `auto` | Se ejecuta directamente. |
| `ask` | Tarjeta de aprobación en la UI para el usuario. |
| `admin` | Solo un administrador puede aprobarla. |
| `deny` | Bloqueada. |

Por defecto: riesgo bajo `auto`, medio `ask`, alto `admin`. Las peticiones que llegan de otros nodos de la red se **elevan un nivel**. Todo queda en la auditoría (agente, argumentos, decisión, aprobador, estado, duración).

## Herramientas incluidas
`calculator`, `datetime_now`, `ubicacion_actual` (ubicación en tiempo real sin internet: GPS del móvil, gpsd, GPS USB, Windows, GeoClue, navegador o posición fija; ver [UBICACION](UBICACION.md)), `memory_search`, `memory_save`, `knowledge_graph` (hechos y atributos de una entidad), `graph_import` (importa como conocimiento un grafo de cualquier formato desde el sandbox o desde un texto, al grafo general, al del agente o repartido; ver [GRAFOS](GRAFOS.md)), `system_info`, `web_search`, `web_fetch` (bloquea direcciones de red local; pagina el texto largo con `offset`/`next_offset`), `url_to_pdf` y `crawl_docs` (rastreo de un sitio de documentación hacia la biblioteca RAG; los dominios grandes van por lotes reanudables, ver [RAG](RAG.md#rastreo-crawling-de-un-sitio-de-documentación)), `rag_search`, `rag_list`, `file_list/read/write` (confinadas a `data/sandbox`; `file_read` lee por tramos con `offset`/`max_chars` y `file_list` pagina de 500 en 500; `file_list` acepta una subcarpeta, p. ej. `proyectos/<nombre>` de un ZIP importado), `python_exec` (alto riesgo), `generate_image`, `compose_music`, `delegate`, `list_agents`, `propose_tool`, y las de la mesa de bolsa simulada (`bolsa_mercados`, `bolsa_cotizacion`, `bolsa_cartera`, `bolsa_costes`, `bolsa_orden`, `bolsa_cancelar`, `bolsa_informe_fiscal`; ver [BOLSA](BOLSA.md)), y las de edición de imágenes con GIMP (`imagen_editar`, `imagen_info`, `gimp_estado` y `gimp_script`, esta última de alto riesgo; ver [GIMP](GIMP.md)).

## Propuestas de los agentes
1. Un agente (normalmente el **Forjador**) detecta una carencia y llama a `propose_tool` con el código.
2. **Análisis estático**: sintaxis, manifiesto, `run(args, ctx)`, importaciones y llamadas peligrosas (`subprocess`, `os`, `socket`, `eval`, `exec`, `open`, `rmtree`…) → riesgo sugerido.
3. El administrador revisa en **Herramientas → Propuestas**: puede editar el código (se re-analiza), **probarlo en el sandbox** con argumentos de prueba, aprobarlo, rechazarlo o compartirlo con la red.
4. Al aprobar, si el análisis sugiere un riesgo mayor que el declarado, **se eleva automáticamente**.
5. El motivo de un rechazo se guarda como *lección* que el agente verá en el futuro.

## MCP (3.2)

**Agente IA como servidor MCP.** Otros programas usan las herramientas de Agente IA con el Model Context Protocol:

- HTTP: `http://127.0.0.1:8775/mcp`.
- stdio: la orden `nexus-office mcp` (opciones `--url` y `--token`), para los programas que arrancan el servidor ellos
  mismos. Por ejemplo, en la configuración MCP de un asistente de escritorio:
  `{"agente-ia": {"command": "C:/Agente IA/nexus-office.exe", "args": ["mcp"]}}`.

Cada llamada pasa por el supervisor como la de un agente (`mcp:<cliente>`): la política por riesgo, las aprobaciones y
la auditoría son las mismas. Desde otro equipo hace falta la clave de red o el token `mcp.token`. Las herramientas que
vienen de otros servidores MCP no se reexportan.

**Herramientas MCP para los agentes.** En **Herramientas > MCP** (modo administrador) se añaden servidores MCP
externos en `mcp.servidores`:

```json
[{"nombre": "ficheros", "orden": ["npx", "-y", "@modelcontextprotocol/server-filesystem", "C:/Docs"], "riesgo": "medium"},
 {"nombre": "wiki", "url": "http://192.168.1.20:9000/mcp", "cabeceras": {"Authorization": "Bearer ..."}, "riesgo": "low"}]
```

Sus herramientas aparecen en el repositorio como `mcp_<servidor>_<nombre>` (origen MCP) con el riesgo indicado
(`medium` por defecto, que pide aprobación según la política) y los agentes que tengan `*` pueden usarlas.

MCP no acelera la inferencia: es otro canal para las herramientas. Dentro de Agente IA las integradas se siguen
llamando directamente.
