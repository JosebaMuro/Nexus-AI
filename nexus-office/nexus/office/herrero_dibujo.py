"""Oficina Herrero: dibujo de la estructura (malla 3D de los perfiles, vista isometrica, plano de montaje con cotas,
marcas y simbolos de soldadura ISO 2553) en SVG y en PDF vectorial, sin dependencias.

El plano se compone sobre un ``Lienzo`` en milimetros de papel que sabe escribirse como SVG o como pagina PDF; las
caras de los perfiles se ordenan por profundidad (algoritmo del pintor) y se rellenan de blanco en los alzados, que
asi ocultan las lineas que quedan detras como en un plano a mano.
"""
from __future__ import annotations

import datetime as dt
import math
import zlib

from . import herrero_calculo as C
from .herrero_modelo import ACABADOS, Barra, Estructura


# ---------------------------------------------------------------------------------------------- vectores
def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _mul(a, k):
    return (a[0] * k, a[1] * k, a[2] * k)


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm(a):
    n = math.sqrt(_dot(a, a)) or 1.0
    return (a[0] / n, a[1] / n, a[2] / n)


# ---------------------------------------------------------------------------------------------- malla
def seccion(p: C.Perfil, lados_red: int = 10) -> list[tuple[float, float]]:
    """Contorno de la seccion en (u, v): u en el ancho b y v en el canto h, centrado y en sentido antihorario."""
    b, h = p.b / 2, p.h / 2
    if p.forma == "doble_t":
        tw, tf = p.tw / 2, p.tf
        pts = [(-b, -h), (b, -h), (b, -h + tf), (tw, -h + tf), (tw, h - tf), (b, h - tf), (b, h), (-b, h),
               (-b, h - tf), (-tw, h - tf), (-tw, -h + tf), (-b, -h + tf)]
    elif p.forma == "u":
        tw, tf = p.tw, p.tf
        pts = [(-b, -h), (b, -h), (b, -h + tf), (-b + tw, -h + tf), (-b + tw, h - tf), (b, h - tf), (b, h), (-b, h)]
    elif p.forma == "peldano":
        t = p.tw
        pts = [(-b, -h), (-b + t, -h), (-b + t, h - t), (b - t, h - t), (b - t, -h), (b, -h), (b, h), (-b, h)]
    elif p.forma == "angular":
        t = p.t
        pts = [(-b, -h), (b, -h), (b, -h + t), (-b + t, -h + t), (-b + t, h), (-b, h)]
    elif p.forma in ("tubo_red", "macizo_red"):
        pts = [(h * math.cos(2 * math.pi * k / lados_red), h * math.sin(2 * math.pi * k / lados_red))
               for k in range(lados_red)]
    else:
        pts = [(-b, -h), (b, -h), (b, h), (-b, h)]
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]))
    return pts if area > 0 else pts[::-1]


def ejes(b: Barra):
    w = _norm(_sub(b.p2, b.p1))
    if b.orient:
        o = b.orient
    elif abs(w[2]) > 0.95:
        o = (1.0, 0.0, 0.0)
    else:
        o = (0.0, 0.0, 1.0)
    v = _norm(_sub(o, _mul(w, _dot(o, w))))
    u = _cross(v, w)
    return w, u, v


def caras_barra(b: Barra, tramo: float = 0.0, lados_red: int = 10) -> list[tuple]:
    """Caras (puntos, normal, clase) del perfil extruido; ``tramo`` > 0 parte las caras largas en trozos (el orden
    por profundidad falla menos con caras cortas)."""
    w, u, v = ejes(b)
    sec = seccion(b.perfil, lados_red)
    lon = b.longitud
    n = max(1, math.ceil(lon / tramo)) if tramo else 1
    out = []
    anillos = []
    for k in range(n + 1):
        c = _add(b.p1, _mul(w, lon * k / n))
        anillos.append([_add(c, _add(_mul(u, su), _mul(v, sv))) for su, sv in sec])
    m = len(sec)
    for i in range(m):
        (u0, v0), (u1, v1) = sec[i], sec[(i + 1) % m]
        nrm = _norm(_add(_mul(u, v1 - v0), _mul(v, -(u1 - u0))))
        for k in range(n):
            a, bb = anillos[k], anillos[k + 1]
            out.append(([a[i], a[(i + 1) % m], bb[(i + 1) % m], bb[i]], nrm, "acero"))
    out.append((anillos[0][::-1], _mul(w, -1), "corte"))
    out.append((anillos[-1], w, "corte"))
    return out


def caras_caja(c, dx, dy, dz, clase="placa") -> list[tuple]:
    x0, x1 = c[0] - dx / 2, c[0] + dx / 2
    y0, y1 = c[1] - dy / 2, c[1] + dy / 2
    z0, z1 = c[2] - dz / 2, c[2] + dz / 2
    P = lambda x, y, z: (x, y, z)
    return [([P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)], (0, 0, 1), clase),
            ([P(x0, y1, z0), P(x1, y1, z0), P(x1, y0, z0), P(x0, y0, z0)], (0, 0, -1), clase),
            ([P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)], (0, -1, 0), clase),
            ([P(x1, y1, z0), P(x0, y1, z0), P(x0, y1, z1), P(x1, y1, z1)], (0, 1, 0), clase),
            ([P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)], (1, 0, 0), clase),
            ([P(x0, y1, z0), P(x0, y0, z0), P(x0, y0, z1), P(x0, y1, z1)], (-1, 0, 0), clase)]


def _trocear(poli, paso: float) -> list[list]:
    """Parte un cuadrilatero en una reticula de trozos de ``paso`` mm (para ordenar bien por profundidad)."""
    if len(poli) != 4 or not paso:
        return [poli]
    a, b, c, d = poli
    nu = max(1, math.ceil(math.dist(a, b) / paso))
    nv = max(1, math.ceil(math.dist(a, d) / paso))

    def pt(s, t):
        p1 = _add(a, _mul(_sub(b, a), s))
        p2 = _add(d, _mul(_sub(c, d), s))
        return _add(p1, _mul(_sub(p2, p1), t))

    return [[pt(i / nu, j / nv), pt((i + 1) / nu, j / nv), pt((i + 1) / nu, (j + 1) / nv), pt(i / nu, (j + 1) / nv)]
            for i in range(nu) for j in range(nv)]


def malla(e: Estructura, tramo: float = 0.0, paneles: bool = True, lados_red: int = 10,
          trozo_panel: float = 0.0) -> list[tuple]:
    caras = []
    for b in e.barras:
        caras += [(*f, b) for f in caras_barra(b, tramo, lados_red)]
    for p in e.placas:
        d = {"z": (p.a, p.b, p.t), "x": (p.t, p.a, p.b), "y": (p.a, p.t, p.b)}[p.normal]
        caras += [(*f, p) for f in caras_caja(p.centro, *d)]
    if paneles:
        for poli, tipo in e.paneles:
            nrm = _norm(_cross(_sub(poli[1], poli[0]), _sub(poli[2], poli[0])))
            caras += [(q, nrm, "panel:" + tipo, None) for q in _trocear(poli, trozo_panel)]
    return caras


class Vista:
    """Proyeccion paralela con la camara en la direccion ``d`` (de la escena hacia la camara)."""

    def __init__(self, d):
        self.d = _norm(d)
        if abs(self.d[2]) > 0.999:
            self.r, self.u = (1.0, 0.0, 0.0), (0.0, 1.0 if self.d[2] > 0 else -1.0, 0.0)
        else:
            self.r = _norm(_cross((0, 0, 1), self.d))
            self.u = _cross(self.d, self.r)

    def p(self, q):
        return _dot(q, self.r), -_dot(q, self.u)

    def prof(self, q):
        return _dot(q, self.d)


def iso(az=-50.0, el=30.0) -> Vista:
    a, e = math.radians(az), math.radians(el)
    return Vista((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))


# ---------------------------------------------------------------------------------------------- lienzo SVG/PDF
def _rgb(c: str) -> tuple[float, float, float]:
    c = c.lstrip("#")
    if len(c) == 3:
        c = "".join(x * 2 for x in c)
    return tuple(int(c[i:i + 2], 16) / 255 for i in (0, 2, 4))


def _hex(rgb) -> str:
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(round(x)))) for x in rgb)


def _esc(s: str) -> str:
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


_ANCHO = {**{c: 0.556 for c in "0123456789"}, " ": 0.278, ".": 0.278, ",": 0.278, ":": 0.278, "-": 0.333, "–": 0.556,
          "/": 0.278, "(": 0.333, ")": 0.333, "x": 0.5, "i": 0.222, "l": 0.222, "j": 0.222, "t": 0.278, "f": 0.278,
          "r": 0.333, "I": 0.278, "M": 0.833, "W": 0.944, "m": 0.833, "w": 0.722, "Ø": 0.778, "²": 0.333, "%": 0.889}


def ancho_texto(s: str, tam: float, negrita=False) -> float:
    w = sum(_ANCHO.get(ch, 0.667 if ch.isupper() else 0.556) for ch in str(s))
    return w * tam * (1.06 if negrita else 1.0)


class Lienzo:
    """Dibujo en milimetros de papel (y hacia abajo) que se escribe como SVG o como pagina de PDF."""

    def __init__(self, ancho: float = 420, alto: float = 297, fondo: str = "#ffffff"):
        self.ancho, self.alto, self.fondo = ancho, alto, fondo
        self.ops: list[tuple] = []

    def linea(self, x1, y1, x2, y2, g=0.25, color="#000", trazo=None):
        self.ops.append(("l", x1, y1, x2, y2, g, color, trazo))

    def poligono(self, pts, relleno=None, color="#000", g=0.18, cerrado=True, trazo=None):
        self.ops.append(("p", [tuple(p) for p in pts], relleno, color, g, cerrado, trazo))

    def rect(self, x, y, w, h, relleno=None, color="#000", g=0.25):
        self.poligono([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], relleno, color, g)

    def circulo(self, x, y, r, relleno=None, color="#000", g=0.2):
        self.ops.append(("c", x, y, r, relleno, color, g))

    def texto(self, x, y, s, tam=3.0, ancla="start", negrita=False, color="#000", rot=0.0):
        self.ops.append(("t", x, y, str(s), tam, ancla, negrita, color, rot))

    # ---------------------------------------------------------------- SVG
    def svg(self, titulo: str = "") -> str:
        o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.ancho:g} {self.alto:g}" '
             f'width="{self.ancho:g}mm" height="{self.alto:g}mm" font-family="Helvetica, Arial, sans-serif">']
        if titulo:
            o.append(f"<title>{_esc(titulo)}</title>")
        o.append(f'<rect width="100%" height="100%" fill="{self.fondo}"/>')
        for op in self.ops:
            k = op[0]
            if k == "l":
                _, x1, y1, x2, y2, g, c, tr = op
                dash = f' stroke-dasharray="{" ".join(map(str, tr))}"' if tr else ""
                o.append(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="{c}" '
                         f'stroke-width="{g:g}"{dash}/>')
            elif k == "p":
                _, pts, rel, c, g, cerr, tr = op
                tag = "polygon" if cerr else "polyline"
                dash = f' stroke-dasharray="{" ".join(map(str, tr))}"' if tr else ""
                st = f'stroke="{c}" stroke-width="{g:g}" stroke-linejoin="round"' if c and g else 'stroke="none"'
                o.append(f'<{tag} points="{" ".join(f"{x:.2f},{y:.2f}" for x, y in pts)}" '
                         f'fill="{rel or "none"}" {st}{dash}/>')
            elif k == "c":
                _, x, y, r, rel, c, g = op
                o.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r:.2f}" fill="{rel or "none"}" stroke="{c}" '
                         f'stroke-width="{g:g}"/>')
            else:
                _, x, y, s, tam, an, neg, c, rot = op
                a = {"start": "start", "middle": "middle", "end": "end"}[an]
                tr = f' transform="rotate({rot:g} {x:.2f} {y:.2f})"' if rot else ""
                o.append(f'<text x="{x:.2f}" y="{y:.2f}" font-size="{tam:g}" text-anchor="{a}" fill="{c}"'
                         f'{" font-weight=\"bold\"" if neg else ""}{tr}>{_esc(s)}</text>')
        o.append("</svg>")
        return "\n".join(o)

    # ---------------------------------------------------------------- PDF
    def pdf_contenido(self) -> bytes:
        k = 72 / 25.4
        H = self.alto
        X = lambda x: x * k
        Y = lambda y: (H - y) * k
        o = []

        def color(c, trazo=True):
            r, g, b = _rgb(c)
            return f"{r:.3f} {g:.3f} {b:.3f} {'RG' if trazo else 'rg'}"

        if self.fondo and self.fondo.lower() not in ("#fff", "#ffffff"):
            o.append(f"{color(self.fondo, False)} 0 0 {X(self.ancho):.2f} {X(self.alto):.2f} re f")
        for op in self.ops:
            kind = op[0]
            if kind == "l":
                _, x1, y1, x2, y2, g, c, tr = op
                dash = f"[{' '.join(f'{d * k:.2f}' for d in tr)}] 0 d" if tr else "[] 0 d"
                o.append(f"{color(c)} {g * k:.3f} w {dash} {X(x1):.2f} {Y(y1):.2f} m {X(x2):.2f} {Y(y2):.2f} l S")
            elif kind == "p":
                _, pts, rel, c, g, cerr, tr = op
                if not pts:
                    continue
                camino = f"{X(pts[0][0]):.2f} {Y(pts[0][1]):.2f} m " + " ".join(
                    f"{X(x):.2f} {Y(y):.2f} l" for x, y in pts[1:]) + (" h" if cerr else "")
                dash = f"[{' '.join(f'{d * k:.2f}' for d in tr)}] 0 d " if tr else "[] 0 d "
                if rel and c and g:
                    o.append(f"{color(rel, False)} {color(c)} {g * k:.3f} w {dash}{camino} B")
                elif rel:
                    o.append(f"{color(rel, False)} {camino} f")
                else:
                    o.append(f"{color(c)} {g * k:.3f} w {dash}{camino} S")
            elif kind == "c":
                _, x, y, r, rel, c, g = op
                kk = 0.5523 * r
                cx, cy = x, y
                pts = [(cx + r, cy), (cx + r, cy - kk), (cx + kk, cy - r), (cx, cy - r), (cx - kk, cy - r),
                       (cx - r, cy - kk), (cx - r, cy), (cx - r, cy + kk), (cx - kk, cy + r), (cx, cy + r),
                       (cx + kk, cy + r), (cx + r, cy + kk), (cx + r, cy)]
                camino = f"{X(pts[0][0]):.2f} {Y(pts[0][1]):.2f} m " + " ".join(
                    f"{X(pts[i][0]):.2f} {Y(pts[i][1]):.2f} {X(pts[i + 1][0]):.2f} {Y(pts[i + 1][1]):.2f} "
                    f"{X(pts[i + 2][0]):.2f} {Y(pts[i + 2][1]):.2f} c" for i in range(1, 12, 3)) + " h"
                pint = "B" if rel else "S"
                o.append(f"{color(rel, False) if rel else ''} {color(c)} {g * k:.3f} w [] 0 d {camino} {pint}")
            else:
                _, x, y, s, tam, an, neg, c, rot = op
                w = ancho_texto(s, tam, neg)
                dx = {"start": 0, "middle": -w / 2, "end": -w}[an]
                a = math.radians(-rot)
                ca, sa = math.cos(a), math.sin(a)
                px, py = X(x) + dx * k * ca, Y(y) + dx * k * sa
                txt = s.encode("cp1252", "replace").replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")
                o.append(f"BT /{'F2' if neg else 'F1'} {tam * k:.2f} Tf {color(c, False)} {ca:.4f} {sa:.4f} "
                         f"{-sa:.4f} {ca:.4f} {px:.2f} {py:.2f} Tm (".encode() + txt + b") Tj ET")
                continue
        return b"\n".join(x if isinstance(x, bytes) else x.encode() for x in o)


def pdf(paginas: list[Lienzo], titulo: str = "") -> bytes:
    """PDF vectorial con una pagina por lienzo (Helvetica, codificacion WinAnsi para las tildes)."""
    k = 72 / 25.4
    objs: list[bytes] = []

    def nuevo(b: bytes) -> int:
        objs.append(b)
        return len(objs)

    cat = nuevo(b"")
    pags = nuevo(b"")
    f1 = nuevo(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    f2 = nuevo(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>")
    hijos = []
    for lz in paginas:
        datos = zlib.compress(lz.pdf_contenido())
        cont = nuevo(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(datos) + datos + b"\nendstream")
        hijos.append(nuevo((f"<< /Type /Page /Parent {pags} 0 R /MediaBox [0 0 {lz.ancho * k:.2f} {lz.alto * k:.2f}] "
                            f"/Resources << /Font << /F1 {f1} 0 R /F2 {f2} 0 R >> >> /Contents {cont} 0 R >>").encode()))
    objs[pags - 1] = f"<< /Type /Pages /Kids [{' '.join(f'{h} 0 R' for h in hijos)}] /Count {len(hijos)} >>".encode()
    objs[cat - 1] = f"<< /Type /Catalog /Pages {pags} 0 R >>".encode()
    info = nuevo(b"<< /Title (" + titulo.encode("cp1252", "replace").replace(b"(", b"[").replace(b")", b"]") +
                 b") /Producer (Agente IA - Oficina Herrero) >>")
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    pos = []
    for i, ob in enumerate(objs, 1):
        pos.append(len(out))
        out += f"{i} 0 obj\n".encode() + ob + b"\nendobj\n"
    x = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
    out += b"".join(f"{p:010d} 00000 n \n".encode() for p in pos)
    out += f"trailer\n<< /Size {len(objs) + 1} /Root {cat} 0 R /Info {info} 0 R >>\nstartxref\n{x}\n%%EOF\n".encode()
    return bytes(out)


# ---------------------------------------------------------------------------------------------- dibujar vistas
COLOR_PANEL = {"sandwich": (150, 40, 32), "teja": (166, 64, 40), "chapa": (180, 186, 190), "policarbonato": (190, 220, 235),
               "lamas": (120, 84, 50), "tramex": (120, 124, 128), "chapa_suelo": (130, 134, 138),
               "tablero": (176, 140, 96), "colaborante": (170, 170, 165)}


def _color_cara(f, acabado: str, luz, vista_d) -> tuple[str, float]:
    pts, nrm, clase, _ = f
    if _dot(nrm, vista_d) < 0:
        nrm = _mul(nrm, -1)
    base = ACABADOS.get(acabado, ACABADOS["galvanizado"])[1]
    if clase.startswith("panel:"):
        base = COLOR_PANEL.get(clase[6:], (150, 150, 150))
    elif clase == "placa":
        base = tuple(x * 0.9 for x in base)
    elif clase == "corte":
        base = tuple(x * 0.85 for x in base)
    k = 0.42 + 0.58 * max(0.0, _dot(nrm, luz))
    return _hex(tuple(c * k for c in base)), (0.55 if clase.startswith("panel:") else 1.0)


def dibujar_3d(lz: Lienzo, e: Estructura, vista: Vista, caja, modo="color", paneles=True, marcas=True,
               escala=None) -> float:
    """Dibuja la malla en ``caja`` = (x, y, ancho, alto) de papel. ``modo``: color (sombreado) o linea (plano, relleno
    blanco y contorno negro). Devuelve la escala (mm de papel por mm reales)."""
    caras = malla(e, paneles=paneles, lados_red=8 if modo == "linea" else 10)
    vis = []
    for f in caras:
        pts, nrm, clase, _ = f
        if not clase.startswith("panel:") and _dot(nrm, vista.d) <= 1e-6:
            continue
        vis.append(f)
    todos = [vista.p(q) for f in caras for q in f[0]] or [(0, 0)]
    minx, maxx = min(p[0] for p in todos), max(p[0] for p in todos)
    miny, maxy = min(p[1] for p in todos), max(p[1] for p in todos)
    x0, y0, W, H = caja
    if escala is None:
        escala = min(W / max(1e-6, maxx - minx), H / max(1e-6, maxy - miny))
    ox = x0 + (W - (maxx - minx) * escala) / 2 - minx * escala
    oy = y0 + (H - (maxy - miny) * escala) / 2 - miny * escala
    P = lambda q: (ox + vista.p(q)[0] * escala, oy + vista.p(q)[1] * escala)
    vis.sort(key=lambda f: sum(vista.prof(q) for q in f[0]) / len(f[0]))
    luz = _norm((-0.35, -0.65, 0.75))
    for f in vis:
        pts2 = [P(q) for q in f[0]]
        if modo == "linea":
            if f[2].startswith("panel:"):
                continue
            lz.poligono(pts2, "#ffffff", "#111111", 0.13 if f[2] != "corte" else 0.1)
        else:
            col, _ = _color_cara(f, e.parametros.get("acabado", "galvanizado"), luz, vista.d)
            lz.poligono(pts2, col, col, 0.05)
    lz._origen = (ox, oy, escala, vista)
    if marcas:
        poner_marcas(lz, e, vista, P, caja)
    return escala


def poner_marcas(lz: Lienzo, e: Estructura, vista: Vista, P, caja, solo: set | None = None) -> None:
    """Una burbuja por marca, en la pieza de esa marca que mas se ve en la vista."""
    mejor: dict[str, tuple[float, Barra]] = {}
    for b in e.barras:
        if solo is not None and b.marca not in solo:
            continue
        a, c = P(b.p1), P(b.p2)
        lon = math.dist(a, c)
        if lon > 4 and (b.marca not in mejor or lon > mejor[b.marca][0]):
            mejor[b.marca] = (lon, b)
    usados = []
    for marca, (lon, b) in sorted(mejor.items()):
        a, c = P(b.p1), P(b.p2)
        mx, my = (a[0] + c[0]) / 2, (a[1] + c[1]) / 2
        for k in range(8):                                        # aparta las burbujas que se pisan
            bx, by = mx + 6 + (k % 3) * 3, my - 6 - (k // 3) * 3.5
            if all(math.dist((bx, by), u) > 6 for u in usados):
                break
        usados.append((bx, by))
        lz.linea(mx, my, bx - 2.4, by + 1.2, 0.15, "#444")
        lz.circulo(mx, my, 0.5, "#000", "#000", 0.1)
        r = max(2.6, ancho_texto(marca, 2.4, True) / 2 + 0.8)
        lz.circulo(bx, by, r, "#ffffff", "#000", 0.2)
        lz.texto(bx, by + 0.85, marca, 2.4, "middle", True)


def cota(lz: Lienzo, a, b, desplazar: float, texto: str, tam=2.5) -> None:
    """Cota alineada entre a y b (mm de papel), separada ``desplazar`` hacia la izquierda del sentido a->b."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dy) or 1
    nx, ny = -dy / L, dx / L
    a2 = (a[0] + nx * desplazar, a[1] + ny * desplazar)
    b2 = (b[0] + nx * desplazar, b[1] + ny * desplazar)
    s = 1.5 if desplazar >= 0 else -1.5
    lz.linea(a[0] + nx * s * 0.7, a[1] + ny * s * 0.7, a2[0] + nx * s, a2[1] + ny * s, 0.13, "#333")
    lz.linea(b[0] + nx * s * 0.7, b[1] + ny * s * 0.7, b2[0] + nx * s, b2[1] + ny * s, 0.13, "#333")
    lz.linea(*a2, *b2, 0.18, "#333")
    for p in (a2, b2):                                           # trazos oblicuos de arquitectura
        lz.linea(p[0] - 1.1 * (dx / L - nx) / 1.4, p[1] - 1.1 * (dy / L - ny) / 1.4,
                 p[0] + 1.1 * (dx / L - nx) / 1.4, p[1] + 1.1 * (dy / L - ny) / 1.4, 0.35, "#000")
    mx, my = (a2[0] + b2[0]) / 2, (a2[1] + b2[1]) / 2
    ang = math.degrees(math.atan2(dy, dx))
    if ang > 90 or ang < -90:
        ang += 180
    off = 1.0
    lz.texto(mx + nx * off * (1 if desplazar >= 0 else -1) * 0 + (0.9 * math.sin(math.radians(ang))),
             my - 0.9 * math.cos(math.radians(ang)), texto, tam, "middle", rot=ang)


def simbolo_soldadura(lz: Lienzo, x, y, a: float, todo_alrededor: bool, proceso: str = "135") -> float:
    """Simbolo ISO 2553 (sistema A): flecha, linea de referencia, triangulo de soldadura en angulo en el lado de la
    flecha, garganta a la izquierda, circulo de soldadura periferica y proceso ISO 4063 en la cola. Devuelve el
    ancho ocupado."""
    yr = y - 4
    lz.linea(x, y, x + 5, yr, 0.2)
    lz.poligono([(x, y), (x + 0.6, y - 1.5), (x + 1.4, y - 0.8)], "#000", "#000", 0.1)
    lz.linea(x + 5, yr, x + 26, yr, 0.25)
    if todo_alrededor:
        lz.circulo(x + 5, yr, 1.1, None, "#000", 0.2)
    lz.texto(x + 12.4, yr + 3.2, f"a{C._fmt(a)}", 2.4, "end")
    lz.poligono([(x + 13, yr), (x + 13, yr + 3.2), (x + 16.2, yr)], None, "#000", 0.25)
    lz.linea(x + 26, yr, x + 28, yr - 1.6, 0.2)
    lz.linea(x + 26, yr, x + 28, yr + 1.6, 0.2)
    lz.texto(x + 28.8, yr + 0.9, proceso, 2.4)
    return 34


ESCALAS = (5, 10, 20, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500, 750, 1000)


def _escala(necesaria: float) -> int:
    """La primera escala normalizada 1:N que cabe (necesaria = mm de papel por mm reales)."""
    for n in ESCALAS:
        if 1 / n <= necesaria:
            return n
    return ESCALAS[-1]


def _cajetin(lz: Lienzo, e: Estructura, x, y, w, h, plano: str, num: str, escala: str, proyecto_id: str) -> None:
    lz.rect(x, y, w, h, None, "#000", 0.5)
    filas = [("OFICINA HERRERO · AGENTE IA", True, 3.4), (e.titulo.upper(), True, 3.0),
             (_dims(e), False, 2.6), (f"Plano: {plano}", True, 2.8),
             (f"Escala {escala}   ·   Hoja {num}   ·   {dt.date.today():%d/%m/%Y}   ·   Ref. {proyecto_id}", False, 2.4),
             (f"Acero {e.parametros['acero']}JR · {ACABADOS[e.parametros['acabado']][0]}", False, 2.3),
             ("PREDIMENSIONADO ORIENTATIVO: no válido para obra sin el proyecto", True, 2.3),
             ("y la firma de un técnico competente (CTE, EN 1090).", True, 2.3)]
    yy = y + 5
    for txt, neg, tam in filas:
        lz.texto(x + 3, yy, txt, tam, negrita=neg, color="#a00" if txt.startswith(("PREDIM", "y la firma")) else "#000")
        yy += tam + 2.2
        if yy < y + h - 1:
            lz.linea(x, yy - tam - 1.0, x + w, yy - tam - 1.0, 0.1, "#999")


def _dims(e: Estructura) -> str:
    P = e.parametros
    if e.tipo == "escalera":
        return f"Altura a salvar {P['alto']:.2f} m · ancho {P['ancho']:.2f} m · {e.cotas.get('peldanos', '')} peldaños"
    if e.tipo == "barandilla":
        return f"Longitud {P['largo']:.2f} m · altura {e.cotas['alto'] / 1000:.2f} m"
    return f"{P['largo']:.2f} × {P['ancho']:.2f} m · altura {P['alto']:.2f} m"


def plano_montaje(e: Estructura, proyecto_id: str = "") -> Lienzo:
    """Hoja A3 apaisada: alzado frontal, alzado lateral y planta a la misma escala normalizada, con cotas generales,
    entre ejes y marcas de las piezas; cuadro de uniones soldadas con su simbolo ISO 2553 y cajetin."""
    lz = Lienzo(420, 297)
    lz.rect(10, 10, 400, 277, None, "#000", 0.6)
    vistas = [("ALZADO FRONTAL", Vista((0, -1, 0)), (18, 18, 245, 112)),
              ("ALZADO LATERAL", Vista((1, 0, 0)), (275, 18, 125, 112)),
              ("PLANTA", Vista((0, 0, 1)), (18, 146, 245, 124))]
    # escala comun: la que cabe en todas las vistas
    caras = malla(e, paneles=False)
    nec = []
    for _, v, (x, y, w, h) in vistas:
        pts = [v.p(q) for f in caras for q in f[0]]
        ex = max(p[0] for p in pts) - min(p[0] for p in pts)
        ey = max(p[1] for p in pts) - min(p[1] for p in pts)
        nec.append(min((w - 30) / max(ex, 1), (h - 26) / max(ey, 1)))
    n = _escala(min(nec))
    esc = 1 / n
    asignadas: dict[str, int] = {}
    for b in e.barras:                     # cada marca se rotula en la vista donde mas se ve
        largos = []
        for i, (_, v, _) in enumerate(vistas):
            a, c = v.p(b.p1), v.p(b.p2)
            largos.append(math.dist(a, c))
        i = max(range(3), key=lambda k: largos[k])
        if b.marca not in asignadas or largos[i] > 0:
            asignadas.setdefault(b.marca, i)
    for i, (nombre, v, caja) in enumerate(vistas):
        dibujar_3d(lz, e, v, caja, "linea", paneles=False, marcas=False, escala=esc)
        ox, oy, _, _ = lz._origen
        P = lambda q, v=v, ox=ox, oy=oy: (ox + v.p(q)[0] * esc, oy + v.p(q)[1] * esc)
        poner_marcas(lz, e, v, P, caja, {m for m, k in asignadas.items() if k == i})
        x, y, w, h = caja
        lz.texto(x, y + h + 6, f"{nombre}  E 1:{n}", 3.2, negrita=True)
        lz.linea(x, y + h + 7, x + ancho_texto(f"{nombre}  E 1:{n}", 3.2, True), y + h + 7, 0.3)
        _cotas_vista(lz, e, nombre, P)
    # cuadro de uniones soldadas
    x0, y0 = 275, 168
    lz.texto(x0, y0, "UNIONES SOLDADAS (ISO 2553 · proceso 135 MAG)", 2.8, negrita=True)
    yy = y0 + 4
    for u in [u for u in e.uniones if u.cantidad][:6]:
        simbolo_soldadura(lz, x0 + 1, yy + 7, u.soldadura["garganta_mm"], u.todo_alrededor, u.soldadura["proceso"])
        lz.texto(x0 + 37, yy + 3, u.nombre[:46], 2.3)
        lz.texto(x0 + 37, yy + 6.4, f"{u.cantidad} uds · cordón {u.longitud_mm:.0f} mm/ud" +
                 (" · todo alrededor" if u.todo_alrededor else ""), 2.1, color="#444")
        yy += 9.6
    _cajetin(lz, e, 275, 230, 130, 52, "Montaje (alzados y planta)", "1/3", f"1:{n}", proyecto_id)
    return lz


def _cotas_vista(lz: Lienzo, e: Estructura, nombre: str, P) -> None:
    c = e.cotas
    L, B, H = c.get("largo", 0), c.get("ancho", 0), c.get("alto", 0)
    xs, ys = c.get("xs", [0, L]), c.get("ys", [0, B])
    fmt = lambda v: f"{v:.0f}"
    if nombre == "ALZADO FRONTAL":
        lz.linea(*P((-400, 0, 0)), *P((L + 400, 0, 0)), 0.5)
        if 2 < len(xs) <= 14:
            for a, b in zip(xs, xs[1:]):
                cota(lz, P((a, 0, 0)), P((b, 0, 0)), 6, fmt(b - a))
        cota(lz, P((xs[0], 0, 0)), P((xs[-1], 0, 0)), 12 if len(xs) > 2 else 7, fmt(xs[-1] - xs[0]))
        if H:
            cota(lz, P((xs[0] - 300, 0, 0)), P((xs[0] - 300, 0, H)), -6, fmt(H))
        if c.get("alto_max") and abs(c["alto_max"] - H) > 50:
            cota(lz, P((xs[-1] + 300, 0, 0)), P((xs[-1] + 300, 0, c["alto_max"])), 6, fmt(c["alto_max"]))
    elif nombre == "ALZADO LATERAL" and B:
        lz.linea(*P((0, -400, 0)), *P((0, B + 400, 0)), 0.5)
        if 2 < len(ys) <= 10:
            for a, b in zip(ys, ys[1:]):
                cota(lz, P((0, a, 0)), P((0, b, 0)), 6, fmt(b - a))
        cota(lz, P((0, ys[0], 0)), P((0, ys[-1], 0)), 12 if len(ys) > 2 else 7, fmt(ys[-1] - ys[0]))
    elif nombre == "PLANTA":
        if B:
            cota(lz, P((-300, 0, 0)), P((-300, B, 0)), -7, fmt(B))
        cota(lz, P((xs[0], -300, 0)), P((xs[-1], -300, 0)), 7, fmt(xs[-1] - xs[0]))
    if e.tipo == "escalera" and nombre == "ALZADO FRONTAL":
        lz.texto(*_desp(P((c["largo"] / 2, 0, -350)), 0, 4), f"{c['peldanos']} contrahuellas de {c['contrahuella']:.0f} · "
                 f"huella {c['huella']:.0f} mm", 2.4, "middle")


def _desp(p, dx, dy):
    return p[0] + dx, p[1] + dy


def hoja_isometrica(e: Estructura, despiece: list[dict], proyecto_id: str = "") -> Lienzo:
    lz = Lienzo(420, 297)
    lz.rect(10, 10, 400, 277, None, "#000", 0.6)
    dibujar_3d(lz, e, iso(), (18, 20, 230, 200), "color", paneles=False)
    lz.texto(18, 232, "VISTA ISOMÉTRICA (sin escala)", 3.2, negrita=True)
    _tabla_despiece(lz, despiece, 258, 22, 148, 200)
    _cajetin(lz, e, 275, 230, 130, 52, "Vista isométrica y despiece", "2/3", "s/e", proyecto_id)
    for i, nota in enumerate(e.normas[:5]):
        lz.texto(18, 242 + i * 5, "· " + nota[:120], 2.3, color="#333")
    return lz


def _tabla_despiece(lz: Lienzo, filas: list[dict], x, y, w, h) -> None:
    cols = [("Marca", 12, "marca"), ("Ud", 9, "cantidad"), ("Perfil", 44, "perfil"), ("L (mm)", 16, "longitud_mm"),
            ("kg/ud", 15, "kg_ud"), ("kg", 17, "kg_total"), ("Pieza", 35, "rol")]
    alto = min(5.2, (h - 8) / max(1, len(filas) + 2))
    tam = min(2.5, alto * 0.55)
    lz.texto(x, y - 2, "DESPIECE", 3.0, negrita=True)
    cx = x
    lz.rect(x, y, w, alto, "#e8e8e8", "#000", 0.2)
    for nom, cw, _ in cols:
        lz.texto(cx + 1, y + alto * 0.72, nom, tam, negrita=True)
        cx += cw
    yy = y + alto
    total = 0.0
    for f in filas:
        cx = x
        for _, cw, k in cols:
            v = f.get(k, "")
            txt = (f"{v:.1f}" if isinstance(v, float) else str(v))[:28]
            lz.texto(cx + 1, yy + alto * 0.72, txt, tam)
            cx += cw
        lz.linea(x, yy + alto, x + w, yy + alto, 0.08, "#999")
        total += f.get("kg_total", 0)
        yy += alto
        if yy > y + h - alto * 2:
            lz.texto(x + 1, yy + alto * 0.72, "… (sigue en la hoja de lista de corte)", tam, color="#555")
            yy += alto
            break
    lz.rect(x, y, w, yy - y, None, "#000", 0.25)
    lz.texto(x + w - 1, yy + alto * 0.9, f"Peso total: {total:.1f} kg", tam + 0.3, "end", True)


def hoja_listas(e: Estructura, despiece: list[dict], corte: list[dict], placas: list[dict], comprobaciones: list[dict],
                proyecto_id: str = "") -> Lienzo:
    lz = Lienzo(420, 297)
    lz.rect(10, 10, 400, 277, None, "#000", 0.6)
    y = 20
    lz.texto(18, y, "LISTA DE CORTE (barras comerciales, corte de sierra 3 mm)", 3.0, negrita=True)
    y += 6
    for c in corte:
        lz.texto(18, y, f"{c['perfil']}: {c['barras']} barra(s) de {c['largo_barra_mm']} mm · aprovechamiento "
                        f"{c['aprovechamiento_pct']:.0f} % · {c['kg_compra']:.1f} kg a comprar", 2.4, negrita=True)
        y += 4
        for i, barra in enumerate(c["plan"][:6]):
            lz.texto(22, y, f"barra {i + 1}: " + " + ".join(f"{p['marca']} {p['mm']}" for p in barra["piezas"])[:150] +
                     f"  (sobra {barra['sobra_mm']})", 2.1, color="#333")
            y += 3.4
        if len(c["plan"]) > 6:
            lz.texto(22, y, f"… y {len(c['plan']) - 6} barras más con el mismo patrón", 2.1, color="#333")
            y += 3.4
        y += 1.5
        if y > 150:
            break
    y = max(y + 4, 120)
    lz.texto(18, y, "CHAPAS Y PLACAS", 3.0, negrita=True)
    y += 5
    for p in placas:
        lz.texto(18, y, f"{p['marca']} · {p['cantidad']} ud · {p['rol']} {p['a']}×{p['b']}×{p['t']} mm · "
                        f"{p['kg_total']:.1f} kg · {p['pernos']}", 2.3)
        y += 3.8
    y += 3
    lz.texto(18, y, "COMPROBACIONES (aprovechamiento ≤ 100 %)", 3.0, negrita=True)
    y += 5
    for c in comprobaciones:
        u = ", ".join(f"{k} {v * 100:.0f} %" for k, v in c["aprovechamiento"].items())
        lz.texto(18, y, f"{'✓' if c['cumple'] else '✗'} {c['elemento']} · {c['perfil']} · {u}"[:150], 2.3,
                 color="#060" if c["cumple"] else "#a00")
        y += 3.8
    x2 = 275
    lz.texto(x2, 20, "AVISOS Y NORMAS", 3.0, negrita=True)
    yy = 26
    for a in e.avisos + e.normas:
        for linea in _partir(a, 62):
            lz.texto(x2, yy, linea, 2.2, color="#a00" if a in e.avisos else "#222")
            yy += 3.3
        yy += 1
        if yy > 222:
            break
    _cajetin(lz, e, 275, 230, 130, 52, "Lista de corte, placas y comprobaciones", "3/3", "s/e", proyecto_id)
    return lz


def _partir(texto: str, n: int) -> list[str]:
    palabras, lineas, actual = str(texto).split(), [], ""
    for p in palabras:
        if len(actual) + len(p) + 1 > n:
            lineas.append(actual)
            actual = p
        else:
            actual = f"{actual} {p}".strip()
    if actual:
        lineas.append(actual)
    return lineas


def isometrica_svg(e: Estructura) -> str:
    lz = Lienzo(300, 220, "#f4f5f6")
    dibujar_3d(lz, e, iso(), (8, 8, 284, 200), "color", paneles=False)
    lz.texto(8, 215, f"{e.titulo} · {_dims(e)} · vista isométrica", 3.2, color="#333")
    return lz.svg(f"{e.titulo} - isométrica")
