"""1.9: importar un grafo de cualquier tipo y formato como conocimiento (deteccion por contenido, fusion y agentes)."""
import bz2
import gzip
import io
import json
import sqlite3
import tarfile
import zipfile

import pytest

from nexus.memory import kg_io
from nexus.memory.kg_formats import GraphFormatError, detect

from test_knowledge_org import make, names


def edges_of(g):
    nm = {n["id"]: n["name"] for n in g["nodes"]}
    return {(nm[e["src"]], e["rel"], nm[e["dst"]]) for e in g["edges"]}


def node(g, name):
    return next(n for n in g["nodes"] if n["name"] == name)


# (fichero, contenido, formato esperado, tripleta que debe salir)
SAMPLES = [
    # ---- JSON y derivados
    ("d3.json", json.dumps({"nodes": [{"id": "py", "name": "Python"}, {"id": "dj", "name": "Django"}],
                            "links": [{"source": "dj", "target": "py", "label": "escrito en"}]}),
     "json", ("Django", "escrito_en", "Python")),
    ("nx.json", json.dumps({"directed": True, "multigraph": False, "graph": {}, "nodes": [{"id": "A"}, {"id": "B"}],
                            "adjacency": [[{"id": "B", "weight": 2}], []]}), "json", ("A", "relacionado_con", "B")),
    ("g.jsonld", json.dumps({"@context": "https://schema.org", "@graph": [
        {"@id": "http://ex.org/teide", "@type": "Mountain", "name": "Teide", "elevation": 3718,
         "containedInPlace": {"@id": "http://ex.org/tenerife"}},
        {"@id": "http://ex.org/tenerife", "@type": "Island", "name": "Tenerife"}]}),
     "json-ld", ("Teide", "contained_in_place", "Tenerife")),
    ("neo.json", json.dumps([{"n": {"identity": 1, "labels": ["Person"], "properties": {"name": "Ana", "edad": 40}},
                              "r": {"identity": 9, "start": 1, "end": 2, "type": "TRABAJA_EN", "properties": {}},
                              "m": {"identity": 2, "labels": ["Company"], "properties": {"name": "Gestoria Lopez"}}}]),
     "neo4j", ("Ana", "trabaja_en", "Gestoria Lopez")),
    ("http.json", json.dumps({"results": [{"columns": ["p"], "data": [{"graph": {
        "nodes": [{"id": "1", "labels": ["Cliente"], "properties": {"name": "Bar Pepe"}},
                  {"id": "2", "labels": ["Producto"], "properties": {"name": "Cafe"}}],
        "relationships": [{"id": "5", "type": "COMPRA", "startNode": "1", "endNode": "2", "properties": {}}]}}]}],
        "errors": []}), "neo4j", ("Bar Pepe", "compra", "Cafe")),
    ("apoc.json", "\n".join(json.dumps(x) for x in [
        {"type": "node", "id": "0", "labels": ["Libro"], "properties": {"title": "Don Quijote"}},
        {"type": "node", "id": "1", "labels": ["Autor"], "properties": {"name": "Cervantes"}},
        {"type": "relationship", "id": "0", "label": "ESCRITO_POR", "properties": {},
         "start": {"id": "0", "labels": ["Libro"]}, "end": {"id": "1", "labels": ["Autor"]}}]),
     "neo4j-apoc", ("Don Quijote", "escrito_por", "Cervantes")),
    ("tp.json", "\n".join(json.dumps(x) for x in [
        {"id": 1, "label": "person", "outE": {"knows": [{"id": 7, "inV": 2}]},
         "properties": {"name": [{"id": 0, "value": "marko"}], "age": [{"id": 1, "value": 29}]}},
        {"id": 2, "label": "person", "properties": {"name": [{"id": 2, "value": "vadas"}]}}]),
     "graphson", ("marko", "knows", "vadas")),
    ("typed.json", json.dumps({"@type": "g:List", "@value": [
        {"id": {"@type": "g:Int32", "@value": 1}, "label": "person",
         "outE": {"created": [{"id": {"@type": "g:Int32", "@value": 9}, "inV": {"@type": "g:Int32", "@value": 3}}]},
         "properties": {"name": [{"id": {"@type": "g:Int64", "@value": 0}, "value": "marko"}]}},
        {"id": {"@type": "g:Int32", "@value": 3}, "label": "software",
         "properties": {"name": [{"id": {"@type": "g:Int64", "@value": 4}, "value": "lop"}]}}]}),
     "graphson", ("marko", "created", "lop")),
    ("arrows.json", json.dumps({"nodes": [{"id": "n0", "caption": "Factura", "labels": [], "properties": {}},
                                          {"id": "n1", "caption": "Cliente", "labels": [], "properties": {}}],
                                "relationships": [{"id": "r0", "fromId": "n0", "toId": "n1", "type": "EMITIDA_A"}]}),
     "json", ("Factura", "emitida_a", "Cliente")),
    ("mapa.canvas", json.dumps({"nodes": [{"id": "a", "type": "text", "text": "# IVA\nImpuesto indirecto", "x": 0, "y": 0},
                                          {"id": "b", "type": "file", "file": "notas/Modelo 303.md"}],
                                "edges": [{"id": "e", "fromNode": "a", "toNode": "b", "label": "se declara en"}]}),
     "json", ("IVA", "se_declara_en", "Modelo 303")),
    ("cx.cx", json.dumps([{"numberVerification": [{"longNumber": 281474976710655}]},
                          {"nodes": [{"@id": 1, "n": "TP53"}, {"@id": 2, "n": "MDM2"}]},
                          {"edges": [{"@id": 3, "s": 2, "t": 1, "i": "inhibe"}]}]), "cytoscape-cx", ("MDM2", "inhibe", "TP53")),
    ("adj.json", json.dumps({"Ventas": ["Clientes", "Facturas"], "Facturas": ["IVA"]}), "adyacencia",
     ("Facturas", "relacionado_con", "IVA")),
    ("tree.json", json.dumps({"name": "Empresa", "children": [{"name": "Contabilidad", "children": [{"name": "Nominas"}]}]}),
     "arbol", ("Contabilidad", "contiene", "Nominas")),
    ("g.yaml", "nodes:\n  - {id: a, label: Fontaneria}\n  - {id: b, label: Tuberias}\nedges:\n  - {source: a, target: b, label: repara}\n",
     "yaml", ("Fontaneria", "repara", "Tuberias")),
    ("g.toml", '[[triples]]\ns = "Teide"\nr = "esta_en"\no = "Tenerife"\n', "toml", ("Teide", "esta_en", "Tenerife")),
    # ---- tablas
    ("gephi.csv", "Source,Target,Type,Id,Label,Weight\nAna,Luis,Directed,0,conoce,1\n", "csv", ("Ana", "conoce", "Luis")),
    ("rel.csv", ":START_ID,:END_ID,:TYPE,since:int\np1,p2,AMIGO_DE,2001\n", "csv", ("p1", "amigo_de", "p2")),
    ("m.csv", ",Madrid,Bilbao,Sevilla\nMadrid,0,1,1\nBilbao,1,0,0\nSevilla,1,0,0\n", "csv",
     ("Madrid", "relacionado_con", "Bilbao")),
    ("cn.csv", "/a/[/r/IsA/,/c/es/perro/,/c/es/animal/]\t/r/IsA\t/c/es/perro\t/c/es/animal\t{}\n", "tsv",
     ("perro", "is_a", "animal")),
    # ---- XML
    ("yed.graphml", """<?xml version="1.0"?><graphml xmlns="http://graphml.graphdrawing.org/xmlns"
     xmlns:y="http://www.yworks.com/xml/graphml"><key id="d6" for="node" yfiles.type="nodegraphics"/>
     <key id="d9" for="edge" yfiles.type="edgegraphics"/><graph edgedefault="directed">
     <node id="n0"><data key="d6"><y:ShapeNode><y:NodeLabel>Pedido</y:NodeLabel></y:ShapeNode></data></node>
     <node id="n1"><data key="d6"><y:ShapeNode><y:NodeLabel>Almacen</y:NodeLabel></y:ShapeNode></data></node>
     <edge source="n0" target="n1"><data key="d9"><y:PolyLineEdge><y:EdgeLabel>sale de</y:EdgeLabel></y:PolyLineEdge></data></edge>
     </graph></graphml>""", "graphml", ("Pedido", "sale_de", "Almacen")),
    ("a.gexf", """<gexf xmlns="http://gexf.net/1.3"><graph><attributes class="node"><attribute id="0" title="ciudad"/>
     </attributes><nodes><node id="1" label="Ana"><attvalues><attvalue for="0" value="Bilbao"/></attvalues></node>
     <node id="2" label="Luis"/></nodes><edges><edge source="1" target="2" label="conoce"/></edges></graph></gexf>""",
     "gexf", ("Ana", "conoce", "Luis")),
    ("g.rdf", """<?xml version="1.0"?><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
     xmlns:rdfs="http://www.w3.org/2000/01/rdf-schema#" xmlns:ex="http://ex.org/">
     <ex:Lenguaje rdf:about="http://ex.org/Python"><rdfs:label>Python</rdfs:label>
       <ex:creadoPor rdf:resource="http://ex.org/Guido"/><ex:anio>1991</ex:anio></ex:Lenguaje>
     <rdf:Description rdf:about="http://ex.org/Guido"><rdfs:label>Guido van Rossum</rdfs:label></rdf:Description>
     </rdf:RDF>""", "rdf-xml", ("Python", "creado_por", "Guido van Rossum")),
    ("g.owl", """<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:owl="http://www.w3.org/2002/07/owl#"
     xmlns:rdfs="http://www.w3.org/2000/01/rdf-schema#"><owl:Class rdf:about="#Perro">
     <rdfs:subClassOf rdf:resource="#Animal"/></owl:Class><owl:Class rdf:about="#Animal"/></rdf:RDF>""",
     "rdf-xml", ("Perro", "sub_class_of", "Animal")),
    ("g.xgmml", """<graph label="red" xmlns="http://www.cs.rpi.edu/XGMML"><node id="1" label="A"><att name="score" value="3"/></node>
     <node id="2" label="B"/><edge source="1" target="2" label="activa"/></graph>""", "xgmml", ("A", "activa", "B")),
    ("g.dgml", """<DirectedGraph xmlns="http://schemas.microsoft.com/vs/2009/dgml"><Nodes><Node Id="api" Label="API"/>
     <Node Id="db" Label="Base de datos"/></Nodes><Links><Link Source="api" Target="db" Label="consulta"/></Links></DirectedGraph>""",
     "dgml", ("API", "consulta", "Base de datos")),
    ("g.gxl", """<gxl><graph id="g"><node id="a"><attr name="name"><string>Motor</string></attr></node><node id="b">
     <attr name="name"><string>Coche</string></attr></node><edge from="a" to="b"><attr name="label"><string>parte de</string></attr>
     </edge></graph></gxl>""", "gxl", ("Motor", "parte_de", "Coche")),
    ("m.mm", """<map version="1.0.1"><node TEXT="Proyecto" ID="r"><node TEXT="Presupuesto" ID="p"/>
     <node TEXT="Plazos" ID="q"><arrowlink DESTINATION="p"/></node></node></map>""", "mapa-mental",
     ("Proyecto", "contiene", "Presupuesto")),
    ("o.opml", """<opml version="2.0"><body><outline text="Oficina"><outline text="Facturas"/></outline></body></opml>""",
     "opml", ("Oficina", "contiene", "Facturas")),
    # ---- texto
    ("g.gml", 'Creator "igraph"\ngraph [\n directed 1\n node [ id 0 label "Ana" ]\n node [ id 1 label "Luis" ]\n'
              ' edge [ source 0 target 1 label "conoce" ]\n]\n', "gml", ("Ana", "conoce", "Luis")),
    ("g.net", '*Vertices 3\n1 "Ana"\n2 "Luis"\n3 "Eva"\n*Arcs\n1 2 1\n*Edges\n2 3 2\n', "pajek", ("Ana", "relacionado_con", "Luis")),
    ("g.gdf", "nodedef>name VARCHAR,label VARCHAR\nn1,'Ana'\nn2,'Luis'\nedgedef>node1 VARCHAR,node2 VARCHAR,label VARCHAR\n"
              "n1,n2,'conoce'\n", "gdf", ("Ana", "conoce", "Luis")),
    ("g.dl", "DL N=3 FORMAT=EDGELIST1\nLABELS:\nAna,Luis,Eva\nDATA:\nAna Luis 1\nLuis Eva 1\n", "ucinet-dl",
     ("Ana", "relacionado_con", "Luis")),
    ("g.tgf", "1 Ana\n2 Luis\n#\n1 2 conoce\n", "tgf", ("Ana", "conoce", "Luis")),
    ("g.sif", "TP53\tpp\tMDM2\tATM\n", "sif", ("TP53", "pp", "ATM")),
    ("g.edgelist", "Ana Luis 2\nLuis Eva {'weight': 1}\n", "lista-de-aristas", ("Ana", "relacionado_con", "Luis")),
    ("g.nq", '<http://ex.org/a> <http://ex.org/conoce> <http://ex.org/b> <http://ex.org/grafo1> .\n',
     "rdf", ("a", "conoce", "b")),
    ("g.trig", '@prefix ex: <http://ex.org/> .\nex:g1 { ex:Ana ex:trabajaEn ex:Gestoria . ex:Ana ex:edad 40 . }\n',
     "rdf", ("Ana", "trabaja_en", "Gestoria")),
    ("bn.ttl", '@prefix ex: <http://ex.org/> .\nex:Ana ex:vive [ ex:ciudad ex:Bilbao ; ex:cp "48001" ] ; ex:altura 1.72 .\n',
     "rdf", ("Ana", "vive_ciudad", "Bilbao")),
    ("g.cypher", "CREATE (a:Persona {name: 'Ana (jefa)', edad: 40})-[:DIRIGE {desde: 2020}]->(o:Oficina {name: 'Bilbao'}),\n"
                 "(l:Persona {name: 'Luis'})-[:TRABAJA_EN]->(o);\n"
                 "MATCH (x:Persona {name: 'Luis'}), (y:Persona {name: 'Ana (jefa)'}) CREATE (x)<-[:SUPERVISA]-(y);\n",
     "cypher", ("Ana (jefa)", "supervisa", "Luis")),
    ("apoc.cypher", ":begin\nCREATE (:Cliente:`UNIQUE IMPORT LABEL` {name:\"Bar Pepe\", `UNIQUE IMPORT ID`:0});\n"
                    "CREATE (:Producto:`UNIQUE IMPORT LABEL` {name:\"Cafe\", `UNIQUE IMPORT ID`:1});\n:commit\n:begin\n"
                    "MATCH (n1:`UNIQUE IMPORT LABEL`{`UNIQUE IMPORT ID`:0}), (n2:`UNIQUE IMPORT LABEL`{`UNIQUE IMPORT ID`:1}) "
                    "CREATE (n1)-[r:COMPRA]->(n2);\n:commit\n", "cypher", ("Bar Pepe", "compra", "Cafe")),
    ("movies.cypher", "CREATE (TheMatrix:Movie {title:'The Matrix', released:1999})\n"
                      "CREATE (Keanu:Person {name:'Keanu Reeves', born:1964})\n"
                      "CREATE\n(Keanu)-[:ACTED_IN {roles:['Neo']}]->(TheMatrix)\n"
                      "WITH TheMatrix as m\nMATCH (p:Person)-[:ACTED_IN]->(m) RETURN p\n", "cypher",
     ("Keanu Reeves", "acted_in", "The Matrix")),
    ("f.mmd", "flowchart LR\n  A[Presupuesto] -->|aprobado| B(Pedido)\n  B --> C{Factura} & D[Albaran]\n"
              "  C -- se cobra --> E((Banco))\n", "mermaid", ("Factura", "se_cobra", "Banco")),
    ("er.mmd", "erDiagram\n  CLIENTE ||--o{ PEDIDO : hace\n", "mermaid", ("CLIENTE", "hace", "PEDIDO")),
    ("c.puml", "@startuml\nclass Factura\nFactura --> Cliente : emitida a\n[Web] -down-> [API]\n@enduml\n", "plantuml",
     ("Factura", "emitida_a", "Cliente")),
    ("flechas.txt", "Cliente -> Pedido\nPedido --incluye--> Producto\nAlmacen <- Producto\n", "flechas",
     ("Pedido", "incluye", "Producto")),
    ("adj.txt", "Ventas: Clientes, Facturas\nFacturas: IVA\n", "adyacencia", ("Ventas", "relacionado_con", "Clientes")),
    ("esquema.md", "# Empresa\n## Ventas\n- Clientes\n  - Bar Pepe\n- Facturas\n", "esquema", ("Clientes", "contiene", "Bar Pepe")),
    ("nota.md", "Python es un lenguaje. Ver [[Django]].", "markdown-wiki", ("nota", "enlaza_con", "Django")),
]


@pytest.mark.parametrize("name,data,fmt,triple", SAMPLES, ids=[s[0] for s in SAMPLES])
def test_every_format(name, data, fmt, triple):
    g = kg_io.parse_bytes(data.encode("utf-8"), name)
    assert g["format"] == fmt, g["format"]
    assert triple in edges_of(g), sorted(edges_of(g))
    assert all(n["name"] for n in g["nodes"])


def test_queries_and_documents_do_not_become_nodes():
    g = kg_io.parse_bytes(SAMPLES[[s[0] for s in SAMPLES].index("movies.cypher")][1].encode(), "m.cypher")
    assert {n["name"] for n in g["nodes"]} == {"The Matrix", "Keanu Reeves"}   # MATCH (p:Person) ... RETURN no crea p
    for data, name in ((b"FROM python:3.11\nRUN pip install --no-cache-dir x\n", "Dockerfile"),
                       (b"def f(x):\n    return x -> y\n", "codigo.py")):
        with pytest.raises(GraphFormatError):
            kg_io.parse_bytes(data, name)


def test_xmind_and_canvas_notes():
    xmind = zipped({"content.json": json.dumps([{"title": "Hoja", "rootTopic": {"title": "Negocio", "children": {
        "attached": [{"title": "Clientes"}, {"title": "Proveedores", "children": {"attached": [{"title": "Acme"}]}}]}}}]),
        "metadata.json": "{}", "manifest.json": "{}"})
    g = kg_io.parse_bytes(xmind, "mapa.xmind")
    assert g["format"] == "xmind" and ("Proveedores", "contiene", "Acme") in edges_of(g)
    g = kg_io.parse_bytes(SAMPLES[[s[0] for s in SAMPLES].index("mapa.canvas")][1].encode(), "mapa.canvas")
    assert "Impuesto indirecto" in node(g, "IVA")["note"]


def test_content_wins_over_extension():
    # un GraphML con extension .txt, un JSON llamado .dat y un Turtle sin extension
    gml = SAMPLES[[s[0] for s in SAMPLES].index("yed.graphml")][1].encode()
    assert kg_io.parse_bytes(gml, "exportado.txt")["format"] == "graphml"
    assert kg_io.parse_bytes(b'{"nodes":[{"id":1}],"edges":[{"source":1,"target":2}]}', "x.dat")["format"] == "json"
    assert kg_io.parse_bytes(b"@prefix ex: <http://e/> .\nex:a ex:b ex:c .", "")["format"] == "rdf"
    assert detect("digraph G { a -> b }") == "dot" and detect("graph TD\nA-->B") == "mermaid"
    assert detect('graph [ node [ id 1 ] ]') == "gml"


def test_attributes_go_to_the_note_and_reach_the_agents():
    g = kg_io.parse_bytes(SAMPLES[[s[0] for s in SAMPLES].index("neo.json")][1].encode(), "neo.json")
    assert "edad: 40" in node(g, "Ana")["note"] and node(g, "Ana")["type"] == "person"
    g = kg_io.parse_bytes(SAMPLES[[s[0] for s in SAMPLES].index("g.jsonld")][1].encode(), "g.jsonld")
    assert "elevation: 3718" in node(g, "Teide")["note"] and node(g, "Teide")["type"] == "mountain"
    db, graph, mem, learner, org = make()
    kg_io.insert_graph(graph, g, "")
    assert any("3718" in n for n in graph.general().notes_about(["Teide"]))
    ctx = learner.build_context("¿Cuanto mide el Teide?")
    assert any("3718" in f for f in ctx["facts"]) and any("Teide" in f and "Tenerife" in f for f in ctx["facts"])


def zipped(files: dict[str, str]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, t in files.items():
            z.writestr(n, t)
    return buf.getvalue()


def test_nodes_and_edges_tables_together():
    nodes = "id,label,type,ciudad\n1,Ana,persona,Bilbao\n2,Gestoria Lopez,empresa,Getxo\n"
    edges = "source,target,label\n1,2,trabaja en\n"
    for g in (kg_io.parse_bytes(zipped({"red/nodes.csv": nodes, "red/edges.csv": edges}), "red.zip"),
              kg_io.parse_many([("edges.csv", edges.encode()), ("nodes.csv", nodes.encode())])):
        assert ("Ana", "trabaja_en", "Gestoria Lopez") in edges_of(g)
        assert "ciudad: Bilbao" in node(g, "Ana")["note"] and node(g, "Ana")["type"] == "persona"
    neo = kg_io.parse_many([("people.csv", b"personId:ID,name,:LABEL\np1,Ana,Persona\np2,Luis,Persona\n"),
                            ("rels.csv", b":START_ID,:END_ID,:TYPE\np1,p2,CONOCE\n")])
    assert ("Ana", "conoce", "Luis") in edges_of(neo)


def test_compressed_spreadsheet_sqlite_and_archives(tmp_path):
    ttl = b"@prefix ex: <http://e/> .\nex:Teide ex:estaEn ex:Tenerife .\n"
    assert ("Teide", "esta_en", "Tenerife") in edges_of(kg_io.parse_bytes(gzip.compress(ttl), "g.ttl.gz"))
    assert ("Teide", "esta_en", "Tenerife") in edges_of(kg_io.parse_bytes(bz2.compress(ttl), "g.ttl.bz2"))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        info = tarfile.TarInfo("grafos/g.ttl")
        info.size = len(ttl)
        t.addfile(info, io.BytesIO(ttl))
    assert ("Teide", "esta_en", "Tenerife") in edges_of(kg_io.parse_bytes(buf.getvalue(), "g.tgz"))
    # Excel con una hoja de nodos y otra de aristas
    xlsx = zipped({"xl/workbook.xml": "<workbook/>",
                   "xl/sharedStrings.xml": "<sst>" + "".join(f"<si><t>{x}</t></si>" for x in
                                                            ["id", "name", "a", "Ana", "b", "Luis", "source", "target",
                                                             "relation", "conoce"]) + "</sst>",
                   "xl/worksheets/sheet1.xml": '<worksheet><sheetData>' + "".join(
                       "<row>" + "".join(f'<c t="s"><v>{i}</v></c>' for i in r) + "</row>" for r in [(0, 1), (2, 3), (4, 5)])
                   + "</sheetData></worksheet>",
                   "xl/worksheets/sheet2.xml": '<worksheet><sheetData>' + "".join(
                       "<row>" + "".join(f'<c t="s"><v>{i}</v></c>' for i in r) + "</row>" for r in [(6, 7, 8), (2, 4, 9)])
                   + "</sheetData></worksheet>"})
    g = kg_io.parse_bytes(xlsx, "red.xlsx")
    assert g["format"] == "excel" and ("Ana", "conoce", "Luis") in edges_of(g)
    # SQLite con tablas de nodos y aristas
    db = tmp_path / "red.db"
    con = sqlite3.connect(db)
    con.executescript("CREATE TABLE nodes(id TEXT, name TEXT); INSERT INTO nodes VALUES('1','Ana'),('2','Luis');"
                      "CREATE TABLE edges(source TEXT, target TEXT, relation TEXT); INSERT INTO edges VALUES('1','2','conoce');")
    con.commit()
    con.close()
    g = kg_io.parse_bytes(db.read_bytes(), "red.db")
    assert g["format"] == "sqlite" and ("Ana", "conoce", "Luis") in edges_of(g)
    # un ZIP con grafos de formatos distintos y notas [[enlazadas]]
    g = kg_io.parse_bytes(zipped({"a.graphml": SAMPLES[[s[0] for s in SAMPLES].index("yed.graphml")][1],
                                  "b.mmd": "graph TD\nX-->Y\n", "notas/IVA.md": "Ver [[Modelo 303]]", "foto.png": "x"}),
                          "todo.zip")
    assert {("Pedido", "sale_de", "Almacen"), ("X", "relacionado_con", "Y"), ("IVA", "enlaza_con", "Modelo 303")} <= edges_of(g)
    assert len(g["files"]) == 3


def test_other_nexus_office_database_keeps_scopes(tmp_path):
    from nexus.db import DB
    from nexus.memory import KnowledgeGraph
    src = DB(tmp_path / "otro.db")
    g0 = KnowledgeGraph(src)
    g0.for_agent("programador").relate("Django", "usa", "Python")
    g0.general().relate("Usuario", "vive_en", "Bilbao", "persona", "lugar")
    src.close()
    g = kg_io.parse_bytes((tmp_path / "otro.db").read_bytes(), "nexus.db")
    assert g["format"] == "nexus-kg"
    db, graph, *_, org = make()
    kg_io.insert_graph(graph, g, "auto", org, ["programador"])
    assert "django" in names(db, "programador") and "bilbao" in names(db, "")


def test_merge_with_existing_graph_reports_new_and_merged():
    db, graph, mem, learner, org = make()
    graph.general().relate("Python", "creado_por", "Guido")
    g = kg_io.parse_bytes(b"source,target,relation\nPython,Guido,creado por\nDjango,Python,usa\n", "x.csv")
    res = kg_io.insert_graph(graph, g, "")
    assert res["merged"] == 2 and res["new"] == 1 and res["relations"] == 2
    w = db.query("SELECT r.weight FROM relations r JOIN entities a ON a.id=r.src WHERE a.key='python' AND r.rel='creado_por'")
    assert w[0]["weight"] >= 2   # la relacion repetida se refuerza, no se duplica


def test_errors_are_clear():
    for data, name in ((b"", "x.json"), (b"%PDF-1.4 xxx", "a.pdf"), (b"<html><body>hola</body></html>", "p.html"),
                       (b"\x00\x01\x02\x03" * 100, "g.bin"), (zipped({"word/document.xml": "<w/>"}), "carta.docx")):
        with pytest.raises(GraphFormatError):
            kg_io.parse_bytes(data, name)


def test_zip_documents_are_not_mistaken_for_graphs():
    assert not kg_io.looks_like_graph("package.json", b'{"name": "app", "dependencies": {"x": "1"}}')
    assert not kg_io.looks_like_graph("datos.csv", b"volcan;altura\nTeide;3718\n")
    assert kg_io.looks_like_graph("red.gml", b'graph [ node [ id 1 label "a" ] node [ id 2 ] edge [ source 1 target 2 ] ]')
    assert kg_io.looks_like_graph("aristas.csv", b"source,target\na,b\n")


def test_agent_tool_imports_from_sandbox(services, tmp_path):
    from nexus import paths
    from nexus.tools.builtin.core_tools import graph_import

    class Ctx:
        agent = "programador" if services.agents.get("programador") else ""
        emit = None
    Ctx.services = services
    (paths.sandbox_dir() / "red.mmd").write_text("graph LR\nFastAPI-->Starlette\n", encoding="utf-8")
    res = graph_import({"path": "red.mmd", "target": "agente"}, Ctx)
    assert res["nodes"] == 2 and res["relations"] == 1 and res["format"] == "mermaid"
    res = graph_import({"content": "Teide -> Tenerife : esta en", "name": "t.txt"}, Ctx)
    assert res["relations"] == 1
    assert any("Tenerife" in f for f in services.graph.general().facts_about(["Teide"]))
    with pytest.raises(ValueError):
        graph_import({"path": "../fuera.csv"}, Ctx)


def test_api_imports_several_files_at_once(services):
    from fastapi.testclient import TestClient
    from nexus.server import create_app
    from test_knowledge_org import _wait
    with TestClient(create_app(services)) as c:
        h = {"X-Admin-Token": c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]}
        fmts = c.get("/api/kg/formats").json()
        assert any(f["name"] == "Cypher" for f in fmts["formats"]) and ".graphml" in fmts["extensions"]
        r = c.post("/api/kg/import", files=[("files", ("nodes.csv", b"id,name\n1,Ana\n2,Luis\n")),
                                            ("files", ("edges.csv", b"source,target,label\n1,2,conoce\n"))],
                   data={"target": ""}, headers=h)
        assert r.status_code == 200, r.text
        j = _wait(c, r.json()["id"], h)
        assert j["status"] == "completado" and j["result"]["relations"] == 1, j
        assert c.post("/api/kg/import", files={"file": ("g.bin", b"\x00\x01")}, headers=h).status_code == 400
