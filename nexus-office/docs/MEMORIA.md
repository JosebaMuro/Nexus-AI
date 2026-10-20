# Memoria y gestor de conocimiento

Desde la 1.6.0, Nexus-Office separa lo que es **común** a todos los agentes de lo que es **saber hacer propio** de cada uno.

## Cómo se organiza

| Dónde | Qué guarda | Categorías |
|---|---|---|
| **Grafo general** (común) | Cómo eres y qué te gusta, las pautas de comportamiento que pides, tus relaciones con otras personas, cómo interactúas con los agentes y el conocimiento general que sirve a cualquiera para razonar. | `perfil`, `preferencia`, `comportamiento`, `relacion`, `interaccion`, `comun` |
| **Subgrafo aislado de cada agente** | El saber hacer de su especialidad: conceptos técnicos, procedimientos paso a paso, sus documentos y su experiencia. Ningún otro agente lo ve. | `especialidad`, `procedimiento`, `documento`, `experiencia` |

- Los gustos son comunes: si le dices al artista que prefieres respuestas cortas o que no uses emojis, el programador también lo sabrá.
- Las **pautas de comportamiento** («no uses emojis», «respóndeme siempre con ejemplos», «llámame Joseba») se guardan como nodos `comportamiento` y recuerdos de tipo `behavior`. Todos los agentes las reciben en su prompt, en la sección *Pautas de comportamiento que el usuario te ha pedido*.
- Las **relaciones** («mi hermana Ane es arquitecta») y las **interacciones** (con qué agente hablas, en quién delega cada agente) van al general.
- Cada subgrafo de agente puede enlazar con nodos del general (puentes). Nunca enlaza con el subgrafo de otro agente.

### El organizador
`nexus/memory/organizer.py` decide dónde va cada cosa que se aprende, en la conversación, con `memory_save`, al importar grafos o al leer un ZIP:

1. Busca, con reglas, datos personales, pautas, relaciones e interacciones. Todo eso va al general.
2. Compara el resto con el **perfil de especialidad** de cada agente: su descripción, el campo `specialty` (*Agentes → Editar → Especialidad*) y sus palabras clave.
   - Con un modelo de embeddings, usa la afinidad semántica. El texto va al especialista si su afinidad supera `memory.organizer.min_affinity` (0.35) y gana al conocimiento general por `margin` (0.03).
   - Sin modelo, cuenta las palabras clave de la especialidad que aparecen.
3. Si nada encaja, va al general como `comun`. Si el texto describe pasos («primero…, luego…, finalmente…»), se guarda como `procedimiento`.

El agente generalista `nexus` no tiene especialidad: lo que aprende en sus conversaciones se reparte entre los especialistas o el general. Si no hay ningún especialista definido, cada agente guarda lo suyo como antes.

## Gestor de conocimiento (Ajustes, modo administrador)

| Pestaña | Para qué |
|---|---|
| **Resumen** | Nodos, relaciones, documentos y recuerdos de cada grafo, por categoría, con la especialidad de cada agente. |
| **Explorar y editar** | Filtrar por grafo, categoría y tipo, y buscar por texto o **por significado**. Editar nombre, tipo, categoría, peso y nota. **Mover** un nodo a otro grafo (sus relaciones se recalculan y las que cruzarían agentes se quitan), **fusionar** dos nodos, añadir o borrar relaciones y crear nodos. |
| **Importar / exportar** | Insertar otros grafos de conocimiento en el general, en un agente o en **automático** (cada nodo a su sitio). Exportar cualquier grafo en JSON, GraphML o CSV. |
| **Leer un ZIP** | Ver más abajo. |
| **Mantenimiento** | **Reorganizar** lo aprendido: revisa cada nodo y recuerdo, propone qué mover y lo aplica cuando confirmas. **Duplicados**: nodos del mismo grafo que significan lo mismo, con botón para fusionarlos. **Recuerdos por ámbito**, para moverlos de uno a otro. |
| **Embeddings** | Modelo activo, progreso del indexado, **reindexar** y descarga del modelo recomendado. |

Los trabajos largos (importar, leer un ZIP, reorganizar, buscar duplicados) van en segundo plano y muestran su progreso abajo, sin bloquear el chat.

### Formatos de grafo que se pueden importar
- **JSON**: el propio de Nexus-Office (`nexus-kg`), D3 / vis.js (`nodes` + `links`/`edges`), Cytoscape (`elements`), JSON Graph Format y listas de tripletas (`[["s","r","o"], …]` o `{s, r, o}`).
- **JSONL** con tripletas, **CSV/TSV** (`source,target,relation` o `sujeto,relacion,objeto`), **GraphML**, **GEXF**, **DOT** (Graphviz), **N-Triples/Turtle** básicos.
- **Notas Markdown** con `[[enlaces]]` (Obsidian, Logseq, Foam): cada nota es un nodo y cada enlace una relación.

Si el fichero trae una categoría o un ámbito por nodo, se respetan. En modo automático, los nodos sin ámbito los reparte el organizador.

### Leer un ZIP (también RAR, 7z y TAR)
1. Lo descomprime de forma segura (sin rutas fuera de la carpeta, con límites de tamaño y número de ficheros).
2. Inserta los grafos que traiga (JSON, GraphML, CSV, RDF…) y convierte en grafo las notas con `[[enlaces]]`.
3. Lee hasta 600 documentos, con los README e índices primero, y elige el destino. En automático, el saber hacer va al especialista y lo común al general.
4. Lo añade a la biblioteca RAG, si está marcado.
5. **Agrupa los documentos por temas** con el modelo de embeddings, hasta 12 temas.
6. **Razona** con el LLM los documentos más representativos de cada tema: `memory.zip_reason_docs` (12 por defecto) o lo que indiques en la pestaña. De cada uno saca un resumen, hechos clasificados y procedimientos paso a paso. Los hechos van a su categoría, y los procedimientos quedan como nodos `procedimiento` y recuerdos `skill` del agente.
7. Escribe una síntesis del archivo completo. Sin LLM, usa un resumen extractivo y las reglas.

El informe muestra los temas, qué fue a cada grafo y lo que se aprendió.

## Modelo de embeddings: Qwen3-Embedding 0.6B

Los embeddings convierten un texto en un vector que representa su significado. Nexus-Office los usa para buscar en la biblioteca y en la memoria, decidir qué es común y qué es de cada agente, agrupar documentos por temas y encontrar duplicados.

- **Modelo**: `qwen3-embedding-0.6b`, de `Qwen/Qwen3-Embedding-0.6B-GGUF`, fichero `Qwen3-Embedding-0.6B-Q8_0.gguf` (639 MB). Es multilingüe (más de 100 idiomas, español incluido), admite textos de hasta 32k tokens y está entrenado para buscar y clasificar documentación. Recibe **instrucciones por tarea**: buscar, recordar, clasificar, organizar, deduplicar.
- **Descarga**: en *Modelos* (recomendado ★ para todos los perfiles), en *Ajustes → Gestor de conocimiento → Embeddings* o con `python -m nexus pull qwen3-embedding-0.6b`. Se descarga una vez y queda en `data/models/`, disponible sin conexión. Si es el único de su tipo, se activa solo.
- **En CPU**: corre en un `llama-server` aparte (`--embedding --pooling last`), con el mismo runtime llama.cpp que el chat, incluido en todos los instaladores. Si no hay runtime, usa `llama-cpp-python`. Se arranca al primer uso y se para tras 10 minutos sin peticiones, para liberar RAM.
- **Indexado progresivo**: en un PC modesto (por ejemplo, un i3 sin GPU), calcular los vectores de una biblioteca grande lleva un rato. Por eso los documentos y recuerdos nuevos se indexan al momento con vectores rápidos (hashing). Un proceso de fondo los recalcula con el modelo en lotes de 4 y cede el turno mientras el LLM genera una respuesta o hay una búsqueda esperando. La búsqueda mezcla los ya recalculados y los pendientes, así que todo se encuentra desde el primer momento.
- **Cambiar de modelo** reindexa en segundo plano, con el mismo mecanismo.
- **Ahorrar RAM**: `memory.embed_dims: 512` (o 256) recorta los vectores (Matryoshka), con poca pérdida de precisión.

### Configuración (`data/config.yaml`)
```yaml
memory:
  embedder: auto          # auto | gguf | hashing | ollama | sentence-transformers
  embed_model: qwen3-embedding-0.6b   # se rellena al activar el modelo
  embed_threads: 0        # hilos del servidor de embeddings (0 = los del LLM)
  embed_dims: 0           # recorte Matryoshka; 0 = completo (1024)
  organizer: {min_affinity: 0.35, margin: 0.03, min_keyword_hits: 1}
  zip_reason_docs: 12     # documentos de un ZIP que razona el LLM
```

## API
Ver la sección *Gestor de conocimiento* en [API.md](API.md).
