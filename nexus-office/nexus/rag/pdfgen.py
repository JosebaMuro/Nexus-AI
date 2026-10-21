"""Maquetacion de documentos extraidos en un PDF estructurado (fpdf2).

Portada, resumen y puntos clave, indice con paginas (y marcadores PDF), contenido por secciones
(parrafos, listas, tablas, codigo, citas), galeria de imagenes, enlaces y metadatos.
Usa fuentes TrueType del sistema para Unicode completo; si no hay, fuentes base (latin-1).
"""
from __future__ import annotations

import datetime as dt
import io
import re
import sys
from pathlib import Path

from fpdf import FPDF
from fpdf.enums import XPos, YPos

ACCENT = (0, 168, 196)
DARK = (4, 6, 12)
INK = (24, 32, 44)
MUTED = (110, 128, 148)
FONT_SETS = [
    {"R": "C:/Windows/Fonts/segoeui.ttf", "B": "C:/Windows/Fonts/segoeuib.ttf", "I": "C:/Windows/Fonts/segoeuii.ttf",
     "M": "C:/Windows/Fonts/consola.ttf"},
    {"R": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "B": "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
     "I": "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf", "M": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"},
    {"R": "/usr/share/fonts/TTF/DejaVuSans.ttf", "B": "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
     "I": "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf", "M": "/usr/share/fonts/TTF/DejaVuSansMono.ttf"},
    {"R": "/System/Library/Fonts/Supplemental/Arial.ttf", "B": "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
     "I": "/System/Library/Fonts/Supplemental/Arial Italic.ttf", "M": "/System/Library/Fonts/Supplemental/Courier New.ttf"},
    {"R": "/system/fonts/Roboto-Regular.ttf", "B": "/system/fonts/Roboto-Bold.ttf", "I": "/system/fonts/Roboto-Italic.ttf",
     "M": "/system/fonts/DroidSansMono.ttf"},
]


class DocPDF(FPDF):
    def __init__(self, title: str, source: str) -> None:
        super().__init__(format="A4")
        self.doc_title, self.source = title, source
        self.set_auto_page_break(True, margin=18)
        self.set_margins(18, 18, 18)
        self.unicode = False
        for fs in FONT_SETS:
            if all(Path(p).exists() for p in fs.values()):
                try:
                    self.add_font("ui", "", fs["R"])
                    self.add_font("ui", "B", fs["B"])
                    self.add_font("ui", "I", fs["I"])
                    self.add_font("mono", "", fs["M"])
                    self.unicode = True
                    break
                except Exception:  # noqa: BLE001
                    continue
        self.f_ui, self.f_mono = ("ui", "mono") if self.unicode else ("helvetica", "courier")

    def t(self, text: str) -> str:
        """Normaliza texto para la fuente disponible."""
        text = re.sub(r"[\x00-\x08\x0b-\x1f\u200b-\u200f\ufeff]", "", str(text or ""))
        text = "".join(c for c in text if ord(c) < 0x10000)  # fuera del BMP (emojis) no suele estar en las fuentes
        if not self.unicode:
            text = (text.replace("•", "-").replace("–", "-").replace("—", "-").replace("“", '"').replace("”", '"')
                    .replace("’", "'").replace("…", "..."))
            text = text.encode("latin-1", "replace").decode("latin-1")
        return text

    def header(self):
        if self.page_no() == 1:
            return
        self.set_font(self.f_ui, "", 8)
        self.set_text_color(*MUTED)
        self.cell(0, 6, self.t(f"Nexus-Office · {self.doc_title[:90]}"), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
        self.set_draw_color(*ACCENT)
        self.set_line_width(0.4)
        self.line(self.l_margin, self.get_y(), self.w - self.r_margin, self.get_y())
        self.ln(4)

    def footer(self):
        if self.page_no() == 1:
            return
        self.set_y(-12)
        self.set_font(self.f_ui, "", 8)
        self.set_text_color(*MUTED)
        self.cell(0, 6, self.t(self.source[:100]), align="L")
        self.set_x(self.l_margin)
        self.cell(0, 6, f"{self.page_no()} / {{nb}}", align="R")


def _render_toc(pdf: DocPDF, outline) -> None:
    pdf.set_font(pdf.f_ui, "B", 16)
    pdf.set_text_color(*ACCENT)
    pdf.cell(0, 10, pdf.t("Índice" if pdf.unicode else "Indice"), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
    pdf.ln(2)
    for sec in outline:
        if sec.level > 2:
            continue
        pdf.set_font(pdf.f_ui, "B" if sec.level == 0 else "", 10.5 if sec.level == 0 else 9.5)
        pdf.set_text_color(*INK)
        indent = 6 * sec.level
        pdf.set_x(pdf.l_margin + indent)
        name = pdf.t(sec.name)[:95]
        w = pdf.w - pdf.l_margin - pdf.r_margin - indent - 12
        pdf.cell(w, 6.5, name, link=pdf.add_link(page=sec.page_number))
        pdf.cell(12, 6.5, str(sec.page_number), align="R", new_x=XPos.LMARGIN, new_y=YPos.NEXT)


def _fetch_image(url: str, timeout: float = 10):
    import httpx
    from PIL import Image
    try:
        r = httpx.get(url, timeout=timeout, follow_redirects=True, headers={"User-Agent": "Mozilla/5.0 (Nexus-Office)"})
        if r.status_code != 200 or len(r.content) > 4_000_000 or not r.headers.get("content-type", "").startswith("image"):
            return None
        img = Image.open(io.BytesIO(r.content))
        img.load()
        if img.width < 80 or img.height < 80:
            return None  # iconos y pixeles de seguimiento
        img = img.convert("RGB")
        img.thumbnail((1400, 1400))
        return img
    except Exception:  # noqa: BLE001
        return None


def build_pdf(doc: dict, summary: str, key_points: list[str], out: Path, max_images: int = 8) -> dict:
    title = doc["title"]
    pdf = DocPDF(title, doc["url"])
    pdf.set_title(pdf.t(title))
    pdf.set_author(pdf.t(doc.get("author") or doc.get("site") or "Nexus-Office"))
    pdf.set_subject(pdf.t(doc.get("description", "")[:200]))
    pdf.set_keywords(pdf.t(", ".join(doc.get("headings", [])[:12])))
    pdf.set_creator("Nexus-Office")
    W = lambda: pdf.w - pdf.l_margin - pdf.r_margin  # noqa: E731

    # ---------------- portada (sin salto automatico: el pie se dibuja dentro del margen inferior)
    pdf.add_page()
    pdf.set_auto_page_break(False)
    pdf.set_fill_color(*DARK)
    pdf.rect(0, 0, pdf.w, pdf.h, "F")
    pdf.set_fill_color(*ACCENT)
    pdf.rect(0, 0, 6, pdf.h, "F")
    pdf.set_xy(22, 40)
    pdf.set_font(pdf.f_ui, "B", 11)
    pdf.set_text_color(0, 240, 255)
    pdf.cell(0, 8, "N E X U S   ·   D O C U M E N T O   E X T R A Í D O" if pdf.unicode else "N E X U S  -  DOCUMENTO EXTRAIDO",
             new_x=XPos.LMARGIN, new_y=YPos.NEXT)
    pdf.set_x(22)
    pdf.ln(8)
    pdf.set_x(22)
    pdf.set_font(pdf.f_ui, "B", 26)
    pdf.set_text_color(235, 250, 255)
    pdf.multi_cell(pdf.w - 44, 12, pdf.t(title), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
    if doc.get("description"):
        pdf.ln(4)
        pdf.set_x(22)
        pdf.set_font(pdf.f_ui, "I", 12)
        pdf.set_text_color(170, 200, 215)
        pdf.multi_cell(pdf.w - 44, 6.5, pdf.t(doc["description"][:500]), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
    pdf.set_y(pdf.h - 95)
    meta = [("Fuente", doc["url"]), ("Sitio", doc.get("site", "")), ("Autor", doc.get("author", "")),
            ("Publicado", doc.get("published", "")), ("Idioma", doc.get("lang", "")),
            ("Extraído", dt.datetime.now().strftime("%d/%m/%Y %H:%M")),
            ("Contenido", f"{doc['words']} palabras · {len(doc['sections'])} secciones · {len(doc['images'])} imágenes · {len(doc['links'])} enlaces")]
    for k, v in meta:
        if not v:
            continue
        pdf.set_x(22)
        pdf.set_font(pdf.f_ui, "B", 9)
        pdf.set_text_color(0, 240, 255)
        pdf.cell(28, 6, pdf.t(k.upper()))
        pdf.set_font(pdf.f_ui, "", 9.5)
        pdf.set_text_color(215, 235, 245)
        pdf.multi_cell(pdf.w - 72, 6, pdf.t(v)[:300], link=doc["url"] if k == "Fuente" else "",
                       new_x=XPos.LMARGIN, new_y=YPos.NEXT)
    pdf.set_xy(22, pdf.h - 22)
    pdf.set_font(pdf.f_ui, "", 8)
    pdf.set_text_color(*MUTED)
    pdf.cell(0, 6, pdf.t("Generado automáticamente por Nexus-Office · Copia archivada en su biblioteca RAG"))
    pdf.set_auto_page_break(True, margin=18)

    # ---------------- indice
    pdf.add_page()
    n_toc = 1 + sum(1 for s in doc["sections"] if s["level"] <= 2) // 34
    pdf.insert_toc_placeholder(_render_toc, pages=n_toc)

    def heading(text: str, level: int) -> None:
        if pdf.get_y() > pdf.h - 45:
            pdf.add_page()
        pdf.start_section(pdf.t(text)[:120], level=max(0, level - 1))
        size = {1: 17, 2: 14, 3: 12}.get(level, 12)
        pdf.ln(3 if level > 1 else 5)
        pdf.set_font(pdf.f_ui, "B", size)
        pdf.set_text_color(*(ACCENT if level == 1 else INK))
        pdf.multi_cell(W(), size * 0.5, pdf.t(text), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
        if level == 1:
            pdf.set_draw_color(*ACCENT)
            pdf.set_line_width(0.6)
            pdf.line(pdf.l_margin, pdf.get_y() + 1, pdf.l_margin + 30, pdf.get_y() + 1)
            pdf.ln(3)
        pdf.ln(1)

    def para(text: str, italic: bool = False) -> None:
        pdf.set_font(pdf.f_ui, "I" if italic else "", 10.5)
        pdf.set_text_color(*INK)
        pdf.multi_cell(W(), 5.6, pdf.t(text), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
        pdf.ln(2)

    # ---------------- resumen
    heading("Resumen", 1)
    para(summary)
    if key_points:
        heading("Puntos clave", 2)
        for kp in key_points:
            pdf.set_font(pdf.f_ui, "", 10.5)
            pdf.set_text_color(*ACCENT)
            pdf.cell(6, 5.6, "•" if pdf.unicode else "-")
            pdf.set_text_color(*INK)
            pdf.multi_cell(W() - 6, 5.6, pdf.t(kp), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
        pdf.ln(2)

    # ---------------- contenido
    heading("Contenido", 1)
    for sec in doc["sections"]:
        if not sec["blocks"] and sec["level"] >= 3:
            continue
        heading(sec["heading"], min(3, sec["level"] + 1))
        for b in sec["blocks"]:
            try:
                if b["type"] in ("p", "quote"):
                    if b["type"] == "quote":
                        x0, y0 = pdf.l_margin, pdf.get_y()
                        pdf.set_left_margin(pdf.l_margin + 6)
                        para(b["text"], italic=True)
                        pdf.set_left_margin(x0)
                        pdf.set_draw_color(*ACCENT)
                        pdf.line(x0 + 2, y0, x0 + 2, pdf.get_y() - 2)
                    else:
                        para(b["text"])
                elif b["type"] == "list":
                    pdf.set_font(pdf.f_ui, "", 10.5)
                    for i, item in enumerate(b["items"][:200]):
                        indent = 4 if item.startswith("  ") else 0
                        pdf.set_x(pdf.l_margin + indent)
                        pdf.set_text_color(*ACCENT)
                        pdf.cell(7, 5.6, f"{i + 1}." if b.get("ordered") and not indent else ("•" if pdf.unicode else "-"))
                        pdf.set_text_color(*INK)
                        pdf.multi_cell(W() - 7 - indent, 5.6, pdf.t(item.strip()), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
                    pdf.ln(2)
                elif b["type"] == "code":
                    pdf.set_font(pdf.f_mono, "", 8.5)
                    pdf.set_fill_color(240, 245, 248)
                    pdf.set_text_color(30, 40, 60)
                    pdf.multi_cell(W(), 4.4, pdf.t(b["text"][:6000]), fill=True, new_x=XPos.LMARGIN, new_y=YPos.NEXT)
                    pdf.ln(2)
                elif b["type"] == "table":
                    rows = [r[:8] for r in b["rows"][:80]]
                    ncol = max(len(r) for r in rows)
                    rows = [[pdf.t(c)[:300] for c in r] + [""] * (ncol - len(r)) for r in rows]
                    pdf.set_font(pdf.f_ui, "", 8.5)
                    pdf.set_text_color(*INK)
                    with pdf.table(text_align="LEFT", line_height=4.6, first_row_as_headings=len(rows) > 1,
                                   headings_style=None, borders_layout="SINGLE_TOP_LINE") as table:
                        for r in rows:
                            row = table.row()
                            for c in r:
                                row.cell(c)
                    pdf.ln(3)
            except Exception:  # noqa: BLE001  - un bloque raro no debe romper el documento
                para(" ".join(b.get("items", [])) or b.get("text", "") or "")

    # ---------------- imagenes
    embedded = 0
    imgs = doc["images"][: max_images * 2]
    if imgs and max_images:
        heading("Imágenes" if pdf.unicode else "Imagenes", 1)
        for im in imgs:
            if embedded >= max_images:
                break
            pil = _fetch_image(im["src"])
            if pil is None:
                continue
            w = min(W(), 150)
            h = w * pil.height / pil.width
            if h > 150:
                h, w = 150, 150 * pil.width / pil.height
            if pdf.get_y() + h + 14 > pdf.h - 18:
                pdf.add_page()
            pdf.image(pil, x=pdf.l_margin + (W() - w) / 2, w=w, h=h)
            pdf.set_font(pdf.f_ui, "I", 8.5)
            pdf.set_text_color(*MUTED)
            pdf.multi_cell(W(), 4.5, pdf.t(im["alt"] or im["src"])[:200], align="C", new_x=XPos.LMARGIN, new_y=YPos.NEXT)
            pdf.ln(4)
            embedded += 1
        if not embedded:
            for im in imgs[:20]:
                para(f"{im['alt'] or 'Imagen'}: {im['src']}")

    # ---------------- enlaces y metadatos
    if doc["links"]:
        heading("Enlaces", 1)
        pdf.set_font(pdf.f_ui, "", 8.5)
        for lk in doc["links"][:120]:
            pdf.set_text_color(*INK)
            pdf.multi_cell(W(), 4.6, pdf.t(f"{lk['text']}"), new_x=XPos.LMARGIN, new_y=YPos.NEXT)
            pdf.set_text_color(*ACCENT)
            pdf.multi_cell(W(), 4.2, pdf.t(lk["href"])[:250], link=lk["href"], new_x=XPos.LMARGIN, new_y=YPos.NEXT)
            pdf.ln(1)
    heading("Metadatos", 1)
    for k, v in meta + [("Palabras clave", doc.get("keywords", "")), ("Generador", f"Nexus-Office · Python {sys.version.split()[0]}")]:
        if v:
            para(f"{k}: {v}")

    out.parent.mkdir(parents=True, exist_ok=True)
    pdf.output(str(out))
    return {"path": str(out), "pages": pdf.page_no(), "images_embedded": embedded, "unicode_fonts": pdf.unicode}
