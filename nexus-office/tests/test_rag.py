from pathlib import Path

from pypdf import PdfReader

from nexus.rag import RAGStore, chunk_text, extract_html
from nexus.rag.pdfgen import build_pdf
from nexus.rag.url_pdf import assimilate, extractive_summary

HTML = """<!doctype html><html lang="es"><head><title>Guía de volcanes | GeoBlog</title>
<meta name="description" content="Todo sobre volcanes activos y erupciones">
<meta name="author" content="Ane Etxeberria"><meta property="og:site_name" content="GeoBlog"></head>
<body><nav><a href="/">Inicio</a><a href="/blog">Blog</a></nav>
<article><h1>Guía de volcanes</h1>
<p>Los volcanes son aberturas de la corteza terrestre por donde sale magma, gases y ceniza. Su estudio se llama vulcanología.</p>
<h2>Tipos de volcanes</h2><p>Existen volcanes en escudo, estratovolcanes y conos de ceniza, cada uno con erupciones distintas.</p>
<ul><li>Escudo: lava fluida, como en Hawái.</li><li>Estratovolcán: capas alternas, como el Fuji.</li></ul>
<h2>Datos</h2><table><tr><th>Volcán</th><th>País</th></tr><tr><td>Etna</td><td>Italia</td></tr><tr><td>Teide</td><td>España</td></tr></table>
<pre>altura = 3718  # metros del Teide</pre>
<blockquote>La tierra respira por sus volcanes.</blockquote>
<img src="/img/etna.jpg" alt="El Etna en erupción">
<p>Más información en <a href="https://ejemplo.org/volcanes">el observatorio</a>.</p></article>
<footer><p>Copyright GeoBlog, todos los derechos reservados en todas partes.</p></footer></body></html>"""


def test_extract_html_structure():
    d = extract_html(HTML, "https://geoblog.example/volcanes")
    assert d["title"] == "Guía de volcanes | GeoBlog" and d["author"] == "Ane Etxeberria" and d["site"] == "GeoBlog"
    heads = [s["heading"] for s in d["sections"]]
    assert "Tipos de volcanes" in heads and "Datos" in heads
    kinds = {b["type"] for s in d["sections"] for b in s["blocks"]}
    assert {"p", "list", "table", "code", "quote"} <= kinds
    assert d["images"][0]["src"] == "https://geoblog.example/img/etna.jpg"
    assert any(lk["href"] == "https://ejemplo.org/volcanes" for lk in d["links"])
    assert "Copyright" not in d["text"] and "Inicio" not in d["text"]   # sin pie ni navegacion


def test_chunking_overlap():
    text = "\n\n".join(f"Parrafo {i}. " + "palabra " * 60 for i in range(20))
    chunks = chunk_text(text, 900, 150)
    assert len(chunks) > 3 and all(len(c) <= 1100 for c in chunks)


def test_pdf_ingest_search_and_assimilate(services, tmp_path):
    d = extract_html(HTML, "https://geoblog.example/volcanes")
    summary, points = extractive_summary(d["text"])
    out = tmp_path / "volcanes.pdf"
    info = build_pdf(d, summary, points, out, max_images=0)
    assert out.exists() and info["pages"] >= 3
    text = "\n".join(p.extract_text() for p in PdfReader(str(out)).pages)
    assert "Tipos de volcanes" in text and "Teide" in text and "Resumen" in text
    rag: RAGStore = services.rag
    doc = rag.ingest_file(out, title=d["title"], source=d["url"])
    assert doc["chunks"] >= 1 and Path(doc["path"]).exists()
    hits = rag.search("¿qué tipos de volcanes existen?", k=3)
    assert hits and hits[0]["doc_id"] == doc["id"]
    n = assimilate(services, d, summary, points, doc["id"], d["url"])
    assert n > 0 and any("Guía de volcanes" in f for f in services.graph.facts_about(["Guía de volcanes"]))
    assert services.memory.recall("volcanes", k=3)
    # el agente recibe los fragmentos RAG y el nucleo sin modelo los cita
    r = services.agents.run("nexus", "¿qué tipos de volcanes existen?", services.memory.new_session())
    assert "biblioteca" in r["answer"].lower() and "volcan" in r["answer"].lower()


def test_url_to_pdf_intent():
    from nexus.llm.builtin import detect_intent
    assert detect_intent("Procesa https://ejemplo.org/articulo y hazme un pdf", {"url_to_pdf"}) == \
        ("url_to_pdf", {"url": "https://ejemplo.org/articulo"})
    assert detect_intent("mira https://ejemplo.org", {"url_to_pdf"}) is None
