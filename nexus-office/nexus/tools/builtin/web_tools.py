"""Acceso a internet (riesgo medio: requiere aprobacion segun politica)."""
from __future__ import annotations

import html
import ipaddress
import re
import socket
from urllib.parse import parse_qs, unquote, urlparse

import httpx

UA = {"User-Agent": "Mozilla/5.0 (Nexus-Office agent)"}


def _allow_private() -> bool:
    try:
        from ...config import get_config
        return bool(get_config().get("rag.allow_private_urls", False))
    except Exception:  # noqa: BLE001
        return False


def _public_url(url: str) -> str:
    p = urlparse(url)
    if p.scheme not in ("http", "https") or not p.hostname:
        raise ValueError("solo se permiten URLs http(s)")
    if _allow_private():   # el administrador lo ha permitido (documentacion de la intranet)
        return url
    for info in socket.getaddrinfo(p.hostname, None):
        ip = ipaddress.ip_address(info[4][0])
        if ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_reserved:
            raise ValueError("no se permite acceder a direcciones de red locales")
    return url


def html_to_text(raw: str) -> str:
    raw = re.sub(r"(?is)<(script|style|noscript|svg|head).*?</\1>", " ", raw)
    raw = re.sub(r"(?i)<br\s*/?>|</p>|</h\d>|</li>|</div>", "\n", raw)
    text = html.unescape(re.sub(r"<[^>]+>", " ", raw))
    return re.sub(r"\n\s*\n+", "\n\n", re.sub(r"[ \t]+", " ", text)).strip()


MAX_FETCH_BYTES = 5_000_000


def web_fetch(args, ctx):
    """Texto de una pagina. Las paginas largas se leen por partes: offset + max_chars y next_offset."""
    url = _public_url(args["url"])
    data = bytearray()
    with httpx.stream("GET", url, headers=UA, timeout=20, follow_redirects=True) as r:
        _public_url(str(r.url))   # tambien tras redirecciones
        for chunk in r.iter_bytes(65536):
            data += chunk
            if len(data) > MAX_FETCH_BYTES:
                break
        ctype, final, status, enc = r.headers.get("content-type", ""), str(r.url), r.status_code, r.encoding
    raw = bytes(data).decode(enc or "utf-8", errors="replace")
    text = html_to_text(raw) if "html" in ctype or raw.lstrip()[:15].lower().startswith(("<!doctype", "<html")) else raw
    limit = max(200, int(args.get("max_chars", 4000)))
    offset = max(0, int(args.get("offset", 0)))
    title = re.search(r"(?is)<title>(.*?)</title>", raw)
    part = text[offset:offset + limit]
    out = {"url": final, "status": status, "title": html.unescape(title.group(1).strip()) if title else "",
           "text": part, "offset": offset, "total_chars": len(text)}
    if offset + limit < len(text):
        out["next_offset"] = offset + limit   # hay mas: vuelve a llamar con este offset para la siguiente parte
    return out


def web_search(args, ctx):
    r = httpx.post("https://html.duckduckgo.com/html/", data={"q": args["query"]}, headers=UA, timeout=20)
    results = []
    for m in re.finditer(r'(?s)<a rel="nofollow" class="result__a" href="([^"]+)">(.*?)</a>.*?'
                         r'class="result__snippet"[^>]*>(.*?)</a>', r.text):
        href = m.group(1)
        if "uddg=" in href:
            href = unquote(parse_qs(urlparse(href).query).get("uddg", [href])[0])
        results.append({"title": html_to_text(m.group(2)), "url": href, "snippet": html_to_text(m.group(3))})
        if len(results) >= 2 * int(args.get("max_results", 6)):
            break
    # IA de apoyo: se piden el doble y se quedan los mas relevantes por significado (en cualquier idioma)
    assist = getattr(getattr(ctx, "services", None), "assist", None)
    if assist is not None:
        results = assist.rerank(args["query"], results, key=lambda x: f"{x['title']}. {x['snippet']}")
    return {"query": args["query"], "results": results[:int(args.get("max_results", 6))]}


TOOLS = [
    ({"name": "web_fetch", "description": ("Descarga una pagina web publica y devuelve su texto. Si es larga, devuelve "
                                            "next_offset: vuelve a llamar con offset para leer la parte siguiente"),
      "parameters": {"type": "object", "properties": {"url": {"type": "string"},
                                                      "max_chars": {"type": "integer", "default": 4000},
                                                      "offset": {"type": "integer", "default": 0,
                                                                 "description": "caracter desde el que leer"}},
                     "required": ["url"]}, "risk": "medium", "permissions": ["network"], "timeout": 60}, web_fetch),
    ({"name": "web_search", "description": "Busca en internet (DuckDuckGo) y devuelve titulos, enlaces y resumenes",
      "parameters": {"type": "object", "properties": {"query": {"type": "string"},
                                                      "max_results": {"type": "integer", "default": 6}},
                     "required": ["query"]}, "risk": "medium", "permissions": ["network"]}, web_search),
]
