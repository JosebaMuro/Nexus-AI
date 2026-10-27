"""Cotizaciones reales para la mesa de bolsa (la mesa sigue siendo SIEMPRE una simulacion: nunca hay dinero ni
ordenes reales; lo real son los precios).

Fuentes, por orden, sin clave ni registro:

* Yahoo Finance (``/v8/finance/chart``): acciones, ETF, cripto y divisas con los mismos simbolos del catalogo
  (SAN.MC, AAPL, VWCE.DE, BTC-EUR, EURUSD=X). Retraso tipico de 15 min en bolsa y tiempo real en cripto.
* Stooq (CSV): respaldo para acciones y ETF.
* Coinbase y Kraken: respaldo para cripto en euros.
* BCE (``eurofxref-daily.xml``): respaldo para el tipo de cambio.

Sin internet, o si todas fallan, ``quote`` devuelve ``None`` y la mesa sigue con su simulacion anclada al ultimo
precio real que vio (y al historico guardado en SQLite), asi que todo funciona 100 % offline.

Cada fuente que falla se aparta un rato (backoff) para no bloquear la interfaz ni martillear a nadie.
"""
from __future__ import annotations

import csv
import io
import re
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import httpx

from .. import log

L = log.get("bolsa.datos")
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 "
      "Nexus-Office")
YAHOO = ("https://query1.finance.yahoo.com", "https://query2.finance.yahoo.com")
STOOQ_SUFFIX = {".MC": ".es", ".DE": ".de", ".PA": ".fr", ".AS": ".nl", ".L": ".uk"}
KRAKEN_PAIR = {"BTC-EUR": "XBTEUR", "ETH-EUR": "ETHEUR", "SOL-EUR": "SOLEUR", "ADA-EUR": "ADAEUR"}
# rango -> (rango Yahoo, intervalo Yahoo)
RANGOS = {"1d": ("1d", "1m"), "5d": ("5d", "5m"), "1mo": ("1mo", "30m"), "6mo": ("6mo", "1d"), "1y": ("1y", "1d"),
          "5y": ("5y", "1wk")}


class Fuente:
    """Estado de una fuente: cuando falla se aparta 30 s, 60 s, 120 s... hasta 15 min."""

    def __init__(self, nombre: str) -> None:
        self.nombre, self.fallos, self.hasta = nombre, 0, 0.0

    def disponible(self) -> bool:
        return time.time() >= self.hasta

    def ok(self) -> None:
        self.fallos, self.hasta = 0, 0.0

    def fallo(self, err: Exception | str) -> None:
        self.fallos += 1
        self.hasta = time.time() + min(900, 30 * 2 ** (self.fallos - 1))
        if self.fallos in (1, 4):
            L.info("Cotizaciones: %s no responde (%s); se reintenta mas tarde", self.nombre, err)


class DatosMercado:
    """Proveedor de cotizaciones reales con cache. Seguro entre hilos."""

    def __init__(self, client: httpx.Client | None = None, ttl: float = 20.0, timeout: float = 6.0) -> None:
        self.client = client or httpx.Client(timeout=timeout, headers={"User-Agent": UA}, follow_redirects=True)
        self.ttl = ttl
        self._lock = threading.Lock()
        self._quotes: dict[str, tuple[float, dict]] = {}
        self._series: dict[tuple[str, str], tuple[float, list]] = {}
        self._fx: dict[str, tuple[float, float]] = {}
        self.fuentes = {n: Fuente(n) for n in ("yahoo", "stooq", "coinbase", "kraken", "bce")}
        self.ultima_fuente = ""
        self.ultimo_ok = 0.0

    # ------------------------------------------------------------ utilidades
    def estado(self) -> dict:
        now = time.time()
        return {"fuentes": {n: {"disponible": f.disponible(), "fallos": f.fallos,
                                "reintento_s": max(0, round(f.hasta - now))} for n, f in self.fuentes.items()},
                "en_linea": now - self.ultimo_ok < 300,
                "ultima_fuente": self.ultima_fuente, "cotizaciones": len(self._quotes)}

    def _get(self, fuente: str, url: str, **kw) -> httpx.Response:
        f = self.fuentes[fuente]
        if not f.disponible():
            raise RuntimeError(f"{fuente} apartada")
        try:
            r = self.client.get(url, **kw)
            if r.status_code == 404:   # el simbolo no existe en esa fuente: la fuente esta bien
                f.ok()
                raise LookupError(f"{fuente}: simbolo desconocido")
            r.raise_for_status()
        except (httpx.HTTPError, OSError) as e:
            f.fallo(e)
            raise
        f.ok()
        return r

    # ------------------------------------------------------------ Yahoo
    def _yahoo_chart(self, sym: str, rango: str = "1d", intervalo: str = "1m") -> dict:
        err: Exception | None = None
        for host in YAHOO:
            try:
                r = self._get("yahoo", f"{host}/v8/finance/chart/{sym}",
                              params={"range": rango, "interval": intervalo, "includePrePost": "false"})
                res = (r.json().get("chart") or {}).get("result") or []
                if not res:
                    raise LookupError(f"yahoo: sin datos para {sym}")
                return res[0]
            except LookupError:
                raise
            except Exception as e:   # noqa: BLE001 - probar el otro host
                err = e
        raise err or RuntimeError("yahoo")

    @staticmethod
    def _yahoo_points(res: dict) -> list[dict]:
        ts = res.get("timestamp") or []
        q = ((res.get("indicators") or {}).get("quote") or [{}])[0]
        out = []
        for i, t in enumerate(ts):
            c = (q.get("close") or [None] * len(ts))[i]
            if c is None:
                continue
            row = {"ts": float(t), "precio": float(c)}
            for k, name in (("open", "apertura"), ("high", "maximo"), ("low", "minimo"), ("volume", "volumen")):
                v = (q.get(k) or [None] * len(ts))[i]
                if v is not None:
                    row[name] = float(v)
            out.append(row)
        return out

    def _quote_yahoo(self, sym: str) -> dict:
        res = self._yahoo_chart(sym, "1d", "1m")
        m = res.get("meta") or {}
        px = m.get("regularMarketPrice")
        pts = self._yahoo_points(res)
        if px is None and pts:
            px = pts[-1]["precio"]
        if px is None:
            raise LookupError(f"yahoo: sin precio para {sym}")
        prev = m.get("previousClose") or m.get("chartPreviousClose") or px
        return {"precio": float(px), "cierre_anterior": float(prev), "ts": float(m.get("regularMarketTime") or time.time()),
                "divisa": m.get("currency") or "", "fuente": "Yahoo Finance",
                "retraso": "tiempo real" if sym.endswith("-EUR") or sym.endswith("-USD") else "≈15 min",
                "puntos": pts[-400:]}

    # ------------------------------------------------------------ respaldos
    def _quote_stooq(self, sym: str) -> dict:
        s = sym.lower()
        for suf, st in STOOQ_SUFFIX.items():
            if sym.upper().endswith(suf):
                s = sym[: -len(suf)].lower() + st
                break
        else:
            if "." not in sym and "-" not in sym:
                s = sym.lower() + ".us"
        r = self._get("stooq", "https://stooq.com/q/l/", params={"s": s, "f": "sd2t2ohlcvp", "h": "", "e": "csv"})
        rows = list(csv.DictReader(io.StringIO(r.text)))
        if not rows or rows[0].get("Close") in (None, "", "N/D"):
            raise LookupError(f"stooq: sin datos para {sym}")
        row = rows[0]
        px = float(row["Close"])
        prev = row.get("Prev") or row.get("Open") or px
        try:
            prev = float(prev)
        except ValueError:
            prev = px
        return {"precio": px, "cierre_anterior": prev, "ts": time.time(), "divisa": "", "fuente": "Stooq",
                "retraso": "≈15 min", "puntos": []}

    def _quote_crypto(self, sym: str) -> dict:
        try:
            r = self._get("coinbase", f"https://api.coinbase.com/v2/prices/{sym}/spot")
            px = float(r.json()["data"]["amount"])
            return {"precio": px, "cierre_anterior": px, "ts": time.time(), "divisa": sym.split("-")[-1],
                    "fuente": "Coinbase", "retraso": "tiempo real", "puntos": []}
        except Exception:   # noqa: BLE001
            pair = KRAKEN_PAIR.get(sym.upper())
            if not pair:
                raise
            r = self._get("kraken", "https://api.kraken.com/0/public/Ticker", params={"pair": pair})
            data = next(iter((r.json().get("result") or {}).values()))
            return {"precio": float(data["c"][0]), "cierre_anterior": float(data["o"]), "ts": time.time(),
                    "divisa": "EUR", "fuente": "Kraken", "retraso": "tiempo real", "puntos": []}

    # ------------------------------------------------------------ API publica
    def quote(self, sym: str, max_age: float | None = None) -> dict | None:
        """Ultima cotizacion real de ``sym`` (cache de ``ttl`` segundos) o ``None`` si no hay ninguna fuente."""
        sym = sym.upper()
        age = self.ttl if max_age is None else max_age
        with self._lock:
            hit = self._quotes.get(sym)
        if hit and time.time() - hit[0] < age:
            return hit[1]
        crypto = bool(re.match(r"^[A-Z0-9]+-(EUR|USD)$", sym))
        chain = [self._quote_yahoo] + ([self._quote_crypto] if crypto else [self._quote_stooq])
        for fn in chain:
            try:
                q = fn(sym)
            except Exception:   # noqa: BLE001 - siguiente fuente
                continue
            q["simbolo"] = sym
            with self._lock:
                self._quotes[sym] = (time.time(), q)
            self.ultima_fuente, self.ultimo_ok = q["fuente"], time.time()
            return q
        return hit[1] if hit else None

    def quotes(self, syms: list[str], max_age: float | None = None, workers: int = 6) -> dict[str, dict]:
        """Varias cotizaciones en paralelo."""
        out: dict[str, dict] = {}
        with ThreadPoolExecutor(max_workers=max(1, min(workers, len(syms) or 1))) as ex:
            for s, q in zip(syms, ex.map(lambda s: self.quote(s, max_age), syms)):
                if q:
                    out[s.upper()] = q
        return out

    def serie(self, sym: str, rango: str = "1d") -> list[dict]:
        """Serie real (velas) para graficos y para el analisis del agente: [{ts, precio, apertura, maximo...}]."""
        sym = sym.upper()
        rango = rango if rango in RANGOS else "1d"
        ttl = 60 if rango in ("1d", "5d") else 6 * 3600
        key = (sym, rango)
        with self._lock:
            hit = self._series.get(key)
        if hit and time.time() - hit[0] < ttl:
            return hit[1]
        try:
            pts = self._yahoo_points(self._yahoo_chart(sym, *RANGOS[rango]))
        except Exception:   # noqa: BLE001
            return hit[1] if hit else []
        with self._lock:
            self._series[key] = (time.time(), pts)
        return pts

    def fx(self, divisa: str) -> float | None:
        """Unidades de ``divisa`` por 1 € (EURUSD=X de Yahoo o el BCE), ``None`` sin conexion."""
        divisa = divisa.upper()
        if divisa == "EUR":
            return 1.0
        with self._lock:
            hit = self._fx.get(divisa)
        if hit and time.time() - hit[0] < 300:
            return hit[1]
        rate = None
        try:
            rate = float(self._yahoo_chart(f"EUR{divisa}=X", "1d", "5m")["meta"]["regularMarketPrice"])
        except Exception:   # noqa: BLE001
            try:
                r = self._get("bce", "https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml")
                m = re.search(rf"currency=['\"]{divisa}['\"]\s+rate=['\"]([0-9.]+)", r.text)
                rate = float(m.group(1)) if m else None
            except Exception:   # noqa: BLE001
                rate = None
        if rate:
            with self._lock:
                self._fx[divisa] = (time.time(), rate)
            return rate
        return hit[1] if hit else None


_PROV: DatosMercado | None = None
_PLOCK = threading.Lock()


def proveedor() -> DatosMercado:
    global _PROV
    with _PLOCK:
        if _PROV is None:
            _PROV = DatosMercado()
        return _PROV
