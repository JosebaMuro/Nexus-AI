"""Laboratorio de la mesa de bolsa: lo que se hace antes de arriesgar dinero.

  - senales: cinco estrategias tecnicas con confianza (tendencia, reversion, momento, volatilidad, estadistica)
  - backtest: reproduce dia a dia una estrategia con comisiones y deslizamiento, frente a comprar y mantener, con
    tramo fuera de muestra
  - metricas: rentabilidad, Sharpe, Sortino, caida maxima, Calmar, VaR
  - optimizar: pesos de cartera (igual, inversa de la volatilidad, varianza minima, paridad de riesgo)
  - montecarlo: remuestreo de rentabilidades para ver el abanico de resultados
  - mesa de analistas: tecnico, noticias (opcional), debate alcista/bajista, riesgo y decision; con IA si hay un
    modelo cargado y con reglas si no. Nunca ordena nada: solo informa.

Todo es calculo puro sobre series ({ts, precio, maximo?, minimo?}); se prueba con datos sinteticos.
"""
from __future__ import annotations

import json
import math
import random
import re
import statistics
import time

ESTRATEGIAS = ("senales", "cruce_medias", "comprar_mantener")


# ------------------------------------------------------------------ indicadores
def sma(x: list[float], n: int) -> float | None:
    return sum(x[-n:]) / n if len(x) >= n and n > 0 else None


def ema(x: list[float], n: int) -> float | None:
    if len(x) < n or n <= 0:
        return None
    k = 2 / (n + 1)
    e = sum(x[:n]) / n
    for v in x[n:]:
        e = v * k + e * (1 - k)
    return e


def _ema_serie(x: list[float], n: int) -> list[float]:
    k = 2 / (n + 1)
    out, e = [], sum(x[:n]) / n
    for i, v in enumerate(x):
        if i >= n:
            e = v * k + e * (1 - k)
        out.append(e)
    return out


def rsi(x: list[float], n: int = 14) -> float | None:
    if len(x) <= n:
        return None
    sube = baja = 0.0
    for a, b in zip(x[-n - 1:-1], x[-n:]):
        d = b - a
        sube += max(d, 0)
        baja += max(-d, 0)
    if baja == 0:
        return 100.0
    return 100 - 100 / (1 + sube / baja)


def macd(x: list[float]) -> dict | None:
    if len(x) < 35:
        return None
    linea = [a - b for a, b in zip(_ema_serie(x, 12), _ema_serie(x, 26))][25:]
    senal = _ema_serie(linea, 9)[-1]
    return {"macd": linea[-1], "senal": senal, "histograma": linea[-1] - senal}


def bollinger(x: list[float], n: int = 20, k: float = 2.0) -> dict | None:
    if len(x) < n:
        return None
    v = x[-n:]
    media, desv = sum(v) / n, statistics.pstdev(v)
    alta, baja = media + k * desv, media - k * desv
    return {"media": media, "alta": alta, "baja": baja, "pct_b": (x[-1] - baja) / (alta - baja) if alta > baja else 0.5}


def rentabilidades(x: list[float]) -> list[float]:
    return [b / a - 1 for a, b in zip(x, x[1:]) if a > 0]


def hurst(x: list[float], max_lag: int = 20) -> float | None:
    """Exponente de Hurst por la varianza de las diferencias: <0,5 revierte a la media, >0,5 tiene tendencia."""
    if len(x) < max_lag * 2:
        return None
    lx, ly = [], []
    for lag in range(2, max_lag):
        difs = [x[i + lag] - x[i] for i in range(len(x) - lag)]
        s = statistics.pstdev(difs)
        if s > 0:
            lx.append(math.log(lag))
            ly.append(math.log(s))
    if len(lx) < 3:
        return None
    mx, my = sum(lx) / len(lx), sum(ly) / len(ly)
    den = sum((a - mx) ** 2 for a in lx)
    return sum((a - mx) * (b - my) for a, b in zip(lx, ly)) / den if den else None


def _acotar(v: float) -> float:
    return max(-1.0, min(1.0, v))


def senales_precios(x: list[float]) -> dict | None:
    """Conjunto de estrategias sobre una lista de cierres. None si la serie es corta (menos de 60 puntos)."""
    if len(x) < 60:
        return None
    est = {}
    e8, e21, e55 = ema(x, 8), ema(x, 21), ema(x, 55)
    t = (1 if e8 > e21 else -1) * 0.5 + (1 if e21 > e55 else -1) * 0.5
    est["tendencia"] = {"senal": t, "confianza": min(1.0, abs(e8 - e55) / e55 * 12),
                        "detalle": f"EMA 8/21/55: {e8:.2f} / {e21:.2f} / {e55:.2f}"}
    r, bb = rsi(x), bollinger(x)
    rev = 0.0
    if r < 30 or bb["pct_b"] < 0.05:
        rev = 1.0
    elif r > 70 or bb["pct_b"] > 0.95:
        rev = -1.0
    est["reversion"] = {"senal": rev, "confianza": min(1.0, abs(r - 50) / 35),
                        "detalle": f"RSI {r:.0f}, posición en Bollinger {bb['pct_b'] * 100:.0f} %"}
    m1, m3 = x[-1] / x[-21] - 1, x[-1] / x[-60] - 1
    mc = macd(x)
    mom = _acotar((1 if m1 > 0 else -1) * 0.35 + (1 if m3 > 0 else -1) * 0.35 + (1 if mc["histograma"] > 0 else -1) * 0.3)
    est["momento"] = {"senal": mom, "confianza": min(1.0, (abs(m1) + abs(m3)) * 4),
                      "detalle": f"1 mes {m1 * 100:+.1f} %, 3 meses {m3 * 100:+.1f} %, MACD {mc['histograma']:+.3f}"}
    rr = rentabilidades(x)
    v_corta, v_larga = statistics.pstdev(rr[-10:]), statistics.pstdev(rr[-60:])
    regimen = v_corta / v_larga if v_larga else 1.0
    vol = 0.5 if regimen < 0.8 else (-0.5 if regimen > 1.3 else 0.0)
    est["volatilidad"] = {"senal": vol, "confianza": min(1.0, abs(regimen - 1)),
                          "detalle": f"volatilidad reciente {regimen:.2f} veces la habitual"}
    h = hurst(x)
    sest = 0.0
    if h is not None:
        if h > 0.55:
            sest = t * 0.6          # hay tendencia: se sigue la tendencia
        elif h < 0.45:
            sest = rev * 0.6        # revierte a la media: cuenta la sobrecompra o sobreventa
    est["estadistica"] = {"senal": sest, "confianza": min(1.0, abs((h or 0.5) - 0.5) * 4),
                          "detalle": f"Hurst {h:.2f}" if h is not None else "serie corta para Hurst"}
    pesos = {"tendencia": 0.3, "reversion": 0.15, "momento": 0.3, "volatilidad": 0.1, "estadistica": 0.15}
    comb = sum(est[k]["senal"] * pesos[k] * (0.5 + est[k]["confianza"] / 2) for k in pesos)
    conf = sum(est[k]["confianza"] * pesos[k] for k in pesos)
    for e in est.values():
        e["senal"], e["confianza"] = round(e["senal"], 2), round(e["confianza"], 2)
    return {"estrategias": est, "combinada": round(_acotar(comb), 3), "confianza": round(conf, 2),
            "lectura": "alcista" if comb > 0.2 else ("bajista" if comb < -0.2 else "neutral")}


def senales(puntos: list[dict]) -> dict | None:
    return senales_precios([float(p["precio"]) for p in puntos if p.get("precio")])


# ------------------------------------------------------------------ metricas
def periodos_anio(ts: list[float]) -> float:
    if len(ts) < 3:
        return 252.0
    paso = statistics.median(b - a for a, b in zip(ts, ts[1:]))
    if paso <= 0:
        return 252.0
    if paso < 3600 * 12:            # intradia: sesiones de unas 8 horas
        return 252 * max(1.0, 8 * 3600 / paso)
    return 252.0 if paso < 86400 * 4 else (52.0 if paso < 86400 * 20 else 12.0)


def metricas(valores: list[float], periodos: float = 252) -> dict:
    if len(valores) < 3 or valores[0] <= 0:
        return {}
    r = rentabilidades(valores)
    total = valores[-1] / valores[0] - 1
    anios = len(r) / periodos
    media, desv = sum(r) / len(r), statistics.pstdev(r)
    malas = [min(0.0, v) for v in r]
    desv_baja = math.sqrt(sum(v * v for v in malas) / len(r))
    pico, caida = valores[0], 0.0
    for v in valores:
        pico = max(pico, v)
        caida = min(caida, v / pico - 1)
    anual = (1 + total) ** (1 / anios) - 1 if anios > 0 and total > -1 else total
    orden = sorted(r)
    var95 = orden[max(0, int(len(orden) * 0.05) - 1)] if len(orden) >= 20 else None
    return {"rentabilidad_pct": round(total * 100, 2), "anual_pct": round(anual * 100, 2),
            "volatilidad_pct": round(desv * math.sqrt(periodos) * 100, 2),
            "sharpe": round(media / desv * math.sqrt(periodos), 2) if desv else 0.0,
            "sortino": round(media / desv_baja * math.sqrt(periodos), 2) if desv_baja else 0.0,
            "caida_maxima_pct": round(caida * 100, 2),
            "calmar": round(anual / abs(caida), 2) if caida else 0.0,
            "var95_pct": round(var95 * 100, 2) if var95 is not None else None,
            "periodos": len(r)}


def metricas_operaciones(ganancias: list[float]) -> dict:
    if not ganancias:
        return {"operaciones": 0}
    buenas, malas = [g for g in ganancias if g > 0], [g for g in ganancias if g <= 0]
    return {"operaciones": len(ganancias), "aciertos_pct": round(len(buenas) / len(ganancias) * 100, 1),
            "factor_beneficio": round(sum(buenas) / abs(sum(malas)), 2) if malas and sum(malas) else None,
            "esperanza": round(sum(ganancias) / len(ganancias), 2),
            "mejor": round(max(ganancias), 2), "peor": round(min(ganancias), 2)}


# ------------------------------------------------------------------ backtest
def _objetivo(estrategia: str, x: list[float], p: dict) -> float | None:
    """Exposicion deseada (0..1) con los cierres hasta hoy; None = no cambiar."""
    if estrategia == "comprar_mantener":
        return 1.0
    if estrategia == "cruce_medias":
        rapida, lenta = sma(x, int(p.get("rapida", 20))), sma(x, int(p.get("lenta", 50)))
        if rapida is None or lenta is None:
            return None
        return 1.0 if rapida > lenta else 0.0
    s = senales_precios(x[-260:])
    if s is None:
        return None
    umbral = float(p.get("umbral", 0.2))
    if s["combinada"] > umbral:
        return 1.0
    if s["combinada"] < -umbral:
        return 0.0
    return None


def backtest(series: list[dict], params: dict | None = None) -> dict:
    """series: [{simbolo, puntos: [{ts, precio}]}]. El capital se reparte a partes iguales entre los valores y cada
    uno sigue la estrategia por su cuenta; la orden se ejecuta al cierre siguiente (sin mirar al futuro)."""
    p = params or {}
    estrategia = p.get("estrategia", "senales")
    if estrategia not in ESTRATEGIAS:
        raise ValueError(f"estrategia debe ser una de {ESTRATEGIAS}")
    capital = float(p.get("capital", 10000))
    com_pct, com_min = float(p.get("comision_pct", 0.1)) / 100, float(p.get("comision_min_eur", 1.0))
    desliz = float(p.get("deslizamiento_bps", 5)) / 10000
    fuera = max(0.0, min(float(p.get("fuera_muestra_pct", 30)), 80)) / 100
    limpias = []
    for s in series:
        pts = [(float(q["ts"]), float(q["precio"])) for q in s.get("puntos", []) if q.get("precio")]
        if len(pts) >= 80:
            limpias.append((s.get("simbolo", "?"), pts))
    if not limpias:
        raise ValueError("No hay histórico suficiente: hacen falta 80 sesiones o más de cada valor. Prueba el rango de "
                         "1 año con conexión (el histórico se guarda para usarlo después sin internet)")
    n = min(len(pts) for _, pts in limpias)
    limpias = [(sym, pts[-n:]) for sym, pts in limpias]
    ts = [t for t, _ in limpias[0][1]]
    parte = capital / len(limpias)

    def simular(estr: str) -> tuple[list[float], list[dict], float]:
        curva = [0.0] * n
        ops: list[dict] = []
        gastos = 0.0
        for sym, pts in limpias:
            precios = [q for _, q in pts]
            efectivo, titulos, coste, pendiente = parte, 0.0, 0.0, None
            for i in range(n):
                px = precios[i]
                if pendiente is not None:
                    if pendiente > 0 and titulos == 0:
                        pc = px * (1 + desliz)
                        com = max(com_min, efectivo * com_pct)
                        titulos = max(0.0, (efectivo - com) / pc)
                        coste, gastos, efectivo = efectivo, gastos + com, 0.0
                        ops.append({"ts": ts[i], "simbolo": sym, "lado": "compra", "precio": round(pc, 4)})
                    elif pendiente == 0 and titulos > 0:
                        pv = px * (1 - desliz)
                        bruto = titulos * pv
                        com = max(com_min, bruto * com_pct)
                        efectivo, gastos = bruto - com, gastos + com
                        ops.append({"ts": ts[i], "simbolo": sym, "lado": "venta", "precio": round(pv, 4),
                                    "ganancia": round(efectivo - coste, 2)})
                        titulos = 0.0
                    pendiente = None
                curva[i] += efectivo + titulos * px
                if i < n - 1:
                    obj = _objetivo(estr, precios[:i + 1], p)
                    if obj is not None and ((obj > 0) != (titulos > 0)):
                        pendiente = obj
        return curva, ops, gastos

    curva, ops, gastos = simular(estrategia)
    ref, _, _ = simular("comprar_mantener")
    per = periodos_anio(ts)
    corte = int(n * (1 - fuera)) if fuera else n
    ganancias = [o["ganancia"] for o in ops if "ganancia" in o]
    paso = max(1, n // 240)
    out = {"estrategia": estrategia, "simbolos": [sym for sym, _ in limpias], "puntos": n, "capital": capital,
           "desde": ts[0], "hasta": ts[-1], "valor_final": round(curva[-1], 2), "gastos": round(gastos, 2),
           "metricas": metricas(curva, per), "comprar_mantener": metricas(ref, per),
           "operaciones": metricas_operaciones(ganancias), "ultimas_operaciones": ops[-12:],
           "curva": [{"ts": ts[i], "valor": round(curva[i], 2), "referencia": round(ref[i], 2)}
                     for i in range(0, n, paso)],
           "aviso": "Resultados pasados con datos históricos: no garantizan resultados futuros."}
    if fuera and 30 <= corte < n - 20:
        out["en_muestra"] = metricas(curva[:corte], per)
        out["fuera_muestra"] = metricas(curva[corte - 1:], per)
    m, r = out["metricas"], out["comprar_mantener"]
    if m and r:
        out["veredicto"] = ("La estrategia bate a comprar y mantener" if m["rentabilidad_pct"] > r["rentabilidad_pct"]
                            else "Comprar y mantener habría rendido más") + \
            (" y con menos caída." if abs(m["caida_maxima_pct"]) < abs(r["caida_maxima_pct"]) else
             ", aunque con más caída." if m["rentabilidad_pct"] > r["rentabilidad_pct"] else
             ", aunque la estrategia cae menos." if abs(m["caida_maxima_pct"]) < abs(r["caida_maxima_pct"]) else ".")
    return out


def montecarlo(valores: list[float], simulaciones: int = 500, horizonte: int = 252, semilla: int | None = None) -> dict:
    """Remuestrea las rentabilidades (con reposicion) para ver el abanico de resultados a 'horizonte' periodos."""
    r = rentabilidades(valores)
    if len(r) < 30:
        raise ValueError("Hacen falta al menos 30 periodos para la simulación")
    rnd = random.Random(semilla)
    simulaciones, horizonte = max(50, min(simulaciones, 5000)), max(5, min(horizonte, 1260))
    finales, peores = [], []
    for _ in range(simulaciones):
        v, pico, caida = 1.0, 1.0, 0.0
        for _ in range(horizonte):
            v *= 1 + rnd.choice(r)
            pico = max(pico, v)
            caida = min(caida, v / pico - 1)
        finales.append(v - 1)
        peores.append(caida)
    finales.sort()
    peores.sort()

    def pct(lista, q):
        return round(lista[min(len(lista) - 1, int(len(lista) * q))] * 100, 2)

    return {"simulaciones": simulaciones, "horizonte": horizonte, "mediana_pct": pct(finales, 0.5),
            "p5_pct": pct(finales, 0.05), "p25_pct": pct(finales, 0.25), "p75_pct": pct(finales, 0.75),
            "p95_pct": pct(finales, 0.95),
            "prob_perder_pct": round(sum(1 for f in finales if f < 0) / simulaciones * 100, 1),
            "caida_maxima_mediana_pct": pct(peores, 0.5), "caida_maxima_p5_pct": pct(peores, 0.05)}


def optimizar(series: list[dict], metodo: str = "paridad_riesgo", params: dict | None = None) -> dict:
    """Pesos de cartera. metodo: igual | inversa_vol | min_varianza | paridad_riesgo."""
    import numpy as np
    p = params or {}
    nombres, rets = [], []
    for s in series:
        x = [float(q["precio"]) for q in s.get("puntos", []) if q.get("precio")]
        if len(x) >= 40:
            nombres.append(s.get("simbolo", "?"))
            rets.append(rentabilidades(x))
    if len(nombres) < 2:
        raise ValueError("Hacen falta al menos dos valores con 40 puntos o más")
    n = min(len(r) for r in rets)
    R = np.array([r[-n:] for r in rets])
    cov = np.cov(R) + np.eye(len(nombres)) * 1e-10
    vol = np.sqrt(np.diag(cov))
    if metodo == "igual":
        w = np.ones(len(nombres))
    elif metodo == "inversa_vol":
        w = 1 / vol
    elif metodo == "min_varianza":
        w = np.linalg.solve(cov, np.ones(len(nombres)))
        w = np.clip(w, 0, None)   # solo posiciones largas
        if w.sum() <= 0:
            w = 1 / vol
    elif metodo == "paridad_riesgo":
        w = 1 / vol
        for _ in range(200):   # cada valor aporta el mismo riesgo
            aporta = w * (cov @ w)
            w = w * np.sqrt(aporta.mean() / np.maximum(aporta, 1e-18))
            w = w / w.sum()
    else:
        raise ValueError("metodo debe ser igual, inversa_vol, min_varianza o paridad_riesgo")
    w = w / w.sum()
    tope = float(p.get("max_peso_pct", 100)) / 100
    if tope * len(nombres) >= 1:
        for _ in range(20):   # reparte lo que pase del tope entre los demas
            sobra = np.clip(w - tope, 0, None).sum()
            if sobra < 1e-9:
                break
            w = np.minimum(w, tope)
            libres = w < tope - 1e-12
            if not libres.any():
                break
            w[libres] += sobra * w[libres] / w[libres].sum()
    per = 252.0
    riesgo = float(np.sqrt(w @ cov @ w) * math.sqrt(per))
    rent = float((R.mean(axis=1) @ w) * per)
    out = {"metodo": metodo, "pesos": {s: round(float(v) * 100, 2) for s, v in zip(nombres, w)},
           "volatilidad_anual_pct": round(riesgo * 100, 2), "rentabilidad_anual_pct": round(rent * 100, 2),
           "sharpe": round(rent / riesgo, 2) if riesgo else 0.0, "periodos": n}
    capital = float(p.get("capital", 0) or 0)
    if capital > 0:
        out["importes"] = {s: round(capital * float(v), 2) for s, v in zip(nombres, w)}
    return out


# ------------------------------------------------------------------ mesa de analistas
_POS = ("sube", "subida", "récord", "record", "beneficio", "gana", "supera", "mejora", "alza", "compra", "crece",
        "rally", "surge", "beats", "upgrade", "profit", "growth", "gain", "bullish", "dividendo")
_NEG = ("cae", "caída", "caida", "pérdida", "perdida", "pierde", "desplome", "recorta", "demanda", "multa", "quiebra",
        "baja", "fraude", "falls", "drop", "loss", "downgrade", "lawsuit", "bearish", "cuts", "despidos")


def sentimiento(texto: str) -> float:
    """-1..1 con un lexico financiero en espanol e ingles."""
    t = (texto or "").lower()
    pos, neg = sum(t.count(w) for w in _POS), sum(t.count(w) for w in _NEG)
    return round((pos - neg) / (pos + neg), 2) if pos + neg else 0.0


def extraer_json(respuesta: str) -> dict | None:
    """El ultimo objeto {...} valido de una respuesta de modelo."""
    for m in reversed(list(re.finditer(r"\{[^{}]*\}", respuesta or "", re.S))):
        try:
            j = json.loads(m.group(0))
            if isinstance(j, dict):
                return j
        except ValueError:
            continue
    return None


def informe_mesa(simbolo: str, puntos: list[dict], opciones: dict | None = None, llm=None,
                 titulares: list[str] | None = None) -> dict:
    """Mesa de analistas sobre un valor. llm(prompt) -> texto; None = todos los papeles con reglas."""
    op = opciones or {}
    x = [float(p["precio"]) for p in puntos if p.get("precio")]
    s = senales_precios(x)
    if s is None:
        return {"error": f"No hay histórico suficiente de {simbolo} (hacen falta 60 sesiones): prueba con conexión "
                         "o con el rango de 1 año"}
    riesgo_pct, max_peso = float(op.get("riesgo_pct", 1.0)), float(op.get("max_peso_pct", 20.0))
    rr = rentabilidades(x)
    vol_anual = statistics.pstdev(rr[-60:]) * math.sqrt(252) * 100
    pico = max(x[-120:])
    caida = (x[-1] / pico - 1) * 100
    sent = sentimiento(" ".join(titulares or []))
    traza = []

    def preguntar(papel: str, prompt: str) -> str:
        if llm is None:
            return ""
        try:
            txt = (llm(prompt) or "").strip()
        except Exception as e:  # noqa: BLE001
            txt = ""
            traza.append({"papel": papel, "error": str(e)})
        if txt:
            traza.append({"papel": papel, "ia": True})
        return txt[:1500]

    est = s["estrategias"]
    tecnico = (f"Lectura {s['lectura']} (señal combinada {s['combinada']:+.2f}, confianza {s['confianza']:.2f}). " +
               "; ".join(f"{k}: {v['detalle']}" for k, v in est.items()) + ".")
    noticias = (f"{len(titulares)} titulares recientes, sentimiento {sent:+.2f}." if titulares else
                "Sin titulares disponibles (sin conexión o sin fuente de noticias).")
    datos = (f"Valor: {simbolo}. Precio {x[-1]:.2f}. {tecnico} Volatilidad anual {vol_anual:.0f} %. "
             f"Distancia al máximo de 6 meses: {caida:.1f} %. {noticias}")
    alcista = preguntar("alcista", "Eres el analista alcista de una mesa de inversión. Con estos datos, da en 3 frases "
                        f"los argumentos a favor de comprar.\n{datos}") or \
        "A favor: " + ("; ".join(f"{k} ({v['detalle']})" for k, v in est.items() if v["senal"] > 0) or
                       "no hay señales técnicas a favor ahora mismo") + "."
    bajista = preguntar("bajista", "Eres el analista bajista de una mesa de inversión. Con estos datos, da en 3 frases "
                        f"los riesgos y argumentos para no comprar o vender.\n{datos}") or \
        "En contra: " + ("; ".join(f"{k} ({v['detalle']})" for k, v in est.items() if v["senal"] < 0) or
                         "no hay señales técnicas en contra ahora mismo") + \
        (f"; volatilidad alta ({vol_anual:.0f} % anual)" if vol_anual > 40 else "") + "."
    nota = s["combinada"] + 0.15 * sent
    accion = "comprar" if nota > 0.25 else ("vender" if nota < -0.25 else "mantener")
    # peso: se arriesga riesgo_pct del capital con una parada a 2 desviaciones diarias x raiz(10 sesiones)
    parada = max(2.0, statistics.pstdev(rr[-60:]) * math.sqrt(10) * 2 * 100)
    peso = min(max_peso, riesgo_pct / parada * 100) if accion == "comprar" else 0.0
    perfiles = {"agresivo": round(min(max_peso, peso * 1.5), 1), "neutral": round(peso, 1),
                "conservador": round(peso * 0.5, 1)}
    decision = {"accion": accion, "confianza": round(min(1.0, abs(nota) * 0.8 + s["confianza"] * 0.4), 2),
                "peso_pct": perfiles["neutral"], "parada_pct": round(parada, 1),
                "motivo": f"Señal combinada {s['combinada']:+.2f}" + (f" y sentimiento {sent:+.2f}" if titulares else "") + "."}
    gestor = preguntar("gestor", "Eres el gestor de cartera. Decide sobre el valor con los datos y el debate. "
                       'Responde SOLO con JSON: {"accion": "comprar|mantener|vender", "confianza": 0..1, '
                       f'"peso_pct": 0..{max_peso:.0f}, "motivo": "una frase"}}.\nDatos: {datos}\nAlcista: {alcista}\n'
                       f"Bajista: {bajista}\nPropuesta por reglas: {json.dumps(decision, ensure_ascii=False)}")
    j = extraer_json(gestor) if gestor else None
    if j and j.get("accion") in ("comprar", "mantener", "vender"):
        try:
            decision.update(accion=j["accion"], confianza=round(max(0.0, min(1.0, float(j.get("confianza", decision["confianza"])))), 2),
                            peso_pct=round(max(0.0, min(max_peso, float(j.get("peso_pct", decision["peso_pct"])))), 1),
                            motivo=str(j.get("motivo") or decision["motivo"])[:300])
            if decision["accion"] != "comprar":
                decision["peso_pct"] = 0.0
            decision["con_ia"] = True
        except (TypeError, ValueError):
            pass
    return {"simbolo": simbolo, "ts": time.time(), "precio": x[-1], "con_ia": any(t.get("ia") for t in traza),
            "analistas": {"tecnico": tecnico, "noticias": noticias, "sentimiento": sent, "senales": s},
            "debate": {"alcista": alcista, "bajista": bajista},
            "riesgo": {"volatilidad_anual_pct": round(vol_anual, 1), "distancia_maximo_pct": round(caida, 1),
                       "pesos_por_perfil": perfiles},
            "decision": decision, "traza": traza,
            "aviso": "Informe orientativo: no es asesoramiento financiero y nunca da órdenes por su cuenta."}
