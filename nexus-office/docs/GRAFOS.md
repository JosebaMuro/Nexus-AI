# Importar grafos como conocimiento

Nexus-Office puede incorporar a su conocimiento un grafo de **cualquier tipo y formato**: una red exportada de Gephi o Cytoscape, una base de Neo4j, una ontología RDF u OWL, un diagrama Mermaid, una hoja de Excel con nodos y relaciones, un mapa mental o la base de otro Nexus-Office. Todo funciona sin internet.

El formato se reconoce **por el contenido**. La extensión solo ayuda cuando el contenido no basta: un GraphML guardado como `.txt` o un JSON sin extensión se leen igual.

Lo importado se **fusiona** con lo que Nexus-Office ya sabe. Los nodos con el mismo nombre se unen en uno, las relaciones que ya existían se refuerzan y no se duplican, y cada nodo conserva su nota, su tipo y su categoría si ya los tenía. Desde ese momento los agentes lo usan al responder.

## Cómo se importa

### Desde la app
1. Entra en modo administrador y abre **Ajustes > Gestor de conocimiento > Importar / exportar**.
2. En **Insertar en**, elige dónde va lo importado:
   - **Automático**: cada nodo va a su sitio. Lo común (personas, lugares, preferencias) va al grafo general y el saber hacer de un oficio va al agente especialista.
   - **General**: para todos los agentes.
   - **Un agente**: solo para ese agente.
3. Suelta el fichero en el recuadro, o haz clic para elegirlo. Puedes soltar **varios ficheros a la vez**, por ejemplo `nodes.csv` y `edges.csv`, y se unen en un solo grafo.
4. La importación va en segundo plano y se ve su avance abajo. Al terminar, un aviso indica el formato reconocido, cuántos nodos son nuevos, cuántos se han fusionado con los que ya había y cuántas relaciones se han creado.

En **Formatos admitidos** se despliega la lista completa.

### Desde la terminal
```
python -m nexus grafo formatos                                   # lista de formatos
python -m nexus grafo importar red.graphml                       # al grafo general
python -m nexus grafo importar nodes.csv edges.csv --en auto     # varios ficheros, cada nodo a su sitio
python -m nexus grafo importar recetas.ttl --en cocinero         # solo para el agente «cocinero»
python -m nexus grafo exportar copia.json --en todo              # exportar (json, graphml o csv con --formato)
```

### Desde el chat
Los agentes tienen la herramienta `graph_import`. Puedes pedirles, por ejemplo, «importa como conocimiento el grafo `proveedores.gexf` de la carpeta de trabajo» o «guarda este diagrama como conocimiento: Pedido -> Albarán -> Factura». El agente lee un fichero de su carpeta de trabajo (`data/sandbox`) o el texto que le das, y lo guarda en el grafo general, en el suyo o repartido automáticamente.

### Dentro de un ZIP
Al **leer un ZIP** (también RAR, 7z o TAR) en **Ajustes > Gestor de conocimiento > Leer un ZIP**, los grafos que trae se importan solos, en cualquiera de estos formatos, y el resto de documentos va a la biblioteca.

## Formatos admitidos

| Familia | Extensiones habituales | Qué se lee |
|---|---|---|
| Nexus-Office | `.json`, `.db` | Exportación propia y la base SQLite de otro Nexus-Office, respetando el grafo general y el de cada agente. |
| JSON node-link | `.json` | D3, vis.js, sigma.js, NetworkX (`node_link_data` y `adjacency_data`), JSON Graph Format, Arrows.app. |
| Cytoscape | `.cyjs`, `.cx`, `.cx2`, `.sif`, `.xgmml` | Cytoscape.js, CX y CX2, SIF y XGMML. |
| Neo4j | `.json`, `.jsonl`, `.csv`, `.cypher` | Exportación del Browser, API HTTP, APOC (JSON, Cypher y GraphML) y CSV de `neo4j-admin import` (`:ID`, `:START_ID`, `:END_ID`, `:TYPE`, `:LABEL`). |
| Cypher | `.cypher`, `.cql` | `CREATE`, `MERGE` y `MATCH … CREATE`, a mano o exportado. Las consultas (`MATCH … RETURN`) no crean nada. |
| JSON-LD | `.jsonld`, `.json` | schema.org, Wikidata y cualquier `@graph`. |
| GraphSON | `.json`, `.graphson` | TinkerPop, JanusGraph y Amazon Neptune, con tipos o sin ellos. |
| RDF | `.ttl`, `.nt`, `.nq`, `.trig`, `.n3`, `.rdf`, `.owl` | Turtle, N-Triples, N-Quads, TriG, N3, RDF/XML y ontologías OWL. |
| GraphML | `.graphml` | yEd (con sus etiquetas), Gephi, NetworkX, igraph y APOC. |
| GEXF | `.gexf` | Gephi, con sus atributos. |
| GML | `.gml` | igraph, NetworkX y Cytoscape. |
| Graphviz DOT | `.dot`, `.gv` | `digraph` y `graph`, con etiquetas y pesos. |
| Análisis de redes | `.net`, `.paj`, `.gdf`, `.dl`, `.tgf`, `.lgl`, `.ncol` | Pajek, GDF (GUESS/Gephi), UCINET DL, TGF, LGL y NCOL. |
| Tablas | `.csv`, `.tsv`, `.xlsx`, `.ods`, `.db`, `.sqlite` | Tablas de aristas (`source,target,relation`), de nodos (`id,label,…`), matrices de adyacencia, tripletas y ConceptNet. En Excel y ODS, cada hoja es una tabla. En SQLite, cada tabla. |
| Listas | `.edgelist`, `.adjlist`, `.txt` | `a b [peso]`, `A: B, C` y una tripleta por línea. |
| Diagramas | `.mmd`, `.puml`, `.txt` | Mermaid (flowchart, graph, erDiagram, classDiagram, mindmap), PlantUML y texto con flechas `A -> B : relación`. |
| Mapas mentales y esquemas | `.mm`, `.opml`, `.xmind`, `.md` | FreeMind y Freeplane, OPML, XMind y listas o títulos con sangría. |
| Notas enlazadas | `.md`, `.canvas`, `.zip` | Obsidian, Logseq y Foam (`[[enlaces]]`) y Obsidian Canvas. |
| YAML y TOML | `.yaml`, `.yml`, `.toml` | Con nodos y aristas, con tripletas o con cualquier estructura de objetos con nombre. |
| Comprimidos | `.zip`, `.tar`, `.gz`, `.bz2`, `.xz` | Uno o varios grafos dentro, en cualquiera de los formatos anteriores. |

Si el fichero no tiene forma de grafo (un PDF, una foto, código fuente, una página web), Nexus-Office lo dice y propone llevarlo a la **Biblioteca**.

## Qué se guarda de cada grafo
- **Nodos**: cada entidad del grafo, con su nombre (`name`, `label`, `title`, `rdfs:label`…) y su tipo (la etiqueta de Neo4j, el `rdf:type`, la columna `type`…).
- **Relaciones**: las aristas, con su nombre en minúsculas y con guiones bajos (`TRABAJA_EN` y `trabajaEn` quedan como `trabaja_en`). Si no tienen nombre, quedan como `relacionado_con`. El peso se conserva.
- **Atributos**: las propiedades de cada nodo (propiedades de Neo4j, literales RDF, columnas de una tabla, atributos de GEXF o GraphML) van a su **nota** como `clave: valor`. Los agentes los ven al hablar de esa entidad: si importas que el Teide tiene `altura: 3718`, el agente lo sabe al preguntarle por el Teide.
- **Nodos anónimos de RDF**: se resuelven para que el conocimiento quede entre entidades con nombre. `ex:Ana ex:vive [ ex:ciudad ex:Bilbao ]` queda como *Ana vive_ciudad Bilbao*.
- **Ámbitos**: el grafo de otro Nexus-Office mantiene lo que era general y lo que era de cada agente, si ese agente existe aquí.

Un grafo puede tener hasta 200.000 nodos y la subida, hasta 300 MB. Un grafo de 100.000 relaciones se lee y se inserta en pocos segundos.

## Para desarrolladores
- La lectura está en `nexus/memory/kg_formats.py`: `parse_bytes(datos, nombre)` detecta el formato con `detect()` y devuelve `{"format", "nodes", "edges"}`. `parse_many([(nombre, datos), …])` une varios ficheros; las tablas comparten ids, de modo que `nodes.csv` y `edges.csv` forman un solo grafo.
- La fusión está en `nexus/memory/kg_io.py`: `insert_graph(grafo, leido, destino, organizador, agentes)` devuelve `nodes`, `new`, `merged`, `relations`, `skipped` y `by_scope`.
- Lo que dentro de un ZIP de documentos cuenta como grafo lo decide `looks_like_graph()`: las interpretaciones de último recurso (un JSON o YAML cualquiera, un esquema con sangría) llevan `generic: true` y no cuentan.
- API: `POST /api/kg/import` acepta `file` o varios `files`; `GET /api/kg/formats` da la lista de formatos. Ver [API.md](API.md).
- Pruebas: `tests/test_graph_import.py`, con un ejemplo real de cada formato.
