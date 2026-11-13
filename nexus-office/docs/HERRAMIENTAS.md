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
- **Integradas**: `nexus/tools/builtin/*.py` (pueden declarar varias con `TOOLS = [(manifiesto, función), ...]`).
- **Comunidad**: `data/tools/community/*.py`, solo tras aprobación. Su manifiesto se lee por AST (sin ejecutar el código) y, una vez aprobadas, se cargan y se ejecutan **en el mismo proceso** que las integradas, sin aislamiento (con el límite de tiempo del supervisor).

## Sin sandbox
Desde esta versión las herramientas y los agentes **no se ejecutan en un sandbox**:
- `file_list`, `file_read`, `file_write` y `graph_import` aceptan rutas relativas a la **carpeta de trabajo** (`data/sandbox`, que conserva el nombre para no romper los proyectos ya importados) y también **rutas absolutas** o con `..` a cualquier carpeta del equipo.
- `python_exec` lanza el intérprete normal (sin `-I`), con el entorno completo del servicio y acceso a todo el equipo; empieza en la carpeta de trabajo.
- Las herramientas de la comunidad y la prueba de propuestas se ejecutan dentro del proceso de Agente IA.
- `imagen_editar`, `imagen_info` y `gimp_script` leen y guardan imágenes en cualquier ruta del equipo.

Lo que sigue igual: la **supervisión** (política por riesgo, aprobaciones, límite de tiempo y auditoría), el análisis estático de las propuestas, el bloqueo de la red local en `web_fetch` y la descompresión segura de ZIP/RAR/7z. Como ya no hay aislamiento, **la política de aprobación es la única barrera**: no pongas `python_exec`, `gimp_script` ni el riesgo `high` en `auto` si los agentes reciben textos de fuentes no fiables (web, documentos, red COOP).

## Supervisión
| Política | Comportamiento |
|---|---|
| `auto` | Se ejecuta directamente. |
| `ask` | Tarjeta de aprobación en la UI para el usuario. |
| `admin` | Solo un administrador puede aprobarla. |
| `deny` | Bloqueada. |

Por defecto: riesgo bajo `auto`, medio `ask`, alto `admin`. Las peticiones que llegan de otros nodos de la red se **elevan un nivel**. Todo queda en la auditoría (agente, argumentos, decisión, aprobador, estado, duración).

## Herramientas incluidas
`calculator`, `datetime_now`, `memory_search`, `memory_save`, `knowledge_graph` (hechos y atributos de una entidad), `graph_import` (importa como conocimiento un grafo de cualquier formato desde un fichero (carpeta de trabajo o ruta absoluta) o desde un texto, al grafo general, al del agente o repartido; ver [GRAFOS](GRAFOS.md)), `system_info`, `web_search`, `web_fetch` (bloquea direcciones de red local; pagina el texto largo con `offset`/`next_offset`), `url_to_pdf` y `crawl_docs` (rastreo de un sitio de documentación hacia la biblioteca RAG; los dominios grandes van por lotes reanudables, ver [RAG](RAG.md#rastreo-crawling-de-un-sitio-de-documentación)), `rag_search`, `rag_list`, `file_list/read/write` (rutas de la carpeta de trabajo `data/sandbox` o absolutas, sin confinar; `file_read` lee por tramos con `offset`/`max_chars` y `file_list` pagina de 500 en 500; `file_list` acepta una subcarpeta, p. ej. `proyectos/<nombre>` de un ZIP importado), `python_exec` (alto riesgo, sin aislamiento), `generate_image`, `compose_music`, `delegate`, `list_agents`, `propose_tool`, y las de la mesa de bolsa simulada (`bolsa_mercados`, `bolsa_cotizacion`, `bolsa_cartera`, `bolsa_costes`, `bolsa_orden`, `bolsa_cancelar`, `bolsa_informe_fiscal`; ver [BOLSA](BOLSA.md)), y las de edición de imágenes con GIMP (`imagen_editar`, `imagen_info`, `gimp_estado` y `gimp_script`, esta última de alto riesgo; ver [GIMP](GIMP.md)).

## Propuestas de los agentes
1. Un agente (normalmente el **Forjador**) detecta una carencia y llama a `propose_tool` con el código.
2. **Análisis estático**: sintaxis, manifiesto, `run(args, ctx)`, importaciones y llamadas peligrosas (`subprocess`, `os`, `socket`, `eval`, `exec`, `open`, `rmtree`…) → riesgo sugerido.
3. El administrador revisa en **Herramientas → Propuestas**: puede editar el código (se re-analiza), **probarlo** con argumentos de prueba (se ejecuta en el proceso, sin aislar), aprobarlo, rechazarlo o compartirlo con la red.
4. Al aprobar, si el análisis sugiere un riesgo mayor que el declarado, **se eleva automáticamente**.
5. El motivo de un rechazo se guarda como *lección* que el agente verá en el futuro.
