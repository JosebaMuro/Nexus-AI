"""URL -> extraccion estructurada -> resumen -> PDF maquetado -> biblioteca RAG -> conocimiento.

1. Descarga la pagina (solo direcciones publicas, maximo 8 MB).
2. Extrae metadatos, secciones, listas, tablas, codigo, imagenes y enlaces.
3. Resume y obtiene puntos clave (con el LLM si lo hay; si no, resumen extractivo).
4. Maqueta el PDF y lo publica en data/outputs/docs (enlace para el usuario).
5. Guarda una copia en la biblioteca RAG (troceada e indexada).
6. Asimila el contenido: resumen y puntos clave a la memoria y entidades/relaciones al grafo.
"""
from __future__ import annotations

import re
import time
from collections import Counter
from pathlib import Path

import httpx

from .. import log, paths
from ..memory.embeddings import tokens
from .pdfgen import build_pdf
from .store import STOP
from .web_extract import extract_html

L = log.get("url_pdf")
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Nexus-Office/1.7", "Accept-Language": "es,en;q=0.8"}


def fetch(url: str) -> tuple[str, str]:
    from ..tools.builtin.web_tools import _public_url
    _public_url(url)
    with httpx.stream("GET", url, headers=UA, timeout=30, follow_redirects=True) as r:
        r.raise_for_status()
        _public_url(str(r.url))  # tambien tras redirecciones
        ctype = r.headers.get("content-type", "")
        if "html" not in ctype and "text" not in ctype and "xml" not in ctype:
            raise ValueError(f"la URL no es una pagina web ({ctype or 'tipo desconocido'})")
        data = b""
        for chunk in r.iter_bytes():
            data += chunk
            if len(data) > 8_000_000:
                break
        return data.decode(r.encoding or "utf-8", errors="replace"), str(r.url)


def extractive_summary(text: str, n: int = 4) -> tuple[str, list[str]]:
    sents = [s.strip() for s in re.split(r"(?<=[.!?])\s+", re.sub(r"#+ .*\n", "", text)) if 40 < len(s.strip()) < 400]
    if not sents:
        return text[:600], []
    freq = Counter(t for t in tokens(text) if t not in STOP and len(t) > 3)
    scored = sorted(range(len(sents)), key=lambda i: -sum(freq[t] for t in tokens(sents[i])) / (len(tokens(sents[i])) + 5))
    top = sorted(scored[:n])
    points = [sents[i] for i in scored[n:n + 5]]
    return " ".join(sents[i] for i in top), points


def llm_summary(services, doc: dict) -> tuple[str, list[str]] | None:
    if not services.llm.has_real_llm():
        return None
    excerpt = doc["text"][:3500]
    try:
        raw = services.llm.complete(
            f"Titulo: {doc['title']}\n\nContenido:\n{excerpt}\n\nEscribe en espanol:\nRESUMEN: un parrafo de 3-4 frases.\n"
            "PUNTOS:\n- 5 puntos clave, uno por linea.", max_tokens=380, temperature=0.2)
    except Exception as e:  # noqa: BLE001
        L.warning("Resumen con LLM fallido: %s", e)
        return None
    raw = raw.replace("**", "").replace("__", "")
    parts = re.split(r"(?i)\bPUNTOS(?: CLAVE)?\s*:?", raw, maxsplit=1)
    summary = re.sub(r"(?i)^\s*RESUMEN\s*:?\s*", "", parts[0]).strip()
    points = []
    if len(parts) > 1:  # admite puntos en lineas separadas o seguidos en una sola linea ("- 1. ... - 2. ...")
        points = [p.strip(" .-•*\n") for p in re.split(r"(?:^|\n|\s)(?:[-•*]\s*)?\d+[.)]\s+|(?:^|\n)\s*[-•*]\s+|\s-\s", parts[1])]
    return (summary, [p for p in points if len(p) > 8][:7]) if len(summary) > 40 else None


def process_url(services, url: str, emit=None, max_images: int = 8, agent: str = "") -> dict:
    t0 = time.time()
    step = (lambda s: emit({"type": "progress", "text": s})) if emit else (lambda s: None)
    step("Descargando la pagina")
    raw, final_url = fetch(url)
    step("Extrayendo estructura")
    doc = extract_html(raw, final_url)
    if doc["words"] < 30:
        raise ValueError("la pagina apenas contiene texto (puede requerir JavaScript o inicio de sesion)")
    step("Resumiendo")
    summ = llm_summary(services, doc) or extractive_summary(doc["text"])
    summary, points = summ
    step("Maquetando el PDF")
    slug = re.sub(r"[^a-z0-9]+", "-", doc["title"].lower())[:50].strip("-") or "documento"
    out = paths.sub("outputs", "docs") / f"{int(time.time())}_{slug}.pdf"
    info = build_pdf(doc, summary, points, out, max_images=max_images)
    step("Archivando en la biblioteca RAG")
    rag_doc = services.rag.ingest_file(out, title=doc["title"], source=final_url,
                                       meta={"origin": "url_to_pdf", "site": doc["site"], "summary": summary},
                                       agent=agent, link=False)
    step("Incorporando al conocimiento")
    learned = assimilate(services, doc, summary, points, rag_doc["id"], final_url, agent=agent)
    return {"title": doc["title"], "url": f"/outputs/docs/{out.name}", "source": final_url, "pages": info["pages"],
            "sections": len(doc["sections"]), "words": doc["words"], "images": len(doc["images"]),
            "images_embedded": info["images_embedded"], "links": len(doc["links"]), "summary": summary,
            "key_points": points, "rag_doc_id": rag_doc["id"], "rag_chunks": rag_doc["chunks"], "learned": learned,
            "seconds": round(time.time() - t0, 1), "agent": agent}


def assimilate(services, doc: dict, summary: str, points: list[str], doc_id: int, url: str, agent: str = "") -> int:
    """Asume el contenido del PDF como conocimiento propio: memoria + grafo (el general o el del agente)."""
    mem, graph = services.memory, services.graph.for_agent(agent)
    src = f"rag:{doc_id}"
    n = 0
    mem.remember(f"Documento «{doc['title']}» ({url}): {summary}", "summary", 0.75, source=src, agent=agent)
    n += 1
    for p in points:
        mem.remember(f"{p} (según «{doc['title'][:80]}»)", "fact", 0.6, source=src, agent=agent)
        n += 1
    title = doc["title"][:60]
    graph.upsert_entity(title, "documento", weight=2)
    graph.relate(graph.self_name, "ha_leido", title, graph.self_type, "documento", 1.0)
    if doc.get("site"):
        graph.relate(title, "publicado_en", doc["site"], "documento", "organizacion", 0.8)
    if doc.get("author"):
        graph.relate(title, "escrito_por", doc["author"][:60], "documento", "persona", 0.8)
    for kw in graph.keywords(f"{doc['title']}. {summary} " + " ".join(doc["headings"][:15]), 8):
        graph.relate(title, "trata_de", kw, "documento", "concepto", 0.6)
        n += 1
    for h in doc["headings"][:10]:
        if 3 < len(h) < 60:
            graph.relate(title, "contiene", h, "documento", "concepto", 0.3)
    graph.extract(summary, "assistant")
    return n
