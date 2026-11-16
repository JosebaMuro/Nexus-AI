"""Oficina Herrero: tipologias de estructura metalica -> geometria (barras y placas), perfiles elegidos por calculo,
uniones soldadas y avisos de norma.

Cada tipologia (porche, pergola, nave, entreplanta, escalera, barandilla, pasarela) sigue los mismos pasos:
1. cargas segun el CTE DB-SE-AE (herrero_calculo.Cargas),
2. elige el perfil mas ligero que cumple cada comprobacion (flexion, cortante, flecha, pandeo),
3. coloca las barras en 3D (mm; x a lo largo, y a lo ancho/fondo, z hacia arriba) y las placas,
4. anota las uniones soldadas (garganta segun EN 1993-1-8, simbolo ISO 2553) y los avisos (CTE DB-SUA, EN 1090).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

from . import herrero_calculo as C

Vec = tuple[float, float, float]
ROLES = {   # rol -> (letra de la marca, nombre en plural para el despiece)
    "pilar": ("P", "Pilares"), "jacena": ("V", "Jácenas"), "viga": ("V", "Vigas"), "dintel": ("D", "Dinteles"),
    "correa": ("C", "Correas"), "vigueta": ("G", "Viguetas"), "atado": ("T", "Vigas de atado"),
    "arriostramiento": ("A", "Arriostramientos"), "zanca": ("Z", "Zancas"), "peldano": ("E", "Peldaños"),
    "montante": ("M", "Montantes"), "pasamanos": ("R", "Pasamanos"), "barrote": ("B", "Barrotes"),
    "travesano": ("X", "Travesaños"),
}
ACABADOS = {
    "galvanizado": ("Galvanizado en caliente (UNE-EN ISO 1461)", (168, 172, 176)),
    "pintado": ("Imprimación epoxi + esmalte (RAL 7016 gris antracita)", (56, 62, 66)),
    "oxiron": ("Imprimación + pintura de forja tipo oxirón negro", (40, 40, 42)),
    "blanco": ("Imprimación + esmalte RAL 9010 blanco", (232, 232, 226)),
    "verde": ("Imprimación + esmalte RAL 6005 verde musgo", (30, 70, 52)),
    "corten": ("Acero patinable (aspecto corten)", (128, 66, 38)),
}


@dataclass
class Barra:
    rol: str
    perfil: C.Perfil
    p1: Vec
    p2: Vec
    orient: Vec | None = None     # direccion del canto (h) del perfil; None = vertical o la del plano
    marca: str = ""

    @property
    def longitud(self) -> float:
        return math.dist(self.p1, self.p2)


@dataclass
class Placa:
    rol: str                  # placa base, placa de cabeza, cartela...
    a: float                  # mm
    b: float
    t: float
    centro: Vec
    normal: str = "z"         # z: horizontal; x o y: vertical
    pernos: str = ""          # p. ej. «4 pernos M16»
    marca: str = ""

    @property
    def kg(self) -> float:
        return self.a * self.b * self.t * 1e-9 * C.DENSIDAD


@dataclass
class Union:
    nombre: str
    cantidad: int
    soldadura: dict           # salida de herrero_calculo.soldadura_angulo
    longitud_mm: float        # longitud de cordon en cada union
    todo_alrededor: bool = False
    nota: str = ""


@dataclass
class Estructura:
    tipo: str
    titulo: str
    parametros: dict
    cargas: C.Cargas | None = None
    barras: list[Barra] = field(default_factory=list)
    placas: list[Placa] = field(default_factory=list)
    uniones: list[Union] = field(default_factory=list)
    comprobaciones: list[dict] = field(default_factory=list)
    avisos: list[str] = field(default_factory=list)
    normas: list[str] = field(default_factory=list)
    paneles: list[tuple[list[Vec], str]] = field(default_factory=list)   # cubierta/suelo para dibujar: (poligono, tipo)
    cotas: dict = field(default_factory=dict)        # medidas generales para el plano (mm)
    puestos: dict = field(default_factory=dict)      # notas de cada puesto de la oficina

    def barra(self, rol, perfil, p1, p2, orient=None) -> Barra:
        b = Barra(rol, perfil, tuple(map(float, p1)), tuple(map(float, p2)), orient)
        self.barras.append(b)
        return b

    def comprobar(self, elemento: str, perfil: C.Perfil, resultado: dict, metodo: str) -> None:
        self.comprobaciones.append({"elemento": elemento, "perfil": perfil.nombre, "metodo": metodo,
                                    **{k: v for k, v in resultado.items() if k != "insuficiente"},
                                    "cumple": resultado["max"] <= 1.0})
        if resultado.get("insuficiente"):
            self.avisos.append(f"{elemento}: ni el perfil mayor de la serie ({perfil.nombre}) cumple. Reduce luces, "
                               "añade apoyos o pide un cálculo específico.")

    def soldar(self, nombre: str, cantidad: int, t1: float, t2: float, longitud: float, todo_alrededor=False,
               fuerza_kn: float = 0.0, nota: str = "") -> None:
        s = C.soldadura_angulo(t1, t2, self.parametros["acero"], fuerza_kn)
        igual = next((u for u in self.uniones if u.nombre == nombre and u.longitud_mm == round(longitud)
                      and u.soldadura["garganta_mm"] == s["garganta_mm"]), None)
        if igual:                                    # la misma union en otro tramo: se suman
            igual.cantidad += int(cantidad)
            return
        self.uniones.append(Union(nombre, int(cantidad), s, round(longitud), todo_alrededor, nota))


# ---------------------------------------------------------------------------------------------- utilidades
def reparto(longitud: float, max_sep: float) -> list[float]:
    n = max(1, math.ceil(longitud / max_sep - 1e-9))
    return [i * longitud / n for i in range(n + 1)]


def _kn(v: float) -> float:
    """kN/m2 -> N/mm2."""
    return v * 1e-3


def _placa_base(e: Estructura, pilar: C.Perfil, n_ed: float, m_ed: float = 0.0, t_ed: float = 0.0) -> tuple[Placa, str]:
    """Placa base de un pilar sobre hormigon HA-25 (fjd ~ fcd = 16,7 MPa) con pernos de acero 8.8 (o B500S)."""
    fy = C.ACEROS[e.parametros["acero"]][0]
    vuelo = 50.0
    a = max(200.0, math.ceil((pilar.h + 2 * vuelo + 2 * 40) / 10) * 10)
    b = max(200.0, math.ceil((pilar.b + 2 * vuelo + 2 * 40) / 10) * 10)
    sigma = min(16.7, (n_ed / (a * b)) + (6 * m_ed / (b * a * a) if m_ed else 0))
    t_comp = vuelo * math.sqrt(3 * sigma * C.G_M0 / fy)
    # traccion en los pernos de un lado (2 pernos) por flector o levantamiento por viento
    brazo = a - 80
    traccion = max(t_ed / 4, (m_ed / brazo - n_ed / 2) / 2 if m_ed else 0.0, 0.0)
    pernos = 4
    diam = next((d for d, ft in ((16, 90.4e3), (20, 141e3), (24, 203e3)) if traccion <= ft), 27)
    t_trac = math.sqrt(6 * traccion * 40 / (b / 2 * fy / C.G_M0)) if traccion else 0.0
    t = next(x for x in (10, 12, 15, 20, 25, 30, 35, 40) if x >= max(t_comp, t_trac, pilar.t + 2))
    texto = f"{pernos} pernos M{diam} 8.8 (longitud de anclaje ≥ {15 * diam} mm en HA-25)"
    return Placa("placa base", a, b, t, (0, 0, 0), "z", texto), texto


def _add_placas_base(e: Estructura, pilares: list[Barra], placa: Placa) -> None:
    for p in pilares:
        x, y, _ = p.p1
        e.placas.append(Placa(placa.rol, placa.a, placa.b, placa.t, (x, y, placa.t / 2), "z", placa.pernos))


def _cubierta_peso(e: Estructura, cub: str) -> tuple[str, float, float]:
    nombre, g, sep = C.CUBIERTAS.get(cub, C.CUBIERTAS["sandwich"])
    return nombre, g, sep * 1000


# ---------------------------------------------------------------------------------------------- barandilla
def _barandilla(e: Estructura, inicio: Vec, fin: Vec, altura: float, q_h: float, inclinada: bool = False,
                calcular: bool = True, sep_max: float = 1200.0, base_placa: bool = True) -> None:
    """Barandilla recta de ``inicio`` a ``fin`` (puntos del suelo o de la linea de peldanos). ``q_h`` kN/m."""
    acero = e.parametros["acero"]
    lon_h = math.dist(inicio[:2], fin[:2])
    if lon_h < 200:
        return
    pasos = reparto(lon_h, sep_max)
    sep = lon_h / (len(pasos) - 1)
    f = 1.5 * q_h * sep                                  # N en cabeza del montante (q_h kN/m = N/mm)
    if calcular or not hasattr(e, "_bar_perfiles"):
        mont, rm = C.elegir(["PLT", "SHS"], lambda p: C.comprobar_viga(p, acero, altura, 0, 0, 100, "voladizo",
                                                                       peso_propio=False) | _vol(p, acero, f, altura),
                            minimo="Pletina 50x8")
        pas, rp = C.elegir(["CHS", "SHS", "RHS"], lambda p: C.comprobar_viga(p, acero, sep, 1.5 * q_h, q_h, 150,
                                                                            "continua", peso_propio=False),
                           minimo="Tubo Ø42,4x2")
        e._bar_perfiles = (mont, pas)
        e.comprobar("Montante de barandilla", mont, rm, f"voladizo h = {altura:.0f} mm con {q_h} kN/m · CTE DB-SE-AE 3.2")
        e.comprobar("Pasamanos", pas, rp, f"continuo entre montantes cada {sep:.0f} mm con {q_h} kN/m")
    mont, pas = e._bar_perfiles
    barrote = C.perfil("Cuadradillo 14" if altura > 1000 else "Cuadradillo 12")
    dx, dy, dz = (fin[0] - inicio[0]) / lon_h, (fin[1] - inicio[1]) / lon_h, (fin[2] - inicio[2]) / lon_h
    normal = (-dy, dx, 0.0)                              # el canto del montante resiste el empuje horizontal
    zpas = altura - pas.h / 2
    for s in pasos:
        x, y, z = inicio[0] + dx * s, inicio[1] + dy * s, inicio[2] + dz * s
        e.barra("montante", mont, (x, y, z), (x, y, z + altura - pas.h), normal)
        if base_placa and not inclinada:
            e.placas.append(Placa("placa de anclaje", 120, 120, 8, (x, y, z + 4), "z", "4 anclajes químicos M10"))
    a, b = inicio, fin
    e.barra("pasamanos", pas, (a[0], a[1], a[2] + zpas), (b[0], b[1], b[2] + zpas))
    zinf = 100.0
    e.barra("travesano", pas if pas.serie != "CHS" else C.perfil("Tubo 40x40x2"),
            (a[0], a[1], a[2] + zinf), (b[0], b[1], b[2] + zinf))
    paso = 95 + barrote.b                                # hueco libre < 100 mm (CTE DB-SUA 3.2.3)
    n_barrotes = 0
    for i in range(len(pasos) - 1):
        s0, s1 = pasos[i] + mont.b / 2, pasos[i + 1] - mont.b / 2
        n = max(1, math.ceil((s1 - s0) / paso) - 1)
        for k in range(1, n + 1):
            s = s0 + (s1 - s0) * k / (n + 1)
            x, y, z = a[0] + dx * s, a[1] + dy * s, a[2] + dz * s
            e.barra("barrote", barrote, (x, y, z + zinf + 20), (x, y, z + zpas - pas.h / 2))
            n_barrotes += 1
    n_mont = len(pasos)
    e.soldar("Montante – placa de anclaje", n_mont if base_placa and not inclinada else 0, mont.t, 8,
             2 * (mont.h + mont.b), True)
    if not base_placa:
        e.soldar("Montante – estructura (viga o zanca)", n_mont, mont.t, 6, 2 * (mont.h + mont.b), True)
    e.soldar("Barrote – pasamanos y travesaño", 2 * n_barrotes, barrote.b, pas.t, 4 * barrote.b, True,
             nota="cada barrote, arriba y abajo")
    e.soldar("Montante – pasamanos", n_mont, mont.tmin, pas.t, 2 * (mont.h + mont.b), True)


def _vol(p: C.Perfil, acero: str, f: float, h: float) -> dict:
    """Voladizo con carga puntual en la punta (montante de barandilla)."""
    fy = C.ACEROS[acero][0]
    m = f * h
    flecha = (f / 1.5) * h ** 3 / (3 * C.E * p.Iy)
    u = {"flexion": m / (p.Wply * fy / C.G_M0), "flecha": flecha / (h / 60)}   # h/60: limite usual de barandillas
    return {"M_kNm": round(m / 1e6, 3), "flecha_mm": round(flecha, 1), "flecha_limite_mm": round(h / 60, 1),
            "aprovechamiento": {k: round(v, 3) for k, v in u.items()}, "max": round(max(u.values()), 3)}


# ---------------------------------------------------------------------------------------------- tipologias
def porche(e: Estructura) -> None:
    P = e.parametros
    acero = P["acero"]
    L, B, H = P["largo"] * 1000, P["ancho"] * 1000, P["alto"] * 1000
    pend = P["pendiente_pct"] / 100
    nombre_cub, g_cub, sep_cor = _cubierta_peso(e, P["cubierta"])
    sk = P["nieve_kn_m2"] if P.get("nieve_kn_m2") is not None else C.nieve(P["altitud"])
    w = C.viento(P["zona_viento"], H / 1000)
    e.cargas = C.Cargas(g_cub + 0.10, P["uso_kn_m2"] if P.get("uso_kn_m2") is not None else 0.4, sk, w,
                        [f"Cubierta: {nombre_cub} ({g_cub} kN/m²) + correas ≈ 0,10 kN/m²",
                         f"Nieve sk = {sk} kN/m² (altitud {P['altitud']:.0f} m)",
                         f"Viento qe ≈ {w} kN/m² (zona {P['zona_viento']})"])
    qd, qk = _kn(e.cargas.elu()), _kn(e.cargas.els())
    xs = reparto(L, P.get("separacion_mm") or 5000)
    s = L / (len(xs) - 1)
    ys = reparto(B, sep_cor)
    sc = B / (len(ys) - 1)
    continua = "continua" if len(xs) > 2 else "biapoyada"
    cor, rc = C.elegir(["RHS", "SHS", "IPE"], lambda p: C.comprobar_viga(p, acero, s, qd * sc, qk * sc, 300, continua))
    e.comprobar("Correa", cor, rc, f"{continua}, luz {s:.0f} mm, ancho tributario {sc:.0f} mm, flecha L/300")
    jac, rj = C.elegir(["IPE", "RHS", "HEA"], lambda p: C.comprobar_viga(p, acero, B, qd * s, qk * s, 300, "biapoyada"))
    e.comprobar("Jácena (viga del pórtico)", jac, rj, f"biapoyada, luz {B:.0f} mm, ancho tributario {s:.0f} mm")
    n_ed = qd * s * B / 2
    f_h = n_ed / 200 + 1.5 * _kn(w) * s * 400 / 2 + 1.5 * _kn(w) * 150 * H   # imperfeccion + viento en alero y pilar
    pil, rp = C.elegir(["SHS", "HEB", "HEA"], lambda p: C.comprobar_pilar(p, acero, H, n_ed, f_h * H, 2.0, 2.0))
    e.comprobar("Pilar empotrado en la base", pil, rp, f"ménsula h = {H:.0f} mm (Lcr = 2h), N + M por viento e imperfección")
    # levantamiento por succion en cubierta ligera (cp ≈ -1,2 en marquesinas abiertas)
    t_up = max(0.0, (1.5 * _kn(w) / 1.3 * 1.2 - 0.8 * _kn(e.cargas.permanente)) * s * B / 2)
    placa, pernos = _placa_base(e, pil, n_ed, f_h * H, t_up)
    if t_up:
        e.avisos.append(f"Con viento la cubierta tiende a levantarse ({t_up / 1e3:.1f} kN por pilar): los pernos y "
                        "la cimentación deben anclarla.")
    # geometria: la cubierta cae hacia el frente (y = 0) con la pendiente dada
    zt = lambda y: H + y * pend
    pilares = []
    for x in xs:
        for y in (0.0, B):
            pilares.append(e.barra("pilar", pil, (x, y, 0), (x, y, zt(y) - jac.h), (0, 1, 0)))
        e.barra("jacena", jac, (x, -cor.b / 2 - 40, zt(-cor.b / 2 - 40) - jac.h / 2),
                (x, B + cor.b / 2 + 40, zt(B + cor.b / 2 + 40) - jac.h / 2))
    for y in ys:
        e.barra("correa", cor, (-150, y, zt(y) + cor.h / 2), (L + 150, y, zt(y) + cor.h / 2))
    _add_placas_base(e, pilares, placa)
    for p in pilares:
        x, y, _ = p.p2
        e.placas.append(Placa("placa de cabeza", pil.b + 40, pil.h + 40, 10, (x, y, p.p2[2] + 5), "z",
                              "2 tornillos M12 8.8 a la jácena"))
    if P["cubierta"] != "ninguna":
        zc = lambda y: zt(y) + cor.h + 45
        e.paneles.append(([(-150, -200, zc(-200)), (L + 150, -200, zc(-200)), (L + 150, B + 200, zc(B + 200)),
                           (-150, B + 200, zc(B + 200))], P["cubierta"]))
    e.soldar("Pilar – placa base", len(pilares), pil.t, placa.t, 2 * (pil.h + pil.b), True)
    e.soldar("Pilar – placa de cabeza", len(pilares), pil.t, 10, 2 * (pil.h + pil.b), True)
    e.soldar("Correa – jácena", len(xs) * len(ys), cor.tmin, jac.tf or jac.t, 2 * cor.b, nota="cordón a ambos lados")
    e.cotas = {"largo": L, "ancho": B, "alto": H, "alto_max": zt(B), "xs": xs, "ys": ys}
    e.normas += ["CTE DB-SE-AE (acciones: nieve, viento, uso G1)", "CTE DB-SE-A / EN 1993-1-1 (perfiles)",
                 "EN 1993-1-8 (uniones soldadas)"]
    e.puestos["calculista"] = (f"Pórticos cada {s / 1000:.2f} m ({len(xs)} pórticos), correas cada {sc / 1000:.2f} m. "
                               f"Pilares empotrados en la base: sin cruces de arriostramiento.")


def pergola(e: Estructura) -> None:
    e.parametros.setdefault("cubierta", "lamas")
    porche(e)


def nave(e: Estructura) -> None:
    P = e.parametros
    acero = P["acero"]
    L, B, H = P["largo"] * 1000, P["ancho"] * 1000, P["alto"] * 1000
    pend = P["pendiente_pct"] / 100
    nombre_cub, g_cub, sep_cor = _cubierta_peso(e, P["cubierta"])
    sk = P["nieve_kn_m2"] if P.get("nieve_kn_m2") is not None else C.nieve(P["altitud"])
    hc = H + B / 2 * pend
    w = C.viento(P["zona_viento"], hc / 1000)
    e.cargas = C.Cargas(g_cub + 0.12, P["uso_kn_m2"] if P.get("uso_kn_m2") is not None else 0.4, sk, w,
                        [f"Cubierta: {nombre_cub} ({g_cub} kN/m²) + correas e instalaciones ligeras ≈ 0,12 kN/m²",
                         f"Nieve sk = {sk} kN/m² (altitud {P['altitud']:.0f} m)",
                         f"Viento qe ≈ {w} kN/m² (zona {P['zona_viento']}, altura de cumbrera {hc / 1000:.1f} m)"])
    qd, qk = _kn(e.cargas.elu()), _kn(e.cargas.els())
    xs = reparto(L, P.get("separacion_mm") or 5000)
    s = L / (len(xs) - 1)
    ld = (B / 2) / math.cos(math.atan(pend))                       # longitud de cada faldon
    us = reparto(ld, sep_cor)
    sc = ld / (len(us) - 1)
    cor, rc = C.elegir(["IPE", "RHS"], lambda p: C.comprobar_viga(p, acero, s, qd * sc, qk * sc, 250,
                                                                  "continua" if len(xs) > 2 else "biapoyada"))
    e.comprobar("Correa", cor, rc, f"luz {s:.0f} mm, separación {sc:.0f} mm, flecha L/250 (cubierta)")
    dint, rd = C.elegir(["IPE", "HEA"], lambda p: C.comprobar_viga(p, acero, B, qd * s, qk * s, 250, "portico"))
    e.comprobar("Dintel del pórtico", dint, rd, f"pórtico a dos aguas, luz {B:.0f} mm, M ≈ qL²/10, flecha L/250")
    n_ed = qd * s * B / 2
    m_col = qd * s * B ** 2 / 10 + 0.9 * _kn(w) * s * H ** 2 / 8
    pil, rp = C.elegir(["IPE", "HEA", "HEB"], lambda p: C.comprobar_pilar(p, acero, H, n_ed, m_col, 2.0, 1.0),
                       minimo=f"{dint.serie} {dint.nombre.split()[-1]}" if dint.serie == "IPE" else "")
    e.comprobar("Pilar del pórtico", pil, rp, f"h = {H:.0f} mm, pórtico traslacional (Lcr,y = 2h, Lcr,z = h)")
    # arriostramiento: viento en el pinon repartido entre cubierta y fachadas, cruces en los vanos extremos
    f_pinon = 1.5 * _kn(w) * B * (H + hc) / 2 / 2                  # mitad a cada extremo
    diag = math.hypot(s, ld)
    t_diag = f_pinon / 2 * diag / s
    red, rr = C.elegir("RED", lambda p: C.comprobar_tirante(p, acero, t_diag), minimo="Redondo Ø16")
    e.comprobar("Cruz de San Andrés (tirante)", red, rr, f"viento en el piñón {f_pinon / 1e3:.1f} kN por extremo")
    atado, ra = C.elegir("SHS", lambda p: C.comprobar_pilar(p, acero, s, f_pinon, 0, 1.0, 1.0), minimo="Tubo 80x80x3")
    e.comprobar("Viga de atado (alero y cumbrera)", atado, ra, f"puntal biarticulado de {s:.0f} mm")
    placa, pernos = _placa_base(e, pil, n_ed, 0.0, 0.0)
    pilares = []
    zt = lambda y: H + (y if y <= B / 2 else B - y) * pend
    for x in xs:
        for y in (0.0, B):
            pilares.append(e.barra("pilar", pil, (x, y, 0), (x, y, H), (0, 1, 0)))
        e.barra("dintel", dint, (x, 0, H - dint.h / 2), (x, B / 2, hc - dint.h / 2))
        e.barra("dintel", dint, (x, B / 2, hc - dint.h / 2), (x, B, H - dint.h / 2))
    for u in us:
        yy = u * math.cos(math.atan(pend))
        for y in (yy, B - yy) if yy < B / 2 - 1 else (B / 2,):
            e.barra("correa", cor, (-100, y, zt(y) + cor.h / 2), (L + 100, y, zt(y) + cor.h / 2))
    for y in (0.0, B):
        e.barra("atado", atado, (0, y, H + atado.h / 2), (L, y, H + atado.h / 2))
    vanos = [(xs[0], xs[1]), (xs[-2], xs[-1])] if len(xs) > 2 else [(xs[0], xs[1])]
    for x0, x1 in vanos:
        for y0, y1 in ((0, B / 2), (B, B / 2)):                    # faldones de cubierta
            e.barra("arriostramiento", red, (x0, y0, zt(y0)), (x1, y1, zt(y1)))
            e.barra("arriostramiento", red, (x1, y0, zt(y0)), (x0, y1, zt(y1)))
        for y in (0.0, B):                                         # fachadas laterales
            e.barra("arriostramiento", red, (x0, y, 300), (x1, y, H - 100))
            e.barra("arriostramiento", red, (x1, y, 300), (x0, y, H - 100))
    _add_placas_base(e, pilares, placa)
    zc = lambda y: zt(y) + cor.h + 20
    e.paneles.append(([(-100, -150, zc(0) - 150 * pend), (L + 100, -150, zc(0) - 150 * pend), (L + 100, B / 2, zc(B / 2)),
                       (-100, B / 2, zc(B / 2))], P["cubierta"]))
    e.paneles.append(([(-100, B / 2, zc(B / 2)), (L + 100, B / 2, zc(B / 2)), (L + 100, B + 150, zc(B) - 150 * pend),
                       (-100, B + 150, zc(B) - 150 * pend)], P["cubierta"]))
    nudos = len(xs)
    e.soldar("Pilar – placa base", len(pilares), pil.t, placa.t, 2 * (pil.b * 2 + pil.h), True)
    e.soldar("Dintel – placa de testa (nudo de alero)", 2 * nudos, dint.t, 20, 2 * (2 * dint.b + dint.h),
             nota="placa de testa 20 mm atornillada al pilar con 6 tornillos M20 10.9 y cartela")
    e.soldar("Dintel – placa de testa (cumbrera)", 2 * nudos, dint.t, 15, 2 * (2 * dint.b + dint.h))
    e.soldar("Ejión de correa – dintel", len(xs) * 2 * len(us), 6, dint.t, 2 * 80, nota="ejión L 80x8, a cada lado")
    e.soldar("Orejeta del tirante – pilar/dintel", 8 * len(vanos) * 2, 8, pil.t, 2 * 80)
    e.cotas = {"largo": L, "ancho": B, "alto": H, "alto_max": hc, "xs": xs, "ys": [0, B / 2, B]}
    e.normas += ["CTE DB-SE-AE (nieve, viento)", "CTE DB-SE-A / EN 1993-1-1", "EN 1993-1-8 (nudos y soldaduras)",
                 "CTE DB-SI (resistencia al fuego según uso: puede exigir protección)"]
    e.avisos.append("Una nave necesita proyecto técnico, licencia y estudio geotécnico; este predimensionado sirve "
                    "para presupuestar y preparar el taller.")
    e.puestos["calculista"] = (f"{len(xs)} pórticos a dos aguas cada {s / 1000:.2f} m, luz {B / 1000:.2f} m y "
                               f"pendiente {P['pendiente_pct']:.0f} %; correas cada {sc / 1000:.2f} m; cruces de San "
                               "Andrés en los vanos extremos (cubierta y fachadas).")


def entreplanta(e: Estructura) -> None:
    P = e.parametros
    acero = P["acero"]
    L, B, H = P["largo"] * 1000, P["ancho"] * 1000, P["alto"] * 1000
    nombre_f, g_f, sep_v = C.FORJADOS.get(P["forjado"], C.FORJADOS["tablero"])
    uso_n, q_uso, q_h = C.USOS.get(P["uso"], C.USOS["almacen"])
    if P.get("uso_kn_m2") is not None:
        q_uso = P["uso_kn_m2"]
    e.cargas = C.Cargas(g_f + 0.25, q_uso, 0, 0, [f"Suelo: {nombre_f} ({g_f} kN/m²) + estructura ≈ 0,25 kN/m²",
                                                  f"Uso: {uso_n} ({q_uso} kN/m²)"])
    qd, qk = _kn(e.cargas.elu()), _kn(e.cargas.els())
    xs, ys = reparto(L, P.get("separacion_mm") or 5000), reparto(B, P.get("separacion_mm") or 5000)
    sx, sy = L / (len(xs) - 1), B / (len(ys) - 1)
    vs = reparto(L, sep_v * 1000)
    sv = L / (len(vs) - 1)
    vig, rv = C.elegir(["IPE", "RHS"], lambda p: C.comprobar_viga(p, acero, sy, qd * sv, qk * sv, 300))
    e.comprobar("Vigueta", vig, rv, f"biapoyada, luz {sy:.0f} mm, separación {sv:.0f} mm, flecha L/300")
    viga, rg = C.elegir(["IPE", "HEA", "HEB"], lambda p: C.comprobar_viga(p, acero, sx, qd * sy, qk * sy, 300,
                                                                          "continua" if len(xs) > 2 else "biapoyada"))
    e.comprobar("Viga principal", viga, rg, f"luz {sx:.0f} mm, ancho tributario {sy:.0f} mm, flecha L/300")
    n_ed = qd * sx * sy
    pil, rp = C.elegir(["SHS", "HEB"], lambda p: C.comprobar_pilar(p, acero, H, n_ed, n_ed * 0.05 * p.h / 2, 1, 1))
    e.comprobar("Pilar", pil, rp, f"biarticulado h = {H:.0f} mm, axil del vano interior y excentricidad h/20")
    total = qd * L * B
    f_arr = total / 200 + 1.5 * q_h * max(L, B)                  # imperfeccion + empuje en barandillas
    diag = math.hypot(sx, H)
    red, rr = C.elegir("RED", lambda p: C.comprobar_tirante(p, acero, f_arr / 2 * diag / sx), minimo="Redondo Ø16")
    e.comprobar("Cruz de San Andrés", red, rr, f"fuerza horizontal {f_arr / 1e3:.1f} kN (1/200 del peso + barandillas)")
    placa, _ = _placa_base(e, pil, n_ed)
    ztop = H                                                       # cota del suelo terminado
    zv = ztop - 40 - vig.h / 2 if P["forjado"] != "colaborante" else ztop - 120 - vig.h / 2
    zg = zv - vig.h / 2 - viga.h / 2
    pilares = [e.barra("pilar", pil, (x, y, 0), (x, y, zg - viga.h / 2), (1, 0, 0)) for x in xs for y in ys]
    for y in ys:
        e.barra("viga", viga, (-pil.h / 2, y, zg), (L + pil.h / 2, y, zg))
    for x in vs:
        xx = min(max(x, vig.b / 2), L - vig.b / 2)
        e.barra("vigueta", vig, (xx, 0, zv), (xx, B, zv))
    _add_placas_base(e, pilares, placa)
    zsuelo = zv + vig.h / 2
    e.paneles.append(([(0, 0, zsuelo), (L, 0, zsuelo), (L, B, zsuelo), (0, B, zsuelo)], P["forjado"]))
    alt = 1100 if H >= 6000 else 900
    lados = P.get("lados_barandilla", 3)
    tramos = [((0, 0, zsuelo + 40), (L, 0, zsuelo + 40))]
    if lados >= 2:
        tramos.append(((0, B, zsuelo + 40), (0, 0, zsuelo + 40)))
    if lados >= 3:
        tramos.append(((L, 0, zsuelo + 40), (L, B, zsuelo + 40)))
    if lados >= 4:
        tramos.append(((L, B, zsuelo + 40), (0, B, zsuelo + 40)))
    for i, (a, b) in enumerate(tramos):
        _barandilla(e, a, b, alt, q_h, calcular=i == 0, base_placa=False)
    for x0, x1 in ((xs[0], xs[1]),):
        for y in (ys[0], ys[-1]):
            e.barra("arriostramiento", red, (x0, y, 200), (x1, y, zg - viga.h))
            e.barra("arriostramiento", red, (x1, y, 200), (x0, y, zg - viga.h))
    for y0, y1 in ((ys[0], ys[1]),):
        for x in (xs[0], xs[-1]):
            e.barra("arriostramiento", red, (x, y0, 200), (x, y1, zg - viga.h))
            e.barra("arriostramiento", red, (x, y1, 200), (x, y0, zg - viga.h))
    e.soldar("Pilar – placa base", len(pilares), pil.t, placa.t, 2 * (pil.h + pil.b), True)
    e.soldar("Pilar – placa de cabeza / viga", len(pilares), pil.t, 12, 2 * (pil.h + pil.b), True)
    e.soldar("Vigueta – viga principal", len(vs) * len(ys), vig.tmin, viga.tf or viga.t, 2 * vig.b,
             nota="vigueta apoyada sobre la viga, cordón a ambos lados")
    e.cotas = {"largo": L, "ancho": B, "alto": H, "alto_max": zsuelo + alt, "xs": xs, "ys": ys}
    e.normas += ["CTE DB-SE-AE tabla 3.1 (sobrecarga de uso)", "CTE DB-SE-A / EN 1993-1-1",
                 "CTE DB-SUA 1 y 3.2 (barandillas: altura y huecos < 10 cm)", "EN 1993-1-8"]
    e.avisos.append("Comprueba que la solera o la cimentación aguanta el axil de cada pilar "
                    f"({n_ed / 1e3:.1f} kN mayorado en los interiores).")
    e.avisos.append("Coloca un cartel con la sobrecarga máxima admisible de la entreplanta "
                    f"({q_uso} kN/m² ≈ {q_uso * 100:.0f} kg/m²).")
    e.puestos["calculista"] = (f"Malla de {len(xs)}×{len(ys)} pilares ({sx / 1000:.2f} × {sy / 1000:.2f} m), "
                               f"viguetas cada {sv / 1000:.2f} m; cruces de San Andrés en un vano de cada dirección.")


def escalera(e: Estructura) -> None:
    P = e.parametros
    acero = P["acero"]
    Hs, W = P["alto"] * 1000, P["ancho"] * 1000
    uso_n, q_uso, q_h = C.USOS.get(P["uso"], C.USOS["vivienda"])
    if P.get("uso_kn_m2") is not None:
        q_uso = P["uso_kn_m2"]
    restringido = P["uso"] == "vivienda" and P.get("uso_restringido")
    c_max = 200 if restringido else 175
    n = max(2, math.ceil(Hs / c_max))
    ch = Hs / n
    hu = max(220 if restringido else 280, min(330, 630 - 2 * ch))
    hu = round(hu / 5) * 5
    rec = (n - 1) * hu
    ang = math.degrees(math.atan2(Hs, rec))
    e.cargas = C.Cargas(0.6, q_uso, 0, 0, ["Peldaños de chapa y barandilla ≈ 0,6 kN/m²", f"Uso: {uso_n} ({q_uso} kN/m²)"])
    qd, qk = _kn(e.cargas.elu()), _kn(e.cargas.els())
    pel = C.peldano(hu, 4 if W <= 1000 else 5)
    lp = W
    q_conc = 2 * 1.5 * 2000 / lp                                   # carga puntual de 2 kN en el centro (CTE 3.1.1)
    rpel = C.comprobar_viga(pel, acero, lp, qd * hu + q_conc, qk * hu, 250)
    e.comprobar("Peldaño", pel, rpel, f"biapoyado en las zancas, luz {lp:.0f} mm + 2 kN puntual")
    if rpel["max"] > 1:
        pel = C.peldano(hu, 6, 50)
        rpel = C.comprobar_viga(pel, acero, lp, qd * hu + q_conc, qk * hu, 250)
        e.comprobar("Peldaño (reforzado)", pel, rpel, "chapa de 6 mm con alas de 50 mm")
    linc = math.hypot(rec, Hs)
    q_z = qd * W / 2 * linc / rec                                  # por zanca, en proyeccion horizontal
    zan, rz = C.elegir(["UPN", "RHS", "PLT"], lambda p: C.comprobar_viga(p, acero, rec, q_z, qk * W / 2 * linc / rec, 300),
                       minimo="UPN 120")
    e.comprobar("Zanca", zan, rz, f"viga inclinada, luz horizontal {rec:.0f} mm ({ang:.0f}°), flecha L/300")
    e.cargas.notas.append(f"{n} contrahuellas de {ch:.0f} mm y huella de {hu:.0f} mm (2C + H = {2 * ch + hu:.0f} mm)")
    # geometria: arranca en x = 0 y sube hacia +x; zancas en y = 0 e y = W
    e.barra("zanca", zan, (0, -zan.b / 2, 0), (rec + hu, -zan.b / 2, Hs), None)
    e.barra("zanca", zan, (0, W + zan.b / 2, 0), (rec + hu, W + zan.b / 2, Hs), None)
    for i in range(n):
        z = (i + 1) * ch
        x = i * hu + hu / 2
        if i == n - 1:
            continue                                               # la ultima contrahuella llega al forjado
        e.barra("peldano", pel, (x, 0, z - pel.h / 2), (x, W, z - pel.h / 2))
    lados = int(P.get("lados_barandilla", 1))
    for k, y in enumerate((-zan.b - 30, W + zan.b + 30)[:max(1, min(2, lados))]):
        _barandilla(e, (hu / 2, y, ch), (rec + hu / 2, y, Hs), 900, q_h, inclinada=True, calcular=k == 0,
                    base_placa=False)
    for x, z in ((0, 0), (rec + hu, Hs)):
        for y in (-zan.b / 2, W + zan.b / 2):
            e.placas.append(Placa("placa de anclaje", 160, 200, 10, (x, y, z + 5), "z", "2 anclajes químicos M12"))
    e.soldar("Peldaño – zanca", 2 * (n - 1), pel.t, zan.tmin, 2 * (pel.h + 40), nota="cada extremo del peldaño")
    e.soldar("Zanca – placa de anclaje", 4, zan.t, 10, 2 * (zan.h + zan.b), True)
    e.cotas = {"largo": rec + hu, "ancho": W, "alto": Hs, "alto_max": Hs + 900, "huella": hu, "contrahuella": ch,
               "peldanos": n, "xs": [0, rec + hu], "ys": [0, W]}
    e.normas += ["CTE DB-SUA 1 4.2 (huella, contrahuella, 54 ≤ 2C + H ≤ 70 cm)", "CTE DB-SUA 3.2 (barandillas)",
                 "CTE DB-SE-AE 3.1 y 3.2", "EN 1993-1-1"]
    if not restringido and not (130 <= ch <= 185 and hu >= 280 and 540 <= 2 * ch + hu <= 700):
        e.avisos.append("Las medidas de los peldaños no cumplen el CTE DB-SUA para uso general: revisa la altura o "
                        "el espacio disponible.")
    if Hs > 3200:
        e.avisos.append("Un tramo de más de 3,20 m de altura necesita una meseta intermedia (CTE DB-SUA 4.2.2).")
    if W < 800:
        e.avisos.append("El ancho mínimo útil es 0,80 m en uso restringido y 1,00 m en el resto (CTE DB-SUA 4.2.2).")
    e.puestos["calculista"] = (f"{n} contrahuellas de {ch:.0f} mm, huella {hu:.0f} mm, recorrido {rec / 1000:.2f} m, "
                               f"inclinación {ang:.0f}°.")


def barandilla(e: Estructura) -> None:
    P = e.parametros
    L = P["largo"] * 1000
    desnivel = P.get("desnivel", 0) or 0
    alto = P["alto"] * 1000 if P.get("alto_dado") else (1100 if desnivel >= 6 else 900)
    if desnivel >= 6 and alto < 1100:
        e.avisos.append("Con más de 6 m de desnivel la barandilla debe medir al menos 1,10 m (CTE DB-SUA 3.2.1).")
    if alto < 900:
        e.avisos.append("La altura mínima de una barandilla es 0,90 m (CTE DB-SUA 3.2.1).")
    uso_n, _, q_h = C.USOS.get(P["uso"], C.USOS["vivienda"])
    e.cargas = C.Cargas(0.0, 0.0, 0, 0, [f"Fuerza horizontal en el pasamanos: {q_h} kN/m ({uso_n}, CTE DB-SE-AE 3.2)"])
    _barandilla(e, (0, 0, 0), (L, 0, 0), alto, q_h)
    mont, pas = e._bar_perfiles
    f = 1.5 * q_h * min(1200, L)
    t_perno = (f * alto / 80) / 2
    e.cargas.notas.append(f"Tracción en cada anclaje M10 ≈ {t_perno / 1e3:.1f} kN: compruébala con la ETA del taco químico.")
    e.cotas = {"largo": L, "ancho": 0, "alto": alto, "alto_max": alto, "xs": [0, L], "ys": [0]}
    e.normas += ["CTE DB-SUA 3.2 (altura ≥ 0,90/1,10 m, no escalable, huecos < 10 cm)", "CTE DB-SE-AE 3.2",
                 "EN 1993-1-1"]
    e.puestos["calculista"] = f"Montantes {mont.nombre} y pasamanos {pas.nombre}; barrotes con hueco libre de 95 mm."


def pasarela(e: Estructura) -> None:
    P = e.parametros
    acero = P["acero"]
    L, W, H = P["largo"] * 1000, P["ancho"] * 1000, P["alto"] * 1000
    nombre_f, g_f, sep_v = C.FORJADOS.get(P["forjado"], C.FORJADOS["tramex"])
    uso_n, q_uso, q_h = C.USOS.get(P["uso"], C.USOS["industrial"])
    if P.get("uso_kn_m2") is not None:
        q_uso = P["uso_kn_m2"]
    e.cargas = C.Cargas(g_f + 0.3, q_uso, 0, 0, [f"Suelo: {nombre_f} ({g_f} kN/m²) + barandillas ≈ 0,3 kN/m²",
                                                 f"Uso: {uso_n} ({q_uso} kN/m²)"])
    qd, qk = _kn(e.cargas.elu()), _kn(e.cargas.els())
    ts = reparto(L, sep_v * 1000)
    st = L / (len(ts) - 1)
    tra, rt = C.elegir(["UPN", "RHS", "IPE"], lambda p: C.comprobar_viga(p, acero, W, qd * st, qk * st, 300), minimo="UPN 80")
    e.comprobar("Travesaño", tra, rt, f"biapoyado, luz {W:.0f} mm, separación {st:.0f} mm")
    vig, rv = C.elegir(["IPE", "HEA", "RHS"], lambda p: C.comprobar_viga(p, acero, L, qd * W / 2, qk * W / 2, 350))
    e.comprobar("Viga principal", vig, rv, f"biapoyada, luz {L:.0f} mm, flecha L/350 (vibraciones)")
    zv = H - 40 - tra.h - vig.h / 2 if H else vig.h / 2
    zv = max(zv, vig.h / 2)
    for y in (0.0, W):
        e.barra("viga", vig, (0, y, zv), (L, y, zv))
    zt = zv + vig.h / 2 + tra.h / 2
    for x in ts:
        xx = min(max(x, tra.b / 2), L - tra.b / 2)
        e.barra("travesano", tra, (xx, -vig.b / 2, zt), (xx, W + vig.b / 2, zt))
    zsuelo = zt + tra.h / 2
    e.paneles.append(([(0, 0, zsuelo), (L, 0, zsuelo), (L, W, zsuelo), (0, W, zsuelo)], P["forjado"]))
    if H > 1000:
        n_ed = qd * W / 2 * L / 2
        pil, rp = C.elegir("SHS", lambda p: C.comprobar_pilar(p, acero, zv - vig.h / 2, n_ed, n_ed / 200 * H, 2, 2))
        e.comprobar("Pilar de apoyo", pil, rp, f"ménsula h = {H:.0f} mm (Lcr = 2h)")
        pilares = [e.barra("pilar", pil, (x, y, 0), (x, y, zv - vig.h / 2), (1, 0, 0)) for x in (0, L) for y in (0, W)]
        placa, _ = _placa_base(e, pil, n_ed, n_ed / 200 * H * 1.5)
        _add_placas_base(e, pilares, placa)
        e.soldar("Pilar – placa base", 4, pil.t, placa.t, 2 * (pil.h + pil.b), True)
    red, rr = C.elegir("RED", lambda p: C.comprobar_tirante(p, acero, 1.5 * q_h * L / 2 * math.hypot(st, W) / W),
                       minimo="Redondo Ø12")
    e.comprobar("Cruz de arriostramiento del tablero", red, rr, f"empuje de las barandillas {q_h} kN/m en los vanos extremos")
    for x0, x1 in ((ts[0], ts[1]), (ts[-2], ts[-1])):
        e.barra("arriostramiento", red, (x0, 0, zv), (x1, W, zv))
        e.barra("arriostramiento", red, (x1, 0, zv), (x0, W, zv))
    alt = 1100 if H >= 6000 else 900
    for i, y in enumerate((0, W)):
        _barandilla(e, (0, y, zsuelo + 40), (L, y, zsuelo + 40), alt, q_h, calcular=i == 0, base_placa=False)
    e.soldar("Travesaño – viga principal", 2 * len(ts), tra.tmin, vig.tf or vig.t, 2 * (tra.h + tra.b) / 2)
    e.cotas = {"largo": L, "ancho": W, "alto": H, "alto_max": zsuelo + alt, "xs": [0, L], "ys": [0, W]}
    e.normas += ["CTE DB-SE-AE 3.1 y 3.2", "CTE DB-SE-A / EN 1993-1-1", "CTE DB-SUA 3.2 (barandillas)", "EN 1993-1-8"]
    if L > 8000:
        e.avisos.append("Pasarela de más de 8 m: comprueba la vibración (frecuencia propia > 5 Hz) con el técnico.")
    e.puestos["calculista"] = f"Dos vigas de {L / 1000:.2f} m, travesaños cada {st / 1000:.2f} m, suelo de {nombre_f.lower()}."


TIPOS = {
    "porche": (porche, "Porche, cochera o marquesina con cubierta a un agua", dict(largo=6, ancho=4, alto=2.6, pendiente_pct=5, cubierta="sandwich")),
    "pergola": (pergola, "Pérgola de lamas", dict(largo=4, ancho=3, alto=2.5, pendiente_pct=0, cubierta="lamas")),
    "nave": (nave, "Nave a dos aguas con pórticos", dict(largo=20, ancho=12, alto=6, pendiente_pct=10, cubierta="sandwich")),
    "entreplanta": (entreplanta, "Entreplanta o altillo", dict(largo=6, ancho=4, alto=2.8, uso="almacen", forjado="tablero")),
    "escalera": (escalera, "Escalera recta con barandilla", dict(alto=2.8, ancho=1.0, uso="vivienda")),
    "barandilla": (barandilla, "Barandilla de barrotes", dict(largo=5, alto=0.9, uso="vivienda")),
    "pasarela": (pasarela, "Pasarela peatonal o de mantenimiento", dict(largo=6, ancho=1.2, alto=3, uso="industrial", forjado="tramex")),
}
BASE = dict(acero="S275", acabado="galvanizado", cubierta="sandwich", uso="mantenimiento", forjado="tablero",
            altitud=200, zona_viento="B", pendiente_pct=5, largo=6, ancho=4, alto=2.6)


def construir(tipo: str, parametros: dict) -> Estructura:
    if tipo not in TIPOS:
        raise ValueError(f"tipo desconocido: {tipo} (usa {', '.join(TIPOS)})")
    fn, titulo, defecto = TIPOS[tipo]
    P = {**BASE, **defecto, **{k: v for k, v in parametros.items() if v not in (None, "")}}
    P["acero"] = str(P["acero"]).upper().replace("JR", "").replace("J0", "").replace("J2", "")[:4]
    if P["acero"] not in C.ACEROS:
        P["acero"] = "S275"
    for k in ("largo", "ancho", "alto", "altitud", "pendiente_pct"):
        P[k] = float(P[k])
    lim = {"largo": (0.5, 80), "ancho": (0.3, 40), "alto": (0.3, 15)}
    for k, (lo, hi) in lim.items():
        if not lo <= P[k] <= hi:
            raise ValueError(f"{k} fuera de rango para esta oficina ({lo}-{hi} m): {P[k]}")
    e = Estructura(tipo, titulo, P)
    fn(e)
    if P["acabado"] not in ACABADOS:
        P["acabado"] = "galvanizado"
    e.normas.insert(0, f"EN 1090-1/-2: marcado CE y clase de ejecución {C.clase_ejecucion(tipo, P['uso'])}")
    e.normas.append(f"Acero {P['acero']}JR según UNE-EN 10025-2 (tubos EN 10219), tornillería 8.8 (ISO 898-1)")
    e.normas.append("Soldadura: procesos ISO 4063 (135 MAG), soldadores UNE-EN ISO 9606-1, símbolos ISO 2553")
    e.normas.append(f"Protección: {ACABADOS[P['acabado']][0]}")
    if P["acabado"] == "galvanizado":
        e.avisos.append("Galvanizado: los tubos cerrados llevan taladros de venteo y drenaje (Ø ≥ 10 mm) en cada "
                        "extremo antes de ir a la cuba (UNE-EN ISO 14713-2).")
    marcar(e)
    return e


def marcar(e: Estructura) -> None:
    """Marca cada pieza: las iguales (rol, perfil y longitud) comparten marca, como en el despiece."""
    grupos: dict[tuple, str] = {}
    cont: dict[str, int] = {}
    for b in sorted(e.barras, key=lambda b: (list(ROLES).index(b.rol) if b.rol in ROLES else 99, -b.longitud)):
        k = (b.rol, b.perfil.nombre, round(b.longitud))
        if k not in grupos:
            letra = ROLES.get(b.rol, ("X",))[0]
            cont[letra] = cont.get(letra, 0) + 1
            grupos[k] = f"{letra}{cont[letra]}"
        b.marca = grupos[k]
    gp: dict[tuple, str] = {}
    for p in e.placas:
        k = (p.rol, p.a, p.b, p.t)
        if k not in gp:
            gp[k] = f"PL{len(gp) + 1}"
        p.marca = gp[k]
