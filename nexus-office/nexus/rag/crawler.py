"""Rastreo (crawling) de un sitio de documentacion -> biblioteca RAG -> conocimiento.

Parte de una URL y recorre en anchura las paginas enlazadas del mismo sitio:
- Ambito: por defecto la "seccion" de la URL inicial (https://docs.x.org/3/library/ -> solo /3/library/...);
  con scope="domain", TODO el dominio: el host, www. y sus subdominios (docs.x.org, blog.x.org...), sin limite de
  paginas ni de profundidad, leyendo tambien los documentos enlazados (PDF, Word, OpenDocument, PowerPoint, Excel,
  EPUB, RTF, texto) y las paginas cortas. Con el se construye ademas un grafo de conocimiento propio del dominio
  (nexus.rag.domain_kg).
- Respeta robots.txt (Disallow y Crawl-delay) y usa el sitemap.xml como semilla si existe.
- Solo direcciones publicas (tambien tras redirecciones); no sale del ambito aunque redirija.
- Limites: paginas, profundidad, tiempo, 3 MB por pagina y 6 millones de caracteres en total.
- Sigue tambien los enlaces de la navegacion lateral (en la extraccion de contenido se descarta).
- Rapido: varias descargas a la vez sobre conexiones persistentes y el analisis HTML repartido entre todos
  los nucleos (nexus.parallel), aplicando los resultados en orden para que sea reproducible.

Todo el sitio se indexa como un unico documento del tipo "web": una pagina del documento por pagina
rastreada, con la URL delante de cada fragmento para que las citas digan de donde sale. Ademas se
guarda una copia legible en Markdown en data/rag/files y se asimila un resumen a la memoria y al grafo.
"""
from __future__ import annotations

import hashlib
import re
import threading
import time
from collections import deque
from html.parser import HTMLParser
from pathlib import PurePosixPath
from typing import Callable
from urllib.parse import parse_qsl, urlencode, urljoin, urlparse, urlunparse
from urllib.robotparser import RobotFileParser

from .. import log
from .web_extract import clean, extract_html

L = log.get("crawler")
UA_NAME = "Nexus-Office"
UA = {"User-Agent": f"Mozilla/5.0 (compatible; {UA_NAME}/1.0; documentation reader)", "Accept-Language": "es,en;q=0.8"}

MAX_PAGES_LIMIT = 500               # tope duro aunque se pida mas
MAX_PAGE_BYTES = 3_000_000
MAX_DOC_BYTES = 60_000_000          # PDF y documentos enlazados (solo en el rastreo de dominio completo)
MAX_TOTAL_CHARS = 6_000_000
MAX_CRAWL_DELAY = 5.0
MIN_WORDS = 30
MIN_WORDS_DOMAIN = 5                # dominio completo: tambien las paginas cortas (fichas, definiciones...)
DOC_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub", ".rtf", ".txt", ".csv", ".md"}
DOC_CTYPES = {"application/pdf": ".pdf", "wordprocessingml": ".docx", "spreadsheetml": ".xlsx", "presentationml": ".pptx",
              "opendocument.text": ".odt", "opendocument.spreadsheet": ".ods", "opendocument.presentation": ".odp",
              "epub": ".epub", "rtf": ".rtf", "text/plain": ".txt", "text/csv": ".csv", "text/markdown": ".md"}
SLD = {"co", "com", "org", "net", "gov", "gob", "edu", "ac", "nhs", "ltd", "plc", "or", "ne", "go"}

SKIP_EXT = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".bmp", ".tif", ".tiff", ".avif",
            ".css", ".js", ".mjs", ".map", ".json", ".xml", ".rss", ".atom", ".woff", ".woff2", ".ttf", ".otf", ".eot",
            ".zip", ".gz", ".tgz", ".bz2", ".xz", ".7z", ".rar", ".tar", ".whl", ".exe", ".msi", ".dmg", ".pkg",
            ".deb", ".rpm", ".apk", ".jar", ".iso", ".bin", ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
            ".odt", ".mp3", ".mp4", ".wav", ".ogg", ".webm", ".avi", ".mov", ".mkv", ".txt", ".csv", ".epub"}
SKIP_PATH = re.compile(r"(^|/)(_sources|_static|_images|_downloads|static|assets|search|genindex|login|logout|signin|"
                       r"signup|register|account|cart|edit|print|feed)(/|\.html?$|$)", re.I)
SKIP_QUERY = re.compile(r"(^|&)(action=(edit|history|raw)|q=|query=|search=|share=|print=|replytocom=|oldid=|diff=)", re.I)
TRACKING = re.compile(r"^(utm_.*|fbclid|gclid|mc_cid|mc_eid|ref|ref_src|_ga|_gl)$", re.I)

# fetcher(url) -> (status, content_type, text, url_final); lanza excepcion si la red falla
Fetcher = Callable[[str], "tuple[int, str, str, str]"]


# ---------------------------------------------------------------------------------------------- URLs y ambito
def normalize(url: str) -> str:
    """Quita el fragmento y los parametros de seguimiento; host en minusculas; '/' como ruta vacia."""
    p = urlparse(url.strip())
    query = urlencode([(k, v) for k, v in parse_qsl(p.query, keep_blank_values=True) if not TRACKING.match(k)])
    path = re.sub(r"/{2,}", "/", p.path or "/")
    path = re.sub(r"/index\.html?$", "/", path)
    return urlunparse((p.scheme.lower(), (p.netloc or "").lower(), path, "", query, ""))


def _host(netloc: str) -> str:
    h = netloc.lower().split("@")[-1]
    h = h[1:].split("]")[0] if h.startswith("[") else h.split(":")[0]   # [::1]:8080 -> ::1
    return h[4:] if h.startswith("www.") else h


def site_root(netloc: str) -> str:
    """Dominio registrable aproximado: docs.python.org -> python.org, www.bbc.co.uk -> bbc.co.uk."""
    host = _host(netloc)
    if ":" in host or host.replace(".", "").isdigit():   # IP: no tiene subdominios
        return host
    parts = host.split(".")
    if len(parts) >= 3 and parts[-2] in SLD and len(parts[-1]) == 2:
        return ".".join(parts[-3:])
    return ".".join(parts[-2:])


def doc_suffix(url: str, ctype: str = "") -> str:
    """Extension de documento legible ('.pdf', '.docx'...) por la URL o el tipo MIME; '' si es una pagina."""
    suf = PurePosixPath(urlparse(url).path).suffix.lower()
    if suf in DOC_EXT:
        return suf
    c = (ctype or "").lower()
    return next((v for k, v in DOC_CTYPES.items() if k in c), "")


def scope_prefix(start: str, scope: str) -> str:
    """Prefijo de ruta del ambito: la carpeta de la URL inicial (o '/' para todo el dominio)."""
    if scope == "domain":
        return "/"
    path = urlparse(start).path or "/"
    if not path.endswith("/"):
        last = PurePosixPath(path).name
        path = path.rsplit("/", 1)[0] + "/" if "." in last else path + "/"
    return path


def in_scope(url: str, start: str, prefix: str, domain: bool = False) -> bool:
    """domain=True: cualquier pagina o documento del dominio y sus subdominios."""
    p, s = urlparse(url), urlparse(start)
    if p.scheme not in ("http", "https"):
        return False
    if domain:
        host, root = _host(p.netloc), site_root(s.netloc)
        if host != root and not host.endswith("." + root):
            return False
    elif _host(p.netloc) != _host(s.netloc):
        return False
    path = p.path or "/"
    if not domain and not (path + "/").startswith(prefix) and not path.startswith(prefix):
        return False
    suf = PurePosixPath(path).suffix.lower()
    if (suf in SKIP_EXT and not (domain and suf in DOC_EXT)) or SKIP_PATH.search(path) or SKIP_QUERY.search(p.query):
        return False
    return True


# ---------------------------------------------------------------------------------------------- enlaces
class _Links(HTMLParser):
    """Todos los enlaces de la pagina (navegacion incluida), la canonica y las directivas de robots."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.hrefs: list[str] = []
        self.canonical = ""
        self.base = ""
        self.noindex = self.nofollow = False

    def handle_starttag(self, tag, attrs):
        a = {k.lower(): (v or "") for k, v in attrs}
        if tag == "a" and a.get("href") and "nofollow" not in a.get("rel", "").lower():
            self.hrefs.append(a["href"])
        elif tag == "link" and "canonical" in a.get("rel", "").lower() and a.get("href"):
            self.canonical = a["href"]
        elif tag == "link" and a.get("rel", "").lower() == "next" and a.get("href"):
            self.hrefs.append(a["href"])
        elif tag == "base" and a.get("href"):
            self.base = a["href"]
        elif tag == "meta" and a.get("name", "").lower() in ("robots", UA_NAME.lower()):
            c = a.get("content", "").lower()
            self.noindex |= "noindex" in c or "none" in c
            self.nofollow |= "nofollow" in c or "none" in c


def parse_links(raw: str, url: str) -> tuple[list[str], str, bool, bool]:
    """(enlaces absolutos normalizados, canonica, noindex, nofollow)."""
    p = _Links()
    try:
        p.feed(raw)
    except Exception:  # noqa: BLE001 - HTML roto: nos quedamos con lo leido
        pass
    base = urljoin(url, p.base) if p.base else url
    out, seen = [], set()
    for h in p.hrefs:
        h = h.strip()
        if not h or h.startswith(("#", "mailto:", "javascript:", "tel:", "data:")):
            continue
        n = normalize(urljoin(base, h))
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out, (normalize(urljoin(base, p.canonical)) if p.canonical else ""), p.noindex, p.nofollow


def sitemap_urls(text: str) -> tuple[list[str], list[str]]:
    """(paginas, sitemaps hijos) de un sitemap.xml o indice de sitemaps."""
    locs = [clean(m) for m in re.findall(r"<loc>\s*(.*?)\s*</loc>", text, re.I | re.S)]
    if re.search(r"<sitemapindex", text, re.I):
        return [], locs
    return locs, []


# ---------------------------------------------------------------------------------------------- red
def http_fetcher(connections: int = 8) -> tuple[Fetcher, Callable[[], None]]:
    """Fetcher real: solo direcciones publicas (antes y despues de redirigir) y tamano limitado.
    Un unico cliente con conexiones persistentes (keep-alive) compartido por todas las descargas en paralelo."""
    import httpx

    from ..tools.builtin.web_tools import _public_url
    limits = httpx.Limits(max_connections=connections, max_keepalive_connections=connections)
    client = httpx.Client(headers=UA, timeout=25, follow_redirects=True, limits=limits)

    def fetch(url: str) -> tuple[int, str, str | bytes, str]:
        """Las paginas llegan como texto; los documentos (PDF, Office...) como bytes, para leerlos despues."""
        _public_url(url)
        with client.stream("GET", url) as r:
            _public_url(str(r.url))
            ctype = r.headers.get("content-type", "")
            is_doc = "html" not in ctype.lower() and bool(doc_suffix(str(r.url), ctype))
            cap = MAX_DOC_BYTES if is_doc else MAX_PAGE_BYTES
            data = bytearray()
            if r.status_code == 200:
                for chunk in r.iter_bytes(65536):
                    data += chunk
                    if len(data) > cap:
                        break
            body = bytes(data) if is_doc else bytes(data).decode(r.encoding or "utf-8", errors="replace")
            return r.status_code, ctype, body, str(r.url)

    return fetch, client.close


def _robots(fetch: Fetcher, start: str) -> tuple[RobotFileParser | None, list[str]]:
    p = urlparse(start)
    try:
        status, _, text, _ = fetch(f"{p.scheme}://{p.netloc}/robots.txt")
    except Exception:  # noqa: BLE001
        return None, []
    if isinstance(text, bytes):   # robots.txt llega como documento .txt (bytes) del fetcher
        text = text.decode("utf-8", "replace")
    if status != 200 or not text.strip():
        return None, []
    rp = RobotFileParser()
    rp.parse(text.splitlines())
    sitemaps = re.findall(r"(?im)^\s*sitemap:\s*(\S+)", text)
    return rp, sitemaps


def _sitemap_seeds(fetch: Fetcher, start: str, declared: list[str], limit: int, max_files: int = 6) -> list[str]:
    p = urlparse(start)
    queue = list(declared) or [f"{p.scheme}://{p.netloc}/sitemap.xml"]
    pages: list[str] = []
    read = 0
    while queue and read < max_files and len(pages) < limit:
        sm = queue.pop(0)
        read += 1
        try:
            status, ctype, text, _ = fetch(sm)
        except Exception:  # noqa: BLE001
            continue
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        if status != 200 or "<" not in text[:200]:
            continue
        locs, children = sitemap_urls(text)
        pages.extend(locs)
        queue.extend(children)
    return pages[:limit]


# ---------------------------------------------------------------------------------------------- rastreo
def analyze_page(raw: str, url: str) -> dict:
    """Trabajo de CPU de una pagina (enlaces + extraccion + huella). De nivel de modulo para ir a otro nucleo."""
    links, canonical, noindex, nofollow = parse_links(raw, url)
    out = {"links": links, "canonical": canonical, "noindex": noindex, "nofollow": nofollow}
    if not noindex:
        doc = extract_html(raw, url)
        out["doc"] = {k: doc[k] for k in ("title", "text", "words", "headings", "description", "site", "author", "lang")}
        out["hash"] = hashlib.sha1(doc["text"].encode("utf-8", "replace")).hexdigest()
    return out


URL_IN_TEXT = re.compile(r"https?://[^\s<>\"'\)\]]+")


def analyze_document(data: str | bytes, url: str, ctype: str = "") -> dict:
    """Un documento enlazado (PDF, Word, hoja de calculo, texto...) leido con los lectores de la biblioteca RAG.
    Devuelve lo mismo que analyze_page; los enlaces son las URLs que aparezcan en su texto."""
    import os
    import tempfile

    from .readers import read_document
    suf = doc_suffix(url, ctype) or ".txt"
    raw = data.encode("utf-8") if isinstance(data, str) else data
    fd, tmp = tempfile.mkstemp(suffix=suf)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(raw)
        title, pages = read_document(tmp)
    finally:
        os.unlink(tmp)
    text = clean("\n\n".join(p for p in pages if p and p.strip()))
    name = PurePosixPath(urlparse(url).path).name
    if not title or title.startswith("tmp"):
        title = name or url
    links = [normalize(u.rstrip(".,;:")) for u in URL_IN_TEXT.findall(text)][:500]
    return {"links": links, "canonical": "", "noindex": False, "nofollow": False,
            "doc": {"title": f"{title} ({suf[1:].upper()})", "text": text, "words": len(text.split()), "headings": [],
                    "description": "", "site": "", "author": "", "lang": ""},
            "hash": hashlib.sha1(text.encode("utf-8", "replace")).hexdigest(), "document": suf}


def _analyze_doc_safe(data, url: str, ctype: str) -> dict:
    try:
        return analyze_document(data, url, ctype)
    except Exception as e:  # noqa: BLE001 - PDF cifrado o roto
        return {"error": f"documento ilegible: {type(e).__name__}: {e}"[:160]}


def default_connections() -> int:
    from .. import parallel
    try:
        from ..config import get_config
        n = int(get_config().get("performance.crawl_connections", 0) or 0)
    except Exception:  # noqa: BLE001
        n = 0
    return max(1, min(n, 32)) if n > 0 else max(4, min(8, parallel.cpu_count()))


def crawl(start_url: str, max_pages: int = 40, max_depth: int = 3, scope: str = "path", fetch: Fetcher | None = None,
          emit=None, delay: float = 0.1, max_seconds: float = 900, use_sitemap: bool = True,
          respect_robots: bool = True, connections: int | None = None, state: dict | None = None,
          max_chars: int = MAX_TOTAL_CHARS, should_stop: Callable[[], bool] | None = None,
          offset: int = 0, total: int = 0, seed_limit: int = 0) -> dict:
    """Recorre el sitio y devuelve {'pages': [{url, title, text, depth, ...}], 'skipped': [...], ...}.
    No toca la biblioteca: eso lo hace crawl_site().

    Las descargas van en paralelo (`connections` a la vez) y el analisis de cada pagina se reparte entre los
    nucleos, pero los resultados se aplican en el orden de descubrimiento: el resultado es el mismo que
    rastreando de una en una. `delay` es la separacion minima entre peticiones al sitio; con Crawl-delay en
    robots.txt se va de una en una respetandolo.

    Por lotes: `state` es el estado que devolvio el lote anterior (cola, visitadas, huellas...). Con el, el
    rastreo sigue exactamente donde lo dejo, en el mismo orden, sin repetir paginas. Cada lote para al llegar
    a `max_pages` paginas, `max_chars` caracteres, `max_seconds` o cuando `should_stop()` diga que pare, y
    devuelve en "state" lo que falta (con "done" si ya no queda nada). `offset` y `total` solo sirven para
    numerar el progreso (paginas de lotes anteriores y total previsto)."""
    from concurrent.futures import Future, ThreadPoolExecutor

    from .. import parallel
    if not re.match(r"^https?://", start_url or "", re.I):
        raise ValueError("la URL debe empezar por http:// o https://")
    max_pages = max(1, min(int(max_pages), MAX_PAGES_LIMIT))
    max_depth = max(0, int(max_depth))
    max_chars = max(1, min(int(max_chars), MAX_TOTAL_CHARS))
    if state:
        start, prefix = state["start"], state["prefix"]
        scope = state.get("scope", scope)
    else:
        start = normalize(start_url)
        prefix = scope_prefix(start, scope)
    close = None
    if fetch is None:
        fetch, close = http_fetcher(connections or default_connections())
    step = (lambda s: emit({"type": "progress", "text": s})) if emit else (lambda s: None)
    dom = scope == "domain"            # dominio completo: subdominios, documentos, sin limite de profundidad
    min_words = MIN_WORDS_DOMAIN if dom else MIN_WORDS
    scoped = lambda u: in_scope(u, start, prefix, dom)   # noqa: E731
    t0 = time.time()
    pool = None
    try:
        robots, declared = _robots(fetch, start) if respect_robots else (None, [])
        wait = max(0.0, float(delay))
        conns = max(1, int(connections or default_connections()))
        if robots:
            cd = robots.crawl_delay(UA_NAME) or robots.crawl_delay("*")
            if cd:
                wait, conns = max(wait, min(float(cd), MAX_CRAWL_DELAY)), 1
        start_host = urlparse(start).netloc
        host_robots: dict[str, RobotFileParser | None] = {start_host: robots}

        def allowed(u: str) -> bool:
            if not respect_robots:
                return True
            h = urlparse(u).netloc
            if h not in host_robots:      # cada subdominio tiene su propio robots.txt
                host_robots[h] = _robots(fetch, u)[0]
            rp = host_robots[h]
            return rp.can_fetch(UA_NAME, u) if rp else True
        cpu_split = max_pages >= 8 and parallel.process_pool() is not None   # paginas sueltas: no compensa

        if state:
            queue: deque[tuple[str, int]] = deque((u, int(d)) for u, d in state["queue"])
            queued = set(state["queued"])
        else:
            queue = deque([(start, 0)])
            queued = {start}
        if use_sitemap and not state:
            step("Leyendo el sitemap")
            for u in _sitemap_seeds(fetch, start, declared, seed_limit or max_pages * 5, 2000 if dom else 6):
                n = normalize(u)
                if n not in queued and scoped(n):
                    queued.add(n)
                    queue.append((n, 1))

        pace = threading.Lock()
        slot = [0.0]
        counter = [0]

        def task(url: str) -> dict:
            if wait:
                with pace:   # separacion minima entre inicios de peticion (cortesia con el servidor)
                    now = time.time()
                    at = max(now, slot[0])
                    slot[0] = at + wait
                time.sleep(max(0.0, at - now))
            with pace:
                counter[0] += 1
            try:
                status, ctype, raw, final = fetch(url)
            except Exception as e:  # noqa: BLE001
                return {"error": f"error: {e}"[:160]}
            res = {"status": status, "ctype": ctype, "final": normalize(final or url)}
            if status == 200 and dom and (isinstance(raw, bytes) or doc_suffix(res["final"], ctype)) \
                    and "html" not in (ctype or "").lower():
                got = _analyze_doc_safe(raw, res["final"], ctype)
                if "error" in got:
                    return got
                res["page"] = got
            elif status == 200 and (not ctype or "html" in ctype.lower()):
                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", "replace")
                res["page"] = parallel.run_cpu(analyze_page, raw, res["final"]) if cpu_split else analyze_page(raw, res["final"])
            return res

        def ready(value: dict) -> Future:
            f: Future = Future()
            f.set_result(value)
            return f

        pool = ThreadPoolExecutor(max_workers=conns, thread_name_prefix="nexus-crawl")
        window: deque[tuple[str, int, Future]] = deque()
        pages, skipped = [], []
        hashes, visited = set((state or {}).get("hashes", ())), set((state or {}).get("visited", ()))
        total_chars, site, committed = 0, (state or {}).get("site", ""), int(bool(state))
        stopped = ""
        while True:
            # llenar la ventana de descargas en curso (la portada sola hasta conocer su destino)
            while queue and len(window) < conns and len(pages) + len(window) < max_pages and (committed or not window):
                url, depth = queue.popleft()
                if url in visited or (pages or skipped or state) and not scoped(url):
                    continue   # (el ambito puede haber cambiado si la portada redirigio)
                visited.add(url)
                fut = ready({"robots": True}) if not allowed(url) else pool.submit(task, url)
                window.append((url, depth, fut))
            if not window:
                break
            if time.time() - t0 > max_seconds:
                stopped = "tiempo agotado"
                break
            if total_chars >= max_chars:
                stopped = "limite de texto alcanzado"
                break
            if should_stop and should_stop():
                stopped = "detenido"
                break
            url, depth, fut = window.popleft()
            res = fut.result()
            committed += 1
            if res.get("robots"):
                skipped.append((url, "robots.txt"))
                continue
            of = "?" if dom and (total or max_pages) >= 10 ** 9 else (total or max_pages)
            step(f"Rastreando {offset + len(pages) + 1}/{of}: {url}")
            if "error" in res:
                skipped.append((url, res["error"]))
                continue
            status, ctype, final = res["status"], res["ctype"], res["final"]
            if status != 200:
                skipped.append((url, f"HTTP {status}"))
                continue
            if ctype and "html" not in ctype.lower() and "page" not in res:
                skipped.append((url, f"no es HTML ({ctype.split(';')[0]})"))
                continue
            if final != url and url == start and not pages and not skipped and not state:
                # la portada redirige (http -> https, /docs -> /docs/en/latest/...): el ambito se toma del destino
                start, prefix = final, scope_prefix(final, scope)
                visited.add(final)
            elif final != url:
                if not scoped(final) and final != start:
                    skipped.append((url, f"redirige fuera del ambito ({final})"))
                    continue
                if final in visited and url != start:
                    continue
                visited.add(final)
            info = res["page"]
            inside = []
            if (dom or depth < max_depth) and not info["nofollow"]:
                for link in info["links"]:
                    if scoped(link):
                        inside.append(link)
                        if link not in queued:
                            queued.add(link)
                            queue.append((link, depth + 1))
            canonical = info["canonical"]
            if canonical and canonical != final and canonical in visited and scoped(canonical):
                skipped.append((url, "duplicado (canonica)"))
                continue
            if info["noindex"]:
                skipped.append((url, "noindex"))
                continue
            doc = info["doc"]
            if doc["words"] < min_words:
                skipped.append((url, "apenas tiene texto"))
                continue
            if info["hash"] in hashes:
                skipped.append((url, "contenido duplicado"))
                continue
            hashes.add(info["hash"])
            site = site or doc["site"]
            text = doc["text"][:MAX_TOTAL_CHARS - total_chars]
            total_chars += len(text)
            pages.append({"url": final, "title": doc["title"], "text": text, "depth": depth, "words": doc["words"],
                          "headings": doc["headings"], "description": doc["description"], "site": doc["site"],
                          "author": doc["author"], "lang": doc["lang"], "links": inside[:200],
                          "document": info.get("document", "")})
            if len(pages) >= max_pages:
                break
        for _, _, f in window:
            f.cancel()
        # lo que estaba en vuelo vuelve a la cabeza de la cola, en su orden: el siguiente lote empieza por ahi
        for url, depth, _ in reversed(window):
            visited.discard(url)
            queue.appendleft((url, depth))
        rest = [(u, d) for u, d in queue if u not in visited]
        pending = len(rest)
        if stopped and window:
            skipped.append((window[0][0], stopped))
        site = site or _host(urlparse(start).netloc)
        new_state = {"start": start, "prefix": prefix, "scope": scope, "site": site, "queue": rest,
                     "queued": sorted(queued), "visited": sorted(visited), "hashes": sorted(hashes),
                     "done": not rest, "stopped": stopped}
        return {"start": start, "scope": scope, "prefix": prefix, "site": site,
                "pages": pages, "skipped": skipped, "fetched": counter[0], "pending": pending,
                "robots": robots is not None, "seconds": round(time.time() - t0, 1), "connections": conns,
                "state": new_state, "stopped": stopped}
    finally:
        if pool is not None:
            pool.shutdown(wait=True, cancel_futures=True)
        if close:
            close()


def _site_title(res: dict) -> str:
    pages = res["pages"]
    first = pages[0]["title"] if pages else ""
    site = res["site"]
    # "Tutorial — Docs de X 3.2" -> "Docs de X 3.2"
    parts = [p.strip() for p in re.split(r"\s+[|—–·-]\s+", first) if p.strip()]
    name = parts[-1] if len(parts) > 1 else (site or first)
    return f"Documentación: {name}"[:200]


def to_markdown(res: dict, title: str) -> str:
    lines = [f"# {title}", "", f"Rastreado desde {res['start']} (ámbito {res['prefix']}) el "
             f"{time.strftime('%Y-%m-%d %H:%M')}. {len(res['pages'])} páginas.", "", "## Índice", ""]
    lines += [f"{i}. [{p['title']}]({p['url']})" for i, p in enumerate(res["pages"], 1)]
    for p in res["pages"]:
        lines += ["", "---", "", f"<!-- {p['url']} -->", "", p["text"]]
    return "\n".join(lines) + "\n"


def _slug(res: dict) -> str:
    return re.sub(r"[^a-z0-9]+", "-", f"{_host(urlparse(res['start']).netloc)}{res['prefix']}".lower()).strip("-")[:60]


def _ingest_batch(services, res: dict, title: str, agent: str, learn: bool, llm: bool, meta_extra: dict) -> dict:
    """Indexa las paginas de un lote como un documento RAG (con su copia en Markdown) y, si se pide, lo aprende."""
    from .url_pdf import assimilate, extractive_summary, llm_summary
    pages, rag = res["pages"], services.rag
    md_path = rag.files / f"{int(time.time())}_{_slug(res) or 'docs'}{meta_extra.get('suffix', '')}.md"
    md_path.write_text(to_markdown(res, title), encoding="utf-8")
    combined = "\n\n".join(p["text"] for p in pages[:12])
    doc = {"title": title, "text": combined, "site": res["site"], "author": "",
           "headings": [p["title"] for p in pages]}
    summary, points = (llm_summary(services, doc) if learn and llm else None) or extractive_summary(combined)
    meta = {"origin": "crawl", "start": res["start"], "scope": res["scope"], "prefix": res["prefix"],
            "site": res["site"], "pages_indexed": len(pages), "fetched": res["fetched"], "skipped": len(res["skipped"]),
            "pending": res["pending"], "urls": [p["url"] for p in pages], "summary": summary, "crawled": time.time(),
            **{k: v for k, v in meta_extra.items() if k != "suffix"}}
    try:
        rag_doc = rag.ingest_pages(title, [f"# {p['title']}\n\n{p['text']}" for p in pages], source=res["start"],
                                   kind="web", path=str(md_path), meta=meta, labels=[f"[{p['url']}]" for p in pages],
                                   agent=agent, link=not learn)
    except Exception:
        md_path.unlink(missing_ok=True)
        raise
    learned = assimilate(services, doc, summary, points, rag_doc["id"], res["start"], agent=agent) if learn else 0
    return {"doc": rag_doc, "summary": summary, "points": points, "learned": learned}


def _replace_old(rag, start: str, prefix: str, agent: str, keep: set[int]) -> list[int]:
    """Volver a rastrear la misma documentacion la actualiza en vez de duplicarla (lotes antiguos incluidos)."""
    out = []
    for d in rag.list():
        m = d.get("meta") or {}
        if d["id"] not in keep and m.get("origin") == "crawl" and m.get("start") == start \
                and m.get("prefix") == prefix and d.get("agent", "") == agent:
            rag.delete(d["id"])
            out.append(d["id"])
    return out


def crawl_site(services, start_url: str, max_pages: int = 40, max_depth: int = 3, scope: str = "path", emit=None,
               fetch: Fetcher | None = None, delay: float = 0.1, replace: bool = True, learn: bool = True,
               max_seconds: float = 900, connections: int | None = None, agent: str = "",
               batch_pages: int | None = None, job: str | None = None) -> dict:
    """Rastrea la documentacion, la indexa en el RAG como un documento "web" y la asimila al conocimiento.

    agent: "" = grafo general (todos los agentes); un id = solo el subgrafo de ese agente.

    Si `max_pages` no cabe en un lote (tamano segun la RAM, ver crawl_jobs.auto_batch), el sitio se procesa en
    lotes correlativos: un documento RAG por lote, en orden, y un trabajo reanudable en data/rag/crawls. Si se
    agota `max_seconds` el trabajo queda pausado y se reanuda con job=<id> (o desde la Biblioteca).
    `job` reanuda un trabajo existente (los demas parametros se toman de el)."""
    from . import crawl_jobs
    auto_pages, auto_chars = crawl_jobs.auto_batch(getattr(services, "profile", None))
    bpages = max(1, min(int(batch_pages or auto_pages), MAX_PAGES_LIMIT))
    if job or int(max_pages) > bpages:
        j = crawl_jobs.get(job) if job else crawl_jobs.create(start_url, max_pages, max_depth, scope, agent, learn,
                                                               bpages, auto_chars)
        if not j:
            raise ValueError(f"no existe el rastreo {job}")
        ev = crawl_jobs._claim(j["id"])
        try:
            j["status"], j["reason"], j["error"] = crawl_jobs.RUNNING, "", ""
            j = crawl_batches(services, j, emit=emit, fetch=fetch, delay=delay, max_seconds=max_seconds,
                              connections=connections, should_stop=ev.is_set, replace=replace)
        finally:
            crawl_jobs._running.pop(j["id"], None)
        if not j["batches"]:
            raise ValueError(j.get("error") or "no se ha podido leer ninguna pagina con texto")
        return job_result(services, j)

    # una sola tanda: un unico documento, como siempre
    res = crawl(start_url, max_pages, max_depth, scope, fetch=fetch, emit=emit, delay=delay, max_seconds=max_seconds,
                connections=connections)
    pages = res["pages"]
    if not pages:
        why = "; ".join(f"{u}: {r}" for u, r in res["skipped"][:3])
        raise ValueError(f"no se ha podido leer ninguna pagina con texto ({why or 'sin enlaces en el ambito'})")
    title = _site_title(res)
    step = (lambda s: emit({"type": "progress", "text": s})) if emit else (lambda s: None)
    step(f"Indexando {len(pages)} páginas en la biblioteca RAG")
    if learn:
        step("Incorporando al conocimiento")
    out = _ingest_batch(services, res, title, agent, learn, True, {"max_pages": max_pages, "max_depth": max_depth})
    rag_doc = out["doc"]
    replaced = _replace_old(services.rag, res["start"], res["prefix"], agent, {rag_doc["id"]}) if replace else []
    L.info("Rastreo de %s: %d paginas, %d omitidas, %d fragmentos", res["start"], len(pages), len(res["skipped"]),
           rag_doc["chunks"])
    return {"title": title, "start": res["start"], "scope": res["scope"], "prefix": res["prefix"], "site": res["site"],
            "pages": len(pages), "fetched": res["fetched"], "pending": res["pending"], "robots": res["robots"],
            "words": sum(p["words"] for p in pages), "page_titles": [p["title"] for p in pages[:40]],
            "skipped": [{"url": u, "reason": r} for u, r in res["skipped"][:30]], "skipped_total": len(res["skipped"]),
            "rag_doc_id": rag_doc["id"], "rag_chunks": rag_doc["chunks"], "replaced": replaced, "summary": out["summary"],
            "key_points": out["points"], "learned": out["learned"], "url": f"/api/rag/{rag_doc['id']}/file",
            "seconds": round(res["seconds"], 1), "connections": res["connections"], "agent": agent, "batches": 1}


def crawl_batches(services, job: dict, emit=None, fetch: Fetcher | None = None, delay: float = 0.1,
                  max_seconds: float | None = None, connections: int | None = None, should_stop=None,
                  replace: bool = True, batch_seconds: float = 900) -> dict:
    """Procesa los lotes que le faltan a un trabajo (crawl_jobs), guardando el estado tras cada uno."""
    from . import crawl_jobs
    step = (lambda s: emit({"type": "progress", "text": s})) if emit else (lambda s: None)
    t0 = time.time() - float(job.get("seconds") or 0)
    began = time.time()
    agent = job.get("agent", "")
    stop = should_stop or (lambda: False)
    while job["pages_done"] < job["max_pages"]:
        if stop():
            job.update(status=crawl_jobs.PAUSED, reason="pausado por el usuario")
            break
        left_time = None if max_seconds is None else max_seconds - (time.time() - began)
        if left_time is not None and left_time <= 5:
            job.update(status=crawl_jobs.PAUSED, reason="tiempo agotado: reanudalo para seguir")
            break
        n = len(job["batches"]) + 1
        want = min(job["batch_pages"], job["max_pages"] - job["pages_done"])
        step(f"Lote {n}: rastreando hasta {want} páginas (llevas {job['pages_done']}"
             + (" de todo el dominio)" if job.get("complete") else f" de {job['max_pages']})"))
        res = crawl(job["url"], want, job["max_depth"], job["scope"], fetch=fetch, emit=emit, delay=delay,
                    max_seconds=min(batch_seconds, left_time) if left_time is not None else batch_seconds,
                    connections=connections, state=job["state"], max_chars=job["batch_chars"], should_stop=stop,
                    offset=job["pages_done"], total=job["max_pages"], seed_limit=job["max_pages"] * 2)
        job["start"], job["prefix"], job["site"] = res["start"], res["prefix"], res["site"]
        job["fetched"] += res["fetched"]
        job["skipped"] += len([1 for _, r in res["skipped"] if r not in ("tiempo agotado", "detenido",
                                                                         "limite de texto alcanzado")])
        if res["pages"]:
            if not job["title"]:
                job["title"] = _site_title(res)
            first, last = job["pages_done"] + 1, job["pages_done"] + len(res["pages"])
            title = (f"{job['title']} · lote {n:03d} (págs. {first}-{last})"[:200]
                     if job["max_pages"] > job["batch_pages"] or n > 1 else job["title"])
            step(f"Lote {n}: indexando {len(res['pages'])} páginas en la biblioteca RAG")
            out = _ingest_batch(services, res, title, agent, job["learn"], n == 1,
                                {"job": job["id"], "batch": n, "page_from": first, "page_to": last,
                                 "max_pages": job["max_pages"], "max_depth": job["max_depth"], "suffix": f"_lote{n:03d}"})
            if job.get("complete"):   # dominio completo: cada lote alimenta su grafo multicapa (en disco)
                from .domain_kg import DomainGraph
                step(f"Lote {n}: añadiendo al grafo del dominio")
                DomainGraph(crawl_jobs.kg_path(job["id"])).add_pages(res["pages"])
            job["batches"].append({"n": n, "doc_id": out["doc"]["id"], "title": title, "pages": len(res["pages"]),
                                   "page_from": first, "page_to": last, "chunks": out["doc"]["chunks"],
                                   "words": sum(p["words"] for p in res["pages"]), "learned": out["learned"],
                                   "summary": out["summary"] if n == 1 else "", "first_url": res["pages"][0]["url"],
                                   "last_url": res["pages"][-1]["url"], "points": out["points"] if n == 1 else []})
            job["pages_done"] = last
            if n == 1 and replace:
                keep = {out["doc"]["id"]}
                job["replaced"] = _replace_old(services.rag, res["start"], res["prefix"], agent, keep)
                for other in crawl_jobs.list_jobs():   # trabajos anteriores de la misma documentacion
                    if other["id"] != job["id"] and other.get("start") == res["start"] \
                            and other.get("prefix") == res["prefix"] and other.get("agent", "") == agent \
                            and not other["running"]:
                        crawl_jobs.delete(other["id"])
        job["state"] = res["state"]
        job["pending"] = res["pending"]
        job["seconds"] = round(time.time() - t0, 1)
        if res["state"]["done"]:
            break
        if not res["pages"] and res["stopped"] not in ("tiempo agotado", "detenido"):
            break   # nada nuevo que leer en el ambito
        crawl_jobs.save(job)
        if emit and res["pages"]:
            emit({"type": "crawl_job", "job": job["id"], "status": job["status"], "batch": n})
    st = job.get("state") or {}
    if job["status"] == crawl_jobs.RUNNING:
        if st.get("done") or job["pages_done"] >= job["max_pages"] or not st.get("queue"):
            job["status"] = crawl_jobs.DONE
            job["reason"] = ("sitio completo" if st.get("done") or not st.get("queue")
                             else f"alcanzado el máximo de {job['max_pages']} páginas")
        else:
            job.update(status=crawl_jobs.PAUSED, reason=job.get("reason") or "detenido")
    if not job["batches"] and job["status"] == crawl_jobs.DONE:
        job.update(status=crawl_jobs.FAILED, error="no se ha podido leer ninguna pagina con texto")
    elif job["status"] == crawl_jobs.DONE and job.get("complete"):
        try:
            crawl_jobs.build_knowledge(services, job, emit)
        except Exception as e:  # noqa: BLE001
            L.warning("Grafo del dominio %s no construido: %s", job["id"], e)
            job["knowledge_error"] = str(e)[:300]
    job["seconds"] = round(time.time() - t0, 1)
    crawl_jobs.save(job)
    L.info("Rastreo por lotes %s (%s): %d lotes, %d paginas, %s", job["id"], job["start"] or job["url"],
           len(job["batches"]), job["pages_done"], job["status"])
    if emit:
        emit({"type": "crawl_job", "job": job["id"], "status": job["status"]})
    return job


def job_result(services, job: dict) -> dict:
    """Resumen de un trabajo por lotes con la misma forma que el de crawl_site (y los datos de cada lote)."""
    b = job["batches"]
    first = b[0] if b else {}
    return {"title": job["title"], "start": job["start"], "scope": job["scope"], "prefix": job["prefix"],
            "site": job["site"], "pages": job["pages_done"], "fetched": job["fetched"],
            "pending": len((job.get("state") or {}).get("queue") or ()), "robots": True,
            "words": sum(x["words"] for x in b), "page_titles": [x["title"] for x in b[:40]],
            "skipped": [], "skipped_total": job["skipped"], "rag_doc_id": first.get("doc_id"),
            "rag_doc_ids": [x["doc_id"] for x in b], "rag_chunks": sum(x["chunks"] for x in b),
            "replaced": job.get("replaced", []), "summary": first.get("summary", ""), "key_points": first.get("points", []),
            "learned": sum(x["learned"] for x in b), "url": f"/api/rag/{first['doc_id']}/file" if b else "",
            "seconds": job["seconds"], "agent": job.get("agent", ""), "batches": len(b), "job": job["id"],
            "status": job["status"], "reason": job.get("reason", ""), "max_pages": job["max_pages"],
            "batch_pages": job["batch_pages"],
            "lotes": [{k: x[k] for k in ("n", "doc_id", "title", "pages", "page_from", "page_to")} for x in b]}
