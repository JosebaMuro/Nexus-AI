"""Cotizaciones reales para la mesa de bolsa (la mesa sigue siendo SIEMPRE una simulacion: nunca hay dinero ni
ordenes reales; lo real son los precios).

Fuentes, por orden, sin clave ni registro:

* Yahoo Finance (``/v8/finance/chart``): acciones, ETF, cripto y divisas con los mismos simbolos del catalogo
  (SAN.MC, AAPL, VWCE.DE, BTC-EUR, EURUSD=X). Retraso tipico de 15 min en bolsa y tiempo real en cripto.
* Google Finance (pagina de cotizacion, atributo ``data-last-price``): respaldo para acciones y ETF de todas las bolsas.
* Stooq (CSV): otro respaldo para acciones y ETF.
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
GOOGLE_EXCHANGE = {".MC": "BME", ".DE": "ETR", ".PA": "EPA", ".AS": "AMS", ".L": "LON", ".MI": "BIT", ".SW": "SWX"}
US_EXCHANGE = {"NASDAQ": "NASDAQ", "NYSE": "NYSE"}
CNBC_SUFFIX = {".MC": "-ES", ".DE": "-DE", ".PA": "-FR", ".AS": "-NL", ".L": "-GB", ".MI": "-IT", ".SW": "-CH"}
CNBC = "https://quote.cnbc.com/quote-html-webservice/restQuote/symbolType/symbol"
KRAKEN_PAIR = {"BTC-EUR": "XBTEUR", "ETH-EUR": "ETHEUR", "SOL-EUR": "SOLEUR", "ADA-EUR": "ADAEUR"}
# rango -> (rango Yahoo, intervalo Yahoo)
CNBC_CHART = "https://ts-api.cnbc.com/harmony/app/charts/{rango}.json"
CNBC_RANGO = {"1d": "1D", "5d": "5D", "1mo": "1M", "6mo": "6M", "1y": "1Y", "5y": "5Y"}
# rango -> (granularidad de Coinbase en s, intervalo de Kraken en min, segundos que cubre): velas de cripto
CRIPTO_VELAS = {"1d": (300, 5, 86400), "5d": (3600, 30, 5 * 86400), "1mo": (21600, 240, 30 * 86400),
                "6mo": (86400, 1440, 182 * 86400), "1y": (86400, 1440, 365 * 86400), "5y": (None, 10080, 5 * 365 * 86400)}
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
        self.fuentes = {n: Fuente(n) for n in ("yahoo", "cnbc", "google", "stooq", "coinbase", "kraken", "bce")}
        self.mercado_de: dict[str, str] = {}   # simbolo -> mercado del catalogo (para Google Finance en EE. UU.)
        self.ultima_fuente = ""
        self.ultimo_ok = 0.0
        self.ultimo_http: dict[str, int] = {}

    # ------------------------------------------------------------ utilidades
    def estado(self) -> dict:
        now = time.time()
        return {"fuentes": {n: {"disponible": f.disponible(), "fallos": f.fallos,
                                "reintento_s": max(0, round(f.hasta - now))} for n, f in self.fuentes.items()},
                "en_linea": now - self.ultimo_ok < 300,
                "ultima_fuente": self.ultima_fuente, "cotizaciones": len(self._quotes), "http": dict(self.ultimo_http)}

    def _get(self, fuente: str, url: str, **kw) -> httpx.Response:
        f = self.fuentes[fuente]
        if not f.disponible():
            raise RuntimeError(f"{fuente} apartada")
        try:
            r = self.client.get(url, **kw)
            self.ultimo_http[fuente] = r.status_code
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

    @staticmethod
    def _cnbc_symbol(sym: str) -> str:
        for suf, cs in CNBC_SUFFIX.items():
            if sym.upper().endswith(suf):
                return sym[: -len(suf)].upper() + cs
        return sym.upper()

    @staticmethod
    def _num(v) -> float | None:
        try:
            return float(str(v).replace(",", "").replace("%", "").strip())
        except (TypeError, ValueError):
            return None

    def _quote_cnbc(self, sym: str) -> dict:
        """CNBC (acciones y ETF de EE. UU. y Europa): responde tambien desde servidores donde Yahoo limita."""
        r = self._get("cnbc", CNBC, params={"symbols": self._cnbc_symbol(sym), "requestMethod": "itv", "noform": "1",
                                             "partnerId": "2", "fund": "1", "exthrs": "1", "output": "json"})
        q = (((r.json().get("FormattedQuoteResult") or {}).get("FormattedQuote")) or [{}])[0]
        px = self._num(q.get("last"))
        if not px or px <= 0:
            raise LookupError(f"cnbc: sin precio para {sym}")
        prev = self._num(q.get("previous_day_closing"))
        if not prev:   # sin cierre anterior: se deduce de la variacion
            ch, pct = self._num(q.get("change")), self._num(q.get("change_pct"))
            prev = px - ch if ch is not None else px / (1 + pct / 100) if pct not in (None, -100) else px
        return {"precio": px, "cierre_anterior": prev, "ts": time.time(), "divisa": q.get("currencyCode") or "",
                "fuente": "CNBC", "retraso": "≈15 min" if q.get("realTime") != "true" else "tiempo real", "puntos": []}

    def _cnbc_points(self, sym: str, rango: str) -> list[dict]:
        r = self._get("cnbc", CNBC_CHART.format(rango=CNBC_RANGO.get(rango, "1D")), params={"symbol": self._cnbc_symbol(sym)})
        out = []
        for b in ((r.json().get("barData") or {}).get("priceBars") or []):
            c, t = self._num(b.get("close")), self._num(b.get("tradeTimeinMills"))
            if not c or not t:
                continue
            row = {"ts": t / 1000, "precio": c}
            for k, name in (("open", "apertura"), ("high", "maximo"), ("low", "minimo"), ("volume", "volumen")):
                v = self._num(b.get(k))
                if v is not None:
                    row[name] = v
            out.append(row)
        return out

    def _google_symbol(self, sym: str) -> str:
        for suf, ex in GOOGLE_EXCHANGE.items():
            if sym.upper().endswith(suf):
                return f"{sym[: -len(suf)].upper()}:{ex}"
        if re.match(r"^[A-Z0-9]+-(EUR|USD)$", sym.upper()):
            return sym.upper()
        return f"{sym.upper()}:{US_EXCHANGE.get(self.mercado_de.get(sym.upper(), ''), 'NASDAQ')}"

    def _quote_google(self, sym: str) -> dict:
        gs = self._google_symbol(sym)
        r = self._get("google", f"https://www.google.com/finance/quote/{gs}", params={"hl": "en"},
                      cookies={"CONSENT": "YES+cb", "SOCS": "CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"})
        m = re.search(r'data-last-price="([0-9.]+)"', r.text)
        if not m and ":NASDAQ" in gs:   # puede ser de la NYSE
            r = self._get("google", f"https://www.google.com/finance/quote/{gs.replace(':NASDAQ', ':NYSE')}",
                          params={"hl": "en"}, cookies={"CONSENT": "YES+cb"})
            m = re.search(r'data-last-price="([0-9.]+)"', r.text)
        if not m:
            raise LookupError(f"google: sin precio para {sym}")
        px = float(m.group(1))
        prev = re.search(r'Previous close</div>.*?<div[^>]*>[^0-9]*([0-9][0-9,]*\.?[0-9]*)', r.text, re.S)
        cur = re.search(r'data-currency-code="([A-Z]{3})"', r.text)
        try:
            prevf = float(prev.group(1).replace(",", "")) if prev else px
        except ValueError:
            prevf = px
        return {"precio": px, "cierre_anterior": prevf, "ts": time.time(), "divisa": cur.group(1) if cur else "",
                "fuente": "Google Finance", "retraso": "≈15 min", "puntos": []}

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

    def _crypto_points(self, sym: str, rango: str) -> list[dict]:
        """Velas de cripto de Coinbase o Kraken (respaldo de Yahoo, que limita mucho desde servidores)."""
        gran, interval, span = CRIPTO_VELAS.get(rango, CRIPTO_VELAS["1d"])
        desde = time.time() - span
        if gran and span / gran <= 300:     # Coinbase da como mucho 300 velas: [ts, min, max, apertura, cierre, vol]
            try:
                r = self._get("coinbase", f"https://api.exchange.coinbase.com/products/{sym}/candles",
                              params={"granularity": gran})
                rows = sorted((v for v in r.json() if isinstance(v, list) and len(v) >= 6 and v[0] >= desde),
                              key=lambda v: v[0])
                if rows:
                    return [{"ts": float(t), "precio": float(c), "apertura": float(o), "maximo": float(h),
                             "minimo": float(lo), "volumen": float(vol)} for t, lo, h, o, c, vol, *_ in rows]
            except Exception:   # noqa: BLE001 - Kraken
                pass
        pair = KRAKEN_PAIR.get(sym)
        if not pair:
            return []
        r = self._get("kraken", "https://api.kraken.com/0/public/OHLC", params={"pair": pair, "interval": interval})
        res = r.json().get("result") or {}
        rows = next((v for k, v in res.items() if k != "last" and isinstance(v, list)), [])
        return [{"ts": float(v[0]), "precio": float(v[4]), "apertura": float(v[1]), "maximo": float(v[2]),
                 "minimo": float(v[3]), "volumen": float(v[6])} for v in rows if len(v) >= 7 and v[0] >= desde]

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
        chain = [self._quote_yahoo] + ([self._quote_crypto] if crypto else [self._quote_cnbc, self._quote_google, self._quote_stooq])
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
            pts = []
        if not pts:
            crypto = bool(re.match(r"^[A-Z0-9]+-(EUR|USD)$", sym))
            try:            # respaldo: velas de Coinbase/Kraken para cripto y de CNBC para el resto
                pts = self._crypto_points(sym, rango) if crypto else self._cnbc_points(sym, rango)
            except Exception:   # noqa: BLE001
                pts = []
        if not pts:
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
