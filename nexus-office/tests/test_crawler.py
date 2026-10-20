from pathlib import Path

import pytest

from nexus.rag import crawler

BASE = "https://docs.volcanes.example"


def page(title: str, body: str, links: list[str] = (), extra_head: str = "") -> str:
    nav = "".join(f'<a href="{h}">{h}</a>' for h in links)
    return (f"<!doctype html><html lang='es'><head><title>{title} — Docs de Volcanes</title>{extra_head}"
            f"<meta property='og:site_name' content='Docs de Volcanes'></head><body><nav>{nav}</nav>"
            f"<main><h1>{title}</h1><p>{body}</p></main></body></html>")


LONG = ("Este apartado explica con detalle como funciona la herramienta de medicion de volcanes, sus opciones "
        "de configuracion y los pasos que hay que seguir para obtener datos fiables en cada erupcion. ")

SITE = {
    "/robots.txt": (200, "text/plain", "User-agent: *\nDisallow: /guia/privado/\nSitemap: " + BASE + "/sitemap.xml\n"),
    "/sitemap.xml": (200, "application/xml", f"<urlset><url><loc>{BASE}/guia/apendice.html</loc></url>"
                                             f"<url><loc>{BASE}/blog/noticia.html</loc></url></urlset>"),
    "/guia/": (200, "text/html; charset=utf-8", page("Inicio", LONG + "Empieza por la instalacion.",
               ["instalacion.html", "/guia/api/medir.html#ejemplo", "/guia/privado/secreto.html", "/blog/noticia.html",
                "https://otro.example/guia/x.html", "logo.png", "/guia/copia.html?utm_source=x", "mailto:a@b.c"])),
    "/guia/instalacion.html": (200, "text/html", page("Instalacion", LONG + "Instala con pip install volcanes-medidor.",
                               ["/guia/", "/guia/api/medir.html"])),
    "/guia/api/medir.html": (200, "text/html", page("API medir", LONG + "La funcion medir_altura devuelve 3718 metros "
                             "para el Teide.", ["/guia/api/profundo.html"])),
    "/guia/api/profundo.html": (200, "text/html", page("Profundo", LONG + "Pagina a demasiada profundidad.", [])),
    "/guia/copia.html": (200, "text/html", page("Instalacion", LONG + "Instala con pip install volcanes-medidor.", [])),
    "/guia/apendice.html": (200, "text/html", page("Apendice", LONG + "Glosario de terminos de vulcanologia.", [])),
    "/guia/privado/secreto.html": (200, "text/html", page("Secreto", LONG + "No deberia leerse.", [])),
    "/blog/noticia.html": (200, "text/html", page("Noticia", LONG + "Fuera del ambito.", [])),
}


def fake_fetch(calls: list[str] | None = None, site: dict = SITE):
    def fetch(url: str):
        if calls is not None:
            calls.append(url)
        assert url.startswith(BASE), f"salio del sitio: {url}"
        path = url[len(BASE):].split("?")[0] or "/"
        if path not in site:
            return 404, "text/html", "", url
        status, ctype, body = site[path]
        return status, ctype, body, url
    return fetch


def test_scope_and_normalize():
    assert crawler.scope_prefix(f"{BASE}/guia/", "path") == "/guia/"
    assert crawler.scope_prefix(f"{BASE}/guia/index.html", "path") == "/guia/"
    assert crawler.scope_prefix(f"{BASE}/guia", "path") == "/guia/"
    assert crawler.scope_prefix(f"{BASE}/guia/", "domain") == "/"
    assert crawler.normalize(f"{BASE}/guia/index.html?utm_source=x&v=2#top") == f"{BASE}/guia/?v=2"
    start = f"{BASE}/guia/"
    assert crawler.in_scope(f"https://www.docs.volcanes.example/guia/a.html", start, "/guia/")
    assert not crawler.in_scope(f"{BASE}/blog/a.html", start, "/guia/")
    assert not crawler.in_scope(f"{BASE}/guia/logo.png", start, "/guia/")
    assert not crawler.in_scope(f"{BASE}/guia/_sources/a.txt", start, "/guia/")
    assert not crawler.in_scope("https://otro.example/guia/a.html", start, "/guia/")


def test_crawl_respects_scope_robots_depth_and_duplicates():
    calls: list[str] = []
    res = crawler.crawl(f"{BASE}/guia/", max_pages=20, max_depth=1, fetch=fake_fetch(calls), delay=0)
    urls = [p["url"] for p in res["pages"]]
    assert urls[0] == f"{BASE}/guia/"
    assert f"{BASE}/guia/instalacion.html" in urls and f"{BASE}/guia/api/medir.html" in urls
    assert f"{BASE}/guia/apendice.html" in urls                    # semilla del sitemap
    assert f"{BASE}/guia/api/profundo.html" not in urls            # profundidad 2 > 1
    assert not any("/blog/" in u or "privado" in u or "otro.example" in u for u in urls)
    skipped = dict(res["skipped"])
    assert skipped[f"{BASE}/guia/privado/secreto.html"] == "robots.txt"
    assert skipped[f"{BASE}/guia/copia.html"] == "contenido duplicado"
    assert not any(c.endswith((".png", "secreto.html", "noticia.html")) for c in calls)
    assert res["robots"] and res["prefix"] == "/guia/"


def test_crawl_page_limit_and_domain_scope():
    res = crawler.crawl(f"{BASE}/guia/", max_pages=2, max_depth=5, fetch=fake_fetch(), delay=0)
    assert len(res["pages"]) == 2 and res["pending"] > 0
    res = crawler.crawl(f"{BASE}/guia/", max_pages=50, max_depth=5, scope="domain", fetch=fake_fetch(), delay=0)
    assert f"{BASE}/blog/noticia.html" in [p["url"] for p in res["pages"]]


def test_crawl_follows_start_redirect_and_noindex():
    site = dict(SITE)
    site["/guia/"] = (200, "text/html", page("Oculta", LONG, ["/guia/instalacion.html"],
                                             "<meta name='robots' content='noindex'>"))

    def fetch(url):
        if url == f"{BASE}/docs":   # la portada redirige a la seccion real
            return fake_fetch(site=site)(f"{BASE}/guia/")[:3] + (f"{BASE}/guia/",)
        return fake_fetch(site=site)(url)

    res = crawler.crawl(f"{BASE}/docs", max_pages=5, max_depth=1, fetch=fetch, delay=0, use_sitemap=False)
    assert res["start"] == f"{BASE}/guia/" and res["prefix"] == "/guia/"
    assert [p["url"] for p in res["pages"]] == [f"{BASE}/guia/instalacion.html"]   # la portada es noindex


def test_crawl_site_indexes_learns_and_updates(services):
    res = crawler.crawl_site(services, f"{BASE}/guia/", max_pages=10, max_depth=1, fetch=fake_fetch(), delay=0)
    assert res["pages"] == 4 and res["title"] == "Documentación: Docs de Volcanes" and res["learned"] > 0
    doc = services.rag.get(res["rag_doc_id"])
    assert doc["kind"] == "web" and doc["meta"]["origin"] == "crawl" and doc["pages"] == 4
    md = Path(doc["path"])
    assert md.exists() and "Instala con pip install volcanes-medidor" in md.read_text(encoding="utf-8")
    hits = services.rag.search("altura del Teide medir_altura", k=3)
    assert hits and hits[0]["doc_id"] == doc["id"] and f"[{BASE}/guia/api/medir.html]" in hits[0]["text"]
    learned = [m for m in services.memory.list(kind="summary") if m["source"] == f"rag:{doc['id']}"]
    assert learned and "Documentación: Docs de Volcanes" in learned[0]["content"]
    # volver a rastrear actualiza el documento en lugar de duplicarlo
    res2 = crawler.crawl_site(services, f"{BASE}/guia/", max_pages=10, max_depth=1, fetch=fake_fetch(), delay=0)
    assert res2["replaced"] == [doc["id"]] and services.rag.get(doc["id"]) is None and not md.exists()
    assert len([d for d in services.rag.list() if d["kind"] == "web"]) == 1


def test_crawl_site_without_text_fails(services):
    site = {"/guia/": (200, "text/html", "<html><body><p>Hola</p></body></html>")}
    with pytest.raises(ValueError, match="ninguna pagina"):
        crawler.crawl_site(services, f"{BASE}/guia/", fetch=fake_fetch(site=site), delay=0)
    with pytest.raises(ValueError):
        crawler.crawl("ftp://x.example/", fetch=fake_fetch())


def test_crawl_intent():
    from nexus.llm.builtin import detect_intent
    tools = {"crawl_docs", "url_to_pdf"}
    assert detect_intent("Rastrea la documentación de https://docs.python.org/3/library/ y apréndela", tools) == \
        ("crawl_docs", {"url": "https://docs.python.org/3/library/", "scope": "path"})
    assert detect_intent("Aprende de la documentación de todo el dominio https://docs.x.org", tools)[1]["scope"] == "domain"
    assert detect_intent("Procesa https://ejemplo.org/articulo y hazme un pdf", tools)[0] == "url_to_pdf"


def test_crawl_api(services, monkeypatch):
    from fastapi.testclient import TestClient

    from nexus.server.app import create_app
    real = crawler.crawl_site
    monkeypatch.setattr(crawler, "crawl_site", lambda s, url, mp, md, scope, emit, **kw:
                        real(s, url, mp, md, scope, emit, fetch=fake_fetch(), delay=0, **kw))
    with TestClient(create_app(services)) as c:
        r = c.post("/api/rag/crawl", json={"url": f"{BASE}/guia/", "max_pages": 3})
        assert r.status_code == 200, r.text
        assert r.json()["pages"] == 3
        assert c.post("/api/rag/crawl", json={"url": f"{BASE}/guia/", "scope": "todo"}).status_code == 400
        docs = c.get("/api/rag").json()["documents"]
        assert docs[0]["kind"] == "web"
        assert c.get(f"/api/rag/{docs[0]['id']}/file").status_code == 200
