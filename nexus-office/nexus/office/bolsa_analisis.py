"""Analisis para el agente de bolsa: indicadores, opciones, riesgo, valoracion, cribas, alertas y expedientes.

Todo es calculo local y determinista (sin red ni modelo): cada cifra que devuelve una herramienta sale de aqui y
se puede repetir. Las ideas vienen de OpenTerminalUI (constructor de estrategias con opciones, VaR/CVaR, EWMA,
escenarios de estres, DCF inverso, tamano de posicion, cribas de inversores conocidos, alertas con varias
condiciones y expedientes con tesis y evidencias; licencia MIT). No se ha copiado codigo.
"""
from __future__ import annotations

import ast
import json
import math
import operator
import statistics
import threading
import time

from . import bolsa_lab as lab

AVISO = "Cálculo orientativo con datos públicos; no es asesoramiento financiero."


def _r(v, n: int = 4):
    return None if v is None or (isinstance(v, float) and (math.isnan(v) or math.isinf(v))) else round(float(v), n)


# ------------------------------------------------------------------ indicadores tecnicos
def atr(pts: list[dict], n: int = 14) -> float | None:
    """Rango medio verdadero. Sin maximos y minimos usa la variacion entre cierres."""
    tr = []
    for a, b in zip(pts, pts[1:]):
        c0, c1 = float(a["precio"]), float(b["precio"])
        hi, lo = float(b.get("maximo") or max(c0, c1)), float(b.get("minimo") or min(c0, c1))
        tr.append(max(hi - lo, abs(hi - c0), abs(lo - c0)))
    if len(tr) < n:
        return None
    v = sum(tr[:n]) / n
    for x in tr[n:]:
        v = (v * (n - 1) + x) / n
    return v


def vwap(pts: list[dict]) -> float | None:
    num = den = 0.0
    for p in pts:
        vol = float(p.get("volumen") or 0)
        if vol <= 0:
            continue
        tipico = (float(p.get("maximo") or p["precio"]) + float(p.get("minimo") or p["precio"]) + float(p["precio"])) / 3
        num += tipico * vol
        den += vol
    return num / den if den else None


def estocastico(pts: list[dict], n: int = 14) -> float | None:
    if len(pts) < n:
        return None
    v = pts[-n:]
    hi = max(float(p.get("maximo") or p["precio"]) for p in v)
    lo = min(float(p.get("minimo") or p["precio"]) for p in v)
    return 50.0 if hi == lo else (float(pts[-1]["precio"]) - lo) / (hi - lo) * 100


def indicadores(pts: list[dict]) -> list[dict]:
    """Tabla {indicador, valor, lectura} con lo ultimo de la serie."""
    x = [float(p["precio"]) for p in pts if p.get("precio")]
    if len(x) < 15:
        raise ValueError("Hacen falta al menos 15 precios para calcular indicadores")
    px = x[-1]
    filas: list[dict] = [{"indicador": "Precio", "valor": _r(px), "lectura": ""}]

    def fila(nombre, valor, lectura=""):
        if valor is not None:
            filas.append({"indicador": nombre, "valor": _r(valor), "lectura": lectura})

    for n in (20, 50, 200):
        m = lab.sma(x, n)
        fila(f"Media simple {n}", m, "" if m is None else ("precio por encima" if px > m else "precio por debajo"))
    for n in (12, 26):
        fila(f"Media exponencial {n}", lab.ema(x, n))
    r = lab.rsi(x)
    fila("RSI 14", r, "" if r is None else ("sobrecompra" if r > 70 else "sobreventa" if r < 30 else "neutral"))
    m = lab.macd(x)
    if m:
        vals = [v for v in m.values() if isinstance(v, (int, float))]
        hist = m.get("histograma", m.get("hist", vals[-1] if vals else None))
        fila("MACD", m.get("macd", vals[0] if vals else None))
        fila("MACD señal", m.get("senal", m.get("signal")))
        fila("MACD histograma", hist, "" if hist is None else ("impulso alcista" if hist > 0 else "impulso bajista"))
    b = lab.bollinger(x)
    if b:
        alta = b.get("alta", b.get("superior", b.get("upper")))
        baja = b.get("baja", b.get("inferior", b.get("lower")))
        fila("Bollinger alta", alta)
        fila("Bollinger baja", baja)
        if alta is not None and baja is not None and alta > baja:
            pos = (px - baja) / (alta - baja) * 100
            fila("Posición en las bandas (%)", pos, "cerca de la banda alta" if pos > 85 else
                 "cerca de la banda baja" if pos < 15 else "zona media")
    a = atr(pts)
    fila("ATR 14", a, "" if a is None else f"{a / px * 100:.2f} % del precio")
    fila("VWAP del periodo", vwap(pts))
    k = estocastico(pts)
    fila("Estocástico 14", k, "" if k is None else ("sobrecompra" if k > 80 else "sobreventa" if k < 20 else "neutral"))
    rets = lab.rentabilidades(x)
    if len(rets) > 10:
        periodos = lab.periodos_anio([float(p["ts"]) for p in pts if p.get("ts")])
        fila("Volatilidad anualizada (%)", statistics.pstdev(rets) * math.sqrt(periodos) * 100)
        fila("Volatilidad EWMA (%)", volatilidad_ewma(rets) * math.sqrt(periodos) * 100)
    fila("Máximo del periodo", max(x), f"{(px / max(x) - 1) * 100:+.1f} % desde el máximo")
    fila("Mínimo del periodo", min(x), f"{(px / min(x) - 1) * 100:+.1f} % desde el mínimo")
    fila("Rentabilidad del periodo (%)", (px / x[0] - 1) * 100)
    return filas


# ------------------------------------------------------------------ opciones
def _N(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def _n(x: float) -> float:
    return math.exp(-0.5 * x * x) / math.sqrt(2 * math.pi)


def black_scholes(S: float, K: float, t: float, r: float, sigma: float, tipo: str = "call", q: float = 0.0) -> dict:
    """Precio y griegas de una opcion europea. t en anios; theta por dia natural; vega y rho por punto porcentual."""
    S, K, t, sigma = float(S), float(K), max(float(t), 1e-6), max(float(sigma), 1e-6)
    call = str(tipo).lower().startswith("c")
    d1 = (math.log(S / K) + (r - q + sigma * sigma / 2) * t) / (sigma * math.sqrt(t))
    d2 = d1 - sigma * math.sqrt(t)
    df, dq = math.exp(-r * t), math.exp(-q * t)
    if call:
        precio = S * dq * _N(d1) - K * df * _N(d2)
        delta = dq * _N(d1)
        theta = (-S * dq * _n(d1) * sigma / (2 * math.sqrt(t)) - r * K * df * _N(d2) + q * S * dq * _N(d1)) / 365
        rho = K * t * df * _N(d2) / 100
    else:
        precio = K * df * _N(-d2) - S * dq * _N(-d1)
        delta = -dq * _N(-d1)
        theta = (-S * dq * _n(d1) * sigma / (2 * math.sqrt(t)) + r * K * df * _N(-d2) - q * S * dq * _N(-d1)) / 365
        rho = -K * t * df * _N(-d2) / 100
    return {"precio": _r(precio), "delta": _r(delta), "gamma": _r(dq * _n(d1) / (S * sigma * math.sqrt(t)), 6),
            "theta": _r(theta), "vega": _r(S * dq * _n(d1) * math.sqrt(t) / 100), "rho": _r(rho),
            "prob_en_dinero_pct": _r((_N(d2) if call else _N(-d2)) * 100, 2)}


def volatilidad_implicita(precio: float, S: float, K: float, t: float, r: float = 0.04, tipo: str = "call") -> float | None:
    """Volatilidad que explica el precio de mercado (biseccion). None si el precio no es coherente."""
    precio, S, K, t = float(precio), float(S), float(K), max(float(t), 1e-6)
    call = str(tipo).lower().startswith("c")
    intrinseco = max(0.0, S - K * math.exp(-r * t)) if call else max(0.0, K * math.exp(-r * t) - S)
    if precio <= intrinseco + 1e-9 or precio >= (S if call else K):
        return None
    lo, hi = 0.005, 6.0
    for _ in range(70):
        mid = (lo + hi) / 2
        if black_scholes(S, K, t, r, mid, tipo)["precio"] > precio:
            hi = mid
        else:
            lo = mid
    v = (lo + hi) / 2
    return None if v > 5.9 or v < 0.006 else v


# plantillas: cada pata es (tipo, lado, desplazamiento del strike en % sobre el subyacente, cantidad)
ESTRATEGIAS = {
    "call_comprada": ("Call comprada: alcista con pérdida limitada a la prima", [("call", "compra", 0, 1)]),
    "put_comprada": ("Put comprada: bajista o seguro", [("put", "compra", 0, 1)]),
    "call_cubierta": ("Call cubierta: acciones más call vendida; renta a cambio de limitar la subida",
                      [("accion", "compra", 0, 1), ("call", "venta", 5, 1)]),
    "put_protectora": ("Put protectora: acciones más put comprada; seguro contra caídas",
                       [("accion", "compra", 0, 1), ("put", "compra", -5, 1)]),
    "collar": ("Collar: acciones, put comprada y call vendida", [("accion", "compra", 0, 1), ("put", "compra", -5, 1),
                                                                ("call", "venta", 5, 1)]),
    "diferencial_alcista": ("Diferencial alcista con calls", [("call", "compra", 0, 1), ("call", "venta", 5, 1)]),
    "diferencial_bajista": ("Diferencial bajista con puts", [("put", "compra", 0, 1), ("put", "venta", -5, 1)]),
    "cono": ("Cono comprado (straddle): gana si se mueve mucho en cualquier sentido",
             [("call", "compra", 0, 1), ("put", "compra", 0, 1)]),
    "cuna": ("Cuna comprada (strangle): como el cono pero más barata",
             [("call", "compra", 5, 1), ("put", "compra", -5, 1)]),
    "mariposa": ("Mariposa con calls: gana si el precio se queda quieto",
                 [("call", "compra", -5, 1), ("call", "venta", 0, 2), ("call", "compra", 5, 1)]),
    "condor_hierro": ("Cóndor de hierro: cobra prima si el precio se queda en un rango",
                      [("put", "compra", -10, 1), ("put", "venta", -5, 1), ("call", "venta", 5, 1),
                       ("call", "compra", 10, 1)]),
}


def patas_de(estrategia: str, subyacente: float, dias: float = 30, vol: float = 0.25, r: float = 0.04,
             cadena: list[dict] | None = None) -> list[dict]:
    """Patas de una plantilla. Con cadena real toma el strike mas cercano y su prima; sin ella, prima teorica."""
    if estrategia not in ESTRATEGIAS:
        raise ValueError(f"estrategia desconocida; disponibles: {', '.join(ESTRATEGIAS)}")
    t = max(float(dias), 0.5) / 365
    patas = []
    for tipo, lado, desp, cantidad in ESTRATEGIAS[estrategia][1]:
        if tipo == "accion":
            patas.append({"tipo": "accion", "lado": lado, "precio": float(subyacente), "cantidad": cantidad})
            continue
        strike = float(subyacente) * (1 + desp / 100)
        prima = None
        if cadena:
            f = min(cadena, key=lambda x: abs(float(x["strike"]) - strike))
            strike = float(f["strike"])
            o = f.get(tipo) or {}
            prima = ((o["compra"] + o["venta"]) / 2 if o.get("compra") and o.get("venta") else o.get("ultimo"))
        if not prima:
            prima = black_scholes(subyacente, strike, t, r, vol, tipo)["precio"]
        patas.append({"tipo": tipo, "lado": lado, "strike": round(strike, 2), "prima": round(float(prima), 4),
                      "cantidad": cantidad})
    return patas


def estrategia_opciones(patas: list[dict], subyacente: float, multiplicador: int = 100, dias: float | None = None,
                        vol: float | None = None, r: float = 0.04) -> dict:
    """Resultado a vencimiento de una combinacion de opciones y acciones: coste, maximos, puntos de equilibrio y
    curva. Cada pata: {tipo: call|put|accion, lado: compra|venta, strike, prima, cantidad} (accion: precio)."""
    if not patas:
        raise ValueError("Hace falta al menos una pata")
    S0 = float(subyacente)
    norm = []
    for p in patas:
        tipo = str(p.get("tipo", "call")).lower()
        signo = 1 if str(p.get("lado", "compra")).lower().startswith("c") else -1
        cant = float(p.get("cantidad") or 1)
        if tipo.startswith("a"):
            norm.append(("accion", signo, float(p.get("precio") or S0), 0.0, cant))
        else:
            if p.get("strike") is None:
                raise ValueError("Cada opción necesita su strike")
            prima = p.get("prima")
            if prima is None:
                if not (dias and vol):
                    raise ValueError("Sin prima hay que dar dias y vol para calcularla")
                prima = black_scholes(S0, float(p["strike"]), dias / 365, r, vol, tipo)["precio"]
            norm.append(("call" if tipo.startswith("c") else "put", signo, float(p["strike"]), float(prima), cant))

    def resultado(S: float) -> float:
        total = 0.0
        for tipo, signo, k, prima, cant in norm:
            if tipo == "accion":
                total += signo * (S - k) * cant * multiplicador
            else:
                valor = max(0.0, S - k) if tipo == "call" else max(0.0, k - S)
                total += signo * (valor - prima) * cant * multiplicador
        return total

    niveles = sorted({k for t, _, k, _, _ in norm if t != "accion"} | {S0})
    lo, hi = min(niveles) * 0.5, max(niveles) * 1.5
    paso = (hi - lo) / 240
    xs = sorted({lo + i * paso for i in range(241)} | set(niveles))
    ys = [resultado(x) for x in xs]
    equilibrio = []
    for (x0, y0), (x1, y1) in zip(zip(xs, ys), zip(xs[1:], ys[1:])):
        if y0 == 0:
            equilibrio.append(x0)
        elif y0 * y1 < 0:
            equilibrio.append(x0 - y0 * (x1 - x0) / (y1 - y0))
    pend_arriba = sum(s * c for t, s, _, _, c in norm if t in ("call", "accion"))
    en_cero = resultado(0.0)
    maximo = None if pend_arriba > 1e-9 else max(max(ys), en_cero)
    minimo = None if pend_arriba < -1e-9 else min(min(ys), en_cero)
    coste = sum(s * prima * c * multiplicador for t, s, _, prima, c in norm if t != "accion")
    out = {"coste_neto": _r(coste, 2), "tipo_coste": "se paga" if coste > 0 else "se cobra",
           "beneficio_maximo": "ilimitado" if maximo is None else _r(maximo, 2),
           "perdida_maxima": "ilimitada" if minimo is None else _r(minimo, 2),
           "puntos_equilibrio": [_r(e, 2) for e in equilibrio], "resultado_al_precio_actual": _r(resultado(S0), 2),
           "curva": [{"precio": _r(x, 2), "resultado": _r(y, 2)} for x, y in list(zip(xs, ys))[::12]],
           "patas": [{"tipo": t, "lado": "compra" if s > 0 else "venta", "strike": k if t != "accion" else None,
                      "prima": prima if t != "accion" else None, "cantidad": c} for t, s, k, prima, c in norm],
           "aviso": AVISO}
    if dias and vol:   # griegas agregadas y probabilidad de acabar en beneficio (lognormal)
        t = max(float(dias), 0.5) / 365
        g = {"delta": 0.0, "gamma": 0.0, "theta": 0.0, "vega": 0.0}
        for tipo, signo, k, _, cant in norm:
            if tipo == "accion":
                g["delta"] += signo * cant * multiplicador
                continue
            b = black_scholes(S0, k, t, r, vol, tipo)
            for clave in g:
                g[clave] += signo * cant * multiplicador * b[clave]
        out["griegas"] = {k: _r(v, 3) for k, v in g.items()}
        mu, sd = math.log(S0) + (r - vol * vol / 2) * t, vol * math.sqrt(t)
        prob, previo = 0.0, None
        for x, y in zip(xs, ys):
            c = _N((math.log(x) - mu) / sd) if x > 0 else 0.0
            if previo is not None and (y > 0 or previo[1] > 0):
                peso = 1.0 if (y > 0 and previo[1] > 0) else 0.5
                prob += (c - previo[0]) * peso
            previo = (c, y)
        out["prob_beneficio_pct"] = _r(prob * 100, 1)
    return out


# ------------------------------------------------------------------ riesgo
def volatilidad_ewma(rets: list[float], lam: float = 0.94) -> float:
    """Volatilidad con media movil exponencial (da mas peso a lo reciente)."""
    if not rets:
        return 0.0
    var = rets[0] ** 2
    for x in rets[1:]:
        var = lam * var + (1 - lam) * x * x
    return math.sqrt(var)


def _cuantil(orden: list[float], q: float) -> float:
    if not orden:
        return 0.0
    pos = q * (len(orden) - 1)
    i = int(pos)
    return orden[i] + (orden[min(i + 1, len(orden) - 1)] - orden[i]) * (pos - i)


def caida_maxima(valores: list[float]) -> float:
    pico, peor = valores[0], 0.0
    for v in valores:
        pico = max(pico, v)
        peor = min(peor, v / pico - 1)
    return peor


def _corr(a: list[float], b: list[float]) -> float | None:
    n = min(len(a), len(b))
    if n < 5:
        return None
    a, b = a[-n:], b[-n:]
    ma, mb = sum(a) / n, sum(b) / n
    va, vb = sum((x - ma) ** 2 for x in a), sum((x - mb) ** 2 for x in b)
    return None if va <= 0 or vb <= 0 else sum((x - ma) * (y - mb) for x, y in zip(a, b)) / math.sqrt(va * vb)


def beta(rets: list[float], mercado: list[float]) -> float | None:
    n = min(len(rets), len(mercado))
    if n < 10:
        return None
    a, b = rets[-n:], mercado[-n:]
    mb = sum(b) / n
    var = sum((x - mb) ** 2 for x in b)
    ma = sum(a) / n
    return None if var <= 0 else sum((x - ma) * (y - mb) for x, y in zip(a, b)) / var


def riesgo(series: list[dict], pesos: dict | None = None, capital: float = 10000.0, confianza: float = 0.95,
           horizonte: int = 1, referencia: list[dict] | None = None) -> dict:
    """Riesgo de una cartera. series: [{simbolo, puntos}]; pesos {simbolo: peso} (por defecto, iguales).
    Devuelve VaR historico y parametrico, CVaR, volatilidades, caida maxima, beta y correlaciones."""
    limpias = [(s.get("simbolo", "?"), [float(p["precio"]) for p in s.get("puntos", []) if p.get("precio")])
               for s in series]
    limpias = [(n, x) for n, x in limpias if len(x) >= 30]
    if not limpias:
        raise ValueError("Hacen falta al menos 30 precios de cada valor")
    n = min(len(x) for _, x in limpias)
    rets = {nombre: lab.rentabilidades(x[-n:]) for nombre, x in limpias}
    w = {k: float((pesos or {}).get(k, 1.0)) for k in rets}
    total = sum(abs(v) for v in w.values()) or 1.0
    w = {k: v / total for k, v in w.items()}
    cartera = [sum(w[k] * rets[k][i] for k in rets) for i in range(n - 1)]
    orden = sorted(cartera)
    q = _cuantil(orden, 1 - confianza)
    cola = [x for x in orden if x <= q] or [q]
    raiz = math.sqrt(max(1, int(horizonte)))
    sd = statistics.pstdev(cartera)
    z = {0.9: 1.2816, 0.95: 1.6449, 0.975: 1.96, 0.99: 2.3263}.get(round(confianza, 3), 1.6449)
    valor = [1.0]
    for x in cartera:
        valor.append(valor[-1] * (1 + x))
    out = {"valores": list(rets), "pesos_pct": {k: _r(v * 100, 2) for k, v in w.items()}, "sesiones": n - 1,
           "confianza_pct": confianza * 100, "horizonte_sesiones": horizonte,
           "var_historico_pct": _r(-q * raiz * 100, 3), "var_historico": _r(-q * raiz * capital, 2),
           "var_parametrico_pct": _r((z * sd - sum(cartera) / len(cartera)) * raiz * 100, 3),
           "cvar_pct": _r(-sum(cola) / len(cola) * raiz * 100, 3),
           "cvar": _r(-sum(cola) / len(cola) * raiz * capital, 2),
           "volatilidad_anual_pct": _r(sd * math.sqrt(252) * 100, 2),
           "volatilidad_ewma_anual_pct": _r(volatilidad_ewma(cartera) * math.sqrt(252) * 100, 2),
           "caida_maxima_pct": _r(caida_maxima(valor) * 100, 2),
           "peor_sesion_pct": _r(orden[0] * 100, 2), "mejor_sesion_pct": _r(orden[-1] * 100, 2), "aviso": AVISO}
    if referencia:
        ref = lab.rentabilidades([float(p["precio"]) for p in referencia if p.get("precio")])
        out["beta"] = _r(beta(cartera, ref), 3)
        out["correlacion_con_referencia"] = _r(_corr(cartera, ref), 3)
    nombres = list(rets)
    if len(nombres) > 1:
        out["correlaciones"] = [{"a": a, "b": b, "correlacion": _r(_corr(rets[a], rets[b]), 3)}
                                for i, a in enumerate(nombres) for b in nombres[i + 1:]]
        indiv = {k: statistics.pstdev(v) for k, v in rets.items()}
        suma = sum(abs(w[k]) * indiv[k] for k in rets)
        out["ratio_diversificacion"] = _r(suma / sd, 3) if sd > 0 else None
    return out


# choques por clase de activo, en % (orientativos, a partir de episodios historicos)
ESCENARIOS = {
    "crisis_2008": ("Crisis financiera de 2008", {"renta_variable": -45, "bonos": 6, "oro": 5, "cripto": -60,
                                                  "materias_primas": -35, "inmobiliario": -40, "efectivo": 0}),
    "covid_2020": ("Desplome del covid (febrero-marzo de 2020)", {"renta_variable": -34, "bonos": 3, "oro": -3,
                                                                  "cripto": -50, "materias_primas": -30,
                                                                  "inmobiliario": -40, "efectivo": 0}),
    "subida_tipos_2022": ("Subida de tipos de 2022", {"renta_variable": -25, "bonos": -17, "oro": -1, "cripto": -65,
                                                      "materias_primas": 16, "inmobiliario": -26, "efectivo": 0}),
    "punto_com_2000": ("Pinchazo de las puntocom", {"renta_variable": -49, "bonos": 12, "oro": 0, "cripto": 0,
                                                    "materias_primas": -10, "inmobiliario": 10, "efectivo": 0}),
    "inflacion_70": ("Estanflación de los años setenta", {"renta_variable": -40, "bonos": -20, "oro": 70,
                                                          "cripto": 0, "materias_primas": 60, "inmobiliario": -10,
                                                          "efectivo": 0}),
    "correccion": ("Corrección corriente del 10 %", {"renta_variable": -10, "bonos": 1, "oro": 1, "cripto": -20,
                                                     "materias_primas": -5, "inmobiliario": -8, "efectivo": 0}),
}


def estres(posiciones: list[dict], escenarios: list[str] | None = None) -> dict:
    """posiciones: [{simbolo, valor, clase (renta_variable|bonos|oro|cripto|materias_primas|inmobiliario|efectivo),
    beta}]. La beta multiplica el choque de la renta variable."""
    total = sum(float(p.get("valor") or 0) for p in posiciones)
    if total <= 0:
        raise ValueError("Las posiciones necesitan su valor")
    filas = []
    for clave in (escenarios or list(ESCENARIOS)):
        if clave not in ESCENARIOS:
            continue
        nombre, choques = ESCENARIOS[clave]
        perdida, detalle = 0.0, []
        for p in posiciones:
            clase = str(p.get("clase") or "renta_variable")
            choque = float(choques.get(clase, choques["renta_variable"]))
            if clase == "renta_variable":
                choque *= float(p.get("beta") or 1.0)
            choque = max(choque, -100.0)
            d = float(p.get("valor") or 0) * choque / 100
            perdida += d
            detalle.append({"simbolo": p.get("simbolo", "?"), "choque_pct": _r(choque, 1), "resultado": _r(d, 2)})
        filas.append({"escenario": clave, "nombre": nombre, "resultado": _r(perdida, 2),
                      "resultado_pct": _r(perdida / total * 100, 2), "detalle": detalle})
    return {"valor_cartera": _r(total, 2), "escenarios": sorted(filas, key=lambda f: f["resultado"]), "aviso": AVISO}


def tamano_posicion(capital: float, riesgo_pct: float, entrada: float, stop: float, objetivo: float | None = None,
                    prob_acierto: float | None = None, max_peso_pct: float = 100.0) -> dict:
    """Cuantas acciones comprar para arriesgar un % fijo del capital entre la entrada y el stop."""
    capital, entrada, stop = float(capital), float(entrada), float(stop)
    por_accion = abs(entrada - stop)
    if por_accion <= 0 or entrada <= 0:
        raise ValueError("La entrada y el stop tienen que ser distintos y positivos")
    arriesgado = capital * float(riesgo_pct) / 100
    acciones = math.floor(arriesgado / por_accion)
    tope = math.floor(capital * float(max_peso_pct) / 100 / entrada)
    limitado = acciones > tope
    acciones = min(acciones, tope)
    out = {"acciones": acciones, "importe": _r(acciones * entrada, 2), "peso_pct": _r(acciones * entrada / capital * 100, 2),
           "riesgo_por_accion": _r(por_accion, 4), "perdida_si_salta_el_stop": _r(acciones * por_accion, 2),
           "riesgo_real_pct": _r(acciones * por_accion / capital * 100, 3), "limitado_por_peso_maximo": limitado,
           "lado": "largo" if stop < entrada else "corto", "aviso": AVISO}
    if objetivo is not None:
        ratio = abs(float(objetivo) - entrada) / por_accion
        out.update(ratio_beneficio_riesgo=_r(ratio, 2), beneficio_en_objetivo=_r(acciones * abs(float(objetivo) - entrada), 2),
                   acierto_minimo_pct=_r(100 / (1 + ratio), 1))
        if prob_acierto is not None:
            out["kelly"] = kelly(float(prob_acierto), ratio)
    return out


def kelly(prob_acierto: float, ratio: float) -> dict:
    """Fraccion de Kelly: p - (1 - p) / ratio. Se aconseja usar la mitad o menos."""
    p = prob_acierto / 100 if prob_acierto > 1 else prob_acierto
    f = p - (1 - p) / ratio if ratio > 0 else 0.0
    return {"fraccion_pct": _r(max(f, 0) * 100, 2), "media_kelly_pct": _r(max(f, 0) * 50, 2),
            "esperanza_por_unidad_arriesgada": _r(p * ratio - (1 - p), 3),
            "lectura": "sin ventaja: no operar" if f <= 0 else "hay ventaja si la probabilidad estimada es cierta"}


# ------------------------------------------------------------------ valoracion
def dcf(fcf: float, crecimiento_pct: float, descuento_pct: float, terminal_pct: float = 2.0, anios: int = 10,
        deuda_neta: float = 0.0, acciones: float = 0.0, desvanecer: bool = True) -> dict:
    """Descuento de flujos de caja. Con desvanecer, el crecimiento baja linealmente hasta el terminal."""
    g, r, gt = float(crecimiento_pct) / 100, float(descuento_pct) / 100, float(terminal_pct) / 100
    if r <= gt:
        raise ValueError("La tasa de descuento tiene que ser mayor que el crecimiento terminal")
    flujo, valor, filas = float(fcf), 0.0, []
    for i in range(1, int(anios) + 1):
        gi = g + (gt - g) * (i - 1) / max(1, anios - 1) if desvanecer else g
        flujo *= 1 + gi
        actual = flujo / (1 + r) ** i
        valor += actual
        filas.append({"anio": i, "crecimiento_pct": _r(gi * 100, 2), "flujo": _r(flujo, 2), "valor_actual": _r(actual, 2)})
    terminal = flujo * (1 + gt) / (r - gt) / (1 + r) ** anios
    empresa = valor + terminal
    capital = empresa - float(deuda_neta)
    out = {"valor_empresa": _r(empresa, 2), "valor_capital": _r(capital, 2), "valor_flujos": _r(valor, 2),
           "valor_terminal": _r(terminal, 2), "peso_terminal_pct": _r(terminal / empresa * 100, 1) if empresa else None,
           "flujos": filas, "aviso": AVISO}
    if acciones:
        out["valor_por_accion"] = _r(capital / float(acciones), 2)
    return out


def dcf_inverso(precio: float, acciones: float, fcf: float, descuento_pct: float, terminal_pct: float = 2.0,
                anios: int = 10, deuda_neta: float = 0.0) -> dict:
    """Que crecimiento del flujo de caja descuenta ya el precio (biseccion sobre dcf)."""
    objetivo = float(precio) * float(acciones) + float(deuda_neta)
    if fcf <= 0:
        raise ValueError("El DCF inverso necesita un flujo de caja positivo")

    def valor(g):
        return dcf(fcf, g, descuento_pct, terminal_pct, anios, 0.0, 0.0)["valor_empresa"]

    lo, hi = -50.0, 150.0
    if valor(hi) < objetivo:
        return {"crecimiento_implicito_pct": None, "lectura": "ni con un 150 % anual se justifica el precio",
                "aviso": AVISO}
    for _ in range(60):
        mid = (lo + hi) / 2
        if valor(mid) > objetivo:
            hi = mid
        else:
            lo = mid
    g = (lo + hi) / 2
    lectura = ("el precio descuenta un crecimiento muy alto" if g > 20 else "el precio descuenta un crecimiento alto"
               if g > 10 else "el precio descuenta un crecimiento moderado" if g > 3 else
               "el precio descuenta poco o ningún crecimiento")
    return {"crecimiento_implicito_pct": _r(g, 2), "valor_empresa_objetivo": _r(objetivo, 2), "lectura": lectura,
            "supuestos": {"descuento_pct": descuento_pct, "terminal_pct": terminal_pct, "anios": anios}, "aviso": AVISO}


# ------------------------------------------------------------------ formulas y cribas
_BIN = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
        ast.Pow: operator.pow, ast.Mod: operator.mod, ast.FloorDiv: operator.floordiv}
_CMP = {ast.Gt: operator.gt, ast.GtE: operator.ge, ast.Lt: operator.lt, ast.LtE: operator.le, ast.Eq: operator.eq,
        ast.NotEq: operator.ne}
_FUN = {"abs": abs, "min": min, "max": max, "round": round, "sqrt": math.sqrt, "log": math.log, "exp": math.exp}


def evaluar_formula(expr: str, fila: dict):
    """Evalua una formula sobre los campos de una fila («roe_pct > 15 and per < 20», «precio / bpa»). Solo admite
    numeros, campos, operaciones aritmeticas, comparaciones, and/or/not y unas pocas funciones: nada de codigo."""
    arbol = ast.parse(str(expr).replace(" y ", " and ").replace(" o ", " or "), mode="eval")

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float, str, bool)):
            return n.value
        if isinstance(n, ast.Name):
            if n.id not in fila:
                raise KeyError(n.id)
            return fila[n.id]
        if isinstance(n, ast.BinOp) and type(n.op) in _BIN:
            a, b = ev(n.left), ev(n.right)
            if a is None or b is None:
                return None
            if isinstance(n.op, ast.Pow) and abs(float(b)) > 16:
                raise ValueError("exponente demasiado grande")
            return _BIN[type(n.op)](a, b)
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd, ast.Not)):
            v = ev(n.operand)
            return (not v) if isinstance(n.op, ast.Not) else (None if v is None else (-v if isinstance(n.op, ast.USub) else v))
        if isinstance(n, ast.BoolOp):
            vals = [ev(v) for v in n.values]
            return all(vals) if isinstance(n.op, ast.And) else any(vals)
        if isinstance(n, ast.Compare):
            izq = ev(n.left)
            for op, der in zip(n.ops, n.comparators):
                d = ev(der)
                if izq is None or d is None or type(op) not in _CMP or not _CMP[type(op)](izq, d):
                    return False
                izq = d
            return True
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in _FUN and not n.keywords:
            args = [ev(a) for a in n.args]
            return None if any(a is None for a in args) else _FUN[n.func.id](*args)
        raise ValueError("la fórmula solo admite campos, números, operaciones y comparaciones")

    try:
        return ev(arbol)
    except ZeroDivisionError:
        return None


# cribas de inversores conocidos: filtros para la criba (TradingView) mas una formula de orden
CRIBAS = {
    "graham": ("Valor defensivo (Benjamin Graham): barato, solvente y con dividendo",
               "per < 15, precio_valor_contable < 1.5, ratio_corriente > 2, rentabilidad_dividendo_pct > 0",
               "per * precio_valor_contable", False),
    "lynch": ("Crecimiento a precio razonable (Peter Lynch): PEG bajo",
              "per > 0, per < 25, crecimiento_bpa_pct > 10, deuda_sobre_fondos_propios < 1",
              "per / crecimiento_bpa_pct", False),
    "greenblatt": ("Fórmula mágica (Joel Greenblatt): buenos negocios a buen precio",
                   "roe_pct > 15, per > 0, per < 20, capitalizacion > 1000000000", "roe_pct / per", True),
    "buffett": ("Calidad duradera (Warren Buffett): rentabilidad alta, poca deuda, márgenes amplios",
                "roe_pct > 15, margen_neto_pct > 10, deuda_sobre_fondos_propios < 0.8, capitalizacion > 5000000000",
                "roe_pct + margen_neto_pct", True),
    "dividendo": ("Rentas: dividendo alto y sostenible",
                  "rentabilidad_dividendo_pct > 3.5, per > 0, per < 25, deuda_sobre_fondos_propios < 1.5",
                  "rentabilidad_dividendo_pct", True),
    "impulso": ("Impulso: lo que más sube con volumen",
                "semestre_pct > 15, volumen_relativo > 1, capitalizacion > 2000000000", "semestre_pct", True),
    "pequenas_valor": ("Pequeñas empresas baratas", "capitalizacion < 2000000000, capitalizacion > 100000000, per > 0, "
                       "per < 12, precio_valor_contable < 1.5", "per", False),
}


def criba_preparada(nombre: str) -> dict:
    if nombre not in CRIBAS:
        raise ValueError(f"criba desconocida; disponibles: {', '.join(CRIBAS)}")
    titulo, filtros, formula, desc = CRIBAS[nombre]
    return {"nombre": nombre, "titulo": titulo, "filtros": filtros, "formula": formula, "descendente": desc}


def puntuar(filas: list[dict], formula: str, descendente: bool = True, limite: int = 25) -> list[dict]:
    """Ordena filas por una formula; las que no la pueden calcular van al final."""
    con, sin = [], []
    for f in filas:
        try:
            v = evaluar_formula(formula, f)
        except Exception:  # noqa: BLE001
            v = None
        (con if isinstance(v, (int, float)) and not isinstance(v, bool) else sin).append({**f, "puntuacion": _r(v, 4) if v is not None else None})
    con.sort(key=lambda f: f["puntuacion"], reverse=descendente)
    return (con + sin)[:limite]


# ------------------------------------------------------------------ validacion por tramos
def validar_por_tramos(series: list[dict], params: dict | None = None, tramos: int = 4) -> dict:
    """Repite la prueba historica en tramos sucesivos para ver si la estrategia aguanta fuera del periodo en que se
    eligio (walk-forward). Usa bolsa_lab.backtest en cada tramo."""
    n = min((len(s.get("puntos", [])) for s in series), default=0)
    tramos = max(2, min(int(tramos), 8))
    largo = n // tramos
    if largo < 80:
        raise ValueError("No hay histórico para tantos tramos: hacen falta 80 sesiones por tramo")
    filas = []
    for i in range(tramos):
        trozo = [{"simbolo": s.get("simbolo"), "puntos": s["puntos"][-n:][i * largo:(i + 1) * largo]} for s in series]
        try:
            r = lab.backtest(trozo, {**(params or {}), "fuera_muestra_pct": 0})
            m = r.get("metricas") or r.get("estrategia") or {}
            filas.append({"tramo": i + 1, "desde": trozo[0]["puntos"][0].get("ts"), "hasta": trozo[0]["puntos"][-1].get("ts"),
                          **{k: m.get(k) for k in ("rentabilidad_pct", "sharpe", "caida_maxima_pct", "operaciones")
                             if k in m}, "resumen": {k: v for k, v in r.items() if isinstance(v, (int, float, str))}})
        except Exception as e:  # noqa: BLE001
            filas.append({"tramo": i + 1, "error": str(e)[:160]})
    buenos = [f for f in filas if isinstance(f.get("rentabilidad_pct"), (int, float))]
    positivos = sum(1 for f in buenos if f["rentabilidad_pct"] > 0)
    return {"tramos": filas, "tramos_en_positivo": positivos, "tramos_validos": len(buenos),
            "lectura": ("sin datos suficientes" if not buenos else "consistente" if positivos == len(buenos) else
                        "irregular: gana en unos tramos y pierde en otros" if positivos else "pierde en todos los tramos"),
            "aviso": AVISO}


# ------------------------------------------------------------------ alertas y expedientes (persistentes)
_OPS = {">": operator.gt, ">=": operator.ge, "<": operator.lt, "<=": operator.le, "=": operator.eq, "==": operator.eq,
        "!=": operator.ne}
_mu = threading.Lock()


def _ruta(nombre: str):
    from .. import paths
    return paths.sub("bolsa") / nombre


def _leer(nombre: str, defecto):
    try:
        return json.loads(_ruta(nombre).read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return defecto


def _escribir(nombre: str, datos) -> None:
    p = _ruta(nombre)
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(datos, ensure_ascii=False, indent=1), encoding="utf-8")
    tmp.replace(p)


def alertas() -> list[dict]:
    return _leer("alertas.json", [])


def alerta_guardar(simbolo: str, condiciones: list[dict] | str, nota: str = "", todas: bool = True,
                   repetir_h: float = 24.0) -> dict:
    """Alerta con una o varias condiciones {campo, op, valor} sobre precio, cambio_pct, volumen, rsi, sma20,
    sma50, sma200, distancia_maximo_pct. Tambien admite texto: «precio > 200, rsi < 30»."""
    if isinstance(condiciones, str):
        from .mercados import leer_filtros
        condiciones = leer_filtros(condiciones)
    conds = [{"campo": str(c["campo"]), "op": str(c.get("op", ">")), "valor": float(c["valor"])}
             for c in condiciones if c.get("campo") and c.get("op", ">") in _OPS and c.get("valor") is not None]
    if not simbolo or not conds:
        raise ValueError("Una alerta necesita símbolo y al menos una condición (campo, op, valor)")
    with _mu:
        todas_ = alertas()
        a = {"id": (max((x["id"] for x in todas_), default=0) + 1), "simbolo": simbolo.strip().upper(),
             "condiciones": conds, "todas": bool(todas), "nota": nota[:300], "creada": time.time(), "activa": True,
             "repetir_h": float(repetir_h), "ultima": 0.0, "veces": 0}
        todas_.append(a)
        _escribir("alertas.json", todas_)
    return a


def alerta_borrar(aid: int) -> bool:
    with _mu:
        todas_ = alertas()
        quedan = [a for a in todas_ if a["id"] != int(aid)]
        _escribir("alertas.json", quedan)
    return len(quedan) != len(todas_)


def _campos_alerta(sym: str, m) -> dict:
    q = m.cotizacion(sym)
    campos = {"precio": q.get("precio"), "cambio_pct": q.get("cambio_pct"), "volumen": q.get("volumen")}
    try:
        x = [float(p["precio"]) for p in m.velas(sym, "1y") if p.get("precio")]
        campos.update(rsi=lab.rsi(x), sma20=lab.sma(x, 20), sma50=lab.sma(x, 50), sma200=lab.sma(x, 200))
        if x and campos["precio"]:
            campos["distancia_maximo_pct"] = (campos["precio"] / max(x) - 1) * 100
    except Exception:  # noqa: BLE001
        pass
    return campos


def alertas_evaluar(m=None, ahora: float | None = None) -> list[dict]:
    """Revisa las alertas activas y devuelve las que saltan ahora (y las marca para no repetirlas enseguida)."""
    if m is None:
        from . import mercados
        m = mercados.get()
    ahora = ahora or time.time()
    saltan, cache = [], {}
    with _mu:
        todas_ = alertas()
    for a in todas_:
        if not a.get("activa") or ahora - float(a.get("ultima") or 0) < float(a.get("repetir_h") or 24) * 3600:
            continue
        sym = a["simbolo"]
        try:
            campos = cache.get(sym) or cache.setdefault(sym, _campos_alerta(sym, m))
        except Exception:  # noqa: BLE001
            continue
        res = []
        for c in a["condiciones"]:
            v = campos.get(c["campo"])
            res.append(v is not None and _OPS[c["op"]](float(v), c["valor"]))
        if res and (all(res) if a.get("todas", True) else any(res)):
            a.update(ultima=ahora, veces=int(a.get("veces") or 0) + 1)
            saltan.append({**a, "valores": {c["campo"]: _r(campos.get(c["campo"])) for c in a["condiciones"]}})
    if saltan:
        with _mu:
            actuales = {x["id"]: x for x in alertas()}
            for s in saltan:
                if s["id"] in actuales:
                    actuales[s["id"]].update(ultima=s["ultima"], veces=s["veces"])
            _escribir("alertas.json", list(actuales.values()))
    return saltan


def fuente_alertas(ctx) -> list[dict]:
    """Fuente del servicio Mundo: cada alerta que salta queda como registro de la categoria mercados."""
    from .mercados import pais_de_simbolo
    out = []
    for a in alertas_evaluar():
        cond = ", ".join(f"{c['campo']} {c['op']} {c['valor']:g}" for c in a["condiciones"])
        out.append({"uid": f"alerta-{a['id']}-{int(a['ultima'])}", "pais": pais_de_simbolo(a["simbolo"]),
                    "ts": a["ultima"], "valor": 3, "titulo": f"Alerta de {a['simbolo']}: {cond}",
                    "detalle": (a.get("nota") or "") + " Valores: " + ", ".join(f"{k} {v}" for k, v in a["valores"].items()),
                    "url": "", "lat": None, "lon": None, "datos": a})
    return out


def expediente(simbolo: str) -> dict:
    sym = simbolo.strip().upper()
    return _leer("expedientes.json", {}).get(sym) or {"simbolo": sym, "tesis": "", "supuestos": [], "evidencias": [],
                                                      "riesgos": [], "revisiones": [], "creado": None}


def expedientes() -> list[dict]:
    return [{"simbolo": k, "tesis": (v.get("tesis") or "")[:160], "evidencias": len(v.get("evidencias") or []),
             "actualizado": v.get("actualizado")} for k, v in sorted(_leer("expedientes.json", {}).items())]


def expediente_guardar(simbolo: str, tesis: str | None = None, supuesto: str | None = None,
                       evidencia: dict | str | None = None, riesgo_: str | None = None,
                       revision: str | None = None) -> dict:
    """Guarda o amplia el expediente de un valor: tesis de inversion, supuestos que la sostienen, evidencias con su
    fuente y fecha, riesgos y revisiones. Nada se borra: una tesis nueva deja la anterior en las revisiones."""
    sym = simbolo.strip().upper()
    if not sym:
        raise ValueError("Falta el símbolo")
    ahora = time.time()
    with _mu:
        todos = _leer("expedientes.json", {})
        e = todos.get(sym) or {"simbolo": sym, "tesis": "", "supuestos": [], "evidencias": [], "riesgos": [],
                               "revisiones": [], "creado": ahora}
        if tesis is not None and tesis.strip() and tesis.strip() != e["tesis"]:
            if e["tesis"]:
                e["revisiones"].append({"ts": ahora, "texto": "Tesis anterior: " + e["tesis"]})
            e["tesis"] = tesis.strip()[:2000]
        if supuesto:
            e["supuestos"].append({"ts": ahora, "texto": supuesto.strip()[:500], "vigente": True})
        if evidencia:
            ev = {"texto": evidencia} if isinstance(evidencia, str) else dict(evidencia)
            e["evidencias"].append({"ts": ahora, "texto": str(ev.get("texto", ""))[:800],
                                    "fuente": str(ev.get("fuente", ""))[:200], "url": str(ev.get("url", ""))[:400],
                                    "a_favor": bool(ev.get("a_favor", True))})
        if riesgo_:
            e["riesgos"].append({"ts": ahora, "texto": riesgo_.strip()[:500]})
        if revision:
            e["revisiones"].append({"ts": ahora, "texto": revision.strip()[:800]})
        e["actualizado"] = ahora
        todos[sym] = e
        _escribir("expedientes.json", todos)
    return e
