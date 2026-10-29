"""IA de apoyo con embeddings: seleccion de herramientas, enrutado, correcciones y resumen semantico.

Se prueba con un embedder de juguete «multilingue» (palabras de varios idiomas con el mismo significado van a la misma
dimension), que hace el papel de Qwen3-Embedding sin descargar el modelo."""
from types import SimpleNamespace

import numpy as np

from nexus.memory.assist import SemanticAssist, chunks
from nexus.tools.base import ToolSpec

CONCEPTS = [
    {"imagen", "image", "irudia", "foto", "dibujo", "draw", "picture", "pinta", "generate"},
    {"musica", "music", "cancion", "song", "abestia", "compose", "componer", "melodia"},
    {"buscar", "search", "busca", "internet", "web", "bilatu", "noticias", "news"},
    {"calcular", "calculate", "suma", "kalkulatu", "cuanto", "math", "calculator", "expresion"},
    {"recordar", "memory", "memoria", "remember", "gogoratu", "recuerdo"},
    {"bolsa", "stock", "accion", "acciones", "cotizacion", "burtsa", "price", "precio", "shares"},
    {"factura", "invoice", "contabilidad", "accounting", "faktura", "iva", "impuestos", "tax"},
    {"desenfocar", "blur", "lausotu", "difuminar", "desenfoque"},
    {"recortar", "crop", "moztu", "cortar"},
    {"fecha", "hora", "date", "time", "ordua", "datetime"},
    {"delegar", "delegate", "agente", "agent"},
    {"fichero", "file", "archivo", "fitxategia", "leer", "read"},
    {"biblioteca", "documentos", "library", "documents", "liburutegia"},
]


class ToyEmbedder:
    id = "toy-multi"
    slow = False

    def _vec(self, text):
        v = np.zeros(len(CONCEPTS) + 1, np.float32)
        for w in __import__("re").findall(r"\w+", text.lower()):
            for i, c in enumerate(CONCEPTS):
                if w in c:
                    v[i] += 1
        v[-1] = 0.15 * max(1.0, float(v.sum()))     # componente comun proporcional: textos neutros, parecido parejo
        return v / np.linalg.norm(v)

    def embed(self, texts):
        return np.stack([self._vec(t) for t in texts])

    def embed_query(self, texts, task="search"):
        return self.embed(texts)


def tool(name, desc, props=None, req=None, risk="low"):
    return ToolSpec(name=name, description=desc, parameters={"type": "object", "properties": props or {"q": {"type": "string"}},
                                                             "required": req if req is not None else ["q"]}, risk=risk)


TOOLS = [tool("generate_image", "Genera una imagen o dibujo a partir de un texto"),
         tool("compose_music", "Compone musica, una cancion o melodia"),
         tool("web_search", "Busca en internet noticias y paginas web"),
         tool("calculator", "Calcula una expresion matematica"),
         tool("memory_search", "Busca en la memoria y recuerdos"),
         tool("memory_save", "Guarda un recuerdo en la memoria"),
         tool("bolsa_cotizacion", "Precio y cotizacion de una accion en bolsa"),
         tool("factura_crear", "Crea una factura con IVA para la contabilidad"),
         tool("imagen_editar", "Edita una foto: desenfocar, recortar", {"operacion": {"type": "string", "enum": ["desenfocar", "recortar"]}}, []),
         tool("datetime_now", "Fecha y hora actuales"),
         tool("delegate", "Delega en otro agente"),
         tool("file_read", "Lee un fichero o archivo"),
         tool("rag_search", "Busca en la biblioteca de documentos")]


def make(semantic=True, max_tools=5):
    emb = ToyEmbedder() if semantic else SimpleNamespace(id="hash-768")
    cfg = {"assist.max_tools": max_tools}
    return SemanticAssist(lambda: emb, SimpleNamespace(get=lambda k, d=None: cfg.get(k, d)))


def test_select_tools_por_significado_en_varios_idiomas():
    a = make()
    for text, want in [("dibuja un gato", "generate_image"), ("draw me a picture of a cat", "generate_image"),
                       ("marrazkia: irudia sortu", "generate_image"), ("¿a cuánto cotizan las acciones de SAN?", "bolsa_cotizacion"),
                       ("make an invoice with tax", "factura_crear"), ("compose a song", "compose_music")]:
        names = [t.name for t in a.select_tools(text, TOOLS)]
        assert want in names, (text, names)
        assert "memory_search" in names and "delegate" in names        # las basicas, siempre
        assert len(names) < len(TOOLS)
    assert a.last["of"] == len(TOOLS)


def test_sin_embedder_semantico_no_se_filtra_nada():
    a = make(semantic=False)
    assert a.select_tools("dibuja un gato", TOOLS) == TOOLS
    assert a.fix_tool_name("search_web", TOOLS) is None
    assert a.condense("x", "a " * 10000) == "a " * 10000


def test_pocas_herramientas_no_se_filtran():
    assert make(max_tools=20).select_tools("dibuja", TOOLS) == TOOLS


def test_corrige_nombres_y_valores():
    a = make()
    assert a.fix_tool_name("rag-search", TOOLS) == "rag_search"            # mismo nombre con otro separador
    assert a.fix_tool_name("calculate", TOOLS) == "calculator"            # por significado
    assert a.fix_tool_name("zzz", TOOLS) is None
    assert a.fix_enum("blur", ["desenfocar", "recortar"]) == "desenfocar"
    assert a.fix_enum("crop", ["desenfocar", "recortar"]) == "recortar"
    assert a.fix_enum("RECORTAR", ["desenfocar", "recortar"]) == "recortar"
    args, changes = a.repair_args(TOOLS[8].parameters, {"operacion": "lausotu"})
    assert args["operacion"] == "desenfocar" and changes


def test_enrutado_semantico_relativo():
    a = make()
    agents = [SimpleNamespace(id="contable", name="Contable", description="facturas e impuestos", specialty="contabilidad, IVA", keywords=["factura"]),
              SimpleNamespace(id="musico", name="Músico", description="compone canciones", specialty="musica", keywords=[]),
              SimpleNamespace(id="bolsa", name="Bolsa", description="acciones y cotizaciones", specialty="bolsa", keywords=[])]
    b = a.route_bonus("I need an invoice with the tax", agents)
    assert list(b) == ["contable"] and b["contable"] == 3.0
    assert a.route_bonus("hola", agents) == {}            # nadie destaca: no se toca


def test_condense_se_queda_con_lo_relevante():
    a = make()
    filler = "\n\n".join(f"Parrafo {i} sobre el tiempo y la fecha de hoy." for i in range(60))
    text = "Cabecera del documento.\n\n" + filler + "\n\nLa factura lleva IVA del 21 % y se paga a 30 dias.\n\n" + filler
    out = a.condense("¿qué IVA lleva la factura?", text, budget=600)
    assert "IVA del 21 %" in out and "Cabecera" in out and len(out) < 1200 and "[…]" in out
    res = a.condense_result("factura iva", {"text": text, "url": "x"}, 600)
    assert "IVA del 21 %" in res["text"] and res["url"] == "x"


def test_guess_tool_sin_llm():
    a = make()
    assert a.guess_tool("busca noticias en internet", TOOLS) == ("web_search", {"q": "busca noticias en internet"})
    assert a.guess_tool("hola que tal", TOOLS) is None


def test_rerank_y_chunks():
    a = make()
    items = [{"t": "receta de cocina"}, {"t": "cotizacion de acciones en bolsa"}, {"t": "musica"}]
    assert a.rerank("stock price", items, key=lambda x: x["t"])[0]["t"].startswith("cotizacion")
    parts = chunks("uno. " * 400, 300)
    assert all(len(p) <= 450 for p in parts) and len(parts) > 3


def test_supervisor_corrige_herramienta_inventada(tmp_path, monkeypatch):
    from nexus.tools.supervisor import Supervisor
    calls = []
    reg = SimpleNamespace(tools={}, get=lambda n: reg.tools.get(n))
    t = tool("calculator", "Calcula una expresion matematica", {"expr": {"type": "string"}}, ["expr"])
    t.func = lambda args, ctx: calls.append(args) or {"value": 4}
    reg.tools["calculator"] = t
    db = SimpleNamespace(execute=lambda *a, **k: None)
    cfg = SimpleNamespace(get=lambda k, d=None: {"tools.policy": {"low": "auto"}}.get(k, d))
    sup = Supervisor(reg, db, cfg)
    sup.assist = make()
    from nexus.tools.base import ToolContext
    r = sup.execute("calculate", {"expr": "2+2"}, ToolContext(agent="nexus"))
    assert r["value"] == 4 and "calculate -> calculator" in r["_corregido"] and calls == [{"expr": "2+2"}]
