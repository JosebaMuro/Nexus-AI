"""Mesa de bolsa simulada de la oficina de bolsa e inversiones (perfil ``bolsa-inversiones``).

Permite practicar sin dinero en un entorno que imita uno real, 100 % local (SQLite en ``data/bolsa/bolsa.db``):

* carteras ficticias en euros con capital inicial a elegir (por defecto 10.000 €);
* instrumentos de varios mercados (BME, Xetra, Euronext, Londres, NYSE/Nasdaq y cripto 24/7) con divisa propia;
* precios que se mueven solos mientras el mercado esta abierto (paseo aleatorio geometrico con la volatilidad de cada
  activo y un salto en cada apertura) y se quedan quietos con el mercado cerrado; se pueden anclar a cierres reales
  importando un CSV (``importar_csv``), que ademas recalcula la volatilidad;
* horarios de cada bolsa con su hora local, cambio de hora de verano y festivos (Viernes Santo, Navidad, los de EE. UU.);
* ordenes a mercado, limitadas, stop y stop limitada, validas el dia o hasta cancelar (90 dias), que se ejecutan al
  precio de compra/venta (horquilla) con deslizamiento segun el tamano de la orden frente al volumen diario;
* tarifas de comision tipicas de un broker online, un banco y un neobroker, cambio de divisa con su coste, el
  impuesto sobre transacciones financieras (0,2 % en compras de grandes empresas espanolas, 0,3 % en francesas);
* posiciones por lotes FIFO (el criterio de Hacienda para valores homogeneos), ganancia o perdida realizada en euros y
  aviso de la regla de los dos meses (art. 33.5 LIRPF) cuando se recompra tras vender con perdidas.

Modo real: una cartera ``modo="real"`` nunca ejecuta nada por si misma. Las ordenes quedan «por_confirmar»; solo una
persona puede confirmarlas (``confirmar`` con el texto ``CONFIRMO``, desde la app o la terminal, nunca un agente) y
aun asi Nexus-Office no se conecta a ningun broker: la orden confirmada se ejecuta a mano en el broker y despues se
registra la ejecucion real (``registrar_ejecucion``) para llevar el seguimiento de la cartera.

Los precios de referencia del catalogo son ilustrativos y no son cotizaciones reales. Nada de esto es asesoramiento
financiero.
"""
from __future__ import annotations

import datetime as dt
import math
import random
import re
import sqlite3
import threading
from pathlib import Path

from .. import log, paths

L = log.get("bolsa")

AVISO = ("Simulación: nunca se usa dinero ni se envían órdenes reales. Con conexión, los precios son cotizaciones reales "
         "(Yahoo Finance, con el retraso de cada bolsa); sin conexión siguen simulados desde el último precio real. "
         "No es asesoramiento financiero. Invertir conlleva riesgo de perder dinero; antes de operar con dinero real "
         "consulta a un asesor registrado en la CNMV.")
CONFIRMACION = "CONFIRMO"
UTC = dt.timezone.utc

# ---------------------------------------------------------------- mercados y horarios
# zona: "eu" (CET/CEST), "uk" (GMT/BST), "us" (EST/EDT), "utc" (cripto 24/7)
MERCADOS: dict[str, dict] = {
    "BME": {"nombre": "Bolsa de Madrid (BME)", "zona": "eu", "abre": "09:00", "cierra": "17:30", "festivos": "eu",
            "nacional": True},
    "XETRA": {"nombre": "Xetra (Fráncfort)", "zona": "eu", "abre": "09:00", "cierra": "17:30", "festivos": "eu"},
    "EURONEXT": {"nombre": "Euronext (París, Ámsterdam)", "zona": "eu", "abre": "09:00", "cierra": "17:30",
                 "festivos": "eu"},
    "LSE": {"nombre": "Bolsa de Londres (LSE)", "zona": "uk", "abre": "08:00", "cierra": "16:30", "festivos": "uk"},
    "NYSE": {"nombre": "NYSE (Nueva York)", "zona": "us", "abre": "09:30", "cierra": "16:00", "festivos": "us"},
    "NASDAQ": {"nombre": "Nasdaq (Nueva York)", "zona": "us", "abre": "09:30", "cierra": "16:00", "festivos": "us"},
    "CRIPTO": {"nombre": "Criptoactivos (24/7)", "zona": "utc", "abre": "00:00", "cierra": "24:00", "festivos": "",
               "continuo": True},
}


def _nth_weekday(y: int, m: int, wd: int, n: int) -> dt.date:
    """n-esimo dia de la semana ``wd`` (0=lunes) del mes; n=-1 es el ultimo."""
    if n > 0:
        d = dt.date(y, m, 1)
        d += dt.timedelta(days=(wd - d.weekday()) % 7)
        return d + dt.timedelta(weeks=n - 1)
    d = (dt.date(y, m + 1, 1) if m < 12 else dt.date(y + 1, 1, 1)) - dt.timedelta(days=1)
    return d - dt.timedelta(days=(d.weekday() - wd) % 7)


def easter(y: int) -> dt.date:
    """Domingo de Pascua (algoritmo gregoriano anonimo)."""
    a, b, c = y % 19, y // 100, y % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l_ = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l_) // 451
    month = (h + l_ - 7 * m + 114) // 31
    return dt.date(y, month, (h + l_ - 7 * m + 114) % 31 + 1)


def _observed(d: dt.date) -> dt.date:
    """Festivo que cae en fin de semana: se cierra el viernes anterior (sabado) o el lunes siguiente (domingo)."""
    return {5: d - dt.timedelta(days=1), 6: d + dt.timedelta(days=1)}.get(d.weekday(), d)


def holidays(kind: str, y: int) -> set[dt.date]:
    e = easter(y)
    if kind == "eu":    # BME, Xetra y Euronext cierran los mismos dias (calendario TARGET2 mas Nochebuena y Nochevieja)
        return {dt.date(y, 1, 1), e - dt.timedelta(days=2), e + dt.timedelta(days=1), dt.date(y, 5, 1),
                dt.date(y, 12, 24), dt.date(y, 12, 25), dt.date(y, 12, 26), dt.date(y, 12, 31)}
    if kind == "uk":
        out = {e - dt.timedelta(days=2), e + dt.timedelta(days=1), _nth_weekday(y, 5, 0, 1), _nth_weekday(y, 5, 0, -1),
               _nth_weekday(y, 8, 0, -1)}
        ny = dt.date(y, 1, 1)
        out.add(ny + dt.timedelta(days={5: 2, 6: 1}.get(ny.weekday(), 0)))
        xmas, box = dt.date(y, 12, 25), dt.date(y, 12, 26)
        if xmas.weekday() == 5:      # sabado: se pasan al lunes 27 y al martes 28
            out |= {dt.date(y, 12, 27), dt.date(y, 12, 28)}
        elif xmas.weekday() == 6:    # domingo: lunes 26 (San Esteban) y martes 27 (Navidad)
            out |= {box, dt.date(y, 12, 27)}
        else:
            out |= {xmas, box + dt.timedelta(days=2) if box.weekday() == 5 else box}
        return out
    if kind == "us":
        out = {_nth_weekday(y, 1, 0, 3), _nth_weekday(y, 2, 0, 3), e - dt.timedelta(days=2), _nth_weekday(y, 5, 0, -1),
               _observed(dt.date(y, 6, 19)), _observed(dt.date(y, 7, 4)), _nth_weekday(y, 9, 0, 1),
               _nth_weekday(y, 11, 3, 4), _observed(dt.date(y, 12, 25))}
        ny = dt.date(y, 1, 1)
        if ny.weekday() != 5:        # si Año Nuevo cae en sabado la NYSE no cierra el viernes anterior
            out.add(_observed(ny))
        return out
    return set()


def utc_offset(zona: str, t: dt.datetime) -> dt.timedelta:
    """Diferencia con UTC de la zona en el instante ``t`` (UTC), con su horario de verano."""
    y = t.year
    if zona in ("eu", "uk"):     # ultimo domingo de marzo y de octubre a la 01:00 UTC
        start = dt.datetime.combine(_nth_weekday(y, 3, 6, -1), dt.time(1), UTC)
        end = dt.datetime.combine(_nth_weekday(y, 10, 6, -1), dt.time(1), UTC)
        summer = start <= t < end
        return dt.timedelta(hours=(2 if summer else 1) if zona == "eu" else (1 if summer else 0))
    if zona == "us":             # segundo domingo de marzo (07:00 UTC) a primer domingo de noviembre (06:00 UTC)
        start = dt.datetime.combine(_nth_weekday(y, 3, 6, 2), dt.time(7), UTC)
        end = dt.datetime.combine(_nth_weekday(y, 11, 6, 1), dt.time(6), UTC)
        return dt.timedelta(hours=-4 if start <= t < end else -5)
    return dt.timedelta(0)


def _hm(s: str) -> dt.timedelta:
    h, m = s.split(":")
    return dt.timedelta(hours=int(h), minutes=int(m))


def session(market: str, day: dt.date) -> tuple[dt.datetime, dt.datetime] | None:
    """(apertura, cierre) en UTC de la sesion de ``day`` (fecha local) o None si ese dia no abre."""
    m = MERCADOS[market]
    if m.get("continuo"):
        start = dt.datetime.combine(day, dt.time(0), UTC)
        return start, start + dt.timedelta(days=1)
    if day.weekday() >= 5 or day in holidays(m["festivos"], day.year):
        return None
    noon = dt.datetime.combine(day, dt.time(12), UTC)
    off = utc_offset(m["zona"], noon)
    base = dt.datetime.combine(day, dt.time(0), UTC) - off
    return base + _hm(m["abre"]), base + _hm(m["cierra"])


DIAS = ("lun", "mar", "mié", "jue", "vie", "sáb", "dom")


def fmt_local(market: str, t: dt.datetime) -> str:
    lt = local_time(market, t)
    return f"{DIAS[lt.weekday()]} {lt:%d/%m %H:%M}"


def local_time(market: str, t: dt.datetime) -> dt.datetime:
    return (t + utc_offset(MERCADOS[market]["zona"], t)).replace(tzinfo=None)


def is_open(market: str, t: dt.datetime) -> bool:
    s = session(market, local_time(market, t).date())
    return bool(s and s[0] <= t < s[1])


def next_session(market: str, t: dt.datetime) -> tuple[dt.datetime, dt.datetime]:
    """Sesion en curso o la siguiente que abre despues de ``t``."""
    day = local_time(market, t).date()
    for _ in range(15):
        s = session(market, day)
        if s and t < s[1]:
            return s
        day += dt.timedelta(days=1)
    raise RuntimeError(f"sin sesiones en {market}")


def _trading_time(market: str, t0: dt.datetime, t1: dt.datetime) -> tuple[float, int]:
    """Segundos de mercado abierto entre t0 y t1 y numero de aperturas que hay entre medias."""
    if t1 <= t0:
        return 0.0, 0
    if MERCADOS[market].get("continuo"):
        return (t1 - t0).total_seconds(), 0
    t0 = max(t0, t1 - dt.timedelta(days=370))
    secs, opens = 0.0, 0
    day = local_time(market, t0).date() - dt.timedelta(days=1)
    end = local_time(market, t1).date() + dt.timedelta(days=1)
    while day <= end:
        s = session(market, day)
        if s:
            lo, hi = max(s[0], t0), min(s[1], t1)
            if hi > lo:
                secs += (hi - lo).total_seconds()
            if t0 < s[0] <= t1:
                opens += 1
        day += dt.timedelta(days=1)
    return secs, opens


# ---------------------------------------------------------------- catalogo de instrumentos
# precio de referencia ILUSTRATIVO (no es una cotizacion), volatilidad anual, horquilla (pb) y volumen diario medio
_I = [
    ("SAN.MC", "Banco Santander", "BME", "EUR", "acción", 6.0, 0.28, 3, 4.0e7, 0.2),
    ("BBVA.MC", "BBVA", "BME", "EUR", "acción", 10.5, 0.27, 3, 1.5e7, 0.2),
    ("ITX.MC", "Inditex", "BME", "EUR", "acción", 48.0, 0.24, 3, 2.5e6, 0.2),
    ("IBE.MC", "Iberdrola", "BME", "EUR", "acción", 14.0, 0.18, 3, 1.2e7, 0.2),
    ("TEF.MC", "Telefónica", "BME", "EUR", "acción", 4.2, 0.22, 4, 1.6e7, 0.2),
    ("REP.MC", "Repsol", "BME", "EUR", "acción", 13.0, 0.26, 4, 5.0e6, 0.2),
    ("SAP.DE", "SAP", "XETRA", "EUR", "acción", 230.0, 0.26, 3, 1.5e6, 0.0),
    ("SIE.DE", "Siemens", "XETRA", "EUR", "acción", 190.0, 0.24, 3, 1.2e6, 0.0),
    ("MC.PA", "LVMH", "EURONEXT", "EUR", "acción", 600.0, 0.27, 3, 5.0e5, 0.3),
    ("ASML.AS", "ASML", "EURONEXT", "EUR", "acción", 650.0, 0.38, 3, 6.0e5, 0.0),
    ("SHEL.L", "Shell", "LSE", "GBP", "acción", 27.0, 0.24, 4, 6.0e6, 0.0),
    ("AAPL", "Apple", "NASDAQ", "USD", "acción", 220.0, 0.27, 1, 5.0e7, 0.0),
    ("MSFT", "Microsoft", "NASDAQ", "USD", "acción", 420.0, 0.25, 1, 2.0e7, 0.0),
    ("NVDA", "NVIDIA", "NASDAQ", "USD", "acción", 130.0, 0.50, 1, 2.0e8, 0.0),
    ("AMZN", "Amazon", "NASDAQ", "USD", "acción", 190.0, 0.32, 1, 4.0e7, 0.0),
    ("KO", "Coca-Cola", "NYSE", "USD", "acción", 65.0, 0.15, 2, 1.2e7, 0.0),
    ("JNJ", "Johnson & Johnson", "NYSE", "USD", "acción", 155.0, 0.16, 2, 7.0e6, 0.0),
    ("VWCE.DE", "ETF Vanguard FTSE All-World (acumulación)", "XETRA", "EUR", "fondo cotizado (ETF)", 120.0, 0.15, 6,
     3.0e5, 0.0),
    ("IWDA.AS", "ETF iShares Core MSCI World (acumulación)", "EURONEXT", "EUR", "fondo cotizado (ETF)", 95.0, 0.15, 5,
     5.0e5, 0.0),
    ("IBGL.AS", "ETF iShares bonos del Estado zona euro 15-30 años", "EURONEXT", "EUR", "fondo cotizado (ETF)", 160.0,
     0.14, 10, 5.0e4, 0.0),
    ("BTC-EUR", "Bitcoin", "CRIPTO", "EUR", "criptoactivo", 60000.0, 0.60, 8, 2.0e4, 0.0),
    ("ETH-EUR", "Ether", "CRIPTO", "EUR", "criptoactivo", 2500.0, 0.75, 10, 3.0e5, 0.0),
]
FX_REF = {"EUR": 1.0, "USD": 1.10, "GBP": 0.85, "CHF": 0.95}   # unidades de la divisa por 1 €
FX_VOL = 0.07

# tarifas: comision = max(minimo, fijo + pct * efectivo) por operacion; cambio de divisa en % del efectivo
TARIFAS: dict[str, dict] = {
    "broker-online": {"nombre": "Bróker online (tarifa típica)",
                      "nacional": {"fijo": 2.0, "pct": 0.0004, "minimo": 2.0},
                      "internacional": {"fijo": 4.0, "pct": 0.0005, "minimo": 4.0},
                      "cripto": {"fijo": 0.0, "pct": 0.005, "minimo": 0.5}, "cambio_pct": 0.0025},
    "banco": {"nombre": "Banco tradicional (tarifa típica)",
              "nacional": {"fijo": 0.0, "pct": 0.0025, "minimo": 8.0},
              "internacional": {"fijo": 0.0, "pct": 0.004, "minimo": 20.0},
              "cripto": {"fijo": 0.0, "pct": 0.015, "minimo": 2.0}, "cambio_pct": 0.006},
    "neobroker": {"nombre": "Neobróker sin comisión (cobra en el cambio y la horquilla)",
                  "nacional": {"fijo": 1.0, "pct": 0.0, "minimo": 1.0},
                  "internacional": {"fijo": 1.0, "pct": 0.0, "minimo": 1.0},
                  "cripto": {"fijo": 0.0, "pct": 0.01, "minimo": 1.0}, "cambio_pct": 0.0015},
}
CANON_BME = {"fijo": 1.10, "pct": 0.000024}     # canon de bolsa aproximado por operacion en BME
ORDER_TYPES = ("mercado", "limitada", "stop", "stop_limitada")
GTC_DAYS = 90

SCHEMA = """
CREATE TABLE IF NOT EXISTS carteras (id INTEGER PRIMARY KEY, nombre TEXT, modo TEXT, tarifa TEXT,
    capital_inicial REAL, efectivo REAL, creada TEXT);
CREATE TABLE IF NOT EXISTS instrumentos (simbolo TEXT PRIMARY KEY, nombre TEXT, mercado TEXT, divisa TEXT, tipo TEXT,
    precio_ref REAL, vol REAL, spread_bps REAL, adv REAL, impuesto_pct REAL, propio INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS precios (simbolo TEXT PRIMARY KEY, precio REAL, ts REAL, cierre_ant REAL, apertura REAL);
CREATE TABLE IF NOT EXISTS ticks (simbolo TEXT, ts REAL, precio REAL);
CREATE INDEX IF NOT EXISTS ticks_s ON ticks(simbolo, ts);
CREATE TABLE IF NOT EXISTS historico (simbolo TEXT, fecha TEXT, cierre REAL, PRIMARY KEY (simbolo, fecha));
CREATE TABLE IF NOT EXISTS fx (divisa TEXT PRIMARY KEY, tasa REAL, ts REAL);
CREATE TABLE IF NOT EXISTS ordenes (id INTEGER PRIMARY KEY, cartera INTEGER, simbolo TEXT, lado TEXT, tipo TEXT,
    cantidad REAL, limite REAL, stop REAL, validez TEXT, estado TEXT, creada REAL, expira REAL, activada INTEGER DEFAULT 0,
    motivo TEXT DEFAULT '', precio_ejec REAL, ejecutada REAL, origen TEXT DEFAULT '', estimado_eur REAL DEFAULT 0);
CREATE TABLE IF NOT EXISTS operaciones (id INTEGER PRIMARY KEY, cartera INTEGER, orden INTEGER, simbolo TEXT,
    lado TEXT, cantidad REAL, precio REAL, fx REAL, efectivo_eur REAL, comision_eur REAL, impuestos_eur REAL,
    cambio_eur REAL, deslizamiento_eur REAL, total_eur REAL, ganancia_eur REAL, aviso TEXT DEFAULT '', ts REAL);
CREATE TABLE IF NOT EXISTS lotes (id INTEGER PRIMARY KEY, cartera INTEGER, simbolo TEXT, cantidad REAL,
    coste_unit_eur REAL, ts REAL);
"""


def _r2(x: float) -> float:
    return round(x + 0.0, 2)


def _num(x) -> float:
    if isinstance(x, (int, float)):
        return float(x)
    s = str(x).strip().replace("€", "").replace(" ", "")
    if "," in s and "." in s:
        s = s.replace(".", "").replace(",", ".") if s.rfind(",") > s.rfind(".") else s.replace(",", "")
    return float(s.replace(",", "."))


class Bolsa:
    """Mesa de bolsa: simulador de mercado, carteras y ordenes. Segura entre hilos."""

    def __init__(self, path: Path | str | None = None, seed: int | None = None, clock=None) -> None:
        self.path = Path(path) if path else paths.sub("bolsa") / "bolsa.db"
        self.rng = random.Random(seed)
        self.clock = clock or (lambda: dt.datetime.now(UTC))
        self.datos = None   # proveedor de cotizaciones reales (bolsa_datos.DatosMercado); None = solo simulacion
        self._lock = threading.RLock()
        self.con = sqlite3.connect(str(self.path), check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        with self._lock, self.con:
            self.con.executescript(SCHEMA)
            for row in _I:
                self.con.execute("INSERT OR IGNORE INTO instrumentos (simbolo, nombre, mercado, divisa, tipo, "
                                 "precio_ref, vol, spread_bps, adv, impuesto_pct) VALUES (?,?,?,?,?,?,?,?,?,?)",
                                 row[:9] + (row[9] / 100,))

    # ------------------------------------------------------------ utilidades
    def _q(self, sql: str, args=()) -> list[dict]:
        return [dict(r) for r in self.con.execute(sql, args).fetchall()]

    def _one(self, sql: str, args=()) -> dict | None:
        r = self.con.execute(sql, args).fetchone()
        return dict(r) if r else None

    def now(self) -> dt.datetime:
        return self.clock()

    def close(self) -> None:
        self.con.close()

    # ------------------------------------------------------------ mercados
    def mercados(self) -> list[dict]:
        t = self.now()
        out = []
        for code, m in MERCADOS.items():
            s = next_session(code, t)
            op = is_open(code, t)
            out.append({"id": code, "nombre": m["nombre"], "abierto": op,
                        "hora_local": fmt_local(code, t),
                        "horario": "24 horas, todos los días" if m.get("continuo") else f"{m['abre']}-{m['cierra']}",
                        "cierra" if op else "abre": fmt_local(code, s[1] if op else s[0])})
        return out

    # ------------------------------------------------------------ instrumentos y precios
    def instrumentos(self, texto: str = "") -> list[dict]:
        rows = self._q("SELECT * FROM instrumentos ORDER BY mercado, simbolo")
        if texto:
            t = texto.lower()
            rows = [r for r in rows if t in r["simbolo"].lower() or t in r["nombre"].lower() or t in r["tipo"].lower()
                    or t == r["mercado"].lower()]
        return rows

    def instrumento(self, simbolo: str) -> dict:
        s = (simbolo or "").strip().upper()
        r = self._one("SELECT * FROM instrumentos WHERE simbolo=?", (s,))
        if not r:
            near = [x["simbolo"] for x in self.instrumentos(simbolo)][:8]
            raise ValueError(f"no conozco el instrumento {simbolo}" + (f"; ¿quizá {', '.join(near)}?" if near else
                             ". Añádelo en la mesa de bolsa con su precio de referencia o importa sus cierres."))
        return r

    def anadir_instrumento(self, simbolo: str, nombre: str, mercado: str, precio: float, divisa: str = "",
                           tipo: str = "acción", vol: float = 0.25, spread_bps: float = 5, adv: float = 1e6,
                           impuesto_pct: float = 0.0) -> dict:
        mercado = mercado.upper()
        if mercado not in MERCADOS:
            raise ValueError(f"mercado desconocido: {mercado}. Disponibles: {', '.join(MERCADOS)}")
        divisa = (divisa or ("EUR" if MERCADOS[mercado]["zona"] in ("eu", "utc") else "GBP"
                             if mercado == "LSE" else "USD")).upper()
        if divisa not in FX_REF:
            raise ValueError(f"divisa no soportada: {divisa}. Disponibles: {', '.join(FX_REF)}")
        precio, vol = _num(precio), _num(vol)
        if precio <= 0 or not 0 < vol < 5:
            raise ValueError("el precio debe ser positivo y la volatilidad anual entre 0 y 5 (0,25 = 25 %)")
        sym = simbolo.strip().upper()
        with self._lock, self.con:
            self.con.execute("INSERT OR REPLACE INTO instrumentos VALUES (?,?,?,?,?,?,?,?,?,?,1)",
                             (sym, nombre, mercado, divisa, tipo, precio, vol, _num(spread_bps), _num(adv),
                              _num(impuesto_pct) / 100))
            self.con.execute("DELETE FROM precios WHERE simbolo=?", (sym,))
        return self.instrumento(sym)

    def _step(self, inst: dict, price: float, t0: dt.datetime, t1: dt.datetime) -> float:
        secs, opens = _trading_time(inst["mercado"], t0, t1)
        if not secs and not opens:
            return price
        cont = MERCADOS[inst["mercado"]].get("continuo")
        year_secs = (365 * 86400) if cont else 252 * (_hm(MERCADOS[inst["mercado"]]["cierra"]) -
                                                     _hm(MERCADOS[inst["mercado"]]["abre"])).total_seconds()
        vol, mu = inst["vol"], 0.0 if cont else 0.05
        tau = secs / year_secs
        z = self.rng.gauss(0, 1)
        logret = (mu - vol * vol / 2) * tau + vol * math.sqrt(tau) * z
        if opens:   # salto en cada apertura (la noticia de la noche): ~35 % de la volatilidad diaria
            logret += sum(self.rng.gauss(0, vol / math.sqrt(252) * 0.35) for _ in range(min(opens, 60)))
        return max(price * math.exp(max(-0.5, min(0.5, logret))), 1e-6)

    def _hist_last(self, sym: str) -> dict | None:
        return self._one("SELECT fecha, cierre FROM historico WHERE simbolo=? ORDER BY fecha DESC LIMIT 1", (sym,))

    def precio(self, simbolo: str) -> dict:
        """Cotizacion simulada al momento (avanza el precio con el tiempo de mercado transcurrido)."""
        inst = self.instrumento(simbolo)
        sym, t = inst["simbolo"], self.now()
        real = self._real_quote(sym)
        with self._lock, self.con:
            st = self._one("SELECT * FROM precios WHERE simbolo=?", (sym,))
            if real:
                st = {"precio": real["precio"], "ts": t.timestamp(), "cierre_ant": real["cierre_anterior"] or real["precio"],
                      "apertura": (st or {}).get("apertura") or real["precio"]}
            elif not st:
                h = self._hist_last(sym)
                p0 = h["cierre"] if h else inst["precio_ref"]
                st = {"precio": p0, "ts": t.timestamp(), "cierre_ant": p0, "apertura": p0}
            else:
                t0 = dt.datetime.fromtimestamp(st["ts"], UTC)
                p = self._step(inst, st["precio"], t0, t)
                cont = MERCADOS[inst["mercado"]].get("continuo")
                new_day = (local_time(inst["mercado"], t0).date() != local_time(inst["mercado"], t).date() if cont
                           else _trading_time(inst["mercado"], t0, t)[1] > 0)
                if new_day:   # ha empezado otra sesion: el ultimo precio visto es el cierre anterior
                    st["cierre_ant"], st["apertura"] = st["precio"], p
                st.update(precio=p, ts=t.timestamp())
            self.con.execute("INSERT OR REPLACE INTO precios VALUES (?,?,?,?,?)",
                             (sym, st["precio"], st["ts"], st["cierre_ant"], st["apertura"]))
            last = self._one("SELECT ts FROM ticks WHERE simbolo=? ORDER BY ts DESC LIMIT 1", (sym,))
            if not last or st["ts"] - last["ts"] >= (10 if real else 30):
                self.con.execute("INSERT INTO ticks VALUES (?,?,?)", (sym, st["ts"], st["precio"]))
                self.con.execute("DELETE FROM ticks WHERE simbolo=? AND ts < (SELECT ts FROM ticks WHERE simbolo=? "
                                 "ORDER BY ts DESC LIMIT 1 OFFSET 499)", (sym, sym))
        p = st["precio"]
        half = p * inst["spread_bps"] / 2e4
        dec = 2 if p >= 10 else 3 if p >= 1 else 4
        return {"simbolo": sym, "nombre": inst["nombre"], "mercado": inst["mercado"], "divisa": inst["divisa"],
                "tipo": inst["tipo"], "precio": round(p, dec), "compra": round(p - half, dec),
                "venta": round(p + half, dec), "variacion_pct": round((p / st["cierre_ant"] - 1) * 100, 2),
                "cierre_anterior": round(st["cierre_ant"], dec), "abierto": is_open(inst["mercado"], t),
                "hora": t.isoformat(timespec="seconds"), "fuente": real["fuente"] if real else "simulado",
                "retraso": real.get("retraso", "") if real else "", "real": bool(real)}

    def _real_quote(self, sym: str) -> dict | None:
        """Cotizacion real del proveedor (si lo hay y responde) con la divisa del catalogo."""
        if not self.datos:
            return None
        try:
            q = self.datos.quote(sym)
        except Exception as e:   # noqa: BLE001 - sin conexion: sigue la simulacion
            L.debug("cotizacion real de %s no disponible: %s", sym, e)
            return None
        if not q or not q.get("precio"):
            return None
        if q.get("divisa") in ("GBp", "GBX"):   # Londres cotiza en peniques: el catalogo va en libras
            q = {**q, "precio": q["precio"] / 100, "cierre_anterior": (q.get("cierre_anterior") or 0) / 100,
                 "puntos": [{**p, "precio": p["precio"] / 100} for p in q.get("puntos") or []]}
        return q

    def serie(self, simbolo: str, rango: str = "1d") -> dict:
        """Serie de precios para graficos: real si hay conexion (velas de Yahoo), si no los ticks guardados."""
        sym = self.instrumento(simbolo)["simbolo"]
        pts: list[dict] = []
        if self.datos:
            try:
                pts = self.datos.serie(sym, rango)
            except Exception:   # noqa: BLE001
                pts = []
            inst = self.instrumento(sym)
            if pts and inst["divisa"] == "GBP" and pts[-1]["precio"] > 50 * (inst["precio_ref"] or 1):
                pts = [{**p, "precio": p["precio"] / 100} for p in pts]
        if pts:
            if rango in ("6mo", "1y", "5y"):
                self.guardar_historico(sym, pts)
            return {"simbolo": sym, "rango": rango, "real": True, "puntos": pts}
        if rango in ("6mo", "1y", "5y"):
            rows = self._q("SELECT fecha, cierre FROM historico WHERE simbolo=? ORDER BY fecha", (sym,))
            pts = [{"ts": dt.datetime.fromisoformat(r["fecha"]).replace(tzinfo=UTC).timestamp(), "precio": r["cierre"]}
                   for r in rows]
            if pts:
                return {"simbolo": sym, "rango": rango, "real": True, "puntos": pts, "guardado": True}
        return {"simbolo": sym, "rango": rango, "real": False,
                "puntos": [{"ts": r["ts"], "precio": r["precio"]} for r in self.ticks(sym, 400)]}

    def guardar_historico(self, sym: str, pts: list[dict]) -> int:
        """Guarda cierres diarios reales para analizar sin conexion."""
        rows = [(sym, dt.datetime.fromtimestamp(p["ts"], UTC).date().isoformat(), p["precio"]) for p in pts]
        with self._lock, self.con:
            self.con.executemany("INSERT OR REPLACE INTO historico VALUES (?,?,?)", rows)
        return len(rows)

    def ticks(self, simbolo: str, n: int = 200) -> list[dict]:
        sym = self.instrumento(simbolo)["simbolo"]
        rows = self._q("SELECT ts, precio FROM ticks WHERE simbolo=? ORDER BY ts DESC LIMIT ?", (sym, int(n)))
        return rows[::-1]

    def fx(self, divisa: str) -> float:
        """Unidades de ``divisa`` por 1 € (tipo de cambio simulado, se mueve 24 h los dias laborables)."""
        if divisa == "EUR":
            return 1.0
        t = self.now()
        real = None
        if self.datos:
            try:
                real = self.datos.fx(divisa)
            except Exception:   # noqa: BLE001
                real = None
        with self._lock, self.con:
            r = self._one("SELECT * FROM fx WHERE divisa=?", (divisa,))
            rate = r["tasa"] if r else FX_REF[divisa]
            if real:
                rate = real
            elif r:
                tau = max(0.0, t.timestamp() - r["ts"]) / (365 * 86400)
                rate *= math.exp(FX_VOL * math.sqrt(min(tau, 1.0)) * self.rng.gauss(0, 1))
            self.con.execute("INSERT OR REPLACE INTO fx VALUES (?,?,?)", (divisa, rate, t.timestamp()))
        return rate

    def importar_csv(self, simbolo: str, texto: str) -> dict:
        """Cierres reales «fecha;cierre» (o con comas, o columnas Date/Close de una descarga): ancla el precio
        simulado al ultimo cierre y ajusta la volatilidad del activo a la de esos datos."""
        inst = self.instrumento(simbolo)
        rows = []
        lines = [ln for ln in texto.splitlines() if ln.strip()]
        sep = ";" if any(";" in ln for ln in lines[:5]) else "\t" if any("\t" in ln for ln in lines[:5]) else ","
        header = [h.strip().lower() for h in lines[0].split(sep)] if lines else []
        ci = next((i for i, h in enumerate(header) if h in ("close", "cierre", "adj close", "último", "ultimo",
                                                            "precio")), None)
        for ln in lines:
            parts = [p.strip().strip('"') for p in ln.split(sep)]
            m = re.match(r"(\d{4})-(\d{2})-(\d{2})|(\d{1,2})[/.-](\d{1,2})[/.-](\d{4})", parts[0])
            if not m or len(parts) < 2:
                continue
            fecha = (f"{m[1]}-{m[2]}-{m[3]}" if m[1] else f"{m[6]}-{int(m[5]):02d}-{int(m[4]):02d}")
            try:
                close = _num(parts[ci] if ci is not None and ci < len(parts) else parts[1])
            except ValueError:
                continue
            if close > 0:
                rows.append((inst["simbolo"], fecha, close))
        if len(rows) < 2:
            raise ValueError("no encuentro al menos dos filas «fecha;cierre» en el CSV")
        rows.sort(key=lambda r: r[1])
        rets = [math.log(b[2] / a[2]) for a, b in zip(rows, rows[1:])]
        vol = None
        if len(rets) >= 10:
            mean = sum(rets) / len(rets)
            vol = math.sqrt(sum((r - mean) ** 2 for r in rets) / (len(rets) - 1) * 252)
        with self._lock, self.con:
            self.con.executemany("INSERT OR REPLACE INTO historico VALUES (?,?,?)", rows)
            if vol and 0.01 < vol < 5:
                self.con.execute("UPDATE instrumentos SET vol=? WHERE simbolo=?", (vol, inst["simbolo"]))
            last = rows[-1][2]
            prev = rows[-2][2]
            self.con.execute("INSERT OR REPLACE INTO precios VALUES (?,?,?,?,?)",
                             (inst["simbolo"], last, self.now().timestamp(), prev, last))
        return {"simbolo": inst["simbolo"], "filas": len(rows), "desde": rows[0][1], "hasta": rows[-1][1],
                "ultimo_cierre": last, "volatilidad_anual": round(vol, 4) if vol else None}

    # ------------------------------------------------------------ carteras
    def carteras(self) -> list[dict]:
        rows = self._q("SELECT * FROM carteras ORDER BY id")
        if not rows:
            self.crear_cartera()
            rows = self._q("SELECT * FROM carteras ORDER BY id")
        return rows

    def crear_cartera(self, nombre: str = "Cartera simulada", capital: float = 10000.0, tarifa: str = "broker-online",
                      modo: str = "simulado") -> dict:
        capital = _num(capital)
        if tarifa not in TARIFAS:
            raise ValueError(f"tarifa desconocida: {tarifa}. Disponibles: {', '.join(TARIFAS)}")
        if modo not in ("simulado", "real"):
            raise ValueError("modo debe ser «simulado» o «real»")
        if capital < 0 or capital > 1e9:
            raise ValueError("capital fuera de rango")
        with self._lock, self.con:
            cur = self.con.execute("INSERT INTO carteras (nombre, modo, tarifa, capital_inicial, efectivo, creada) "
                                   "VALUES (?,?,?,?,?,?)", (nombre, modo, tarifa, capital, capital,
                                                            self.now().isoformat(timespec="seconds")))
        return self.cartera_info(cur.lastrowid)

    def cartera_info(self, cid: int | None = None) -> dict:
        rows = self.carteras()
        if cid in (None, "", 0):
            return rows[0]
        r = self._one("SELECT * FROM carteras WHERE id=?", (int(cid),))
        if not r:
            raise ValueError(f"no existe la cartera {cid}")
        return r

    def reiniciar(self, cid: int | None = None, capital: float | None = None) -> dict:
        """Vuelve a empezar una cartera simulada: borra ordenes, operaciones y posiciones."""
        c = self.cartera_info(cid)
        if c["modo"] != "simulado":
            raise ValueError("solo se reinician carteras simuladas")
        cap = c["capital_inicial"] if capital is None else _num(capital)
        with self._lock, self.con:
            for tbl in ("ordenes", "operaciones", "lotes"):
                self.con.execute(f"DELETE FROM {tbl} WHERE cartera=?", (c["id"],))
            self.con.execute("UPDATE carteras SET capital_inicial=?, efectivo=? WHERE id=?", (cap, cap, c["id"]))
        return self.cartera_info(c["id"])

    def posiciones(self, cid: int | None = None) -> list[dict]:
        c = self.cartera_info(cid)
        out = []
        for r in self._q("SELECT simbolo, SUM(cantidad) q, SUM(cantidad*coste_unit_eur) coste FROM lotes "
                         "WHERE cartera=? AND cantidad > 1e-12 GROUP BY simbolo ORDER BY simbolo", (c["id"],)):
            q = self.precio(r["simbolo"])
            fx = self.fx(q["divisa"])
            valor = r["q"] * q["compra"] / fx      # valorado a precio de venta (lo que se cobraria)
            out.append({"simbolo": r["simbolo"], "nombre": q["nombre"], "cantidad": round(r["q"], 8),
                        "coste_medio_eur": round(r["coste"] / r["q"], 4), "coste_eur": _r2(r["coste"]),
                        "precio": q["precio"], "divisa": q["divisa"], "valor_eur": _r2(valor),
                        "plusvalia_eur": _r2(valor - r["coste"]),
                        "plusvalia_pct": round((valor / r["coste"] - 1) * 100, 2) if r["coste"] else 0.0,
                        "variacion_dia_pct": q["variacion_pct"]})
        return out

    def resumen(self, cid: int | None = None) -> dict:
        self.procesar()
        c = self.cartera_info(cid)
        pos = self.posiciones(c["id"])
        invertido = sum(p["valor_eur"] for p in pos)
        total = c["efectivo"] + invertido
        real = self._one("SELECT COALESCE(SUM(ganancia_eur),0) g, COALESCE(SUM(comision_eur+impuestos_eur+cambio_eur),0) "
                         "gastos FROM operaciones WHERE cartera=?", (c["id"],))
        for p in pos:
            p["peso_pct"] = round(p["valor_eur"] / total * 100, 2) if total else 0.0
        return {"cartera": c["id"], "nombre": c["nombre"], "modo": c["modo"], "tarifa": c["tarifa"],
                "efectivo_eur": _r2(c["efectivo"]), "invertido_eur": _r2(invertido), "total_eur": _r2(total),
                "capital_inicial_eur": _r2(c["capital_inicial"]),
                "rentabilidad_pct": round((total / c["capital_inicial"] - 1) * 100, 2) if c["capital_inicial"] else 0.0,
                "ganancia_realizada_eur": _r2(real["g"]), "gastos_eur": _r2(real["gastos"]), "posiciones": pos,
                "ordenes_pendientes": len(self.ordenes(c["id"], abiertas=True)), "aviso": AVISO}

    # ------------------------------------------------------------ costes
    def costes(self, cid: int | None, simbolo: str, lado: str, cantidad: float, precio: float) -> dict:
        """Gastos de una operacion en euros: comision, canon, impuesto de transacciones y cambio de divisa."""
        c, inst = self.cartera_info(cid), self.instrumento(simbolo)
        tar = TARIFAS[c["tarifa"]]
        fx = self.fx(inst["divisa"])
        efectivo = cantidad * precio / fx
        tramo = "cripto" if inst["mercado"] == "CRIPTO" else (
            "nacional" if MERCADOS[inst["mercado"]].get("nacional") else "internacional")
        t = tar[tramo]
        com = max(t["minimo"], t["fijo"] + t["pct"] * efectivo)
        if inst["mercado"] == "BME":
            com += CANON_BME["fijo"] + CANON_BME["pct"] * efectivo
        imp = efectivo * inst["impuesto_pct"] if lado == "compra" else 0.0
        cambio = efectivo * tar["cambio_pct"] if inst["divisa"] != "EUR" else 0.0
        return {"efectivo_eur": _r2(efectivo), "comision_eur": _r2(com), "impuestos_eur": _r2(imp),
                "cambio_eur": _r2(cambio), "fx": fx, "tramo": tramo}

    def _fill_price(self, inst: dict, quote: dict, lado: str, cantidad: float) -> tuple[float, float]:
        """Precio de ejecucion a mercado (horquilla + impacto por tamano) y deslizamiento frente al medio."""
        base = quote["venta"] if lado == "compra" else quote["compra"]
        daily = inst["vol"] / math.sqrt(252)
        impact = 0.1 * daily * math.sqrt(max(cantidad, 0) / max(inst["adv"], 1.0))
        noise = abs(self.rng.gauss(0, inst["spread_bps"] / 1e4 / 4))
        frac = min(impact + noise, 0.05)
        px = base * (1 + frac) if lado == "compra" else base * (1 - frac)
        return px, abs(px - quote["precio"]) * cantidad

    # ------------------------------------------------------------ ordenes
    def ordenar(self, simbolo: str, lado: str, cantidad: float, tipo: str = "mercado", limite: float | None = None,
                stop: float | None = None, validez: str = "dia", cartera: int | None = None,
                origen: str = "") -> dict:
        """Da una orden. En carteras simuladas se ejecuta segun el mercado; en las reales queda por confirmar."""
        c, inst = self.cartera_info(cartera), self.instrumento(simbolo)
        lado = {"comprar": "compra", "buy": "compra", "vender": "venta", "sell": "venta"}.get(lado.lower(), lado.lower())
        tipo = tipo.lower().replace(" ", "_").replace("-", "_")
        if lado not in ("compra", "venta"):
            raise ValueError("lado debe ser «compra» o «venta»")
        if tipo not in ORDER_TYPES:
            raise ValueError(f"tipo de orden desconocido: {tipo}. Disponibles: {', '.join(ORDER_TYPES)}")
        if validez not in ("dia", "hasta_cancelar"):
            raise ValueError("validez debe ser «dia» o «hasta_cancelar»")
        cantidad = _num(cantidad)
        frac = inst["mercado"] == "CRIPTO"
        if cantidad <= 0 or (not frac and abs(cantidad - round(cantidad)) > 1e-9):
            raise ValueError("la cantidad debe ser positiva" + ("" if frac else " y entera (títulos completos)"))
        limite = _num(limite) if limite not in (None, "") else None
        stop = _num(stop) if stop not in (None, "") else None
        if tipo in ("limitada", "stop_limitada") and not limite:
            raise ValueError("una orden limitada necesita precio límite")
        if tipo in ("stop", "stop_limitada") and not stop:
            raise ValueError("una orden stop necesita precio de activación")
        if lado == "venta":
            held = self._held(c["id"], inst["simbolo"]) - self._reserved(c["id"], inst["simbolo"])
            if cantidad > held + 1e-9:
                raise ValueError(f"no tienes suficientes {inst['simbolo']} para vender ({held:g} disponibles); "
                                 "en esta mesa no se permiten ventas en corto")
        estimado = 0.0
        if lado == "compra":   # el efectivo de las compras abiertas queda reservado, como en un broker
            q = self.precio(inst["simbolo"])
            px = max(q["venta"], limite or 0, stop or 0)
            g = self.costes(c["id"], inst["simbolo"], lado, cantidad, px)
            estimado = _r2(g["efectivo_eur"] * 1.01 + g["comision_eur"] + g["impuestos_eur"] + g["cambio_eur"])
            reservado = self._one("SELECT COALESCE(SUM(estimado_eur),0) e FROM ordenes WHERE cartera=? AND "
                                  "lado='compra' AND estado IN ('pendiente','por_confirmar','confirmada')", (c["id"],))["e"]
            libre = c["efectivo"] - reservado
            if estimado > libre + 1e-6:
                raise ValueError(f"efectivo insuficiente: la orden necesita unos {estimado:.2f} € y hay {libre:.2f} € "
                                 "libres" + (f" ({reservado:.2f} € reservados en otras órdenes)" if reservado else ""))
        t = self.now()
        s = next_session(inst["mercado"], t)
        expira = (s[1] if validez == "dia" else t + dt.timedelta(days=GTC_DAYS)).timestamp()
        estado = "por_confirmar" if c["modo"] == "real" else "pendiente"
        with self._lock, self.con:
            cur = self.con.execute("INSERT INTO ordenes (cartera, simbolo, lado, tipo, cantidad, limite, stop, validez, "
                                   "estado, creada, expira, origen, estimado_eur) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                   (c["id"], inst["simbolo"], lado, tipo, cantidad, limite, stop, validez, estado,
                                    t.timestamp(), expira, origen, estimado))
        oid = cur.lastrowid
        if estado == "pendiente":
            self.procesar(oid)
        o = self.orden(oid)
        if estado == "por_confirmar":
            o["nota"] = ("Cartera REAL: la orden NO se ha enviado a ningún bróker. Revísala y, si la quieres, "
                         f"confírmala tú escribiendo {CONFIRMACION} en la app o la terminal; después la ejecutas en tu "
                         "bróker y registras la ejecución.")
        elif o["estado"] == "pendiente":
            o["nota"] = (f"{MERCADOS[inst['mercado']]['nombre']} está cerrado: se ejecutará cuando abra "
                         f"({fmt_local(inst['mercado'], s[0])} hora local)"
                         if not is_open(inst["mercado"], t) else "Orden en el libro: espera a que el precio la active")
        o["aviso"] = AVISO
        return o

    def _held(self, cid: int, sym: str) -> float:
        r = self._one("SELECT COALESCE(SUM(cantidad),0) q FROM lotes WHERE cartera=? AND simbolo=?", (cid, sym))
        return r["q"]

    def _reserved(self, cid: int, sym: str) -> float:
        r = self._one("SELECT COALESCE(SUM(cantidad),0) q FROM ordenes WHERE cartera=? AND simbolo=? AND lado='venta' "
                      "AND estado IN ('pendiente','por_confirmar','confirmada')", (cid, sym))
        return r["q"]

    def orden(self, oid: int) -> dict:
        o = self._one("SELECT * FROM ordenes WHERE id=?", (int(oid),))
        if not o:
            raise ValueError(f"no existe la orden {oid}")
        for k in ("creada", "expira", "ejecutada"):
            if o.get(k):
                o[k] = dt.datetime.fromtimestamp(o[k], UTC).isoformat(timespec="seconds")
        op = self._one("SELECT * FROM operaciones WHERE orden=?", (o["id"],))
        if op:
            o["operacion"] = op
        return o

    def ordenes(self, cartera: int | None = None, abiertas: bool = False) -> list[dict]:
        c = self.cartera_info(cartera)
        sql = "SELECT id FROM ordenes WHERE cartera=?" + (
            " AND estado IN ('pendiente','por_confirmar','confirmada')" if abiertas else "") + " ORDER BY id DESC LIMIT 200"
        return [self.orden(r["id"]) for r in self._q(sql, (c["id"],))]

    def cancelar(self, oid: int) -> dict:
        with self._lock, self.con:
            o = self._one("SELECT * FROM ordenes WHERE id=?", (int(oid),))
            if not o:
                raise ValueError(f"no existe la orden {oid}")
            if o["estado"] not in ("pendiente", "por_confirmar", "confirmada"):
                raise ValueError(f"la orden {oid} ya está {o['estado']}")
            self.con.execute("UPDATE ordenes SET estado='cancelada', motivo='cancelada por el usuario' WHERE id=?",
                             (o["id"],))
        return self.orden(oid)

    def procesar(self, only: int | None = None) -> list[int]:
        """Revisa las ordenes pendientes de carteras simuladas: caducidad, activacion de stops y ejecucion."""
        done = []
        with self._lock:
            sql = ("SELECT o.* FROM ordenes o JOIN carteras c ON c.id=o.cartera WHERE o.estado='pendiente' "
                   "AND c.modo='simulado'") + (" AND o.id=?" if only else "") + " ORDER BY o.id"
            for o in self._q(sql, (only,) if only else ()):
                if self._process_one(o):
                    done.append(o["id"])
        return done

    def _process_one(self, o: dict) -> bool:
        inst = self.instrumento(o["simbolo"])
        t = self.now()
        if not is_open(inst["mercado"], t):
            if t.timestamp() >= o["expira"]:
                self._set(o["id"], estado="caducada", motivo="fin de la validez sin ejecutarse")
            return False
        q = self.precio(o["simbolo"])
        if t.timestamp() >= o["expira"]:
            self._set(o["id"], estado="caducada", motivo="fin de la validez sin ejecutarse")
            return False
        buy = o["lado"] == "compra"
        ref = q["venta"] if buy else q["compra"]
        if o["tipo"] in ("stop", "stop_limitada") and not o["activada"]:
            if (buy and q["precio"] >= o["stop"]) or (not buy and q["precio"] <= o["stop"]):
                self._set(o["id"], activada=1)
                o["activada"] = 1
            else:
                return False
        px, slip = self._fill_price(inst, q, o["lado"], o["cantidad"])
        if o["tipo"] in ("limitada", "stop_limitada"):
            if (buy and ref > o["limite"]) or (not buy and ref < o["limite"]):
                return False
            px = min(px, o["limite"]) if buy else max(px, o["limite"])
            slip = abs(px - q["precio"]) * o["cantidad"]
        return self._execute(o, px, slip / self.fx(inst["divisa"]))

    def _set(self, oid: int, **kw) -> None:
        with self._lock, self.con:
            self.con.execute(f"UPDATE ordenes SET {', '.join(k + '=?' for k in kw)} WHERE id=?", (*kw.values(), oid))

    def _execute(self, o: dict, px: float, slip_eur: float, gastos: dict | None = None) -> bool:
        c = self.cartera_info(o["cartera"])
        sym, qty, buy = o["simbolo"], o["cantidad"], o["lado"] == "compra"
        g = gastos or self.costes(c["id"], sym, o["lado"], qty, px)
        fees = g["comision_eur"] + g["impuestos_eur"] + g["cambio_eur"]
        total = g["efectivo_eur"] + fees if buy else g["efectivo_eur"] - fees
        aviso, gain = "", None
        now = self.now().timestamp()
        two_months = 61 * 86400
        with self._lock, self.con:
            if buy and total > c["efectivo"] + 1e-6:
                self.con.execute("UPDATE ordenes SET estado='rechazada', motivo=? WHERE id=?",
                                 (f"efectivo insuficiente: hacen falta {total:.2f} € y hay {c['efectivo']:.2f} €",
                                  o["id"]))
                return False
            if buy:
                self.con.execute("INSERT INTO lotes (cartera, simbolo, cantidad, coste_unit_eur, ts) VALUES (?,?,?,?,?)",
                                 (c["id"], sym, qty, total / qty, now))
                if self._one("SELECT 1 FROM operaciones WHERE cartera=? AND simbolo=? AND lado='venta' AND "
                             "ganancia_eur < 0 AND ts >= ?", (c["id"], sym, now - two_months)):
                    aviso = ("Recompra antes de dos meses tras vender con pérdidas: esa pérdida no se puede compensar "
                             "en el IRPF hasta que vendas estos títulos (art. 33.5 LIRPF).")
            else:
                left, cost = qty, 0.0
                for lot in self._q("SELECT * FROM lotes WHERE cartera=? AND simbolo=? AND cantidad > 1e-12 "
                                   "ORDER BY ts, id", (c["id"], sym)):   # FIFO
                    take = min(left, lot["cantidad"])
                    cost += take * lot["coste_unit_eur"]
                    self.con.execute("UPDATE lotes SET cantidad=? WHERE id=?", (lot["cantidad"] - take, lot["id"]))
                    left -= take
                    if left <= 1e-12:
                        break
                if left > 1e-9:
                    raise ValueError(f"no hay suficientes {sym} para vender")
                self.con.execute("DELETE FROM lotes WHERE cartera=? AND cantidad <= 1e-12", (c["id"],))
                gain = _r2(total - cost)
                # solo cuenta si se conservan titulos homogeneos comprados en los dos meses anteriores
                if gain < 0 and self._one("SELECT 1 FROM lotes WHERE cartera=? AND simbolo=? AND cantidad > 1e-12 "
                                          "AND ts >= ?", (c["id"], sym, now - two_months)):
                    aviso = ("Pérdida con compras de este valor en los dos meses anteriores: no podrás compensarla "
                             "en el IRPF hasta vender esos títulos (art. 33.5 LIRPF).")
            self.con.execute("UPDATE carteras SET efectivo=? WHERE id=?",
                             (_r2(c["efectivo"] - total if buy else c["efectivo"] + total), c["id"]))
            self.con.execute("INSERT INTO operaciones (cartera, orden, simbolo, lado, cantidad, precio, fx, efectivo_eur, "
                             "comision_eur, impuestos_eur, cambio_eur, deslizamiento_eur, total_eur, ganancia_eur, aviso, "
                             "ts) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                             (c["id"], o["id"], sym, o["lado"], qty, px, g["fx"], g["efectivo_eur"], g["comision_eur"],
                              g["impuestos_eur"], g["cambio_eur"], _r2(slip_eur), _r2(total), gain, aviso, now))
            self.con.execute("UPDATE ordenes SET estado='ejecutada', precio_ejec=?, ejecutada=? WHERE id=?",
                             (px, now, o["id"]))
        L.info("Orden %s ejecutada: %s %g %s a %.4f", o["id"], o["lado"], qty, sym, px)
        return True

    def operaciones(self, cartera: int | None = None, limite: int = 100) -> list[dict]:
        c = self.cartera_info(cartera)
        rows = self._q("SELECT * FROM operaciones WHERE cartera=? ORDER BY ts DESC, id DESC LIMIT ?",
                       (c["id"], int(limite)))
        for r in rows:
            r["fecha"] = dt.datetime.fromtimestamp(r["ts"], UTC).isoformat(timespec="seconds")
        return rows

    def informe_fiscal(self, cartera: int | None = None, anio: int | None = None) -> dict:
        """Ganancias y perdidas patrimoniales realizadas en el año (criterio FIFO), para orientar la Renta."""
        c = self.cartera_info(cartera)
        y = int(anio or self.now().year)
        a = dt.datetime(y, 1, 1, tzinfo=UTC).timestamp()
        b = dt.datetime(y + 1, 1, 1, tzinfo=UTC).timestamp()
        rows = self._q("SELECT simbolo, SUM(ganancia_eur) g, COUNT(*) n, SUM(CASE WHEN aviso!='' THEN 1 ELSE 0 END) av "
                       "FROM operaciones WHERE cartera=? AND lado='venta' AND ts>=? AND ts<? GROUP BY simbolo",
                       (c["id"], a, b))
        neto = sum(r["g"] or 0 for r in rows)
        return {"cartera": c["id"], "modo": c["modo"], "anio": y, "por_valor": rows, "neto_eur": _r2(neto),
                "nota": "Orientativo: en la Renta las ganancias y pérdidas por transmisión de acciones, ETF y "
                        "criptoactivos van a la base del ahorro. Los tramos y la normativa foral cambian: confírmalo con "
                        "tu asesor fiscal. En simulado no hay nada que declarar.", "aviso": AVISO}

    # ------------------------------------------------------------ modo real (solo una persona confirma)
    def confirmar(self, oid: int, confirmacion: str) -> dict:
        """Confirmacion explicita de una persona para una orden de cartera real. No envia nada a ningun broker."""
        if (confirmacion or "").strip().upper() != CONFIRMACION:
            raise PermissionError(f"para confirmar una orden real escribe exactamente {CONFIRMACION}")
        o = self._one("SELECT * FROM ordenes WHERE id=?", (int(oid),))
        if not o or o["estado"] != "por_confirmar":
            raise ValueError(f"la orden {oid} no está por confirmar")
        self._set(o["id"], estado="confirmada",
                  motivo="confirmada por el usuario; ejecútala en tu bróker y registra la ejecución")
        return self.orden(oid)

    def registrar_ejecucion(self, oid: int, precio: float, comision_eur: float = 0.0, impuestos_eur: float | None = None,
                            fx: float | None = None) -> dict:
        """Apunta la ejecucion real de una orden confirmada (precio en la divisa del valor) para seguir la cartera.
        Sin ``impuestos_eur`` se estima el impuesto sobre transacciones financieras que corresponda."""
        o = self._one("SELECT * FROM ordenes WHERE id=?", (int(oid),))
        if not o or o["estado"] != "confirmada":
            raise ValueError(f"la orden {oid} no está confirmada")
        inst = self.instrumento(o["simbolo"])
        rate = _num(fx) if fx else self.fx(inst["divisa"])
        px = _num(precio)
        if px <= 0 or rate <= 0:
            raise ValueError("precio y tipo de cambio deben ser positivos")
        efectivo = o["cantidad"] * px / rate
        if impuestos_eur in (None, ""):
            impuestos_eur = efectivo * inst["impuesto_pct"] if o["lado"] == "compra" else 0.0
        g = {"efectivo_eur": _r2(efectivo), "comision_eur": _r2(_num(comision_eur)),
             "impuestos_eur": _r2(_num(impuestos_eur)), "cambio_eur": 0.0, "fx": rate}
        if not self._execute(o, px, 0.0, g):
            raise ValueError(self.orden(oid)["motivo"])
        return self.orden(oid)


_INSTANCES: dict[str, Bolsa] = {}
_ILOCK = threading.Lock()


def get() -> Bolsa:
    """Mesa de bolsa de la carpeta de datos actual (una por carpeta)."""
    path = str(paths.sub("bolsa") / "bolsa.db")
    with _ILOCK:
        if path not in _INSTANCES:
            b = Bolsa(path)
            b.datos = _proveedor_configurado()
            _INSTANCES[path] = b
        return _INSTANCES[path]


def _proveedor_configurado():
    """Cotizaciones reales salvo que la configuracion diga ``bolsa.datos: simulados`` (o NEXUS_BOLSA_OFFLINE=1)."""
    import os

    from ..config import get_config
    modo = str(get_config().get("bolsa.datos", "reales") or "reales")
    if modo == "simulados" or os.environ.get("NEXUS_BOLSA_OFFLINE") == "1":
        return None
    from . import bolsa_datos
    prov = bolsa_datos.proveedor()
    prov.ttl = float(get_config().get("bolsa.refresco_s", 20) or 20)
    return prov


def reset_instances() -> None:
    with _ILOCK:
        for b in _INSTANCES.values():
            try:
                b.close()
            except sqlite3.Error:
                pass
        _INSTANCES.clear()
