"""Agente de bolsa autonomo: gestiona su propia cartera SIMULADA con los datos reales que recopila.

Nunca usa dinero ni envia ordenes reales: opera en la mesa simulada (``bolsa.Bolsa``) igual que una persona, con
sus comisiones, horquilla, deslizamiento y horarios. Lo que es real son los precios (``bolsa_datos``) cuando hay
conexion; sin ella trabaja con los ultimos precios reales guardados y la simulacion de la mesa.

En cada ciclo (por defecto cada 60 s):

1. Recopila cotizaciones y cierres diarios de su universo de valores y guarda el historico para trabajar sin red.
2. Calcula indicadores por valor: medias de 20 y 50 sesiones, RSI de 14, momento de 20 sesiones ajustado por
   volatilidad y ATR (rango medio) para medir el riesgo.
3. Puntua cada valor con unos pesos que aprende de sus operaciones cerradas (sube el peso de los factores que
   acertaron y baja el de los que fallaron).
4. Gestiona lo que tiene: stop de proteccion vivo en el libro (orden stop de venta), stop que sube con el precio
   (trailing), toma de beneficios al llegar al objetivo y salida si se rompe la tendencia.
5. Abre posiciones nuevas en los mejores valores con la bolsa abierta: arriesga como maximo ``riesgo_pct`` del
   patrimonio por operacion (distancia al stop = 2 ATR), sin pasar de ``max_peso_pct`` por valor ni de
   ``max_posiciones``, y guarda ``reserva_pct`` de liquidez.

Todo queda en su diario (por que compra, vende o espera) y en la curva de patrimonio, que la app dibuja en vivo.
"""
from __future__ import annotations

import json
import math
import threading
import time

from .. import log
from . import bolsa as B

L = log.get("bolsa.agente")
NOMBRE_CARTERA = "Agente autónomo (simulada)"
ORIGEN = "agente-autonomo"
FACTORES = ("tendencia", "momento", "rsi", "volatilidad")
PESOS_INICIALES = {"tendencia": 1.0, "momento": 1.0, "rsi": 0.6, "volatilidad": 0.4}
UMBRAL_COMPRA = 0.9
RSI_MAX_COMPRA = 80          # no compra lo sobrecomprado
ESPERA_RECOMPRA_S = 6 * 3600  # tras vender un valor no lo vuelve a comprar en 6 horas
MIN_PUNTOS = 30

SCHEMA = """
CREATE TABLE IF NOT EXISTS agente_estado (clave TEXT PRIMARY KEY, valor TEXT);
CREATE TABLE IF NOT EXISTS agente_diario (id INTEGER PRIMARY KEY, ts REAL, tipo TEXT, simbolo TEXT, texto TEXT,
    datos TEXT);
CREATE TABLE IF NOT EXISTS agente_valor (ts REAL PRIMARY KEY, total_eur REAL, efectivo_eur REAL);
CREATE TABLE IF NOT EXISTS agente_posiciones (simbolo TEXT PRIMARY KEY, entrada REAL, stop REAL, objetivo REAL,
    atr REAL, orden_stop INTEGER, orden_entrada INTEGER, abierta REAL, factores TEXT);
"""


# ---------------------------------------------------------------- indicadores
def sma(x: list[float], n: int) -> float | None:
    return sum(x[-n:]) / n if len(x) >= n else None


def rsi(x: list[float], n: int = 14) -> float | None:
    if len(x) <= n:
        return None
    gains = losses = 0.0
    for a, b in zip(x[-n - 1:-1], x[-n:]):
        d = b - a
        gains += max(d, 0.0)
        losses += max(-d, 0.0)
    if losses == 0:
        return 100.0
    rs = gains / losses
    return 100 - 100 / (1 + rs)


def atr(pts: list[dict], n: int = 14) -> float | None:
    """Rango verdadero medio; si la serie no trae maximos y minimos, la media del cambio absoluto."""
    if len(pts) <= n:
        return None
    trs = []
    for prev, p in zip(pts[-n - 1:-1], pts[-n:]):
        hi, lo = p.get("maximo", p["precio"]), p.get("minimo", p["precio"])
        trs.append(max(hi - lo, abs(hi - prev["precio"]), abs(lo - prev["precio"])))
    return sum(trs) / n


def volatilidad(x: list[float], n: int = 20) -> float | None:
    if len(x) <= n:
        return None
    rets = [math.log(b / a) for a, b in zip(x[-n - 1:-1], x[-n:]) if a > 0 and b > 0]
    if len(rets) < 2:
        return None
    m = sum(rets) / len(rets)
    return math.sqrt(sum((r - m) ** 2 for r in rets) / (len(rets) - 1))


def analizar(pts: list[dict]) -> dict | None:
    """Indicadores y factores normalizados (-1..1) de una serie diaria."""
    x = [p["precio"] for p in pts if p.get("precio")]
    if len(x) < MIN_PUNTOS:
        return None
    s20, s50 = sma(x, 20), sma(x, min(50, len(x)))
    r, a, v = rsi(x), atr(pts), volatilidad(x)
    last = x[-1]
    roc = last / x[-21] - 1 if len(x) > 21 else 0.0
    vol = v or 0.02
    f_tend = (1.0 if last > s50 else -1.0) * 0.5 + (0.5 if s20 > s50 else -0.5)
    f_mom = max(-1.0, min(1.0, roc / (vol * math.sqrt(20) * 1.5)))
    f_rsi = 0.0 if r is None else (1.0 if 45 <= r <= 68 else -1.0 if r >= 78 or r <= 25 else
                                   0.3 if r < 45 else -0.3)
    f_vol = max(-1.0, min(1.0, (0.025 - vol) / 0.02))   # premia lo menos volatil
    return {"precio": last, "sma20": s20, "sma50": s50, "rsi": r, "atr": a or last * vol, "vol_diaria": vol,
            "roc20": roc, "factores": {"tendencia": f_tend, "momento": f_mom, "rsi": f_rsi, "volatilidad": f_vol}}


def puntuar(an: dict, pesos: dict) -> float:
    return sum(pesos.get(k, 0) * an["factores"][k] for k in FACTORES)


class AgenteBolsa:
    """Gestor autonomo de una cartera simulada."""

    def __init__(self, mesa: B.Bolsa, ajustes=None) -> None:
        self.b = mesa
        self.ajustes = ajustes or (lambda: {})
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self.ultimo_ciclo = 0.0
        self.ultimo_error = ""
        self.listeners: list = []
        with self.b._lock, self.b.con:
            self.b.con.executescript(SCHEMA)

    # ------------------------------------------------------------ estado persistente
    def _get(self, k: str, default=None):
        r = self.b._one("SELECT valor FROM agente_estado WHERE clave=?", (k,))
        return json.loads(r["valor"]) if r else default

    def _put(self, k: str, v) -> None:
        with self.b._lock, self.b.con:
            self.b.con.execute("INSERT OR REPLACE INTO agente_estado VALUES (?,?)", (k, json.dumps(v)))

    def cfg(self) -> dict:
        d = {"activo": True, "intervalo_s": 60, "capital": 10000.0, "riesgo_pct": 1.0, "max_posiciones": 8,
             "max_peso_pct": 20.0, "reserva_pct": 10.0, "universo": []}
        d.update({k: v for k, v in (self.ajustes() or {}).items() if v not in (None, "")})
        return d

    def cartera(self) -> int:
        cid = self._get("cartera")
        if cid and self.b._one("SELECT id FROM carteras WHERE id=? AND modo='simulado'", (cid,)):
            return cid
        c = self.b.crear_cartera(NOMBRE_CARTERA, float(self.cfg()["capital"]), "broker-online", "simulado")
        self._put("cartera", c["id"])
        self.diario("inicio", "", f"Cartera simulada creada con {c['capital_inicial']:.2f} € para operar sola.")
        return c["id"]

    def pesos(self) -> dict:
        return {**PESOS_INICIALES, **(self._get("pesos") or {})}

    def universo(self) -> list[str]:
        u = [s.upper() for s in self.cfg().get("universo") or []]
        return u or [i["simbolo"] for i in self.b.instrumentos("")]

    def diario(self, tipo: str, simbolo: str, texto: str, datos: dict | None = None) -> None:
        with self.b._lock, self.b.con:
            self.b.con.execute("INSERT INTO agente_diario (ts, tipo, simbolo, texto, datos) VALUES (?,?,?,?,?)",
                               (time.time(), tipo, simbolo, texto, json.dumps(datos or {}, default=float)))
            self.b.con.execute("DELETE FROM agente_diario WHERE id < (SELECT MAX(id) - 2000 FROM agente_diario)")
        L.info("Agente de bolsa [%s] %s %s", tipo, simbolo, texto)
        for fn in list(self.listeners):
            try:
                fn({"type": "bolsa_agente", "tipo": tipo, "simbolo": simbolo, "texto": texto})
            except Exception:   # noqa: BLE001
                pass

    # ------------------------------------------------------------ datos
    def recopilar(self, sym: str) -> dict | None:
        serie = self.b.serie(sym, "6mo")
        pts = serie["puntos"]
        if len(pts) < MIN_PUNTOS:   # sin historico diario (primer arranque sin red): ticks de la mesa
            pts = self.b.serie(sym, "1d")["puntos"]
        if len(pts) < MIN_PUNTOS:   # ni eso: historia simulada coherente con la volatilidad del valor
            pts = self._historia_simulada(sym)
        an = analizar(pts)
        if an:
            q = self.b.precio(sym)
            an["precio"], an["real"], an["abierto"] = q["precio"], q.get("real", False), q["abierto"]
            an["fuente"] = q["fuente"]
        return an

    def _historia_simulada(self, sym: str, n: int = 90) -> list[dict]:
        """120 sesiones simuladas que acaban en el precio actual (solo sin conexion ni historico guardado)."""
        import random
        inst = self.b.instrumento(sym)
        rng = random.Random(f"{sym}-{int(time.time() // 86400)}")
        p, out, now = self.b.precio(sym)["precio"], [], time.time()
        dv = inst["vol"] / math.sqrt(252)
        for i in range(n):
            out.append({"ts": now - i * 86400, "precio": p})
            p = p / math.exp(rng.gauss(0.0002, dv))
        return out[::-1]

    # ------------------------------------------------------------ ciclo
    def ciclo(self) -> dict:
        """Un ciclo completo de decision. Devuelve un resumen de lo que ha hecho."""
        with self._lock:
            cid = self.cartera()
            self.b.procesar()
            self._sincronizar(cid)
            c = self.cfg()
            pesos = self.pesos()
            analisis: dict[str, dict] = {}
            for sym in self.universo():
                try:
                    an = self.recopilar(sym)
                except Exception as e:   # noqa: BLE001 - un valor sin datos no para al resto
                    L.debug("sin datos de %s: %s", sym, e)
                    continue
                if an:
                    an["puntuacion"] = round(puntuar(an, pesos), 3)
                    analisis[sym] = an
            hechos = self._gestionar(cid, analisis) + self._abrir(cid, analisis, c)
            res = self.b.resumen(cid)
            with self.b._lock, self.b.con:
                self.b.con.execute("INSERT OR REPLACE INTO agente_valor VALUES (?,?,?)",
                                   (time.time(), res["total_eur"], res["efectivo_eur"]))
            self._put("ranking", sorted(({"simbolo": s, "puntuacion": a["puntuacion"], "precio": a["precio"],
                                          "rsi": a["rsi"] and round(a["rsi"], 1), "roc20_pct": round(a["roc20"] * 100, 2),
                                          "real": a.get("real", False)} for s, a in analisis.items()),
                                        key=lambda r: -r["puntuacion"])[:25])
            self.ultimo_ciclo = time.time()
            return {"cartera": cid, "acciones": hechos, "valores_analizados": len(analisis),
                    "total_eur": res["total_eur"]}

    def _sincronizar(self, cid: int) -> None:
        """Cierra en el registro las posiciones que ya no estan (salto el stop) y aprende del resultado."""
        held = {p["simbolo"] for p in self.b.posiciones(cid)}
        for p in self.b._q("SELECT * FROM agente_posiciones"):
            if p["simbolo"] in held:
                continue
            ent = p["orden_entrada"] and self.b.orden(p["orden_entrada"])
            if ent and ent["estado"] == "pendiente":
                continue   # la compra aun no se ha ejecutado (bolsa cerrada)
            if ent and ent["estado"] in ("rechazada", "caducada", "cancelada"):
                self._borrar(p["simbolo"])
                continue
            op = self.b._one("SELECT * FROM operaciones WHERE cartera=? AND simbolo=? AND lado='venta' "
                             "ORDER BY ts DESC LIMIT 1", (cid, p["simbolo"]))
            if op:
                self._aprender(p, op["ganancia_eur"] or 0.0)
                self.diario("stop", p["simbolo"], f"Saltó el stop de protección a {op['precio']:.4g}: resultado "
                                                  f"{op['ganancia_eur']:+.2f} € con gastos.", {"orden": op["orden"]})
            self._borrar(p["simbolo"])

    def _borrar(self, sym: str) -> None:
        vend = self._get("vendidos") or {}
        vend[sym] = time.time()
        self._put("vendidos", {k: v for k, v in vend.items() if time.time() - v < ESPERA_RECOMPRA_S})
        with self.b._lock, self.b.con:
            self.b.con.execute("DELETE FROM agente_posiciones WHERE simbolo=?", (sym,))

    def _aprender(self, p: dict, ganancia: float) -> None:
        f = json.loads(p["factores"] or "{}")
        if not f:
            return
        pesos = self.pesos()
        sign = 1.0 if ganancia > 0 else -1.0
        for k in FACTORES:
            pesos[k] = round(max(0.05, min(2.5, pesos[k] + 0.08 * sign * f.get(k, 0))), 4)
        self._put("pesos", pesos)
        stats = self._get("estadisticas") or {"cerradas": 0, "ganadoras": 0, "resultado_eur": 0.0}
        stats["cerradas"] += 1
        stats["ganadoras"] += 1 if ganancia > 0 else 0
        stats["resultado_eur"] = round(stats["resultado_eur"] + ganancia, 2)
        self._put("estadisticas", stats)

    def _vender(self, cid: int, sym: str, qty: float, motivo: str, p: dict) -> str | None:
        if p.get("orden_stop"):
            o = self.b.orden(p["orden_stop"])
            if o["estado"] == "pendiente":
                self.b.cancelar(o["id"])
        try:
            o = self.b.ordenar(sym, "venta", qty, "mercado", cartera=cid, origen=ORIGEN)
        except ValueError as e:
            self.diario("error", sym, f"No pudo vender: {e}")
            return None
        if o["estado"] == "ejecutada":
            op = self.b._one("SELECT ganancia_eur FROM operaciones WHERE orden=?", (o["id"],))
            g = (op or {}).get("ganancia_eur") or 0.0
            self._aprender(p, g)
            self._borrar(sym)
            self.diario("venta", sym, f"{motivo}. Vende {qty:g} a {o['precio_ejec']:.4g}: {g:+.2f} € con gastos.",
                        {"orden": o["id"]})
        else:
            self.diario("venta", sym, f"{motivo}. Orden de venta #{o['id']} en espera: {o.get('nota', '')}",
                        {"orden": o["id"]})
        return f"venta {sym}"

    def _gestionar(self, cid: int, analisis: dict) -> list[str]:
        hechos = []
        pos = {p["simbolo"]: p for p in self.b.posiciones(cid)}
        for p in self.b._q("SELECT * FROM agente_posiciones"):
            sym = p["simbolo"]
            if sym not in pos:
                continue
            qty = pos[sym]["cantidad"]
            an = analisis.get(sym)
            px = an["precio"] if an else self.b.precio(sym)["precio"]
            if p["objetivo"] and px >= p["objetivo"]:
                r = self._vender(cid, sym, qty, f"Objetivo alcanzado ({p['objetivo']:.4g})", p)
                hechos += [r] if r else []
                continue
            if an and an["factores"]["tendencia"] <= -1.0 and an["puntuacion"] < 0:
                r = self._vender(cid, sym, qty, "Tendencia rota (precio y media de 20 bajo la de 50)", p)
                hechos += [r] if r else []
                continue
            a = (an or {}).get("atr") or p["atr"]
            nuevo = px - 2 * a
            if nuevo > p["stop"] * 1.002 and px > p["entrada"]:   # trailing: el stop solo sube
                self._poner_stop(cid, sym, qty, nuevo, p)
                hechos.append(f"stop {sym}")
            elif not p["orden_stop"] or self.b.orden(p["orden_stop"])["estado"] not in ("pendiente", "ejecutada"):
                self._poner_stop(cid, sym, qty, p["stop"], p)   # stop caducado o perdido: se repone
        return hechos

    def _poner_stop(self, cid: int, sym: str, qty: float, stop: float, p: dict) -> None:
        if p.get("orden_stop"):
            o = self.b.orden(p["orden_stop"])
            if o["estado"] == "pendiente":
                self.b.cancelar(o["id"])
        dec = 2 if stop >= 10 else 3 if stop >= 1 else 4
        stop = round(stop, dec)
        try:
            o = self.b.ordenar(sym, "venta", qty, "stop", stop=stop, validez="hasta_cancelar", cartera=cid,
                               origen=ORIGEN)
        except ValueError as e:
            self.diario("error", sym, f"No pudo poner el stop: {e}")
            return
        with self.b._lock, self.b.con:
            self.b.con.execute("UPDATE agente_posiciones SET stop=?, orden_stop=? WHERE simbolo=?", (stop, o["id"], sym))
        if p.get("stop") and stop > p["stop"]:
            self.diario("stop", sym, f"Sube el stop de {p['stop']:.4g} a {stop:.4g} para asegurar beneficio.",
                        {"orden": o["id"]})

    def _abrir(self, cid: int, analisis: dict, c: dict) -> list[str]:
        res = self.b.resumen(cid)
        total, libre = res["total_eur"], res["efectivo_eur"]
        reservado = self.b._one("SELECT COALESCE(SUM(estimado_eur),0) e FROM ordenes WHERE cartera=? AND lado='compra' "
                                "AND estado='pendiente'", (cid,))["e"]
        libre -= reservado + total * float(c["reserva_pct"]) / 100
        abiertas = {p["simbolo"] for p in res["posiciones"]} | {
            r["simbolo"] for r in self.b._q("SELECT simbolo FROM agente_posiciones")}
        huecos = int(c["max_posiciones"]) - len(abiertas)
        vend = self._get("vendidos") or {}
        recientes = {s for s, t in vend.items() if time.time() - t < ESPERA_RECOMPRA_S}
        cands = sorted(((s, a) for s, a in analisis.items()
                        if s not in abiertas and s not in recientes and a["abierto"] and a["puntuacion"] >= UMBRAL_COMPRA
                        and (a["rsi"] is None or a["rsi"] <= RSI_MAX_COMPRA)),
                       key=lambda kv: -kv[1]["puntuacion"])
        hechos = []
        for sym, an in cands[:max(0, huecos)]:
            inst = self.b.instrumento(sym)
            fx = self.b.fx(inst["divisa"])
            px_eur = an["precio"] / fx
            riesgo_eur = total * float(c["riesgo_pct"]) / 100
            dist_eur = max(2 * an["atr"] / fx, px_eur * 0.01)
            qty = min(riesgo_eur / dist_eur, total * float(c["max_peso_pct"]) / 100 / px_eur, libre * 0.97 / px_eur)
            qty = math.floor(qty * 1e4) / 1e4 if inst["mercado"] == "CRIPTO" else math.floor(qty)
            if qty <= 0 or qty * px_eur < 150:   # operaciones minusculas: la comision se come el beneficio
                continue
            try:
                o = self.b.ordenar(sym, "compra", qty, "mercado", cartera=cid, origen=ORIGEN)
            except ValueError as e:
                self.diario("espera", sym, f"No compra: {e}")
                continue
            entrada = o.get("precio_ejec") or an["precio"]
            stop, objetivo = entrada - 2 * an["atr"], entrada + 4 * an["atr"]
            with self.b._lock, self.b.con:
                self.b.con.execute("INSERT OR REPLACE INTO agente_posiciones VALUES (?,?,?,?,?,?,?,?,?)",
                                   (sym, entrada, stop, objetivo, an["atr"], None, o["id"], time.time(),
                                    json.dumps(an["factores"])))
            libre -= qty * px_eur
            motivo = (f"Puntuación {an['puntuacion']:.2f}: precio {'sobre' if an['factores']['tendencia'] > 0 else 'bajo'}"
                      f" su media de 50, momento {an['roc20'] * 100:+.1f} % en 20 sesiones, RSI {an['rsi']:.0f}"
                      if an["rsi"] is not None else f"Puntuación {an['puntuacion']:.2f}")
            if o["estado"] == "ejecutada":
                self.diario("compra", sym, f"Compra {qty:g} a {entrada:.4g}. {motivo}. Stop {stop:.4g}, objetivo "
                                           f"{objetivo:.4g}.", {"orden": o["id"]})
                self._poner_stop(cid, sym, qty, stop, {"stop": 0})
            else:
                self.diario("compra", sym, f"Orden de compra #{o['id']} de {qty:g} en espera. {motivo}.",
                            {"orden": o["id"]})
            hechos.append(f"compra {sym}")
        if not hechos and not cands and analisis:
            mejor = max(analisis.items(), key=lambda kv: kv[1]["puntuacion"])
            ultimo = self._get("ultima_espera", 0)
            if time.time() - ultimo > 1800:
                if mejor[1]["puntuacion"] >= UMBRAL_COMPRA and not mejor[1]["abierto"]:
                    txt = (f"Mercado cerrado: {mejor[0]} tiene la mejor puntuación ({mejor[1]['puntuacion']:.2f}); "
                           "esperará a la apertura para decidir con precios del día.")
                else:
                    txt = (f"Sin oportunidades claras: el mejor es {mejor[0]} con {mejor[1]['puntuacion']:.2f} "
                           f"(compra desde {UMBRAL_COMPRA}).")
                self.diario("espera", "", txt)
                self._put("ultima_espera", time.time())
        return hechos

    # ------------------------------------------------------------ consulta
    def estado(self) -> dict:
        cid = self.cartera()
        c = self.cfg()
        res = self.b.resumen(cid)
        diario = self.b._q("SELECT * FROM agente_diario ORDER BY id DESC LIMIT 60")
        for d in diario:
            d["datos"] = json.loads(d["datos"] or "{}")
        curva = self.b._q("SELECT ts, total_eur FROM agente_valor ORDER BY ts DESC LIMIT 1500")[::-1]
        gest = {p["simbolo"]: p for p in self.b._q("SELECT simbolo, entrada, stop, objetivo, orden_stop "
                                                     "FROM agente_posiciones")}
        for p in res["posiciones"]:
            p.update({k: v for k, v in gest.get(p["simbolo"], {}).items() if k != "simbolo"})
        return {"activo": bool(c["activo"]) and self.vivo(), "configurado": bool(c["activo"]), "ajustes": c,
                "cartera": cid, "resumen": res, "diario": diario, "curva": curva, "pesos": self.pesos(),
                "estadisticas": self._get("estadisticas") or {"cerradas": 0, "ganadoras": 0, "resultado_eur": 0.0},
                "ranking": self._get("ranking") or [], "ultimo_ciclo": self.ultimo_ciclo, "error": self.ultimo_error,
                "datos": self.b.datos.estado() if self.b.datos else {"en_linea": False, "fuentes": {}},
                "aviso": B.AVISO}

    # ------------------------------------------------------------ hilo
    def vivo(self) -> bool:
        return bool(self._thread and self._thread.is_alive())

    def start(self) -> None:
        if self.vivo():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="bolsa-agente", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def _run(self) -> None:
        self._stop.wait(5)   # deja arrancar el servidor
        while not self._stop.is_set():
            if not self.cfg()["activo"]:
                break
            try:
                self.ciclo()
                self.ultimo_error = ""
            except Exception as e:   # noqa: BLE001 - el agente nunca tumba el servidor
                self.ultimo_error = str(e)
                L.warning("Agente de bolsa: ciclo fallido: %s", e)
            self._stop.wait(max(15.0, float(self.cfg()["intervalo_s"])))


_AGENTES: dict[int, AgenteBolsa] = {}
_ALOCK = threading.Lock()


def get() -> AgenteBolsa:
    from ..config import get_config
    mesa = B.get()
    with _ALOCK:
        if id(mesa) not in _AGENTES:
            _AGENTES[id(mesa)] = AgenteBolsa(mesa, lambda: get_config().get("bolsa.agente", {}) or {})
        return _AGENTES[id(mesa)]


def reset() -> None:
    with _ALOCK:
        for a in _AGENTES.values():
            a.stop()
        _AGENTES.clear()
