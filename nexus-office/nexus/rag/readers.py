"""Lectura de documentos sueltos para la biblioteca RAG, sin dependencias pesadas.

Formatos: PDF (pypdf, por paginas), Word (.docx), OpenDocument (.odt/.ods/.odp), PowerPoint (.pptx),
Excel (.xlsx), EPUB, RTF, HTML y cualquier texto plano o codigo fuente (.txt, .md, .csv, .json, .py...).
Cada lector devuelve (titulo_sugerido, paginas_de_texto). Las funciones son de nivel de modulo para poder
leer varios ficheros a la vez en distintos nucleos (nexus.parallel).
"""
from __future__ import annotations

import html as htmllib
import re
import zipfile
from pathlib import Path, PurePosixPath

MAX_TEXT_BYTES = 20 * 1024 * 1024        # texto plano: suficiente para cualquier documento razonable

PDF = {".pdf"}
OFFICE = {".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub", ".rtf"}
MARKUP = {".html", ".htm", ".xhtml"}
TEXT = {".txt", ".md", ".markdown", ".rst", ".adoc", ".org", ".tex", ".log", ".csv", ".tsv", ".json", ".jsonl",
        ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".xml", ".srt", ".vtt"}
CODE = {".py", ".pyi", ".ipynb", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".vue", ".svelte", ".java", ".kt",
        ".kts", ".scala", ".groovy", ".gradle", ".c", ".h", ".cc", ".cpp", ".hpp", ".cs", ".go", ".rs", ".rb",
        ".php", ".swift", ".m", ".mm", ".dart", ".lua", ".r", ".jl", ".pl", ".sh", ".bash", ".zsh", ".ps1",
        ".bat", ".cmd", ".sql", ".graphql", ".proto", ".properties", ".css", ".scss", ".sass", ".less", ".el",
        ".clj", ".ex", ".exs", ".erl", ".hs", ".ml", ".fs", ".vb", ".asm", ".cmake", ".tf", ".nix"}
DOCUMENTS = PDF | OFFICE | MARKUP | TEXT | CODE
ARCHIVES = (".zip", ".rar", ".7z", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz")


def archive_suffix(name: str) -> str:
    """'.tar.gz', '.zip', ... o '' si no es un archivo comprimido soportado."""
    low = name.lower()
    return next((s for s in sorted(ARCHIVES, key=len, reverse=True) if low.endswith(s)), "")


def supported(name: str) -> bool:
    return bool(archive_suffix(name)) or Path(name).suffix.lower() in DOCUMENTS


def _xml_text(xml: str, para: str = r"</(?:w:p|text:p|text:h|a:p|p|h\d|li|tr|div|br)\s*/?>") -> str:
    xml = re.sub(r"<(?:w:tab|text:tab)\s*/>", "\t", xml)
    xml = re.sub(r"<text:s\b[^>]*/>", " ", xml)
    xml = re.sub(para, "\n", xml)
    text = htmllib.unescape(re.sub(r"<[^>]+>", "", xml))
    return re.sub(r"\n{3,}", "\n\n", re.sub(r"[ \t]+\n", "\n", text)).strip()


def _natural(name: str) -> list:
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", name)]


def read_docx(path: Path) -> tuple[str, list[str]]:
    with zipfile.ZipFile(path) as z:
        xml = z.read("word/document.xml").decode("utf-8", "replace")
    text = re.sub(r"<w:p[ >].*?</w:p>", lambda m: re.sub(r"<[^>]+>", "", m.group(0)) + "\n\n", xml, flags=re.S)
    return path.stem, [htmllib.unescape(re.sub(r"<[^>]+>", "", text))]


def read_opendocument(path: Path) -> tuple[str, list[str]]:
    with zipfile.ZipFile(path) as z:
        xml = z.read("content.xml").decode("utf-8", "replace")
        meta = z.read("meta.xml").decode("utf-8", "replace") if "meta.xml" in z.namelist() else ""
    title = re.search(r"<dc:title>(.*?)</dc:title>", meta, re.S)
    if path.suffix.lower() == ".odp":   # una pagina por diapositiva
        pages = [_xml_text(s) for s in re.findall(r"<draw:page\b.*?</draw:page>", xml, re.S)]
    elif path.suffix.lower() == ".ods":  # una pagina por hoja, celdas separadas por tabuladores
        pages = []
        for sheet in re.findall(r"<table:table\b.*?</table:table>", xml, re.S):
            rows = []
            for row in re.findall(r"<table:table-row\b.*?</table:table-row>", sheet, re.S):
                cells = [_xml_text(c) for c in re.findall(r"<table:table-cell\b[^>]*>(.*?)</table:table-cell>", row, re.S)]
                if any(cells):
                    rows.append("\t".join(cells).rstrip())
            pages.append("\n".join(rows))
    else:
        pages = [_xml_text(xml)]
    return (htmllib.unescape(title.group(1)).strip() if title else "") or path.stem, [p for p in pages if p.strip()] or [""]


def read_pptx(path: Path) -> tuple[str, list[str]]:
    with zipfile.ZipFile(path) as z:
        slides = sorted((n for n in z.namelist() if re.match(r"ppt/slides/slide\d+\.xml$", n)), key=_natural)
        pages = [_xml_text(z.read(n).decode("utf-8", "replace")) for n in slides]
    return path.stem, pages or [""]


def read_xlsx(path: Path) -> tuple[str, list[str]]:
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        shared = []
        if "xl/sharedStrings.xml" in names:
            xml = z.read("xl/sharedStrings.xml").decode("utf-8", "replace")
            shared = [htmllib.unescape(re.sub(r"<[^>]+>", "", si)) for si in re.findall(r"<si>(.*?)</si>", xml, re.S)]
        pages = []
        for n in sorted((n for n in names if re.match(r"xl/worksheets/sheet\d+\.xml$", n)), key=_natural):
            xml = z.read(n).decode("utf-8", "replace")
            rows = []
            for row in re.findall(r"<row\b.*?</row>", xml, re.S):
                cells = []
                for attrs, body in re.findall(r"<c\b([^>]*)>(.*?)</c>", row, re.S):
                    v = re.search(r"<v>(.*?)</v>", body, re.S)
                    t = re.search(r'\bt="(\w+)"', attrs)
                    if t and t.group(1) == "s" and v:
                        cells.append(shared[int(v.group(1))] if int(v.group(1)) < len(shared) else "")
                    elif t and t.group(1) == "inlineStr":
                        cells.append(htmllib.unescape(re.sub(r"<[^>]+>", "", body)))
                    else:
                        cells.append(htmllib.unescape(v.group(1)) if v else "")
                if any(c.strip() for c in cells):
                    rows.append("\t".join(cells).rstrip())
            pages.append("\n".join(rows))
    return path.stem, pages or [""]


def read_epub(path: Path) -> tuple[str, list[str]]:
    from .web_extract import extract_html
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        container = z.read("META-INF/container.xml").decode("utf-8", "replace") if "META-INF/container.xml" in names else ""
        m = re.search(r'full-path="([^"]+)"', container)
        opf_path = m.group(1) if m else next((n for n in names if n.endswith(".opf")), "")
        order, title = [], ""
        if opf_path:
            opf = z.read(opf_path).decode("utf-8", "replace")
            t = re.search(r"<dc:title[^>]*>(.*?)</dc:title>", opf, re.S)
            title = htmllib.unescape(t.group(1)).strip() if t else ""
            items = {i: h for i, h in re.findall(r'<item\b[^>]*\bid="([^"]+)"[^>]*\bhref="([^"]+)"', opf)}
            items.update({i: h for h, i in re.findall(r'<item\b[^>]*\bhref="([^"]+)"[^>]*\bid="([^"]+)"', opf)})
            base = PurePosixPath(opf_path).parent
            for ref in re.findall(r'<itemref\b[^>]*\bidref="([^"]+)"', opf):
                if ref in items:
                    order.append(str(base / items[ref]) if str(base) != "." else items[ref])
        if not order:
            order = sorted((n for n in names if n.lower().endswith((".xhtml", ".html", ".htm"))), key=_natural)
        pages = []
        for n in order:
            if n in names:
                doc = extract_html(z.read(n).decode("utf-8", "replace"), "")
                if doc["text"].strip():
                    pages.append(doc["text"])
    return title or path.stem, pages or [""]


def read_rtf(path: Path) -> tuple[str, list[str]]:
    raw = path.read_bytes()[:MAX_TEXT_BYTES].decode("latin-1", "replace")
    text = re.sub(r"\\'([0-9a-fA-F]{2})", lambda m: bytes([int(m.group(1), 16)]).decode("cp1252", "replace"), raw)
    text = re.sub(r"\\u(-?\d+)\??", lambda m: chr(int(m.group(1)) % 65536), text)
    text = re.sub(r"\{\\\*[^{}]*\}", "", text)                       # destinos opcionales
    text = re.sub(r"\{\\(fonttbl|colortbl|stylesheet|info)[^{}]*(\{[^{}]*\}[^{}]*)*\}", "", text)
    text = re.sub(r"\\(par|line)\b ?", "\n", text)
    text = re.sub(r"\\tab\b ?", "\t", text)
    text = re.sub(r"\\[a-zA-Z]+-?\d* ?", "", text)
    text = text.replace("\\{", "{").replace("\\}", "}").replace("\\\\", "\\")
    return path.stem, [re.sub(r"[{}]", "", text).strip()]


def read_text(path: Path) -> tuple[str, list[str]]:
    data = path.read_bytes()[:MAX_TEXT_BYTES]
    if b"\x00" in data[:4096]:
        return path.stem, []          # binario con extension de texto
    for enc in ("utf-8-sig", "utf-16") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else ("utf-8-sig",):
        try:
            return path.stem, [data.decode(enc)]
        except UnicodeDecodeError:
            pass
    return path.stem, [data.decode("cp1252", errors="replace")]


def read_document(path: Path) -> tuple[str, list[str]]:
    """Devuelve (titulo_sugerido, paginas_de_texto)."""
    path = Path(path)
    suf = path.suffix.lower()
    if suf == ".pdf":
        from pypdf import PdfReader
        reader = PdfReader(str(path))
        title = (reader.metadata.title if reader.metadata and reader.metadata.title else "") or path.stem
        return title, [(p.extract_text() or "") for p in reader.pages]
    if suf in MARKUP:
        from .web_extract import extract_html
        doc = extract_html(path.read_text(encoding="utf-8", errors="replace"), "")
        return doc["title"] or path.stem, [doc["text"]]
    readers = {".docx": read_docx, ".odt": read_opendocument, ".ods": read_opendocument, ".odp": read_opendocument,
               ".pptx": read_pptx, ".xlsx": read_xlsx, ".epub": read_epub, ".rtf": read_rtf}
    return readers.get(suf, read_text)(path)


def read_safe(path: str) -> tuple[str, list[str], str]:
    """Para los pools: nunca lanza. (titulo, paginas, error)."""
    try:
        title, pages = read_document(Path(path))
        return title, pages, ""
    except Exception as e:  # noqa: BLE001
        return Path(path).stem, [], f"{type(e).__name__}: {e}"[:200]
