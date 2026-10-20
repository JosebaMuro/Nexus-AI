"""Biblioteca RAG, conversion de URLs en PDF estructurado y rastreo de sitios de documentacion."""
from __future__ import annotations


def _target(args, ctx) -> str:
    """destino "agente": solo el subgrafo del agente que llama; "general" (por defecto): para todos."""
    return ctx.agent if args.get("target") == "agente" and ctx.agent else ""


TARGET = {"type": "string", "enum": ["general", "agente"], "default": "general",
          "description": "general: grafo comun a todos los agentes; agente: solo tu grafo individual"}


def url_to_pdf(args, ctx):
    from ...rag.url_pdf import process_url
    res = process_url(ctx.services, args["url"], emit=ctx.emit, max_images=int(args.get("max_images", 8)),
                      agent=_target(args, ctx))
    if ctx.emit:
        ctx.emit({"type": "media", "kind": "document", "url": res["url"], "title": res["title"]})
        ctx.emit({"type": "graph_update"})
    return res


def crawl_docs(args, ctx):
    from ...rag import crawl_jobs
    from ...rag.crawler import crawl_site
    job = args.get("job") or ""
    if not job and args.get("continue"):
        prev = crawl_jobs.find_resumable(args["url"], _target(args, ctx))
        job = prev["id"] if prev else ""
    if args.get("background"):   # dominios enormes: sigue solo, por lotes, mientras el agente hace otras cosas
        if not job:
            pages, chars = crawl_jobs.auto_batch(getattr(ctx.services, "profile", None))
            job = crawl_jobs.create(args["url"], int(args.get("max_pages", 40)), int(args.get("max_depth", 3)),
                                    args.get("scope", "path"), _target(args, ctx), True, pages, chars)["id"]
        crawl_jobs.run_background(ctx.services, job, emit=ctx.emit)
        return {"job": job, "status": "en curso", "note": "Rastreo por lotes en segundo plano. Cada lote aparece en "
                "la biblioteca RAG en cuanto termina; consulta con rag_search."}
    res = crawl_site(ctx.services, args["url"], max_pages=int(args.get("max_pages", 40)),
                     max_depth=int(args.get("max_depth", 3)), scope=args.get("scope", "path"), emit=ctx.emit,
                     agent=_target(args, ctx), job=job or None, max_seconds=1500)
    if res.get("status") == crawl_jobs.PAUSED:
        res["note"] = (f"Quedan {res['pending']} paginas por rastrear. Vuelve a llamar a crawl_docs con "
                       f"job=\"{res['job']}\" para seguir por el siguiente lote.")
    if ctx.emit:
        ctx.emit({"type": "graph_update"})
    return res


def rag_search(args, ctx):
    hits = ctx.services.rag.search(args["query"], k=int(args.get("k", 5)), agent=ctx.agent or "")   # general + propios
    return {"results": [{"document": h["title"], "source": h["source"], "page": h["page"], "score": h["score"],
                         "text": h["text"][:700]} for h in hits]}


def rag_list(args, ctx):
    return {"documents": [{"id": d["id"], "title": d["title"], "source": d["source"], "pages": d["pages"]}
                          for d in ctx.services.rag.list(agent=ctx.agent or "")[:50]]}


TOOLS = [
    ({"name": "url_to_pdf",
      "description": ("Procesa una URL: extrae toda su informacion, la maqueta en un PDF estructurado (portada, resumen, "
                      "indice, secciones, tablas, imagenes, enlaces), la publica al usuario, la guarda en la biblioteca "
                      "RAG y la incorpora al conocimiento"),
      "parameters": {"type": "object", "properties": {"url": {"type": "string"},
                                                      "max_images": {"type": "integer", "default": 8},
                                                      "target": TARGET},
                     "required": ["url"]}, "risk": "medium", "permissions": ["network", "memory"], "timeout": 900}, url_to_pdf),
    ({"name": "crawl_docs",
      "description": ("Rastrea (crawling) un sitio de documentacion a partir de una URL: recorre las paginas enlazadas "
                      "de esa seccion (o de todo el dominio con scope=domain), respetando robots.txt, las indexa en la "
                      "biblioteca RAG con la URL de cada pagina y aprende un resumen. Si el sitio no cabe de una vez se "
                      "procesa en lotes correlativos y ordenados (un documento por lote) y se puede reanudar con job. "
                      "Volver a rastrear la misma URL lo actualiza. Despues consulta con rag_search"),
      "parameters": {"type": "object", "properties": {
          "url": {"type": "string", "description": "pagina inicial de la documentacion"},
          "max_pages": {"type": "integer", "default": 40,
                        "description": "maximo de paginas en total (hasta 100000; por encima del lote se divide sola)"},
          "max_depth": {"type": "integer", "default": 3, "description": "saltos de enlace desde la pagina inicial"},
          "scope": {"type": "string", "enum": ["path", "domain"], "default": "path",
                    "description": "path: solo la seccion de la URL; domain: todo el sitio"},
          "job": {"type": "string", "description": "id de un rastreo por lotes pausado para continuarlo"},
          "continue": {"type": "boolean", "default": False,
                       "description": "continuar el ultimo rastreo sin terminar de esta URL"},
          "background": {"type": "boolean", "default": False,
                         "description": "seguir en segundo plano (para dominios grandes) y responder ya"},
          "target": TARGET},
          "required": ["url"]}, "risk": "medium", "permissions": ["network", "memory"], "timeout": 1800}, crawl_docs),
    ({"name": "rag_search", "description": "Busca en la biblioteca de documentos (RAG) y devuelve fragmentos con su fuente",
      "parameters": {"type": "object", "properties": {"query": {"type": "string"}, "k": {"type": "integer", "default": 5}},
                     "required": ["query"]}, "risk": "low", "permissions": ["memory"]}, rag_search),
    ({"name": "rag_list", "description": "Lista los documentos de la biblioteca RAG",
      "parameters": {"type": "object", "properties": {}}, "risk": "low", "permissions": ["memory"]}, rag_list),
]
