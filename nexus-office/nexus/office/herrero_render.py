"""Oficina Herrero: imagen realista de la estructura calculada, sin modelo de IA.

Render en perspectiva de la geometria real (los mismos perfiles del despiece) con cielo, suelo con perspectiva,
sombras arrojadas por el sol, luz difusa y reflejo especular segun el acabado (galvanizado, pintado, oxiron...),
una persona de 1,75 m como escala, antialias por sobremuestreo y grano de foto. Solo usa Pillow y numpy, asi que
funciona igual en el movil. Si hay modelo de imagen (stable-diffusion.cpp), el servicio usa este render como imagen
de partida para una version fotografica que respeta la composicion (herrero.py).
"""
from __future__ import annotations

import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from .herrero_dibujo import COLOR_PANEL, _add, _cross, _dot, _mul, _norm, _sub, malla
from .herrero_modelo import ACABADOS, Estructura

SOL = _norm((-0.45, -0.75, 0.95))          # direccion hacia el sol (tarde, desde delante a la izquierda)
BRILLO = {"galvanizado": (0.55, 30), "pintado": (0.28, 45), "oxiron": (0.12, 12), "blanco": (0.3, 50),
          "verde": (0.25, 45), "corten": (0.06, 8)}


class Camara:
    def __init__(self, objetivo, distancia, az=-58.0, el=9.0, fov=42.0, ancho=1280, alto=800):
        a, e = math.radians(az), math.radians(el)
        d = (math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e))
        self.pos = _add(objetivo, _mul(d, distancia))
        self.f = _norm(_sub(objetivo, self.pos))                 # hacia delante
        self.r = _norm(_cross(self.f, (0, 0, 1)))
        self.u = _cross(self.r, self.f)
        self.ancho, self.alto = ancho, alto
        self.focal = (ancho / 2) / math.tan(math.radians(fov) / 2)

    def p(self, q):
        v = _sub(q, self.pos)
        z = _dot(v, self.f)
        if z < 50:
            return None
        return (self.ancho / 2 + self.focal * _dot(v, self.r) / z, self.alto / 2 - self.focal * _dot(v, self.u) / z, z)


def _caja(e: Estructura):
    pts = [q for b in e.barras for q in (b.p1, b.p2)] + [p.centro for p in e.placas] or [(0, 0, 0)]
    mn = tuple(min(p[i] for p in pts) for i in range(3))
    mx = tuple(max(p[i] for p in pts) for i in range(3))
    return mn, mx


def render(e: Estructura, ancho: int = 1280, alto: int = 800, sobre: int = 2, semilla: int = 7) -> Image.Image:
    W, H = ancho * sobre, alto * sobre
    mn, mx = _caja(e)
    centro = ((mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2 * 0.8)
    radio = max(1500.0, math.dist(mn, mx) / 2)
    cam = Camara(centro, radio * 3.1, ancho=W, alto=H)
    img = Image.new("RGB", (W, H))
    # cielo y suelo
    hz = cam.p(_add(cam.pos, _mul(_norm((cam.f[0], cam.f[1], 0)), 1e8)))
    yh = max(0, min(H - 1, int(hz[1]) if hz else H // 3))
    rng = np.random.default_rng(semilla)
    fondo = np.zeros((H, W, 3), np.float32)
    t = np.clip(np.arange(H, dtype=np.float32) / max(1, yh), 0, 1)[:, None] ** 1.6
    fondo[:] = ((1 - t) * np.array([70, 122, 186]) + t * np.array([206, 220, 232]))[:, None, :]
    sx = W * 0.18                                                  # brillo del sol arriba a la izquierda
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    fondo += (np.exp(-(((xx - sx) ** 2 + (yy + H * 0.1) ** 2) / (2 * (W * 0.25) ** 2))) * 40)[..., None] * \
        np.array([1.0, 0.95, 0.8])
    if yh < H:
        g = np.clip((np.arange(H - yh, dtype=np.float32)) / max(1, H - yh), 0, 1)[:, None] ** 0.7
        hierba = (1 - g) * np.array([150, 160, 136]) + g * np.array([88, 110, 64])     # bruma lejana -> cesped
        fondo[yh:] = hierba[:, None, :]
        grano = rng.normal(0, 1, (H - yh, W // 4 + 1)).astype(np.float32)
        grano = np.repeat(grano, 4, axis=1)[:, :W] * (6 + 10 * g)
        fondo[yh:] += grano[..., None] * np.array([0.8, 1.0, 0.6])
    img = Image.fromarray(np.clip(fondo, 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(img)
    # arboleda lejana en el horizonte: siluetas superpuestas, desenfocadas y con bruma
    capa = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dc = ImageDraw.Draw(capa)
    for fila, (base, esc) in enumerate((((112, 132, 116), 0.75), ((84, 106, 82), 1.0))):
        x = -60.0
        while x < W + 60:
            r = rng.uniform(0.008, 0.022) * W * esc
            alto_arbol = r * rng.uniform(1.2, 2.2)
            k = rng.uniform(0.85, 1.12)
            tono = tuple(int(min(255, v * k)) for v in base) + (255,)
            y0 = yh + fila * 2 * sobre
            dc.ellipse([x - r, y0 - alto_arbol, x + r, y0 + r * 0.3], fill=tono)
            x += r * rng.uniform(0.7, 1.3)
    capa = capa.filter(ImageFilter.GaussianBlur(1.2 * sobre))
    img.paste(capa, (0, 0), capa)
    dr = ImageDraw.Draw(img)
    # losa de hormigon bajo la estructura con juntas cada metro
    m = 1200.0
    x0, x1, y0, y1 = mn[0] - m, mx[0] + m, mn[1] - m, mx[1] + m
    losa = [cam.p((x0, y0, 0)), cam.p((x1, y0, 0)), cam.p((x1, y1, 0)), cam.p((x0, y1, 0))]
    if all(losa):
        dr.polygon([(p[0], p[1]) for p in losa], fill=(196, 194, 186))
        paso = 1000.0
        for xx in np.arange(x0, x1 + 1, paso):
            a, b = cam.p((xx, y0, 0)), cam.p((xx, y1, 0))
            if a and b:
                dr.line([(a[0], a[1]), (b[0], b[1])], fill=(170, 168, 160), width=max(1, sobre))
        for yy in np.arange(y0, y1 + 1, paso):
            a, b = cam.p((x0, yy, 0)), cam.p((x1, yy, 0))
            if a and b:
                dr.line([(a[0], a[1]), (b[0], b[1])], fill=(170, 168, 160), width=max(1, sobre))
    caras = malla(e, tramo=600.0, paneles=True, lados_red=12, trozo_panel=700.0)
    # sombras arrojadas sobre el suelo (z = 0) en una mascara difuminada
    mascara = Image.new("L", (W, H), 0)
    dm = ImageDraw.Draw(mascara)
    for pts, _, clase, _ in caras:
        sp = [cam.p(_sub(q, _mul(SOL, max(0.0, q[2]) / SOL[2]))) for q in pts]
        if all(sp):
            dm.polygon([(p[0], p[1]) for p in sp], fill=150 if clase.startswith("panel:") and "lamas" in clase else 200)
    persona = (mn[0] - 500, mn[1] - 900, 0)
    _persona(dm, cam, persona, sombra=True)
    mascara = mascara.filter(ImageFilter.GaussianBlur(2.2 * sobre))
    arr = np.asarray(img).astype(np.float32)
    arr *= (1 - 0.5 * np.asarray(mascara, np.float32)[..., None] / 255)
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
    dr = ImageDraw.Draw(img)
    # caras de la estructura, de la mas lejana a la mas cercana
    acabado = e.parametros.get("acabado", "galvanizado")
    base_acero = ACABADOS.get(acabado, ACABADOS["galvanizado"])[1]
    ks, brillo = BRILLO.get(acabado, (0.3, 30))
    lista = []
    for pts, nrm, clase, _ in caras:
        pp = [cam.p(q) for q in pts]
        if not all(pp):
            continue
        c = (sum(q[0] for q in pts) / len(pts), sum(q[1] for q in pts) / len(pts), sum(q[2] for q in pts) / len(pts))
        ver = _norm(_sub(cam.pos, c))
        abajo = False
        if _dot(nrm, ver) <= 0:
            if not clase.startswith("panel:"):
                continue
            nrm = _mul(nrm, -1)
            abajo = nrm[2] < 0
        elif clase.startswith("panel:"):
            abajo = nrm[2] < 0
        lista.append((math.dist(c, cam.pos), pp, nrm, clase, ver, abajo))
    lista.sort(key=lambda x: -x[0])
    for _, pp, nrm, clase, ver, abajo in lista:
        if clase.startswith("panel:"):
            base, k_s, br = COLOR_PANEL.get(clase[6:], (150, 150, 150)), 0.15, 20
            if abajo and clase[6:] in ("sandwich", "teja", "chapa"):
                base = (214, 214, 208)                             # cara inferior lacada en blanco gris
            elif abajo and clase[6:] in ("tablero", "colaborante"):
                base = (150, 154, 158)                             # chapa de apoyo vista desde abajo
        else:
            base, k_s, br = base_acero, ks, brillo
            if clase == "placa":
                base = tuple(x * 0.92 for x in base)
        dif = max(0.0, _dot(nrm, SOL))
        cielo_k = 0.18 * (nrm[2] + 1) / 2
        h = _norm(_add(SOL, ver))
        spec = k_s * max(0.0, _dot(nrm, h)) ** br
        col = tuple(int(min(255, c * (0.30 + cielo_k + 0.72 * dif * w) + 255 * spec)) for c, w in zip(base, (1.0, 0.97, 0.9)))
        dr.polygon([(p[0], p[1]) for p in pp], fill=col)
    _persona(dr, cam, persona)
    img = img.resize((ancho, alto), Image.LANCZOS)
    # vineteado suave y grano de foto
    arr = np.asarray(img).astype(np.float32)
    yy, xx = np.mgrid[0:alto, 0:ancho]
    r = np.sqrt(((xx - ancho / 2) / (ancho / 2)) ** 2 + ((yy - alto / 2) / (alto / 2)) ** 2)
    arr *= (1 - 0.18 * np.clip(r - 0.55, 0, 1))[..., None]
    arr += rng.normal(0, 2.2, arr.shape).astype(np.float32)
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))


def _persona(dr: ImageDraw.ImageDraw, cam: Camara, pie, sombra: bool = False) -> None:
    """Silueta de una persona de 1,75 m (escala) o su sombra en el suelo."""
    if sombra:
        pts = [cam.p(_sub(_add(pie, (dx, dy, z)), _mul(SOL, z / SOL[2]))) for z in (0, 1750)
               for dx, dy in ((0, 0),)]
        if all(pts):
            (a, b) = pts
            w = abs(a[1] - b[1]) * 0.12 + 2
            dr.line([(a[0], a[1]), (b[0], b[1])], fill=200, width=int(w))
        return
    a, b = cam.p(pie), cam.p(_add(pie, (0, 0, 1750)))
    if not (a and b):
        return
    h = a[1] - b[1]
    x = a[0]
    gris = (70, 74, 80)
    cab = h * 0.065
    dr.ellipse([x - cab, b[1], x + cab, b[1] + 2 * cab], fill=gris)
    dr.rounded_rectangle([x - h * 0.11, b[1] + 2.1 * cab, x + h * 0.11, b[1] + h * 0.55], radius=h * 0.05,
                         fill=(52, 70, 96))
    dr.rectangle([x - h * 0.09, b[1] + h * 0.53, x - h * 0.01, a[1]], fill=(40, 40, 46))
    dr.rectangle([x + h * 0.01, b[1] + h * 0.53, x + h * 0.09, a[1]], fill=(40, 40, 46))


def prompt_foto(e: Estructura) -> str:
    """Descripcion en ingles para el modelo de imagen (los modelos de difusion entienden ingles)."""
    P = e.parametros
    acab = {"galvanizado": "hot-dip galvanized steel", "pintado": "anthracite grey painted steel",
            "oxiron": "black wrought iron finish steel", "blanco": "white painted steel", "verde": "moss green painted steel",
            "corten": "weathering corten steel"}.get(P.get("acabado"), "galvanized steel")
    que = {"porche": "carport canopy with sandwich panel roof", "pergola": "pergola with slats",
           "nave": "industrial portal frame warehouse structure", "entreplanta": "steel mezzanine platform with railing",
           "escalera": "straight steel staircase with checker plate steps and railing",
           "barandilla": "steel balustrade railing with vertical bars", "pasarela": "steel footbridge walkway with grating floor"}[e.tipo]
    return (f"professional architectural photograph of a {que}, {acab}, {P['largo']:.0f} by {P['ancho']:.0f} meters, "
            "sharp details, welded joints, concrete floor, natural daylight, realistic shadows, 35mm lens, high detail")
