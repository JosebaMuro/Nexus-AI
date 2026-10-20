"""Genera Manual-de-usuario.pdf y Documentacion-tecnica.pdf a partir de los .md de docs/ (y la guia de la app
autonoma), con portada, indice y pie «Nexus-Office <version> · pagina/total». Los usa packaging/build.py --docs.

    venv/bin/pip install markdown playwright      # Chromium: el de Playwright o PLAYWRIGHT_BROWSERS_PATH
    venv/bin/python packaging/build_docs_pdf.py [carpeta_salida]     # por defecto dist/
"""
from __future__ import annotations

import html
import os
import re
import sys
from pathlib import Path

import markdown

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from nexus import __version__ as VERSION  # noqa: E402

BOOKS = {
    "Manual-de-usuario": ("Manual de usuario", "Oficina de IA local para pymes",
                          ["docs/USUARIO.md", "docs/OFICINA.md", "docs/BOLSA.md", "docs/SERVICIO.md", "docs/AVATAR.md", "docs/SALON.md"]),
    "Documentacion-tecnica": ("Documentación técnica", "Arquitectura, plataformas, administración y API",
                              ["docs/ARQUITECTURA.md", "docs/PLATAFORMAS.md", "android-autonomo/README.md",
                               "docs/ADMINISTRADOR.md", "docs/SERVICIO.md", "docs/API.md", "docs/HERRAMIENTAS.md",
                               "docs/MEMORIA.md", "docs/RAG.md", "docs/RED_COOPERATIVA.md", "docs/VPN.md",
                               "docs/ACCESO_REMOTO.md"]),
}

CSS = """
@page { size: A4; margin: 18mm 16mm 20mm 16mm; }
body { font-family: 'DejaVu Sans', 'Noto Sans', Arial, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1d2330; }
h1 { font-size: 20pt; color: #0b4f6c; border-bottom: 2px solid #0b4f6c; padding-bottom: 4px; page-break-before: always; }
h2 { font-size: 14pt; color: #0b4f6c; margin-top: 1.4em; } h3 { font-size: 11.5pt; color: #24384a; }
h2, h3 { page-break-after: avoid; }
table { border-collapse: collapse; width: 100%; margin: .6em 0; font-size: 9pt; page-break-inside: auto; }
th, td { border: 1px solid #c7d0da; padding: 3px 6px; vertical-align: top; text-align: left; }
th { background: #e8eef4; } tr { page-break-inside: avoid; }
code { font-family: 'DejaVu Sans Mono', monospace; font-size: 8.8pt; background: #f1f4f7; padding: 0 2px; }
pre { background: #f1f4f7; padding: 6px 8px; white-space: pre-wrap; word-break: break-word; font-size: 8.5pt; }
pre code { background: none; padding: 0; }
a { color: #0b4f6c; text-decoration: none; }
.cover { height: 250mm; display: flex; flex-direction: column; justify-content: center; text-align: center; }
.cover .v { font-size: 30pt; font-weight: bold; color: #0b4f6c; } .cover .t { font-size: 22pt; margin-top: 8mm; }
.cover .s { font-size: 13pt; color: #556; margin-top: 4mm; }
.toc { page-break-before: always; } .toc ul { list-style: none; padding-left: 1.2em; } .toc > ul { padding-left: 0; }
.toc li { margin: 2px 0; } .toc .l1 { font-weight: bold; margin-top: 6px; }
"""


def _slug(text: str, used: set[str]) -> str:
    s = re.sub(r"[^\w\- ]", "", text.lower(), flags=re.U).strip().replace(" ", "-") or "s"
    base, n = s, 1
    while s in used:
        n += 1
        s = f"{base}-{n}"
    used.add(s)
    return s


def build_html(title: str, subtitle: str, files: list[str]) -> str:
    md = markdown.Markdown(extensions=["tables", "fenced_code", "sane_lists"])
    used: set[str] = set()
    toc, body = [], []
    for f in files:
        text = (ROOT / f).read_text(encoding="utf-8")
        part = md.reset().convert(text)

        def head(m: re.Match) -> str:
            level, inner = int(m.group(1)), m.group(2)
            plain = html.unescape(re.sub(r"<[^>]+>", "", inner))
            sid = _slug(plain, used)
            if level <= 3:
                toc.append((level, plain, sid))
            return f'<h{level} id="{sid}">{inner}</h{level}>'
        part = re.sub(r"<h([1-6])>(.*?)</h\1>", head, part, flags=re.S)
        part = re.sub(r'href="(?!https?:|#)[^"]*"', 'href="#"', part)   # enlaces a otros .md: sin destino en el PDF
        body.append(part)
    items = "".join(f'<li class="l{lvl}" style="margin-left:{(lvl - 1) * 1.2}em"><a href="#{sid}">{html.escape(t)}</a></li>'
                    for lvl, t, sid in toc)
    return (f"<!doctype html><html lang='es'><head><meta charset='utf-8'><title>{title}</title><style>{CSS}</style></head>"
            f"<body><div class='cover'><div class='v'>Nexus-Office {VERSION}</div><div class='t'>{title}</div>"
            f"<div class='s'>{subtitle}</div></div><div class='toc'><h2>{title}</h2><ul>{items}</ul></div>"
            f"{''.join(body)}</body></html>")


def main(out: Path) -> list[Path]:
    from playwright.sync_api import sync_playwright
    out.mkdir(parents=True, exist_ok=True)
    done = []
    footer = (f"<div style='font-size:8px;width:100%;text-align:right;padding-right:16mm;color:#667'>Nexus-Office {VERSION} · "
              "<span class='pageNumber'></span>/<span class='totalPages'></span></div>")
    with sync_playwright() as p:
        exe = os.environ.get("NEXUS_CHROMIUM")   # Chromium ya instalado, si el de Playwright no coincide de version
        browser = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
        page = browser.new_page()
        for name, (title, subtitle, files) in BOOKS.items():
            page.set_content(build_html(title, subtitle, files), wait_until="load")
            dest = out / f"{name}.pdf"
            page.pdf(path=str(dest), format="A4", print_background=True, display_header_footer=True,
                     header_template="<span></span>", footer_template=footer,
                     margin={"top": "18mm", "bottom": "20mm", "left": "16mm", "right": "16mm"})
            done.append(dest)
            print(f"[docs] -> {dest} ({dest.stat().st_size / 1e3:.0f} KB)")
        browser.close()
    return done


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist")
