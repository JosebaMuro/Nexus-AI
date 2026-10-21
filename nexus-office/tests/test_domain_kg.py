"""Rastreo de TODO un dominio (subdominios, documentos, sin limite de profundidad) y su grafo de conocimiento
multicapa: afirmaciones contrastadas entre fuentes, temas, paquete importable y conocimiento dado a un agente."""
from urllib.parse import urlparse

from nexus.rag import crawl_jobs, crawler, domain_kg

ROOT = "https://www.saber.example"
BLOG = "https://blog.saber.example"


def html(title, body, links=()):
    nav = "".join(f'<a href="{h}">{h}</a>' for h in links)
    return f"<html><head><title>{title} | Saber</title></head><body><nav>{nav}</nav><main><h1>{title}</h1>{body}</main></body></html>"


def para(*s):
    return "".join(f"<p>{x}</p>" for x in s)


TEIDE = "El volcán Teide tiene una altura de 3718 metros sobre el nivel del mar."
CALDERA = "La caldera de Las Cañadas tiene un diámetro de {} kilómetros en su eje mayor."
COMMON = ("Los volcanes de Canarias se formaron por la actividad de un punto caliente del manto. "
          "La erupción de un volcán expulsa lava, cenizas y gases a la atmósfera. ")


def pdf_bytes(text):
    from fpdf import FPDF
    pdf = FPDF()
    pdf.add_page()
    pdf.set_font("helvetica", size=11)
    pdf.multi_cell(0, 6, text)
    return bytes(pdf.output())


def site():
    pages = {
        (ROOT, "/robots.txt"): (200, "text/plain", "User-agent: *\nDisallow: /privado/\n"),
        (ROOT, "/"): (200, "text/html", html("Inicio", para(COMMON, "Esta enciclopedia explica los volcanes de Canarias."),
                                                ["/a", "/b", "/c1", "/corta", "/docs/manual.pdf", "/notas.txt",
                                                 f"{BLOG}/post", "https://otro.example/x", "/privado/x", "/logo.png"])),
        (ROOT, "/a"): (200, "text/html", html("Teide", para(TEIDE, COMMON, "El Teide es un estratovolcán activo de Tenerife."))),
        (ROOT, "/b"): (200, "text/html", html("Altura del Teide", para(
            "Según el instituto geográfico, el volcán Teide tiene una altura de 3718 metros sobre el nivel del mar.",
            COMMON))),
        (ROOT, "/corta"): (200, "text/html", html("Glosario", "<p>Lava: roca fundida que sale del volcán.</p>")),
        (ROOT, "/notas.txt"): (200, "text/plain", "Notas de campo. El volcán Teide tiene una altura de 3718 metros "
                                                  "sobre el nivel del mar y es el pico más alto de España."),
        (ROOT, "/docs/manual.pdf"): (200, "application/pdf", pdf_bytes(
            "Manual de vulcanologia. La lava basaltica es muy fluida y forma coladas extensas en Canarias. "
            "La erupcion de un volcan expulsa lava, cenizas y gases a la atmosfera. Visita " + ROOT + "/pdf-enlace")),
        (ROOT, "/pdf-enlace"): (200, "text/html", html("Coladas", para("Las coladas de lava basáltica recorren kilómetros "
                                                                        "antes de enfriarse y solidificarse.", COMMON))),
        (BLOG, "/robots.txt"): (404, "text/plain", ""),
        (BLOG, "/post"): (200, "text/html", html("Entrada del blog", para(CALDERA.format(16), COMMON))),
    }
    # cadena profunda: c1 -> c2 -> ... -> c9 (mas alla de cualquier profundidad por defecto)
    for i in range(1, 10):
        body = para(f"Capítulo {i} de la historia eruptiva de las islas Canarias y sus volcanes.", COMMON,
                    CALDERA.format(17) if i == 9 else "")
        pages[(ROOT, f"/c{i}")] = (200, "text/html", html(f"Capítulo {i}", body, [f"/c{i + 1}"] if i < 9 else []))
    return pages


def fetcher(calls=None):
    pages = site()

    def fetch(url):
        if calls is not None:
            calls.append(url)
        p = urlparse(url)
        assert p.netloc.endswith("saber.example"), f"salio del dominio: {url}"
        host = f"{p.scheme}://{p.netloc}".replace("://saber", "://www.saber")
        hit = pages.get((host, p.path or "/"))
        if not hit:
            return 404, "text/html", "", url
        return hit[0], hit[1], hit[2], url
    return fetch


def test_crawl_dominio_completo_sin_excepciones():
    calls = []
    res = crawler.crawl(f"{ROOT}/", max_pages=500, max_depth=2, scope="domain", fetch=fetcher(calls), delay=0)
    urls = {p["url"] for p in res["pages"]}
    assert f"{ROOT}/c9" in urls                               # sin limite de profundidad
    assert f"{BLOG}/post" in urls                             # subdominio
    assert f"{ROOT}/docs/manual.pdf" in urls and f"{ROOT}/notas.txt" in urls   # documentos
    assert f"{ROOT}/pdf-enlace" in urls                       # enlace sacado del texto del PDF
    assert f"{ROOT}/corta" in urls                            # paginas cortas tambien
    pdf = next(p for p in res["pages"] if p["url"].endswith("manual.pdf"))
    assert "basaltica" in pdf["text"] and pdf["document"] == ".pdf"
    assert not any("otro.example" in c or "/privado/" in c or c.endswith(".png") for c in calls)
    assert f"{BLOG}/robots.txt" in calls                      # cada subdominio con su robots.txt
    assert res["state"]["done"]


def test_seccion_sigue_igual():
    res = crawler.crawl(f"{ROOT}/", max_pages=500, max_depth=2, scope="path", fetch=fetcher(), delay=0)
    urls = {p["url"] for p in res["pages"]}
    assert f"{ROOT}/c9" not in urls and f"{BLOG}/post" not in urls and not any(u.endswith(".pdf") for u in urls)


def test_afirmaciones_contrastadas_y_discrepantes(tmp_path):
    res = crawler.crawl(f"{ROOT}/", max_pages=500, scope="domain", fetch=fetcher(), delay=0)
    g = domain_kg.DomainGraph(tmp_path / "d.kg.sqlite")
    assert g.add_pages(res["pages"][:5]) == 5 and g.add_pages(res["pages"]) == len(res["pages"]) - 5   # por lotes
    assert g.add_pages(res["pages"]) == 0                     # sin duplicar
    built = g.build("Saber", "saber.example")
    nodes = built["graph"]["nodes"]
    notes = {n["note"] for n in nodes if n["type"] == "afirmacion"}
    teide = next(n for n in notes if "3718" in n)
    assert "contrastada" in teide and ("3 fuentes" in teide or "4 fuentes" in teide)
    assert any("Cañadas" in n and "discrepante" in n for n in notes)
    types = {n["type"] for n in nodes}
    assert {"dominio", "seccion", "pagina", "documento", "concepto", "afirmacion"} <= types
    rels = {e["rel"] for e in built["graph"]["edges"]}
    assert {"contiene", "menciona", "enlaza", "sobre", "fuente"} <= rels
    m = built["manifest"]
    assert m["pages"] == len(res["pages"]) and m["facts_by_status"]["contrastada"] >= 1 and m["sections"] >= 3
    assert "# Conocimiento del dominio" in built["markdown"] and f"{ROOT}/a" in built["markdown"]


def test_trabajo_completo_grafo_y_agente(services):
    out = crawler.crawl_site(services, f"{ROOT}/", scope="domain", fetch=fetcher(), delay=0, batch_pages=4, learn=False)
    assert out["status"] == "completado" and out["batches"] >= 4
    job = crawl_jobs.get(out["job"])
    assert job["complete"] and job["max_pages"] >= 10 ** 9 and job["knowledge"]["facts"] > 0
    pkg = domain_kg.read_package(crawl_jobs.knowledge_path(job["id"]))
    assert pkg["manifest"]["format"] == "nexus-domain-knowledge" and pkg["graph"]["format"] == "nexus-kg"
    r = domain_kg.import_package(services, pkg, "investigador")
    assert r["graph"]["nodes"] > 10 and r["graph"]["relations"] > 10
    rows = services.graph.db.query("SELECT name FROM entities WHERE scope='investigador' AND name LIKE '%Teide%'")
    assert rows
    hits = services.rag.search("altura del Teide en metros", k=5)
    assert any(h["doc_id"] == r["rag_doc_id"] and h["agent"] == "investigador" for h in hits)


def test_api_conocimiento_del_dominio(services, monkeypatch):
    from fastapi.testclient import TestClient

    from nexus.server import app as appmod
    monkeypatch.setattr(crawler, "http_fetcher", lambda connections=8: (fetcher(), lambda: None))
    with TestClient(appmod.create_app(services)) as c:
        r = c.post("/api/rag/crawl", json={"url": f"{ROOT}/", "scope": "domain", "max_pages": 3, "batch_pages": 5})
        jid = r.json()["job"]["id"]
        assert r.json()["job"]["complete"]                     # el maximo no aplica a todo el dominio
        crawl_jobs.wait(jid, 60)
        job = next(j for j in c.get("/api/rag/crawls").json() if j["id"] == jid)
        assert job["status"] == "completado" and job["pages_done"] > 10 and job["knowledge"]["nodes"] > 0
        z = c.get(f"/api/rag/crawls/{jid}/knowledge")
        assert z.status_code == 200 and z.content[:2] == b"PK"
        k = c.post(f"/api/rag/crawls/{jid}/knowledge").json()
        assert k["facts"] == job["knowledge"]["facts"]
        imp = c.post(f"/api/rag/crawls/{jid}/knowledge/import", json={"agent": "investigador"}).json()
        assert imp["agent"] == "investigador" and imp["graph"]["nodes"] > 0
        up = c.post("/api/rag/domain-knowledge/import", data={"agent": ""},
                    files={"file": ("saber.zip", z.content, "application/zip")}).json()
        assert up["agent"] == "" and up["rag_chunks"] > 0
        bad = c.post("/api/rag/domain-knowledge/import", files={"file": ("x.zip", b"hola", "application/zip")})
        assert bad.status_code == 400
        assert c.delete(f"/api/rag/crawls/{jid}?docs=true").status_code == 200
        assert not crawl_jobs.kg_path(jid).exists() and not crawl_jobs.knowledge_path(jid).exists()


def test_sintesis_razonada_con_llm(tmp_path):
    res = crawler.crawl(f"{ROOT}/", max_pages=500, scope="domain", fetch=fetcher(), delay=0)
    g = domain_kg.DomainGraph(tmp_path / "d.kg.sqlite")
    g.add_pages(res["pages"])
    asked = []

    def llm(prompt):
        asked.append(prompt)
        if "SOLO JSON" in prompt:
            return '{"sintesis": "Canarias es volcanica por un punto caliente; el Teide mide 3718 m.", ' \
                   '"conclusiones": ["El Teide es el techo de Espana", "La caldera necesita revisarse: cifras distintas"]}'
        return "Sitio sobre los volcanes de Canarias."
    built = g.build("Saber", "saber.example", llm=llm)
    assert built["manifest"]["reasoned_topics"] >= 1 and "3718" in asked[0]
    assert "contrastada" in asked[0]
    types = [n["type"] for n in built["graph"]["nodes"]]
    assert "tema" in types and types.count("conclusion") >= 2
    assert built["markdown"].splitlines()[2] == "Sitio sobre los volcanes de Canarias."
