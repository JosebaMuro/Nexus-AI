"""Documentos sueltos en muchos formatos e importacion de varios a la vez (en paralelo)."""
import io
import time
import zipfile
from pathlib import Path

import numpy as np

from nexus import parallel
from nexus.memory.embeddings import HashingEmbedder
from nexus.rag import readers


def zipdoc(path: Path, files: dict[str, str]) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for n, t in files.items():
            z.writestr(n, t)
    return path


def make_samples(d: Path) -> dict[str, Path]:
    out = {}
    out["odt"] = zipdoc(d / "informe.odt", {
        "content.xml": '<office:document-content><office:body><office:text><text:h>Informe</text:h>'
                       '<text:p>El volcán Teide<text:s/>mide 3718 metros.</text:p></office:text></office:body></office:document-content>',
        "meta.xml": "<office:meta><dc:title>Informe volcánico</dc:title></office:meta>"})
    out["pptx"] = zipdoc(d / "charla.pptx", {
        "ppt/slides/slide1.xml": "<p:sld><a:p><a:r><a:t>Diapositiva uno: introducción</a:t></a:r></a:p></p:sld>",
        "ppt/slides/slide2.xml": "<p:sld><a:p><a:r><a:t>Diapositiva dos: erupciones de La Palma</a:t></a:r></a:p></p:sld>",
        "ppt/slides/slide10.xml": "<p:sld><a:p><a:r><a:t>Diapositiva diez: conclusiones</a:t></a:r></a:p></p:sld>"})
    out["xlsx"] = zipdoc(d / "datos.xlsx", {
        "xl/sharedStrings.xml": "<sst><si><t>Volcán</t></si><si><t>Altura</t></si><si><t>Teide</t></si></sst>",
        "xl/worksheets/sheet1.xml": '<worksheet><sheetData><row><c t="s"><v>0</v></c><c t="s"><v>1</v></c></row>'
                                    '<row><c t="s"><v>2</v></c><c><v>3718</v></c></row></sheetData></worksheet>'})
    out["epub"] = zipdoc(d / "libro.epub", {
        "META-INF/container.xml": '<container><rootfiles><rootfile full-path="OEBPS/content.opf"/></rootfiles></container>',
        "OEBPS/content.opf": '<package><metadata><dc:title>Libro de volcanes</dc:title></metadata><manifest>'
                             '<item id="c2" href="cap2.xhtml"/><item id="c1" href="cap1.xhtml"/></manifest>'
                             '<spine><itemref idref="c1"/><itemref idref="c2"/></spine></package>',
        "OEBPS/cap1.xhtml": "<html><body><h1>Capítulo 1</h1><p>Los volcanes de Canarias son de origen oceánico y muy antiguos.</p></body></html>",
        "OEBPS/cap2.xhtml": "<html><body><h1>Capítulo 2</h1><p>El Teide es el pico más alto de España con 3718 metros.</p></body></html>"})
    rtf = d / "nota.rtf"
    rtf.write_text(r"{\rtf1\ansi{\fonttbl{\f0 Arial;}}\f0 Nota sobre el volc\'e1n\par Segunda l\u237?nea}", encoding="latin-1")
    out["rtf"] = rtf
    csv = d / "tabla.csv"
    csv.write_text("volcan;altura\nTeide;3718\n", encoding="utf-8")
    out["csv"] = csv
    return out


def test_readers(tmp_path):
    s = make_samples(tmp_path)
    title, pages = readers.read_document(s["odt"])
    assert title == "Informe volcánico" and "El volcán Teide mide 3718 metros." in pages[0]
    _, pages = readers.read_document(s["pptx"])
    assert len(pages) == 3 and "conclusiones" in pages[2] and "La Palma" in pages[1]   # orden natural 1, 2, 10
    _, pages = readers.read_document(s["xlsx"])
    assert pages == ["Volcán\tAltura\nTeide\t3718"]
    title, pages = readers.read_document(s["epub"])
    assert title == "Libro de volcanes" and "Canarias" in pages[0] and "3718" in pages[1]
    _, pages = readers.read_document(s["rtf"])
    assert "Nota sobre el volcán" in pages[0] and "Segunda línea" in pages[0] and "Arial" not in pages[0]
    assert readers.supported("x.tar.gz") and readers.supported("a.RAR") and readers.supported("b.kt")
    assert not readers.supported("foto.png") and readers.archive_suffix("doc.tar.bz2") == ".tar.bz2"


def test_ingest_many_parallel_and_errors(services, tmp_path):
    s = make_samples(tmp_path)
    bad = tmp_path / "roto.pdf"
    bad.write_bytes(b"%PDF-1.4 esto no es un pdf")
    z = tmp_path / "proyecto.zip"
    with zipfile.ZipFile(z, "w") as zz:
        zz.writestr("leeme.md", "Proyecto de prueba sobre volcanes y erupciones históricas.")
    items = [(p, p.name) for p in s.values()] + [(bad, "roto.pdf"), (z, "proyecto.zip")]
    res = services.rag.ingest_many(items)
    assert [r["name"] for r in res] == [n for _, n in items]                  # mismo orden que la entrada
    ok = {r["name"]: r["doc"] for r in res if "doc" in r}
    assert set(ok) == {p.name for p in s.values()} | {"proyecto.zip"}
    assert "error" in res[-2] and ok["proyecto.zip"]["meta"]["sandbox"] == "proyectos/proyecto"
    hits = services.rag.search("altura del Teide 3718", k=8)
    assert {h["title"] for h in hits} & {"Informe volcánico", "Libro de volcanes", "datos"}


def test_import_endpoint(services, tmp_path):
    from fastapi.testclient import TestClient
    from nexus.server.app import create_app
    s = make_samples(tmp_path)
    with TestClient(create_app(services)) as c:
        files = [("files", (p.name, p.read_bytes(), "application/octet-stream")) for p in s.values()]
        files.append(("files", ("foto.png", b"\x89PNG", "image/png")))
        r = c.post("/api/rag/import", files=files)
        assert r.status_code == 400 and "formato no admitido" in r.json()["detail"]
        r = c.post("/api/rag/import", files=files[:-1])
        assert r.status_code == 200, r.text
        assert r.json()["ok"] == len(s) and r.json()["failed"] == 0
        f = c.get("/api/rag/formats").json()
        assert ".epub" in f["documents"] and ".rar" in f["archives"] and "rar" in f["extractors"]


def test_hashing_embedder_parallel_matches_serial():
    texts = [f"documento numero {i} sobre volcanes, erupciones y lava {i % 7}" for i in range(600)]
    e = HashingEmbedder()
    serial = e._embed(texts)
    par = e.embed(texts)
    assert par.shape == serial.shape and np.allclose(par, serial, atol=1e-6)


def test_parallel_helpers():
    assert parallel.split(list(range(10)), 3) == [[0, 1, 2, 3], [4, 5, 6], [7, 8, 9]]
    assert parallel.map_cpu(abs, [-1, -2, 3]) == [1, 2, 3]
    assert parallel.cpu_count() >= 1


def test_crawler_parallel_is_faster_and_identical(monkeypatch):
    """Con latencia de red simulada, varias conexiones a la vez dan el mismo resultado en mucho menos tiempo."""
    from nexus.rag import crawler
    base = "https://docs.lentos.example"
    long = "Texto de documentacion suficientemente largo para que la pagina cuente como contenido util. " * 4

    def site_fetch(url):
        time.sleep(0.05)
        path = url[len(base):] or "/"
        if path in ("/robots.txt", "/sitemap.xml"):
            return 404, "text/plain", "", url
        n = int(path.strip("/").split("/")[-1].replace(".html", "") or 0) if path != "/docs/" else 0
        links = "".join(f'<a href="/docs/{k}.html">{k}</a>' for k in range(n * 4 + 1, n * 4 + 5) if k < 40)
        return 200, "text/html", f"<html><head><title>P{n}</title></head><body><nav>{links}</nav><main><p>{n} {long}</p></main></body></html>", url

    t0 = time.time()
    one = crawler.crawl(f"{base}/docs/", max_pages=30, max_depth=4, fetch=site_fetch, delay=0, connections=1)
    t1 = time.time()
    many = crawler.crawl(f"{base}/docs/", max_pages=30, max_depth=4, fetch=site_fetch, delay=0, connections=8)
    t2 = time.time()
    assert [p["url"] for p in one["pages"]] == [p["url"] for p in many["pages"]] and len(many["pages"]) == 30
    assert (t2 - t1) < (t1 - t0) / 2.5, (t1 - t0, t2 - t1)
