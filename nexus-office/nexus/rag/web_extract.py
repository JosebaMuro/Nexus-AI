"""Extraccion estructurada de paginas web sin dependencias externas.

Produce: metadatos (titulo, descripcion, autor, fecha, idioma, sitio, imagen), secciones
jerarquicas por encabezados con bloques (parrafo, lista, tabla, codigo, cita), imagenes y enlaces.
Prioriza <article>/<main> y descarta navegacion, pies, formularios, scripts y estilos.
"""
from __future__ import annotations

import html as htmllib
import re
from html.parser import HTMLParser
from urllib.parse import urljoin, urlparse

SKIP = {"script", "style", "noscript", "svg", "nav", "footer", "aside", "form", "iframe", "template", "button",
        "select", "canvas", "object", "dialog"}
BLOCK = {"p", "div", "section", "article", "main", "header", "li", "ul", "ol", "table", "tr", "td", "th", "pre",
         "blockquote", "h1", "h2", "h3", "h4", "h5", "h6", "br", "hr", "figure", "figcaption", "dd", "dt", "dl"}
VOID = {"br", "hr", "img", "meta", "link", "input", "source", "wbr", "area", "base", "col", "embed", "param", "track"}
NOISE = re.compile(r"(cookie|newsletter|subscribe|suscr|share|compart|related|sidebar|menu|banner|advert|promo|comment|breadcrumb|popup|modal)", re.I)


def clean(t: str) -> str:
    return re.sub(r"\s+", " ", htmllib.unescape(t or "")).strip()


class _Parser(HTMLParser):
    def __init__(self, base: str) -> None:
        super().__init__(convert_charrefs=True)
        self.base = base
        self.meta: dict = {}
        self.title = ""
        self.lang = ""
        self.blocks: list[dict] = []        # {type, ..., main: bool}
        self.images: list[dict] = []
        self.links: list[dict] = []
        self.stack: list[str] = []
        self.skip = 0
        self.skip_tag = ""
        self.main = 0
        self.buf = ""
        self.in_title = False
        self.pre = 0
        self.list_stack: list[list[str]] = []
        self.table: list[list[str]] | None = None
        self.row: list[str] | None = None
        self.cell: str | None = None
        self.link: dict | None = None
        self.quote = 0

    # ------------------------------------------------------------------ utilidades
    def _flush(self, kind: str = "p", **extra) -> None:
        text = self.buf if self.pre else clean(self.buf)
        self.buf = ""
        if not text.strip():
            return
        if self.cell is not None:
            self.cell += (" " if self.cell else "") + text
            return
        if self.list_stack and kind == "p":
            self.list_stack[-1].append(text)
            return
        self.blocks.append({"type": "quote" if self.quote and kind == "p" else kind, "text": text,
                            "main": self.main > 0, **extra})

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "html":
            self.lang = a.get("lang", "")
        if tag == "meta":
            key = (a.get("property") or a.get("name") or "").lower()
            if key and a.get("content"):
                self.meta[key] = clean(a["content"])
            return
        if tag == "title":
            self.in_title = True
            return
        if self.skip:  # dentro de una zona omitida: solo se cuenta el anidamiento de la etiqueta que la abrio
            if tag == self.skip_tag:
                self.skip += 1
            return
        if tag in SKIP or (tag in ("div", "section", "ul", "header") and
                           NOISE.search(f"{a.get('class', '')} {a.get('id', '')} {a.get('role', '')}")):
            if tag not in VOID:
                self.skip, self.skip_tag = 1, tag
            return
        if tag not in VOID:
            self.stack.append(tag)
        if tag in ("article", "main") or a.get("role") == "main":
            self.main += 1
        if tag in BLOCK and tag not in ("li", "td", "th", "tr"):
            self._flush()
        if tag in ("ul", "ol"):
            self.list_stack.append([])
        elif tag == "li":
            self._flush()
        elif tag == "table":
            self.table = []
        elif tag == "tr" and self.table is not None:
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self._flush()
            self.cell = ""
        elif tag == "pre":
            self.pre += 1
        elif tag == "blockquote":
            self.quote += 1
        elif tag == "img":
            src = a.get("src") or a.get("data-src") or (a.get("srcset", "").split(" ")[0])
            if src and not src.startswith("data:"):
                self.images.append({"src": urljoin(self.base, src), "alt": clean(a.get("alt", "")), "main": self.main > 0})
        elif tag == "a" and a.get("href"):
            href = urljoin(self.base, a["href"])
            if href.startswith("http"):
                self.link = {"href": href, "text": ""}
        elif tag == "br":
            self.buf += "\n" if self.pre else " "

    def handle_endtag(self, tag):
        if tag == "title":
            self.in_title = False
            return
        if self.skip:
            if tag == self.skip_tag:
                self.skip -= 1
            return
        if tag in VOID:
            return
        if self.stack and tag in self.stack:
            while self.stack and self.stack.pop() != tag:
                pass
        if re.fullmatch(r"h[1-6]", tag):
            self._flush("heading", level=int(tag[1]))
        elif tag == "pre":
            self._flush("code")
            self.pre = max(0, self.pre - 1)
        elif tag == "li":
            self._flush()
        elif tag in ("ul", "ol") and self.list_stack:
            self._flush()
            items = self.list_stack.pop()
            if items:
                if self.list_stack:  # lista anidada: se aplana con sangria
                    self.list_stack[-1].extend("  " + i for i in items)
                else:
                    self.blocks.append({"type": "list", "items": items, "ordered": tag == "ol", "main": self.main > 0})
        elif tag in ("td", "th") and self.row is not None and self.cell is not None:
            self._flush()
            self.row.append(clean(self.cell))
            self.cell = None
        elif tag == "tr" and self.table is not None and self.row is not None:
            if any(self.row):
                self.table.append(self.row)
            self.row = None
        elif tag == "table" and self.table is not None:
            if self.table:
                self.blocks.append({"type": "table", "rows": self.table, "main": self.main > 0})
            self.table = None
        elif tag == "blockquote":
            self._flush()
            self.quote = max(0, self.quote - 1)
        elif tag == "a" and self.link:
            if self.link["text"].strip():
                self.links.append({"href": self.link["href"], "text": clean(self.link["text"])[:120]})
            self.link = None
        elif tag in BLOCK:
            self._flush()
        if tag in ("article", "main"):
            self.main = max(0, self.main - 1)

    def handle_data(self, data):
        if self.in_title:
            self.title += data
            return
        if self.skip:
            return
        self.buf += data
        if self.link is not None:
            self.link["text"] += data


def _sections(blocks: list[dict], title: str) -> list[dict]:
    sections = [{"heading": "Introducción", "level": 1, "blocks": []}]
    for b in blocks:
        if b["type"] == "heading":
            if clean(b["text"]).lower() == clean(title).lower() and not sections[-1]["blocks"] and len(sections) == 1:
                continue  # el h1 repite el titulo de la pagina
            sections.append({"heading": b["text"][:200], "level": min(3, b["level"]), "blocks": []})
        else:
            sections[-1]["blocks"].append(b)
    return [s for s in sections if s["blocks"] or s["level"] < 3]


def _text_of(block: dict) -> str:
    if block["type"] == "list":
        return "\n".join(f"- {i}" for i in block["items"])
    if block["type"] == "table":
        return "\n".join(" | ".join(r) for r in block["rows"])
    return block.get("text", "")


def extract_html(raw: str, url: str) -> dict:
    p = _Parser(url)
    p.feed(raw)
    p._flush()
    blocks = p.blocks
    main_blocks = [b for b in blocks if b.get("main")]
    if sum(len(_text_of(b)) for b in main_blocks) > 400:
        blocks = main_blocks
    # descarta parrafos de 1-2 palabras sueltos (restos de menus) salvo encabezados
    blocks = [b for b in blocks if b["type"] != "p" or len(b["text"].split()) > 3 or b["text"].endswith((".", ":"))]
    m = p.meta
    title = clean(m.get("og:title") or p.title or "")
    for b in blocks:
        if b["type"] == "heading" and b["level"] == 1 and not title:
            title = b["text"]
    title = title or urlparse(url).netloc or "Documento"
    sections = _sections(blocks, title)
    text ="\n\n".join(f"{'#' * s['level']} {s['heading']}\n\n" + "\n\n".join(_text_of(b) for b in s["blocks"]) for s in sections)
    seen = set()
    images = [i for i in p.images if not (i["src"] in seen or seen.add(i["src"]))]
    if any(i["main"] for i in images):
        images = [i for i in images if i["main"]]
    seen_l = set()
    links = [lk for lk in p.links if not (lk["href"] in seen_l or seen_l.add(lk["href"]))]
    return {
        "url": url, "title": title[:250], "description": m.get("og:description") or m.get("description", ""),
        "site": m.get("og:site_name") or urlparse(url).netloc, "author": m.get("author") or m.get("article:author", ""),
        "published": m.get("article:published_time") or m.get("date") or m.get("dc.date", ""),
        "keywords": m.get("keywords", ""), "lang": p.lang, "cover_image": m.get("og:image", ""),
        "sections": sections, "images": images[:40], "links": links[:200],
        "text": text.strip(), "words": len(text.split()),
        "headings": [s["heading"] for s in sections if s["heading"]],
    }
