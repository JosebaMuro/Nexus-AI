"""Rastreo de dominios grandes por lotes correlativos, ordenados y reanudables."""
import json

import pytest

from nexus.rag import crawl_jobs, crawler

BASE = "https://docs.grande.example"
N = 57
TEXT = ("Esta pagina de la documentacion describe con detalle una funcion distinta de la biblioteca, sus parametros, "
        "los valores que devuelve y varios ejemplos de uso pensados para que el lector la entienda. ")


def site(n: int = N) -> dict:
    out = {"/robots.txt": (404, "text/plain", "")}
    for i in range(n):
        links = "".join(f'<a href="/docs/p{j}.html">p{j}</a>' for j in (i + 1, i + 2, (i * 7) % n) if j < n)
        out["/docs/" if i == 0 else f"/docs/p{i}.html"] = (
            200, "text/html", f"<html><head><title>Pagina {i} | Docs Grande</title></head><body><nav>{links}</nav>"
                              f"<main><h1>Pagina {i}</h1><p>{TEXT}Identificador unico pagina{i}fin.</p></main></body></html>")
    return out


def fake_fetch(calls=None, pages=None):
    pages = pages or site()

    def fetch(url):
        if calls is not None:
            calls.append(url)
        path = url[len(BASE):].split("?")[0] or "/"
        if path not in pages:
            return 404, "text/html", "", url
        status, ctype, body = pages[path]
        return status, ctype, body, url
    return fetch


def urls_of(services, job):
    out = []
    for b in job["batches"]:
        out += services.rag.get(b["doc_id"])["meta"]["urls"]
    return out


def test_state_resumes_in_same_order_without_repeating():
    one = crawler.crawl(f"{BASE}/docs/", max_pages=40, max_depth=50, fetch=fake_fetch(), delay=0)
    assert len(one["pages"]) == 40
    state, got, calls = None, [], []
    for _ in range(4):
        r = crawler.crawl(f"{BASE}/docs/", max_pages=10, max_depth=50, fetch=fake_fetch(calls), delay=0, state=state)
        got += [p["url"] for p in r["pages"]]
        state = json.loads(json.dumps(r["state"]))       # se guarda en disco entre lotes
    assert got == [p["url"] for p in one["pages"]]       # mismo orden que de una vez
    assert len(set(got)) == 40
    page_calls = [c for c in calls if not c.endswith(("robots.txt", "sitemap.xml"))]
    assert len(page_calls) == len(set(page_calls))       # ninguna pagina se descarga dos veces


def test_domain_in_batches_creates_ordered_docs(services):
    res = crawler.crawl_site(services, f"{BASE}/docs/", max_pages=500, max_depth=50, fetch=fake_fetch(), delay=0,
                             batch_pages=20, learn=False)
    assert res["batches"] == 3 and res["pages"] == N and res["status"] == "completado"
    job = crawl_jobs.get(res["job"])
    assert [b["page_from"] for b in job["batches"]] == [1, 21, 41]
    assert [b["page_to"] for b in job["batches"]] == [20, 40, N]
    titles = [services.rag.get(b["doc_id"])["title"] for b in job["batches"]]
    assert titles[0].endswith("lote 001 (págs. 1-20)") and titles[2].endswith(f"lote 003 (págs. 41-{N})")
    urls = urls_of(services, job)
    assert len(urls) == N and len(set(urls)) == N
    hit = services.rag.search("pagina45fin", k=1)
    assert hit and hit[0]["doc_id"] == job["batches"][2]["doc_id"]


def test_pause_and_resume(services):
    calls = {"n": 0}

    def stop_after_two():
        return calls["n"] >= 2

    job = crawl_jobs.create(f"{BASE}/docs/", 500, 50, "path", "", False, 15, 6_000_000)
    orig = crawler._ingest_batch

    def counting(*a, **kw):
        calls["n"] += 1
        return orig(*a, **kw)
    crawler._ingest_batch = counting
    try:
        j = crawler.crawl_batches(services, job, fetch=fake_fetch(), delay=0, should_stop=stop_after_two)
    finally:
        crawler._ingest_batch = orig
    assert j["status"] == "pausado" and len(j["batches"]) == 2 and j["pages_done"] == 30
    saved = crawl_jobs.get(j["id"])
    assert saved["state"]["queue"] and saved["status"] == "pausado"
    # reanudar (como tras reiniciar Nexus-Office): sigue por la pagina 31
    res = crawler.crawl_site(services, "", job=j["id"], fetch=fake_fetch(), delay=0)
    assert res["status"] == "completado" and res["pages"] == N and res["batches"] == 4
    job = crawl_jobs.get(j["id"])
    assert job["batches"][2]["page_from"] == 31
    assert len(set(urls_of(services, job))) == N


def test_interrupted_job_is_resumable(services):
    job = crawl_jobs.create(f"{BASE}/docs/", 100, 5, "path", "", False, 10, 6_000_000)
    assert crawl_jobs.get(job["id"])["status"] == "interrumpido"     # "en curso" sin nadie ejecutandolo
    assert crawl_jobs.find_resumable(f"{BASE}/docs/")["id"] == job["id"]


def test_recrawl_replaces_all_old_batches(services):
    r1 = crawler.crawl_site(services, f"{BASE}/docs/", max_pages=500, max_depth=50, fetch=fake_fetch(), delay=0,
                            batch_pages=25, learn=False)
    r2 = crawler.crawl_site(services, f"{BASE}/docs/", max_pages=500, max_depth=50, fetch=fake_fetch(), delay=0,
                            batch_pages=25, learn=False)
    assert sorted(r2["replaced"]) == sorted(r1["rag_doc_ids"])
    assert [d["id"] for d in services.rag.list() if d["kind"] == "web"] == sorted(r2["rag_doc_ids"], reverse=True)
    assert crawl_jobs.get(r1["job"]) is None and len(crawl_jobs.list_jobs()) == 1


def test_background_api(services, monkeypatch):
    from fastapi.testclient import TestClient

    from nexus.server import app as appmod
    monkeypatch.setattr(crawler, "http_fetcher", lambda connections=8: (fake_fetch(), lambda: None))
    app = appmod.create_app(services)
    with TestClient(app) as c:
        r = c.post("/api/rag/crawl", json={"url": f"{BASE}/docs/", "max_pages": 1000, "max_depth": 50,
                                           "background": True, "batch_pages": 20})
        assert r.status_code == 200, r.text
        jid = r.json()["job"]["id"]
        crawl_jobs.wait(jid, 60)
        jobs = c.get("/api/rag/crawls").json()
        job = next(j for j in jobs if j["id"] == jid)
        assert job["status"] == "completado" and job["pages_done"] == N and len(job["batches"]) == 3
        assert c.post(f"/api/rag/crawls/{jid}/resume").json()["status"] == "completado"
        assert c.delete(f"/api/rag/crawls/{jid}?docs=true").status_code == 200
        assert not [d for d in services.rag.list() if d["kind"] == "web"]
