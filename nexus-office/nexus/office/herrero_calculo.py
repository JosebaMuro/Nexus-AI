"""Oficina Herrero: perfiles de acero, acciones y comprobaciones deterministas (predimensionado).

Todo el calculo esta en codigo, no en el modelo de lenguaje, para que el resultado sea el mismo en el movil y en el
escritorio y se pueda revisar:

- Propiedades de las secciones calculadas con sus dimensiones nominales (IPE, HEA y HEB con los radios de acuerdo,
  tubos conformados en frio EN 10219 con sus esquinas redondeadas, tubo redondo, macizos, pletinas y angulares).
  UPN con los valores de las tablas, porque sus alas son inclinadas.
- Acciones segun el CTE DB-SE-AE (peso propio, uso, nieve aproximada por altitud, viento simplificado) y
  combinaciones de ELU y ELS.
- Comprobaciones del CTE DB-SE-A / Eurocodigo 3 (EN 1993-1-1): flexion, cortante, flecha, pandeo por flexion con las
  curvas europeas e interaccion lineal axil + flector.
- Soldaduras en angulo segun EN 1993-1-8 (metodo simplificado) con la garganta minima y maxima.

Es un PREDIMENSIONADO orientativo: una estructura real necesita el proyecto y la firma de un tecnico competente y
su ejecucion segun EN 1090-2 (clase de ejecucion, soldadores cualificados, control).
Unidades internas: mm, N, MPa (N/mm2). Las cargas superficiales se dan en kN/m2 (= 0,001 N/mm2).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

E = 210000.0            # modulo de elasticidad del acero, MPa
G_M0 = 1.05             # CTE DB-SE-A 2.3.3: resistencia de las secciones
G_M1 = 1.05             # inestabilidad
G_M2 = 1.25             # uniones y soldaduras
DENSIDAD = 7850.0       # kg/m3
# acero: (fy, fu, beta_w de la soldadura) para espesores de hasta 40 mm
ACEROS = {"S235": (235.0, 360.0, 0.80), "S275": (275.0, 430.0, 0.85), "S355": (355.0, 490.0, 0.90)}
# imperfeccion de las curvas de pandeo (EN 1993-1-1 tabla 6.1)
ALFA = {"a0": 0.13, "a": 0.21, "b": 0.34, "c": 0.49, "d": 0.76}


@dataclass
class Perfil:
    nombre: str
    serie: str            # IPE, HEA, HEB, UPN, SHS, RHS, CHS, RED, CUAD, PLT, L
    forma: str            # doble_t, u, tubo_rect, tubo_red, macizo_red, macizo_rect, angular
    h: float              # canto (mm), en el eje fuerte
    b: float              # ancho (mm)
    A: float              # area mm2
    Iy: float             # inercia eje fuerte mm4
    Iz: float             # inercia eje debil mm4
    Wply: float
    Wplz: float
    Av: float             # area a cortante en el plano del alma
    t: float              # espesor maximo (alas), mm
    tmin: float           # espesor minimo (alma), mm
    perimetro: float      # mm: superficie a pintar por metro = perimetro/1000 m2
    curva_y: str = "c"
    curva_z: str = "c"
    tw: float = 0.0
    tf: float = 0.0
    r: float = 0.0

    @property
    def kg_m(self) -> float:
        return self.A * 1e-6 * DENSIDAD

    @property
    def iy(self) -> float:
        return math.sqrt(self.Iy / self.A)

    @property
    def iz(self) -> float:
        return math.sqrt(self.Iz / self.A)

    @property
    def Wely(self) -> float:
        return self.Iy / (self.h / 2)

    def publico(self) -> dict:
        return {"nombre": self.nombre, "serie": self.serie, "h_mm": self.h, "b_mm": self.b,
                "area_cm2": round(self.A / 100, 2), "peso_kg_m": round(self.kg_m, 2),
                "Iy_cm4": round(self.Iy / 1e4, 1), "Iz_cm4": round(self.Iz / 1e4, 1),
                "Wply_cm3": round(self.Wply / 1e3, 1), "Wplz_cm3": round(self.Wplz / 1e3, 1),
                "iy_cm": round(self.iy / 10, 2), "iz_cm": round(self.iz / 10, 2),
                "pintura_m2_m": round(self.perimetro / 1000, 3), "espesor_max_mm": self.t}


# ---------------------------------------------------------------------------------------------- secciones
def doble_t(serie: str, h, b, tw, tf, r) -> Perfil:
    af = (1 - math.pi / 4) * r * r                 # area de cada acuerdo
    d = 0.2234 * r                                 # centroide del acuerdo desde la esquina
    yf, zf = h / 2 - tf - d, tw / 2 + d
    hw = h - 2 * tf
    A = 2 * b * tf + hw * tw + 4 * af
    Iy = b * h ** 3 / 12 - (b - tw) * hw ** 3 / 12 + 4 * af * yf ** 2
    Iz = 2 * tf * b ** 3 / 12 + hw * tw ** 3 / 12 + 4 * af * zf ** 2
    Wply = b * tf * (h - tf) + tw * hw ** 2 / 4 + 4 * af * yf
    Wplz = tf * b ** 2 / 2 + hw * tw ** 2 / 4 + 4 * af * zf
    Av = max(A - 2 * b * tf + (tw + 2 * r) * tf, hw * tw)
    U = 4 * b + 2 * h - 2 * tw - (8 - 2 * math.pi) * r
    esbelta = h / b > 1.2
    return Perfil(f"{serie} {int(h) if serie == 'IPE' else _nominal(serie, h, b)}", serie, "doble_t", h, b, A, Iy, Iz,
                  Wply, Wplz, Av, tf, tw, U, "a" if esbelta else "b", "b" if esbelta else "c", tw, tf, r)


def _nominal(serie, h, b) -> int:
    return int(b) if serie in ("HEA", "HEB") else int(h)


def _rect_redondeado(B, H, r):
    """(A, Ix, S_mitad) de un rectangulo B x H (H en el eje y) con esquinas de radio r."""
    if r <= 0:
        return B * H, B * H ** 3 / 12, B * H * H / 8
    c = H / 2 - r
    A = B * H - (4 - math.pi) * r * r
    i_cuad = r * ((H / 2) ** 3 - c ** 3) / 3
    i_quarter = math.pi * r ** 4 / 16 + 2 * c * r ** 3 / 3 + c * c * math.pi * r * r / 4
    Ix = B * H ** 3 / 12 - 4 * (i_cuad - i_quarter)
    s_cuad = r * ((H / 2) ** 2 - c * c) / 2
    s_quarter = c * math.pi * r * r / 4 + r ** 3 / 3
    S = B * (H / 2) ** 2 / 2 - 2 * (s_cuad - s_quarter)
    return A, Ix, S


def tubo_rect(h, b, t) -> Perfil:
    """Tubo conformado en frio EN 10219 (h en el eje fuerte)."""
    ro = 2 * t if t <= 6 else 2.5 * t
    ri = ro - t
    Ao, Iyo, Syo = _rect_redondeado(b, h, ro)
    Ai, Iyi, Syi = _rect_redondeado(b - 2 * t, h - 2 * t, ri)
    _, Izo, Szo = _rect_redondeado(h, b, ro)
    _, Izi, Szi = _rect_redondeado(h - 2 * t, b - 2 * t, ri)
    A = Ao - Ai
    U = 2 * (b + h) - (8 - 2 * math.pi) * ro
    nombre = f"Tubo {int(h)}x{int(b)}x{_fmt(t)}"
    return Perfil(nombre, "SHS" if h == b else "RHS", "tubo_rect", h, b, A, Iyo - Iyi, Izo - Izi,
                  2 * (Syo - Syi), 2 * (Szo - Szi), A * h / (b + h), t, t, U, "c", "c", t, t, ro)


def tubo_redondo(d, t) -> Perfil:
    di = d - 2 * t
    A = math.pi * (d * d - di * di) / 4
    I = math.pi * (d ** 4 - di ** 4) / 64
    W = (d ** 3 - di ** 3) / 6
    return Perfil(f"Tubo Ø{_fmt(d)}x{_fmt(t)}", "CHS", "tubo_red", d, d, A, I, I, W, W, 2 * A / math.pi, t, t,
                  math.pi * d, "c", "c", t, t)


def redondo(d) -> Perfil:
    A = math.pi * d * d / 4
    I = math.pi * d ** 4 / 64
    return Perfil(f"Redondo Ø{_fmt(d)}", "RED", "macizo_red", d, d, A, I, I, d ** 3 / 6, d ** 3 / 6, A * 0.9, d, d,
                  math.pi * d)


def macizo_rect(h, b, serie="PLT") -> Perfil:
    """Pletina (h el lado mayor, de canto) o cuadradillo (h = b)."""
    A = h * b
    nombre = f"Cuadradillo {_fmt(h)}" if h == b else f"Pletina {_fmt(h)}x{_fmt(b)}"
    return Perfil(nombre, "CUAD" if h == b else serie, "macizo_rect", h, b, A, b * h ** 3 / 12, h * b ** 3 / 12,
                  b * h * h / 4, h * b * b / 4, A, max(h, b) if h == b else b, min(h, b), 2 * (h + b))


def peldano(huella: float, t: float = 4.0, ala: float = 40.0) -> Perfil:
    """Peldano de chapa lagrimada plegada en U invertida (huella horizontal y dos alas de ``ala`` mm hacia abajo)."""
    a1, a2 = huella * t, 2 * (ala - t) * t
    A = a1 + a2
    yc = (a1 * t / 2 + a2 * (t + (ala - t) / 2)) / A          # desde la cara superior
    Iy = huella * t ** 3 / 12 + a1 * (yc - t / 2) ** 2 + 2 * t * (ala - t) ** 3 / 12 + a2 * (t + (ala - t) / 2 - yc) ** 2
    W = Iy / max(yc, ala - yc)
    Iz = t * huella ** 3 / 12 + a2 * (huella / 2) ** 2
    return Perfil(f"Peldaño chapa lagrimada {_fmt(t)} mm ({int(huella)}x{int(ala)})", "PEL", "peldano", ala, huella, A,
                  Iy, Iz, W, Iz / (huella / 2), 2 * (ala - t) * t, t, t, 2 * huella + 4 * ala, "c", "c", t, t)


def angular(a, t) -> Perfil:
    A = t * (2 * a - t)
    e = (a * a + a * t - t * t) / (2 * (2 * a - t))
    Iu = (t * (a - e) ** 3 + a * e ** 3 - (a - t) * (e - t) ** 3) / 3
    iv = 0.195 * a                                  # radio de giro minimo aproximado
    return Perfil(f"L {int(a)}x{int(a)}x{_fmt(t)}", "L", "angular", a, a, A, Iu, A * iv * iv, 1.5 * Iu / (a - e),
                  1.5 * Iu / (a - e), a * t, t, t, 4 * a, "c", "c", t, t)


def _fmt(x: float) -> str:
    return str(int(x)) if float(x).is_integer() else f"{x:g}".replace(".", ",")


# UPN de tablas (alas inclinadas): h, b, tw, tf, A cm2, Iy cm4, Wply cm3, Iz cm4, Wplz cm3, superficie m2/m
_UPN = {80: (80, 45, 6.0, 8.0, 11.0, 106, 31.8, 19.4, 12.1, 0.312), 100: (100, 50, 6.0, 8.5, 13.5, 206, 49.0, 29.3, 16.2, 0.372),
        120: (120, 55, 7.0, 9.0, 17.0, 364, 72.6, 43.2, 21.2, 0.434), 140: (140, 60, 7.0, 10.0, 20.4, 605, 103, 62.7, 28.3, 0.489),
        160: (160, 65, 7.5, 10.5, 24.0, 925, 138, 85.3, 35.2, 0.546), 180: (180, 70, 8.0, 11.0, 28.0, 1350, 179, 114, 42.9, 0.611),
        200: (200, 75, 8.5, 11.5, 32.2, 1910, 228, 148, 51.8, 0.661), 220: (220, 80, 9.0, 12.5, 37.4, 2690, 292, 197, 64.1, 0.718),
        240: (240, 85, 9.5, 13.0, 42.3, 3600, 358, 248, 75.7, 0.775), 260: (260, 90, 10.0, 14.0, 48.3, 4820, 442, 317, 91.6, 0.834),
        300: (300, 100, 10.0, 16.0, 58.8, 8030, 632, 495, 130, 0.952)}


def upn(h: int) -> Perfil:
    hh, b, tw, tf, A, Iy, Wply, Iz, Wplz, sup = _UPN[h]
    return Perfil(f"UPN {h}", "UPN", "u", hh, b, A * 100, Iy * 1e4, Iz * 1e4, Wply * 1e3, Wplz * 1e3,
                  hh * tw, tf, tw, sup * 1000, "c", "c", tw, tf)


_IPE = [(80, 46, 3.8, 5.2, 5), (100, 55, 4.1, 5.7, 7), (120, 64, 4.4, 6.3, 7), (140, 73, 4.7, 6.9, 7),
        (160, 82, 5.0, 7.4, 9), (180, 91, 5.3, 8.0, 9), (200, 100, 5.6, 8.5, 12), (220, 110, 5.9, 9.2, 12),
        (240, 120, 6.2, 9.8, 15), (270, 135, 6.6, 10.2, 15), (300, 150, 7.1, 10.7, 15), (330, 160, 7.5, 11.5, 18),
        (360, 170, 8.0, 12.7, 18), (400, 180, 8.6, 13.5, 21), (450, 190, 9.4, 14.6, 21), (500, 200, 10.2, 16.0, 21),
        (550, 210, 11.1, 17.2, 24), (600, 220, 12.0, 19.0, 24)]
_HEB = [(100, 100, 6, 10, 12), (120, 120, 6.5, 11, 12), (140, 140, 7, 12, 12), (160, 160, 8, 13, 15),
        (180, 180, 8.5, 14, 15), (200, 200, 9, 15, 18), (220, 220, 9.5, 16, 18), (240, 240, 10, 17, 21),
        (260, 260, 10, 17.5, 24), (280, 280, 10.5, 18, 24), (300, 300, 11, 19, 27)]
_HEA = [(96, 100, 5, 8, 12), (114, 120, 5, 8, 12), (133, 140, 5.5, 8.5, 12), (152, 160, 6, 9, 15),
        (171, 180, 6, 9.5, 15), (190, 200, 6.5, 10, 18), (210, 220, 7, 11, 18), (230, 240, 7.5, 12, 21),
        (250, 260, 7.5, 12.5, 24), (270, 280, 8, 13, 24), (290, 300, 8.5, 14, 27)]
_SHS = [(40, 2), (40, 3), (50, 2), (50, 3), (60, 3), (70, 3), (80, 3), (80, 4), (90, 4), (100, 4), (100, 5),
        (120, 4), (120, 5), (120, 6), (140, 5), (140, 6), (150, 6), (160, 6), (180, 6), (200, 6), (200, 8)]
_RHS = [(60, 40, 2), (60, 40, 3), (80, 40, 3), (100, 50, 3), (100, 50, 4), (120, 60, 4), (120, 80, 4), (140, 80, 4),
        (150, 100, 5), (160, 80, 5), (200, 100, 5), (200, 100, 6), (250, 150, 6), (300, 200, 8)]


def _series() -> dict[str, list[Perfil]]:
    return {
        "IPE": [doble_t("IPE", *d) for d in _IPE],
        "HEA": [doble_t("HEA", *d) for d in _HEA],
        "HEB": [doble_t("HEB", *d) for d in _HEB],
        "UPN": [upn(h) for h in sorted(_UPN)],
        "SHS": [tubo_rect(a, a, t) for a, t in _SHS],
        "RHS": [tubo_rect(h, b, t) for h, b, t in _RHS],
        "CHS": [tubo_redondo(d, t) for d, t in ((33.7, 2), (42.4, 2), (48.3, 2.5), (60.3, 3), (76.1, 3), (88.9, 3.2),
                                                 (114.3, 4), (139.7, 4), (168.3, 5))],
        "RED": [redondo(d) for d in (10, 12, 16, 20, 24, 27)],
        "CUAD": [macizo_rect(a, a) for a in (10, 12, 14, 16, 20)],
        "PLT": [macizo_rect(h, b) for h, b in ((30, 5), (40, 6), (50, 6), (50, 8), (60, 8), (80, 8), (80, 10),
                                               (100, 10), (120, 10))],
        "L": [angular(a, t) for a, t in ((30, 3), (40, 4), (50, 5), (60, 6), (70, 7), (80, 8))],
    }


SERIES = _series()
def _clave(n: str) -> str:
    return " ".join(str(n).upper().replace("Ø", "").replace(",", ".").split())


POR_NOMBRE = {_clave(p.nombre): p for lista in SERIES.values() for p in lista}


def perfil(nombre: str) -> Perfil:
    """Busca un perfil por nombre: «IPE 200», «HEB200», «tubo 80x80x3», «80x40x3», «UPN 120»..."""
    n = _clave(nombre)
    if n in POR_NOMBRE:
        return POR_NOMBRE[n]
    import re
    m = re.match(r"^(IPE|HEA|HEB|UPN)\s*(\d+)$", n)
    if m:
        for p in SERIES[m.group(1)]:
            if p.nombre.endswith(" " + m.group(2)):
                return p
    m = re.match(r"^(?:TUBO\s*)?(\d+)\s*X\s*(\d+)\s*X\s*([\d.,]+)$", n)
    if m:
        h, b, t = float(m.group(1)), float(m.group(2)), float(m.group(3).replace(",", "."))
        clave = _clave(f"TUBO {int(max(h, b))}X{int(min(h, b))}X{_fmt(t)}")
        return POR_NOMBRE.get(clave) or tubo_rect(max(h, b), min(h, b), t)
    m = re.match(r"^(?:TUBO\s*)?(\d+)\s*X\s*([\d.,]+)$", n)
    if m:
        a, t = float(m.group(1)), float(m.group(2).replace(",", "."))
        return POR_NOMBRE.get(_clave(f"TUBO {int(a)}X{int(a)}X{_fmt(t)}")) or tubo_rect(a, a, t)
    raise KeyError(f"perfil desconocido: {nombre}")


# ---------------------------------------------------------------------------------------------- acciones
CUBIERTAS = {   # peso propio kN/m2 y separacion maxima de correas (m)
    "sandwich": ("Panel sándwich 30-40 mm", 0.12, 1.8),
    "chapa": ("Chapa grecada simple", 0.08, 1.4),
    "policarbonato": ("Policarbonato celular", 0.05, 1.0),
    "teja": ("Panel sándwich imitación teja", 0.15, 1.6),
    "lamas": ("Lamas o celosía de pérgola (sin cubrir)", 0.10, 0.8),
    "ninguna": ("Sin cubierta", 0.0, 2.0),
}
USOS = {        # sobrecarga de uso kN/m2 (CTE DB-SE-AE tabla 3.1) y carga horizontal en barandillas kN/m (3.2)
    "mantenimiento": ("Cubierta solo accesible para conservación (G1)", 0.4, 0.8),
    "vivienda": ("Residencial (A1)", 2.0, 0.8),
    "oficinas": ("Administrativo (B)", 2.0, 0.8),
    "publico": ("Zona de acceso al público (C3)", 5.0, 1.6),
    "comercial": ("Comercial (D1)", 5.0, 1.6),
    "almacen": ("Almacén ligero (valor a confirmar con la mercancía real)", 5.0, 1.6),
    "industrial": ("Pasarela o plataforma industrial de mantenimiento", 3.0, 0.8),
}
FORJADOS = {    # suelo de una entreplanta o pasarela: peso propio kN/m2 y separacion maxima de viguetas (m)
    "tramex": ("Rejilla tipo tramex 30x30", 0.35, 1.0),
    "chapa": ("Chapa lagrimada 5 mm", 0.45, 0.6),
    "tablero": ("Tablero aglomerado hidrófugo 38 mm", 0.6, 0.8),
    "colaborante": ("Losa mixta de chapa colaborante (10 cm)", 2.5, 2.0),
}
ZONAS_VIENTO = {"A": 0.42, "B": 0.45, "C": 0.52}    # presion dinamica qb kN/m2 (CTE DB-SE-AE anejo D)


def nieve(altitud_m: float) -> float:
    """Sobrecarga de nieve caracteristica en un terreno horizontal sk (kN/m2), envolvente conservadora de la tabla E.2
    del CTE DB-SE-AE para la peninsula. Si se conoce la zona de clima invernal, mejor el valor exacto de la tabla."""
    a = max(0.0, float(altitud_m or 0))
    for limite, sk in ((200, 0.4), (400, 0.5), (600, 0.7), (800, 0.9), (1000, 1.2), (1200, 1.5)):
        if a <= limite:
            return sk
    return round(1.5 + (a - 1200) * 0.003, 2)


def viento(zona: str = "B", altura_m: float = 6.0) -> float:
    """Presion estatica qe = qb * ce * cp (kN/m2) con ce de un entorno rural llano (grado II) y cp global 1,3
    (presion + succion en fachadas)."""
    qb = ZONAS_VIENTO.get((zona or "B").upper(), 0.45)
    h = max(3.0, float(altura_m or 6))
    ce = 2.3 if h <= 6 else 2.5 if h <= 9 else 2.7 if h <= 12 else 2.9
    return round(qb * ce * 1.3, 3)


@dataclass
class Cargas:
    """Cargas superficiales caracteristicas en kN/m2 y su combinacion (CTE DB-SE 4.2)."""
    permanente: float
    uso: float = 0.0
    nieve: float = 0.0
    viento: float = 0.0
    notas: list[str] = field(default_factory=list)

    def elu(self) -> float:
        """Combinacion persistente mas desfavorable con una variable principal y el resto con psi0."""
        psi = {"uso": 0.7, "nieve": 0.5, "viento": 0.6}
        var = {"uso": self.uso, "nieve": self.nieve}
        if self.uso <= 0.4:                         # el uso de mantenimiento no se combina con la nieve
            var = {"nieve": max(self.nieve, self.uso)}
        mejor = 0.0
        for k, v in var.items():
            resto = sum(psi[o] * w for o, w in var.items() if o != k)
            mejor = max(mejor, 1.5 * v + 1.5 * resto)
        return round(1.35 * self.permanente + mejor, 3)

    def els(self) -> float:
        var = [self.uso, self.nieve] if self.uso > 0.4 else [max(self.uso, self.nieve)]
        return round(self.permanente + max(var) + 0.7 * (sum(var) - max(var)), 3)

    def publico(self) -> dict:
        return {"permanente_kn_m2": self.permanente, "uso_kn_m2": self.uso, "nieve_kn_m2": self.nieve,
                "viento_kn_m2": self.viento, "elu_kn_m2": self.elu(), "els_kn_m2": self.els(), "notas": self.notas}


# ---------------------------------------------------------------------------------------------- comprobaciones
def chi(perfil_: Perfil, acero: str, lcr: float, eje: str = "z") -> tuple[float, float]:
    """Coeficiente de reduccion por pandeo (EN 1993-1-1 6.3.1.2) y esbeltez reducida."""
    fy = ACEROS[acero][0]
    if lcr <= 0:
        return 1.0, 0.0
    i = perfil_.iy if eje == "y" else perfil_.iz
    lam1 = math.pi * math.sqrt(E / fy)
    lam = (lcr / i) / lam1
    if lam <= 0.2:
        return 1.0, lam
    a = ALFA[perfil_.curva_y if eje == "y" else perfil_.curva_z]
    phi = 0.5 * (1 + a * (lam - 0.2) + lam * lam)
    return min(1.0, 1 / (phi + math.sqrt(phi * phi - lam * lam))), lam


def comprobar_viga(p: Perfil, acero: str, luz: float, q_elu: float, q_els: float, flecha_lim: float = 300,
                   apoyo: str = "biapoyada", axil: float = 0.0, peso_propio: bool = True) -> dict:
    """Viga con carga uniforme (N/mm). ``apoyo``: biapoyada, continua (vano interior ~qL2/10) o voladizo."""
    fy = ACEROS[acero][0]
    pp = p.kg_m * 9.81e-3 if peso_propio else 0.0          # N/mm
    qd, qk = q_elu + 1.35 * pp, q_els + pp
    km, kv, kf = {"biapoyada": (1 / 8, 0.5, 5 / 384), "continua": (1 / 10, 0.6, 2.6 / 384),
                  "portico": (1 / 10, 0.5, 2.5 / 384), "voladizo": (0.5, 1.0, 1 / 8)}[apoyo]
    m, v = km * qd * luz ** 2, kv * qd * luz
    mrd = p.Wply * fy / G_M0
    vrd = p.Av * fy / (math.sqrt(3) * G_M0)
    flecha = kf * qk * luz ** 4 / (E * p.Iy)
    lim = (2 * luz if apoyo == "voladizo" else luz) / flecha_lim
    u = {"flexion": m / mrd, "cortante": v / vrd, "flecha": flecha / lim}
    if axil:
        x, _ = chi(p, acero, luz, "z")
        u["axil_flexion"] = abs(axil) / (x * p.A * fy / G_M1) + m / mrd
    return {"M_kNm": round(m / 1e6, 2), "V_kN": round(v / 1e3, 2), "flecha_mm": round(flecha, 1),
            "flecha_limite_mm": round(lim, 1), "aprovechamiento": {k: round(val, 3) for k, val in u.items()},
            "max": round(max(u.values()), 3)}


def comprobar_pilar(p: Perfil, acero: str, altura: float, n_ed: float, m_ed: float = 0.0, beta_y: float = 1.0,
                    beta_z: float = 1.0) -> dict:
    """Pilar con axil (N) y flector en el eje fuerte (N mm). Lcr = beta * altura en cada eje."""
    fy = ACEROS[acero][0]
    n_ed = n_ed + 1.35 * p.kg_m * 9.81e-3 * altura
    xy, ly = chi(p, acero, beta_y * altura, "y")
    xz, lz = chi(p, acero, beta_z * altura, "z")
    nby, nbz = xy * p.A * fy / G_M1, xz * p.A * fy / G_M1
    mrd = p.Wply * fy / G_M0
    u = {"pandeo_y": n_ed / nby, "pandeo_z": n_ed / nbz}
    if m_ed:
        u["flexion"] = m_ed / mrd
        u["interaccion"] = n_ed / min(nby, nbz) + m_ed / mrd
    esbeltez = max(beta_y * altura / p.iy, beta_z * altura / p.iz)
    if esbeltez > 200:
        u["esbeltez"] = esbeltez / 200                     # limite practico para elementos comprimidos
    return {"N_kN": round(n_ed / 1e3, 2), "M_kNm": round(m_ed / 1e6, 2), "chi_y": round(xy, 3),
            "chi_z": round(xz, 3), "lambda_y": round(ly, 3), "lambda_z": round(lz, 3), "esbeltez": round(esbeltez),
            "aprovechamiento": {k: round(val, 3) for k, val in u.items()}, "max": round(max(u.values()), 3)}


def comprobar_tirante(p: Perfil, acero: str, n_ed: float) -> dict:
    fy, fu, _ = ACEROS[acero]
    nrd = min(p.A * fy / G_M0, 0.9 * p.A * 0.85 * fu / G_M2)   # seccion bruta y neta roscada aproximada
    return {"N_kN": round(n_ed / 1e3, 2), "aprovechamiento": {"traccion": round(n_ed / nrd, 3)},
            "max": round(n_ed / nrd, 3)}


def elegir(series: list[str] | str, prueba, objetivo: float = 0.95, minimo: str = "") -> tuple[Perfil, dict]:
    """El perfil mas ligero de las series dadas que cumple ``prueba(perfil)['max'] <= objetivo``."""
    if isinstance(series, str):
        series = [series]
    cand = sorted((p for s in series for p in SERIES[s]), key=lambda p: (p.kg_m, p.h))
    if minimo:
        pm = perfil(minimo)
        cand = [p for p in cand if p.kg_m >= pm.kg_m * 0.999] or cand
    ultimo = None
    for p in cand:
        r = prueba(p)
        ultimo = (p, r)
        if r["max"] <= objetivo:
            return p, r
    p, r = ultimo
    r = {**r, "insuficiente": True}
    return p, r


# ---------------------------------------------------------------------------------------------- soldaduras
PROCESOS = {"135": "MAG con hilo macizo", "136": "MAG con hilo tubular", "111": "Electrodo revestido",
            "141": "TIG"}


def soldadura_angulo(t1: float, t2: float, acero: str = "S275", fuerza_kn: float = 0.0, doble: bool = True) -> dict:
    """Garganta de una soldadura en angulo entre chapas de espesor t1 y t2 (EN 1993-1-8 4.5 y practica de taller):
    a >= 3 mm, a <= 0,7 t_min; la recomendada iguala la resistencia de la chapa mas delgada cuando hay cordon doble.
    Con ``fuerza_kn`` da la longitud necesaria por el metodo simplificado (4.5.3.3)."""
    fy, fu, bw = ACEROS[acero]
    tmin, tmax = min(t1, t2), max(t1, t2)
    a_min = 3.0 if tmax <= 10 else 4.0 if tmax <= 20 else 5.0 if tmax <= 30 else 6.0
    a_max = max(3.0, math.floor(0.7 * tmin * 2) / 2)
    # resistencia de la chapa a traccion repartida en dos cordones: a * fvw,d * 2 = t * fy/gM0
    fvwd = fu / (math.sqrt(3) * bw * G_M2)
    a_igual = tmin * fy / G_M0 / (fvwd * (2 if doble else 1))
    a = min(a_max, max(a_min, math.ceil(a_igual * 2) / 2))
    out = {"garganta_mm": a, "garganta_min_mm": a_min, "garganta_max_mm": a_max, "fvw_d_mpa": round(fvwd, 1),
           "resistencia_kn_por_cm": round(a * fvwd * 10 / 1000, 2),
           "proceso": "135", "simbolo_iso2553": f"a{_fmt(a)} ◺ 135"}
    if a_min > a_max:
        out["aviso"] = "La chapa fina no admite la garganta mínima: usa soldadura a tope o refuerza la unión."
    if fuerza_kn:
        out["longitud_necesaria_mm"] = math.ceil(fuerza_kn * 1000 / (a * fvwd) + 2 * a)   # + crateres
    if tmax > 25:
        out["precalentamiento"] = "Valora precalentar (EN 1011-2) por espesor mayor de 25 mm."
    return out


def clase_ejecucion(tipo: str, uso: str) -> str:
    """Clase de ejecucion EN 1090-2 recomendada (anejo B: categorias de consecuencias CC y de servicio SC)."""
    if tipo in ("barandilla",) and uso in ("vivienda", "mantenimiento"):
        return "EXC1"
    if uso in ("publico", "comercial"):
        return "EXC2"
    return "EXC2" if tipo in ("nave", "entreplanta", "pasarela", "escalera") else "EXC1"
