"""Datos de mercado para los agentes y para el agente de bolsa, con fuentes publicas y sin clave.

Reune lo que hacen tres terminales abiertos (todos con licencia MIT; aqui esta reescrito en Python, sin su codigo):

* OpenTerminal (ErTasselli): cadena de fuentes con respaldo para cada dato (Nasdaq -> Yahoo -> Stooq; CoinGecko ->
  Binance; TradingView para fundamentales, criba y sectores; FRED para tipos; Forex Factory para el calendario) y
  cache que sirve el ultimo dato bueno si la fuente se cae.
* Open Terminal (spovalova): «recetas» con nombre (cotizacion, velas, noticias, opciones, criba...) que devuelven
  marcos (columnas y filas) y comprobacion de que las cifras de una respuesta salen de los datos consultados.
* OpenTerminalUI (Hitheshkaranth): procedencia de cada dato (en vivo, retrasado, de cache) y campos vacios con motivo.

Los precios vienen de servicios publicos: pueden llegar con retraso o estar mal. Sirven para estudiar, no para
operar. La mesa de bolsa de Agente IA sigue siendo una simulacion.
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import re
import threading
import time
import urllib.parse

from .. import log

L = log.get("mercados")
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 "
      "Safari/537.36")
NASDAQ = "https://api.nasdaq.com/api"
TV_SCAN = "https://scanner.tradingview.com"
# sufijo de Yahoo -> (bolsa de TradingView, mercado de la criba, pais)
BOLSAS = {".MC": ("BME", "spain", "ES"), ".DE": ("XETR", "germany", "DE"), ".PA": ("EURONEXT", "france", "FR"),
          ".AS": ("EURONEXT", "netherlands", "NL"), ".L": ("LSE", "uk", "GB"), ".MI": ("MIL", "italy", "IT"),
          ".SW": ("SIX", "switzerland", "CH"), ".LS": ("EURONEXT", "portugal", "PT"), ".BR": ("EURONEXT", "belgium", "BE"),
          ".T": ("TSE", "japan", "JP"), ".HK": ("HKEX", "hongkong", "HK"), ".TO": ("TSX", "canada", "CA"),
          ".AX": ("ASX", "australia", "AU"), ".SA": ("BMFBOVESPA", "brazil", "BR"), ".MX": ("BMV", "mexico", "MX")}
MERCADOS_TV = {"US": "america", "ES": "spain", "DE": "germany", "FR": "france", "GB": "uk", "IT": "italy",
               "NL": "netherlands", "CH": "switzerland", "PT": "portugal", "JP": "japan", "CA": "canada",
               "AU": "australia", "BR": "brazil", "MX": "mexico", "IN": "india", "CN": "china", "KR": "korea",
               "SE": "sweden", "BE": "belgium", "AR": "argentina", "CL": "chile", "CO": "colombia", "HK": "hongkong"}
INDICES = {"^GSPC": ("S&P 500", "US"), "^NDX": ("Nasdaq 100", "US"), "^DJI": ("Dow Jones", "US"),
           "^IBEX": ("IBEX 35", "ES"), "^STOXX50E": ("Euro Stoxx 50", "EU"), "^GDAXI": ("DAX", "DE"),
           "^FCHI": ("CAC 40", "FR"), "^FTSE": ("FTSE 100", "GB"), "^N225": ("Nikkei 225", "JP"),
           "^HSI": ("Hang Seng", "HK"), "^VIX": ("Volatilidad VIX", "US"), "GC=F": ("Oro", ""),
           "CL=F": ("Petróleo WTI", ""), "BZ=F": ("Petróleo Brent", ""), "EURUSD=X": ("Euro/dólar", "EU"),
           "BTC-USD": ("Bitcoin", ""), "ETH-USD": ("Ether", "")}
SIMBOLOS_FABRICA = ["^GSPC", "^NDX", "^IBEX", "^STOXX50E", "^GDAXI", "^FTSE", "^N225", "^VIX", "GC=F", "BZ=F",
                    "EURUSD=X", "BTC-USD"]
CURVA = [("DGS1MO", "1 mes", 1 / 12), ("DGS3MO", "3 meses", 0.25), ("DGS6MO", "6 meses", 0.5), ("DGS1", "1 año", 1),
         ("DGS2", "2 años", 2), ("DGS5", "5 años", 5), ("DGS10", "10 años", 10), ("DGS30", "30 años", 30)]
TV_COLUMNAS = ["name", "description", "close", "change", "volume", "market_cap_basic", "price_earnings_ttm",
               "earnings_per_share_basic_ttm", "dividend_yield_recent", "sector", "industry", "beta_1_year",
               "price_52_week_high", "price_52_week_low", "Recommend.All", "RSI", "Perf.W", "Perf.1M", "Perf.3M",
               "Perf.YTD", "Perf.Y", "relative_volume_10d_calc", "return_on_equity", "debt_to_equity",
               "price_book_fq", "gross_margin", "operating_margin", "earnings_release_next_date", "exchange",
               "currency", "Perf.6M", "after_tax_margin", "current_ratio",
               "earnings_per_share_diluted_yoy_growth_ttm"]
TV_NOMBRES = {"name": "simbolo", "description": "nombre", "close": "precio", "change": "cambio_pct",
              "volume": "volumen", "market_cap_basic": "capitalizacion", "price_earnings_ttm": "per",
              "earnings_per_share_basic_ttm": "bpa", "dividend_yield_recent": "rentabilidad_dividendo_pct",
              "sector": "sector", "industry": "industria", "beta_1_year": "beta", "price_52_week_high": "max_52s",
              "price_52_week_low": "min_52s", "Recommend.All": "recomendacion_tecnica", "RSI": "rsi",
              "Perf.W": "semana_pct", "Perf.1M": "mes_pct", "Perf.3M": "trimestre_pct", "Perf.YTD": "anio_en_curso_pct",
              "Perf.Y": "anio_pct", "relative_volume_10d_calc": "volumen_relativo", "return_on_equity": "roe_pct",
              "debt_to_equity": "deuda_sobre_fondos_propios", "price_book_fq": "precio_valor_contable",
              "gross_margin": "margen_bruto_pct", "operating_margin": "margen_operativo_pct",
              "earnings_release_next_date": "proximos_resultados", "exchange": "bolsa", "currency": "divisa",
              "Perf.6M": "semestre_pct", "after_tax_margin": "margen_neto_pct", "current_ratio": "ratio_corriente",
              "earnings_per_share_diluted_yoy_growth_ttm": "crecimiento_bpa_pct"}
_TV_INV = {v: k for k, v in TV_NOMBRES.items()}
_OPS = {"mayor": "greater", ">": "greater", "menor": "less", "<": "less", "igual": "equal", "=": "equal",
        "==": "equal", "mayor_igual": "egreater", ">=": "egreater", "menor_igual": "eless", "<=": "eless",
        "entre": "in_range", "distinto": "nequal", "!=": "nequal", "contiene": "match"}


def _num(v, defecto=None):
    try:
        if v is None:
            return defecto
        s = str(v).strip().replace("$", "").replace("%", "").replace(",", "").replace("+", "")
        if s in ("", "N/A", "NA", "--", "-", "."):
            return defecto
        mult = 1.0
        if s[-1:] in "KMBT" and len(s) > 1 and s[:-1].replace(".", "", 1).replace("-", "", 1).isdigit():
            mult = {"K": 1e3, "M": 1e6, "B": 1e9, "T": 1e12}[s[-1]]
            s = s[:-1]
        return float(s) * mult
    except (TypeError, ValueError):
        return defecto


class SinDatos(RuntimeError):
    pass


class Mercados:
    def __init__(self, cliente=None, proveedor=None) -> None:
        self._cliente = cliente
        self._proveedor = proveedor
        self._mu = threading.Lock()
        self._cache: dict[str, tuple[float, object]] = {}
        self.procedencia: dict[str, dict] = {}   # receta/clave -> {"fuente", "ts", "de_cache"}

    # ------------------------------------------------------------ red y cache
    def _http(self):
        if self._cliente is None:
            import httpx
            self._cliente = httpx.Client(timeout=12, follow_redirects=True,
                                         headers={"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9,es;q=0.8"})
        return self._cliente

    def _get(self, url: str, **kw):
        r = self._http().get(url, **kw)
        if r.status_code >= 400:
            raise SinDatos(f"HTTP {r.status_code} de {urllib.parse.urlsplit(url).netloc}")
        return r

    def cotizaciones(self):
        if self._proveedor is None:
            from . import bolsa_datos
            self._proveedor = bolsa_datos.proveedor()
        return self._proveedor

    def con_cache(self, clave: str, ttl: float, fn):
        """Dato de cache si es reciente; si la fuente falla, el ultimo bueno (marcado como antiguo)."""
        ahora = time.time()
        with self._mu:
            previo = self._cache.get(clave)
        if previo and ahora - previo[0] < ttl:
            return previo[1]
        try:
            valor = fn()
        except Exception as e:  # noqa: BLE001
            if previo is not None:
                self.procedencia[clave] = {"de_cache": True, "ts": previo[0], "motivo": str(e)[:120]}
                return previo[1]
            raise
        with self._mu:
            self._cache[clave] = (ahora, valor)
            if len(self._cache) > 600:
                for k in sorted(self._cache, key=lambda k: self._cache[k][0])[:200]:
                    self._cache.pop(k, None)
        self.procedencia[clave] = {"de_cache": False, "ts": ahora}
        return valor

    def _cadena(self, clave: str, ttl: float, pasos: list[tuple[str, object]]):
        """Prueba las fuentes en orden y se queda con la primera que responde."""
        def probar():
            errores = []
            for nombre, fn in pasos:
                try:
                    valor = fn()
                    if valor:
                        return {"fuente": nombre, "valor": valor}
                    errores.append(f"{nombre}: sin datos")
                except Exception as e:  # noqa: BLE001
                    errores.append(f"{nombre}: {type(e).__name__}: {e}"[:140])
            raise SinDatos("; ".join(errores) or "sin fuentes")
        return self.con_cache(clave, ttl, probar)

    # ------------------------------------------------------------ Nasdaq
    def _nasdaq(self, ruta: str, params: dict | None = None) -> dict:
        r = self._get(f"{NASDAQ}/{ruta}", params=params, headers={
            "Accept": "application/json, text/plain, */*", "Origin": "https://www.nasdaq.com",
            "Referer": "https://www.nasdaq.com/"})
        j = r.json()
        datos = j.get("data")
        if not datos:
            motivo = ((j.get("status") or {}).get("bCodeMessage") or [{}])
            raise SinDatos(str((motivo[0] or {}).get("errorMessage") or "Nasdaq no tiene ese dato"))
        return datos

    @staticmethod
    def tablas(datos, ruta: str = "") -> dict[str, list[dict]]:
        """Todas las tablas {headers, rows} que haya dentro de una respuesta de Nasdaq, con su nombre."""
        out: dict[str, list[dict]] = {}
        if isinstance(datos, dict):
            filas = datos.get("rows")
            if isinstance(filas, list) and filas and isinstance(filas[0], dict):
                cab = datos.get("headers") if isinstance(datos.get("headers"), dict) else {}
                out[ruta or "tabla"] = [{str(cab.get(k) or k): v for k, v in f.items() if v not in (None, "")}
                                        for f in filas]
            for k, v in datos.items():
                if k not in ("rows", "headers"):
                    out.update(Mercados.tablas(v, f"{ruta}.{k}" if ruta else k))
        return out

    def _nasdaq_tabla(self, ruta: str, params: dict | None = None, prefiero: str = "") -> list[dict]:
        t = self.tablas(self._nasdaq(ruta, params))
        if not t:
            raise SinDatos("sin filas")
        if prefiero:
            for nombre, filas in t.items():
                if prefiero.lower() in nombre.lower():
                    return filas
        return max(t.values(), key=len)

    def _clase(self, sym: str) -> str:
        return "index" if sym.startswith("^") else "stocks"

    def _cot_nasdaq(self, sym: str) -> dict:
        if "." in sym or "=" in sym or "-" in sym or sym.startswith("^"):
            raise SinDatos("Nasdaq solo tiene valores de Estados Unidos")
        try:
            d = self._nasdaq(f"quote/{sym}/info", {"assetclass": "stocks"})
        except SinDatos:
            d = self._nasdaq(f"quote/{sym}/info", {"assetclass": "etf"})
        p = d.get("primaryData") or {}
        precio = _num(p.get("lastSalePrice"))
        if precio is None:
            raise SinDatos("sin precio")
        est = d.get("keyStats") or {}
        return {"simbolo": sym, "nombre": d.get("companyName") or sym, "precio": precio, "divisa": "USD",
                "cambio": _num(p.get("netChange")), "cambio_pct": _num(p.get("percentageChange")),
                "compra": _num(str(p.get("bidPrice") or "").split("*")[0]),
                "venta": _num(str(p.get("askPrice") or "").split("*")[0]), "volumen": _num(p.get("volume")),
                "bolsa": d.get("exchange") or "", "estado_mercado": d.get("marketStatus") or "",
                "rango_dia": ((est.get("dayrange") or {}).get("value")) or "",
                "rango_52s": ((est.get("fiftyTwoWeekHighLow") or {}).get("value")) or "",
                "tiempo_real": bool(p.get("isRealTime")), "pais": "US"}

    def _cot_basica(self, sym: str) -> dict:
        q = self.cotizaciones().quote(sym)
        if not q:
            raise SinDatos("sin cotización")
        ant = _num(q.get("cierre_anterior") or q.get("previous_close"))
        precio = _num(q.get("precio") or q.get("price"))
        out = {"simbolo": sym, "nombre": q.get("nombre") or INDICES.get(sym, (sym,))[0], "precio": precio,
               "divisa": q.get("divisa") or "", "cambio_pct": _num(q.get("cambio_pct")),
               "volumen": _num(q.get("volumen")), "pais": pais_de_simbolo(sym)}
        if out["cambio_pct"] is None and ant and precio is not None:
            out["cambio_pct"] = round((precio / ant - 1) * 100, 3)
        out["fuente_precio"] = q.get("fuente") or ""
        return out

    def cotizacion(self, sym: str) -> dict:
        sym = sym.strip().upper()
        r = self._cadena(f"cot:{sym}", 20, [("Nasdaq", lambda: self._cot_nasdaq(sym)),
                                            ("Yahoo, Stooq u otra", lambda: self._cot_basica(sym))])
        return {**r["valor"], "fuente": r["fuente"] if r["fuente"] == "Nasdaq" else
                (r["valor"].get("fuente_precio") or r["fuente"])}

    def resumen_valor(self, sym: str) -> dict:
        """Ficha del valor en Nasdaq: sector, PER, BPA, dividendo, objetivo a un año, beta."""
        sym = sym.strip().upper()
        d = self.con_cache(f"res:{sym}", 3600, lambda: self._nasdaq(f"quote/{sym}/summary", {"assetclass": "stocks"}))
        return {str(v.get("label") or k): v.get("value") for k, v in (d.get("summaryData") or {}).items()
                if isinstance(v, dict) and v.get("value") not in (None, "", "N/A")}

    # ------------------------------------------------------------ TradingView
    def _tv(self, mercado: str, cuerpo: dict) -> list[dict]:
        columnas = list(cuerpo.get("columns") or TV_COLUMNAS)
        for _ in range(8):   # si TradingView no conoce una columna, se quita y se repite
            cuerpo = {**cuerpo, "columns": columnas}
            r = self._http().post(f"{TV_SCAN}/{mercado}/scan", json=cuerpo, headers={
                "Origin": "https://www.tradingview.com", "Referer": "https://www.tradingview.com/"})
            if r.status_code == 400:
                m = re.search(r'[Uu]nknown field "?([\w.|]+)"?', r.text)
                if m and m.group(1) in columnas and len(columnas) > 2:
                    columnas.remove(m.group(1))
                    continue
            if r.status_code >= 400:
                raise SinDatos(f"TradingView HTTP {r.status_code}: {r.text[:120]}")
            filas = []
            for x in r.json().get("data") or []:
                fila = {"_tv": x.get("s", "")}
                for c, v in zip(columnas, x.get("d") or []):
                    fila[TV_NOMBRES.get(c, c)] = round(v, 4) if isinstance(v, float) else v
                filas.append(fila)
            return filas
        raise SinDatos("TradingView rechaza las columnas")

    def fundamentales(self, sym: str) -> dict:
        sym = sym.strip().upper()

        def pedir():
            suf = next((s for s in BOLSAS if sym.endswith(s)), "")
            if suf:
                bolsa, mercado, _ = BOLSAS[suf]
                filas = self._tv("global", {"symbols": {"tickers": [f"{bolsa}:{sym[:-len(suf)]}"]}})
            else:
                filas = self._tv("america", {"filter": [{"left": "name", "operation": "equal", "right": sym}],
                                             "options": {"lang": "es"}, "range": [0, 3]})
            if not filas:
                raise SinDatos("TradingView no conoce el valor")
            f = {k: v for k, v in filas[0].items() if v is not None and k != "_tv"}
            if f.get("proximos_resultados"):
                f["proximos_resultados"] = time.strftime("%Y-%m-%d", time.gmtime(float(f["proximos_resultados"])))
            return f
        return self.con_cache(f"fun:{sym}", 900, pedir)

    def cribar(self, mercado: str = "america", filtros: list[dict] | None = None, orden: str = "capitalizacion",
               descendente: bool = True, limite: int = 40, columnas: list[str] | None = None) -> list[dict]:
        mercado = MERCADOS_TV.get(mercado.upper(), mercado.lower()) if mercado else "america"
        filtro = [{"left": "type", "operation": "equal", "right": "stock"}]
        for f in filtros or []:
            campo = _TV_INV.get(str(f.get("campo", "")), str(f.get("campo", "")))
            op = _OPS.get(str(f.get("op", "")).lower())
            if not campo or not op:
                raise ValueError(f"filtro no válido: {f}")
            filtro.append({"left": campo, "operation": op, "right": f.get("valor")})
        cols = [_TV_INV.get(c, c) for c in columnas] if columnas else TV_COLUMNAS
        cuerpo = {"filter": filtro, "options": {"lang": "es"}, "columns": cols,
                  "sort": {"sortBy": _TV_INV.get(orden, orden), "sortOrder": "desc" if descendente else "asc"},
                  "range": [0, max(1, min(int(limite), 500))]}
        clave = f"criba:{mercado}:{filtro}:{orden}:{descendente}:{limite}:{cols}"
        return self.con_cache(clave, 120, lambda: [{k: v for k, v in f.items() if k != "_tv"}
                                                   for f in self._tv(mercado, cuerpo)])

    def sectores(self, mercado: str = "america", n: int = 600) -> list[dict]:
        """Mapa de sectores: capitalizacion y cambio del dia ponderado por capitalizacion."""
        filas = self.cribar(mercado, limite=n, columnas=["simbolo", "sector", "capitalizacion", "cambio_pct"])
        grupos: dict[str, dict] = {}
        for f in filas:
            cap, cambio = _num(f.get("capitalizacion")), _num(f.get("cambio_pct"))
            if not cap or cambio is None or not f.get("sector"):
                continue
            g = grupos.setdefault(f["sector"], {"cap": 0.0, "suma": 0.0, "n": 0, "mejor": None, "peor": None})
            g["cap"] += cap
            g["suma"] += cap * cambio
            g["n"] += 1
            if g["mejor"] is None or cambio > g["mejor"][1]:
                g["mejor"] = (f["simbolo"], cambio)
            if g["peor"] is None or cambio < g["peor"][1]:
                g["peor"] = (f["simbolo"], cambio)
        out = [{"sector": s, "valores": g["n"], "capitalizacion": round(g["cap"]),
                "cambio_pct": round(g["suma"] / g["cap"], 3), "mejor": f"{g['mejor'][0]} ({g['mejor'][1]:+.2f} %)",
                "peor": f"{g['peor'][0]} ({g['peor'][1]:+.2f} %)"} for s, g in grupos.items() if g["cap"]]
        out.sort(key=lambda x: -x["cambio_pct"])
        return out

    def buscar(self, texto: str) -> list[dict]:
        def tv():
            r = self._get("https://symbol-search.tradingview.com/symbol_search/v3/", params={
                "text": texto, "hl": "0", "lang": "es", "domain": "production"},
                headers={"Origin": "https://www.tradingview.com", "Referer": "https://www.tradingview.com/"})
            return [{"simbolo": x.get("symbol"), "nombre": x.get("description"), "tipo": x.get("type"),
                     "bolsa": x.get("exchange"), "divisa": x.get("currency_code"), "pais": x.get("country")}
                    for x in (r.json().get("symbols") or [])[:12]]

        def yahoo():
            r = self._get("https://query2.finance.yahoo.com/v1/finance/search",
                          params={"q": texto, "quotesCount": "10", "newsCount": "0"})
            return [{"simbolo": x.get("symbol"), "nombre": x.get("shortname") or x.get("longname"),
                     "tipo": x.get("typeDisp"), "bolsa": x.get("exchDisp")} for x in r.json().get("quotes") or []]
        r = self._cadena(f"buscar:{texto.lower()}", 3600, [("TradingView", tv), ("Yahoo", yahoo)])
        return r["valor"]

    # ------------------------------------------------------------ velas, opciones, noticias
    def velas(self, sym: str, rango: str = "6mo") -> list[dict]:
        pts = self.cotizaciones().serie(sym.strip().upper(), rango if rango in ("1d", "5d", "1mo", "6mo", "1y", "5y")
                                        else "6mo")
        if not pts:
            raise SinDatos("sin histórico")
        return pts

    def opciones(self, sym: str, vencimiento: str = "", alrededor: int = 8) -> dict:
        """Cadena de opciones (Nasdaq) con la volatilidad implicita y las griegas calculadas aqui."""
        sym = sym.strip().upper()
        d = self.con_cache(f"opc:{sym}", 300, lambda: self._nasdaq(
            f"quote/{sym}/option-chain", {"assetclass": "stocks", "limit": "400", "fromdate": "all", "todate": "undefined",
                                         "excode": "oprac", "callput": "callput", "money": "all", "type": "all"}))
        m = re.search(r"\$?([\d,]+\.\d+)", str(d.get("lastTrade") or ""))
        subyacente = _num(m.group(1)) if m else None
        if subyacente is None:
            try:
                subyacente = self.cotizacion(sym)["precio"]
            except Exception:  # noqa: BLE001
                subyacente = None
        grupos: dict[str, list[dict]] = {}
        actual = ""
        for f in ((d.get("table") or {}).get("rows")) or []:
            if f.get("expirygroup"):
                actual = str(f["expirygroup"])
                continue
            strike = _num(f.get("strike"))
            if strike is None:
                continue
            grupos.setdefault(actual, []).append({
                "strike": strike,
                "call": {"ultimo": _num(f.get("c_Last")), "compra": _num(f.get("c_Bid")), "venta": _num(f.get("c_Ask")),
                         "volumen": _num(f.get("c_Volume")), "interes_abierto": _num(f.get("c_Openinterest"))},
                "put": {"ultimo": _num(f.get("p_Last")), "compra": _num(f.get("p_Bid")), "venta": _num(f.get("p_Ask")),
                        "volumen": _num(f.get("p_Volume")), "interes_abierto": _num(f.get("p_Openinterest"))}})
        if not grupos:
            raise SinDatos("sin cadena de opciones")
        vencimientos = list(grupos)
        elegido = next((v for v in vencimientos if vencimiento and vencimiento.lower() in v.lower()), vencimientos[0])
        filas = sorted(grupos[elegido], key=lambda x: x["strike"])
        if subyacente and alrededor:
            centro = min(range(len(filas)), key=lambda i: abs(filas[i]["strike"] - subyacente))
            filas = filas[max(0, centro - alrededor): centro + alrededor + 1]
        dias = _dias_hasta(elegido)
        if subyacente and dias is not None:
            from . import bolsa_analisis as ba
            t = max(dias, 0.5) / 365.0
            for f in filas:
                for tipo in ("call", "put"):
                    o = f[tipo]
                    precio = ((o["compra"] + o["venta"]) / 2 if o.get("compra") and o.get("venta") else o.get("ultimo"))
                    if not precio:
                        continue
                    vi = ba.volatilidad_implicita(precio, subyacente, f["strike"], t, 0.04, tipo)
                    if vi:
                        g = ba.black_scholes(subyacente, f["strike"], t, 0.04, vi, tipo)
                        o.update(vi_pct=round(vi * 100, 2), delta=g["delta"], gamma=g["gamma"], theta=g["theta"],
                                 vega=g["vega"])
                f["en_dinero"] = "call" if f["strike"] < subyacente else "put"
        return {"simbolo": sym, "subyacente": subyacente, "vencimiento": elegido, "dias": dias,
                "vencimientos": vencimientos[:12], "filas": filas, "fuente": "Nasdaq"}

    def noticias(self, sym: str = "", limite: int = 20, idioma: str = "es") -> list[dict]:
        from ..mundo.fuentes import _fecha, _sin_html, leer_rss

        def leer(url: str, medio: str) -> list[dict]:
            return [{"titulo": _sin_html(x.get("title", ""), 240), "url": x.get("link") or "",
                     "ts": _fecha(x.get("pubdate") or x.get("published") or x.get("updated")),
                     "medio": x.get("source") or medio, "resumen": _sin_html(x.get("description", ""), 240)}
                    for x in leer_rss(self._get(url).text) if x.get("title")]

        def yahoo():
            if not sym:
                raise SinDatos("Yahoo necesita un símbolo")
            return leer("https://feeds.finance.yahoo.com/rss/2.0/headline?" + urllib.parse.urlencode(
                {"s": sym, "region": "US", "lang": "en-US"}), "Yahoo Finance")

        def google():
            q = (sym + " acciones") if sym else ("bolsa mercados" if idioma == "es" else "stock market")
            return leer("https://news.google.com/rss/search?" + urllib.parse.urlencode(
                {"q": q, "hl": idioma, "gl": "ES" if idioma == "es" else "US",
                 "ceid": "ES:es" if idioma == "es" else "US:en"}), "Google Noticias")
        r = self._cadena(f"not:{sym}:{idioma}", 300, [("Yahoo Finance", yahoo), ("Google Noticias", google)])
        vistos, out = set(), []
        for n in sorted(r["valor"], key=lambda x: -(x["ts"] or 0)):
            clave = re.sub(r"\W+", "", n["titulo"].lower())[:60]
            if clave and clave not in vistos:
                vistos.add(clave)
                out.append({**n, "fecha": time.strftime("%Y-%m-%d %H:%M", time.gmtime(n["ts"])) if n["ts"] else ""})
        return out[:limite]

    # ------------------------------------------------------------ cripto, macro, calendario
    def cripto(self, limite: int = 40) -> dict:
        def gecko():
            m = self._get("https://api.coingecko.com/api/v3/coins/markets", params={
                "vs_currency": "usd", "order": "market_cap_desc", "per_page": str(limite), "page": "1",
                "sparkline": "false", "price_change_percentage": "24h,7d"}).json()
            monedas = [{"simbolo": str(x.get("symbol", "")).upper(), "nombre": x.get("name"),
                        "precio_usd": x.get("current_price"), "cambio_24h_pct": x.get("price_change_percentage_24h"),
                        "cambio_7d_pct": x.get("price_change_percentage_7d_in_currency"),
                        "capitalizacion": x.get("market_cap"), "volumen_24h": x.get("total_volume")} for x in m]
            dominio = {}
            try:
                g = self._get("https://api.coingecko.com/api/v3/global").json().get("data") or {}
                dominio = {"btc_pct": round((g.get("market_cap_percentage") or {}).get("btc", 0), 2),
                           "eth_pct": round((g.get("market_cap_percentage") or {}).get("eth", 0), 2),
                           "capitalizacion_total_usd": (g.get("total_market_cap") or {}).get("usd")}
            except Exception:  # noqa: BLE001
                pass
            return {"monedas": monedas, "dominancia": dominio}

        def binance():
            t = self._get("https://api.binance.com/api/v3/ticker/24hr").json()
            usdt = [x for x in t if str(x.get("symbol", "")).endswith("USDT") and _num(x.get("quoteVolume"), 0) > 0]
            usdt.sort(key=lambda x: -_num(x.get("quoteVolume"), 0))
            return {"monedas": [{"simbolo": x["symbol"][:-4], "nombre": x["symbol"][:-4],
                                 "precio_usd": _num(x.get("lastPrice")),
                                 "cambio_24h_pct": _num(x.get("priceChangePercent")),
                                 "volumen_24h": _num(x.get("quoteVolume"))} for x in usdt[:limite]], "dominancia": {}}
        r = self._cadena(f"cripto:{limite}", 120, [("CoinGecko", gecko), ("Binance", binance)])
        return {**r["valor"], "fuente": r["fuente"]}

    def macro(self) -> dict:
        """Curva de tipos del Tesoro de EE. UU. y VIX (FRED, sin clave)."""
        def pedir():
            ids = [c[0] for c in CURVA] + ["VIXCLS"]
            desde = (dt.date.today() - dt.timedelta(days=45)).isoformat()
            texto = self._get("https://fred.stlouisfed.org/graph/fredgraph.csv",
                              params={"id": ",".join(ids), "cosd": desde}).text
            return leer_fred(texto)
        return self.con_cache("macro", 3600, pedir)

    def calendario(self, pais: str = "", importancia: str = "") -> list[dict]:
        def pedir():
            return leer_calendario(self._get("https://nfs.faireconomy.media/ff_calendar_thisweek.json").json())
        filas = self.con_cache("calendario", 1800, pedir)
        if pais:
            from ..mundo import paises
            c = paises.codigo(pais)
            filas = [f for f in filas if f["pais"] == c]
        if importancia:
            filas = [f for f in filas if f["importancia"] == importancia.lower()]
        return filas

    # ------------------------------------------------------------ recetas de Nasdaq
    def resultados_dia(self, fecha: str = "") -> list[dict]:
        fecha = fecha or dt.date.today().isoformat()
        return self.con_cache(f"resdia:{fecha}", 3600,
                              lambda: self._nasdaq_tabla("calendar/earnings", {"date": fecha}))

    def historial_bpa(self, sym: str) -> list[dict]:
        return self.con_cache(f"bpa:{sym}", 21600, lambda: self._nasdaq_tabla(
            f"company/{sym.upper()}/earnings-surprise", prefiero="earningsSurprise"))

    def dividendos(self, sym: str) -> list[dict]:
        return self.con_cache(f"div:{sym}", 21600, lambda: self._nasdaq_tabla(
            f"quote/{sym.upper()}/dividends", {"assetclass": "stocks"}, prefiero="dividends"))

    def internos(self, sym: str) -> list[dict]:
        return self.con_cache(f"int:{sym}", 21600, lambda: self._nasdaq_tabla(
            f"company/{sym.upper()}/insider-trades", {"limit": "20", "type": "ALL", "sortColumn": "lastDate",
                                                      "sortOrder": "DESC"}, prefiero="transactionTable"))

    def instituciones(self, sym: str) -> list[dict]:
        return self.con_cache(f"ins:{sym}", 21600, lambda: self._nasdaq_tabla(
            f"company/{sym.upper()}/institutional-holdings", {"limit": "20", "type": "TOTAL",
                                                              "sortColumn": "marketValue", "sortOrder": "DESC"},
            prefiero="holdingsTransactions"))

    def cortos(self, sym: str) -> list[dict]:
        return self.con_cache(f"cor:{sym}", 21600, lambda: self._nasdaq_tabla(
            f"quote/{sym.upper()}/short-interest", {"assetClass": "stocks"}, prefiero="shortInterest"))

    def estimaciones(self, sym: str) -> dict:
        def pedir():
            out = {}
            try:
                c = self._nasdaq(f"analyst/{sym.upper()}/targetprice").get("consensusOverview") or {}
                out["consenso"] = {k: v for k, v in c.items() if v not in (None, "")}
            except Exception as e:  # noqa: BLE001
                out["consenso_error"] = str(e)[:100]
            try:
                out["previsiones"] = self.tablas(self._nasdaq(f"analyst/{sym.upper()}/earnings-forecast"))
            except Exception as e:  # noqa: BLE001
                out["previsiones_error"] = str(e)[:100]
            if not out.get("consenso") and not out.get("previsiones"):
                raise SinDatos("sin estimaciones")
            return out
        return self.con_cache(f"est:{sym}", 21600, pedir)

    def financieros(self, sym: str, trimestral: bool = False) -> dict:
        return self.con_cache(f"fin:{sym}:{trimestral}", 43200, lambda: self.tablas(self._nasdaq(
            f"company/{sym.upper()}/financials", {"frequency": "2" if trimestral else "1"})))

    def salidas_bolsa(self, mes: str = "") -> dict:
        mes = mes or dt.date.today().strftime("%Y-%m")
        return self.con_cache(f"ipo:{mes}", 21600, lambda: self.tablas(self._nasdaq("ipo/calendar", {"date": mes})))

    def movimientos(self) -> dict:
        return self.con_cache("mov", 300, lambda: self.tablas(self._nasdaq("marketmovers", {"assetclass": "stocks"})))


# ------------------------------------------------------------------ analizadores puros
def leer_fred(texto: str) -> dict:
    filas = list(csv.DictReader(io.StringIO(texto)))
    if not filas:
        raise SinDatos("FRED sin datos")
    campo_fecha = next((k for k in filas[0] if "date" in k.lower()), None)
    ultimo: dict[str, tuple[str, float]] = {}
    previo: dict[str, float] = {}
    for f in filas:
        for k, v in f.items():
            x = _num(v)
            if k == campo_fecha or x is None:
                continue
            if k in ultimo:
                previo[k] = ultimo[k][1]
            ultimo[k] = (f.get(campo_fecha, ""), x)
    curva = [{"plazo": nombre, "anios": anios, "tipo_pct": ultimo[sid][1], "fecha": ultimo[sid][0],
              "cambio": round(ultimo[sid][1] - previo[sid], 3) if sid in previo else None}
             for sid, nombre, anios in CURVA if sid in ultimo]
    d2 = next((c["tipo_pct"] for c in curva if c["anios"] == 2), None)
    d10 = next((c["tipo_pct"] for c in curva if c["anios"] == 10), None)
    return {"curva": curva, "diferencial_10a_2a": round(d10 - d2, 3) if d2 is not None and d10 is not None else None,
            "curva_invertida": bool(d2 is not None and d10 is not None and d10 < d2),
            "vix": ultimo.get("VIXCLS", (None, None))[1], "fuente": "FRED"}


def leer_calendario(datos: list) -> list[dict]:
    from ..mundo import paises
    from ..mundo.fuentes import _fecha
    out = []
    for e in datos if isinstance(datos, list) else []:
        ts = _fecha(e.get("date"))
        moneda = str(e.get("country") or "").upper()
        out.append({"ts": ts, "fecha": time.strftime("%Y-%m-%d %H:%M", time.gmtime(ts)) + " UTC" if ts else "",
                    "pais": paises.MONEDAS.get(moneda, ""), "moneda": moneda, "dato": e.get("title") or "",
                    "importancia": {"high": "alta", "medium": "media", "low": "baja", "holiday": "festivo"}.get(
                        str(e.get("impact") or "").lower(), str(e.get("impact") or "").lower()),
                    "prevision": e.get("forecast") or "", "anterior": e.get("previous") or "",
                    "real": e.get("actual") or ""})
    out.sort(key=lambda x: x["ts"] or 0)
    return out


def _dias_hasta(texto: str) -> float | None:
    for fmt in ("%B %d, %Y", "%b %d, %Y", "%Y-%m-%d", "%B %d %Y"):
        try:
            d = dt.datetime.strptime(texto.strip(), fmt).date()
            return float((d - dt.date.today()).days)
        except ValueError:
            continue
    return None


def pais_de_simbolo(sym: str) -> str:
    sym = sym.upper()
    if sym in INDICES:
        return INDICES[sym][1]
    for suf, (_, _, pais) in BOLSAS.items():
        if sym.endswith(suf):
            return pais
    if "=" in sym or "-" in sym:
        return ""
    return "US"


_NUMERO = re.compile(r"(?<![\w.,])[-+]?\d{1,3}(?:[.,]\d{3})+(?:[.,]\d+)?|(?<![\w.,])[-+]?\d+(?:[.,]\d+)?")


def _a_numero(s: str) -> float | None:
    s = s.strip().lstrip("+")
    if re.fullmatch(r"-?\d{1,3}(\.\d{3})+(,\d+)?", s):      # 1.234.567,89 (formato espanol)
        s = s.replace(".", "").replace(",", ".")
    elif re.fullmatch(r"-?\d{1,3}(,\d{3})+(\.\d+)?", s):     # 1,234,567.89
        s = s.replace(",", "")
    else:
        s = s.replace(",", ".")
    try:
        return float(s)
    except ValueError:
        return None


def _valores(obj, out: list[float]) -> None:
    if isinstance(obj, bool):
        return
    if isinstance(obj, (int, float)):
        out.append(float(obj))
    elif isinstance(obj, str):
        for m in _NUMERO.findall(obj):
            x = _a_numero(m)
            if x is not None:
                out.append(x)
    elif isinstance(obj, dict):
        for v in obj.values():
            _valores(v, out)
    elif isinstance(obj, (list, tuple)):
        for v in obj:
            _valores(v, out)


def verificar_cifras(texto: str, datos, tolerancia: float = 0.006) -> dict:
    """Comprueba que las cifras de una respuesta salen de los datos consultados (idea de Open Terminal: el agente
    no puede «teclear» un precio). Se aceptan redondeos, porcentajes de una fraccion y miles/millones abreviados.
    Devuelve {"cifras", "respaldadas", "sin_respaldo": [..]}; los anos y los enteros de 0 a 12 no se revisan."""
    base: list[float] = []
    _valores(datos, base)
    conjunto = set()
    for x in base:
        for y in (x, x * 100, x / 100, x / 1e3, x / 1e6, x / 1e9, x / 1e12):
            conjunto.add(y)
    ordenados = sorted(conjunto)
    import bisect
    sin, total = [], 0
    for m in _NUMERO.finditer(texto or ""):
        x = _a_numero(m.group(0))
        if x is None:
            continue
        if float(x).is_integer() and (0 <= abs(x) <= 12 or 1900 <= x <= 2100):
            continue
        total += 1
        decimales = len(re.split(r"[.,]", m.group(0))[-1]) if re.search(r"[.,]\d{1,2}$", m.group(0)) else 0
        margen = max(abs(x) * tolerancia, 0.5 * 10 ** (-decimales))
        ok = False
        for candidato in (x, -x):
            i = bisect.bisect_left(ordenados, candidato - margen)
            if i < len(ordenados) and ordenados[i] <= candidato + margen:
                ok = True
                break
        if not ok:
            sin.append(m.group(0))
    return {"cifras": total, "respaldadas": total - len(sin), "sin_respaldo": sin[:20]}


# ------------------------------------------------------------------ recetas
def _marco(receta: str, titulo: str, filas, fuente: str = "", extra: dict | None = None) -> dict:
    if isinstance(filas, dict):
        filas = [filas]
    columnas: list[str] = []
    for f in filas[:50]:
        for k in f:
            if k not in columnas:
                columnas.append(k)
    return {"receta": receta, "titulo": titulo, "columnas": columnas, "filas": filas, "fuente": fuente,
            "recibido": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC", **(extra or {})}


def _lista(v) -> list[str]:
    if isinstance(v, (list, tuple)):
        return [str(x).strip().upper() for x in v if str(x).strip()]
    return [x.strip().upper() for x in re.split(r"[,; ]+", str(v or "")) if x.strip()]


RECETAS = {
    "buscar": ("Encontrar el símbolo de una empresa, fondo o cripto por su nombre", ["texto"]),
    "cotizacion": ("Precio, cambio del día, horquilla y volumen de uno o varios símbolos", ["simbolos"]),
    "velas": ("Histórico de precios de un símbolo (rango: 1d, 5d, 1mo, 6mo, 1y, 5y)", ["simbolo", "rango"]),
    "indicadores": ("Medias, RSI, MACD, bandas de Bollinger y ATR de un símbolo", ["simbolo", "rango"]),
    "metricas": ("Valoración y calidad: capitalización, PER, BPA, dividendo, beta, márgenes, ROE", ["simbolos"]),
    "perfil": ("Ficha del valor: sector, industria, objetivo a un año, dividendo", ["simbolo"]),
    "noticias": ("Titulares recientes de un símbolo o del mercado", ["simbolo"]),
    "opciones": ("Cadena de opciones con volatilidad implícita y griegas", ["simbolo", "vencimiento"]),
    "resultados": ("Empresas que publican resultados en una fecha (AAAA-MM-DD; por defecto hoy)", ["fecha"]),
    "historial_bpa": ("Beneficio por acción publicado frente al esperado y la sorpresa", ["simbolo"]),
    "estimaciones": ("Consenso de analistas: precio objetivo y previsiones de beneficio", ["simbolo"]),
    "financieros": ("Cuenta de resultados, balance y flujos de caja", ["simbolo", "trimestral"]),
    "dividendos": ("Historial de dividendos", ["simbolo"]),
    "internos": ("Compras y ventas de directivos y consejeros", ["simbolo"]),
    "instituciones": ("Mayores accionistas institucionales", ["simbolo"]),
    "cortos": ("Posiciones cortas publicadas", ["simbolo"]),
    "criba": ("Buscar valores por condiciones (mercado y filtros de campo, operador y valor)", ["mercado", "filtros",
                                                                                              "orden", "limite"]),
    "sectores": ("Cómo va cada sector de un mercado hoy", ["mercado"]),
    "movimientos": ("Valores que más suben, bajan y se negocian en Estados Unidos", []),
    "cripto": ("Principales criptomonedas y dominancia", ["limite"]),
    "macro": ("Curva de tipos del Tesoro de Estados Unidos, diferencial 10-2 y VIX", []),
    "calendario": ("Calendario económico de la semana (país e importancia opcionales)", ["pais", "importancia"]),
    "salidas_bolsa": ("Salidas a bolsa de un mes (AAAA-MM)", ["mes"]),
}


def receta(nombre: str, params: dict | None = None, m: Mercados | None = None) -> dict:
    """Ejecuta una receta y devuelve un marco {titulo, columnas, filas, fuente}."""
    m = m or get()
    p = dict(params or {})
    nombre = str(nombre or "").strip().lower()
    simbolos = _lista(p.get("simbolos") or p.get("simbolo"))
    sym = simbolos[0] if simbolos else ""
    necesita = {"velas", "indicadores", "perfil", "opciones", "historial_bpa", "estimaciones", "financieros",
                "dividendos", "internos", "instituciones", "cortos", "cotizacion", "metricas"}
    if nombre in necesita and not sym:
        raise ValueError(f"la receta «{nombre}» necesita un símbolo (búscalo con la receta «buscar»)")
    if nombre == "buscar":
        return _marco(nombre, f"Símbolos para «{p.get('texto', '')}»", m.buscar(str(p.get("texto") or sym)))
    if nombre == "cotizacion":
        filas = []
        for s in simbolos[:15]:
            try:
                filas.append(m.cotizacion(s))
            except Exception as e:  # noqa: BLE001
                filas.append({"simbolo": s, "sin_dato": str(e)[:120]})
        return _marco(nombre, "Cotización de " + ", ".join(simbolos[:15]), filas)
    if nombre == "velas":
        pts = m.velas(sym, str(p.get("rango") or "6mo"))
        return _marco(nombre, f"Precios de {sym}", pts[-int(p.get("limite") or 260):], extra={"simbolo": sym})
    if nombre == "indicadores":
        from . import bolsa_analisis as ba
        pts = m.velas(sym, str(p.get("rango") or "1y"))
        return _marco(nombre, f"Indicadores técnicos de {sym}", ba.indicadores(pts), extra={"simbolo": sym})
    if nombre == "metricas":
        filas = []
        for s in simbolos[:10]:
            try:
                filas.append(m.fundamentales(s))
            except Exception as e:  # noqa: BLE001
                filas.append({"simbolo": s, "sin_dato": str(e)[:120]})
        return _marco(nombre, "Métricas de " + ", ".join(simbolos[:10]), filas, "TradingView")
    if nombre == "perfil":
        return _marco(nombre, f"Ficha de {sym}", {"simbolo": sym, **m.resumen_valor(sym)}, "Nasdaq")
    if nombre == "noticias":
        return _marco(nombre, f"Noticias de {sym}" if sym else "Noticias del mercado",
                      m.noticias(sym, int(p.get("limite") or 15)))
    if nombre == "opciones":
        c = m.opciones(sym, str(p.get("vencimiento") or ""), int(p.get("alrededor") or 6))
        filas = [{"strike": f["strike"], **{f"call_{k}": v for k, v in f["call"].items() if v is not None},
                  **{f"put_{k}": v for k, v in f["put"].items() if v is not None}} for f in c["filas"]]
        return _marco(nombre, f"Opciones de {sym} · {c['vencimiento']}", filas, "Nasdaq",
                      {"subyacente": c["subyacente"], "vencimientos": c["vencimientos"], "dias": c["dias"]})
    if nombre == "resultados":
        return _marco(nombre, "Resultados del " + str(p.get("fecha") or "día"),
                      m.resultados_dia(str(p.get("fecha") or ""))[:int(p.get("limite") or 60)], "Nasdaq")
    if nombre == "estimaciones":
        e = m.estimaciones(sym)
        filas = [{"dato": k, "valor": v} for k, v in (e.get("consenso") or {}).items()]
        return _marco(nombre, f"Estimaciones de {sym}", filas, "Nasdaq", {"previsiones": e.get("previsiones") or {}})
    if nombre == "financieros":
        t = m.financieros(sym, bool(p.get("trimestral")))
        primera = next(iter(t.values()), [])
        return _marco(nombre, f"Estados financieros de {sym}", primera, "Nasdaq", {"tablas": t})
    simples = {"historial_bpa": ("Sorpresas de beneficios de", m.historial_bpa),
               "dividendos": ("Dividendos de", m.dividendos), "internos": ("Operaciones de directivos de", m.internos),
               "instituciones": ("Accionistas institucionales de", m.instituciones),
               "cortos": ("Posiciones cortas de", m.cortos)}
    if nombre in simples:
        titulo, fn = simples[nombre]
        return _marco(nombre, f"{titulo} {sym}", fn(sym)[:int(p.get("limite") or 20)], "Nasdaq")
    if nombre == "criba":
        filtros = p.get("filtros") or []
        if isinstance(filtros, str):
            import json
            filtros = json.loads(filtros) if filtros.strip().startswith("[") else leer_filtros(filtros)
        filas = m.cribar(str(p.get("mercado") or "america"), filtros, str(p.get("orden") or "capitalizacion"),
                         str(p.get("ascendente") or "").lower() not in ("1", "true", "si", "sí"),
                         int(p.get("limite") or 25))
        return _marco(nombre, "Criba de valores", filas, "TradingView")
    if nombre == "sectores":
        return _marco(nombre, "Sectores", m.sectores(str(p.get("mercado") or "america")), "TradingView")
    if nombre == "movimientos":
        t = m.movimientos()
        return _marco(nombre, "Movimientos del día", next(iter(t.values()), []), "Nasdaq", {"tablas": t})
    if nombre == "cripto":
        c = m.cripto(int(p.get("limite") or 30))
        return _marco(nombre, "Criptomonedas", c["monedas"], c["fuente"], {"dominancia": c["dominancia"]})
    if nombre == "macro":
        d = m.macro()
        return _marco(nombre, "Curva de tipos de Estados Unidos", d["curva"], "FRED",
                      {k: d[k] for k in ("diferencial_10a_2a", "curva_invertida", "vix")})
    if nombre == "calendario":
        return _marco(nombre, "Calendario económico",
                      m.calendario(str(p.get("pais") or ""), str(p.get("importancia") or ""))[:80], "Forex Factory")
    if nombre == "salidas_bolsa":
        t = m.salidas_bolsa(str(p.get("mes") or ""))
        return _marco(nombre, "Salidas a bolsa", next(iter(t.values()), []), "Nasdaq", {"tablas": t})
    raise ValueError(f"receta desconocida: {nombre}. Disponibles: {', '.join(RECETAS)}")


def leer_filtros(texto: str) -> list[dict]:
    """«per < 15, rentabilidad_dividendo_pct > 3,5; sector = Finance» -> filtros de la criba. Vale dentro de una
    frase («empresas con per < 15 y roe_pct > 20 en España»)."""
    out = []
    texto = re.sub(r"(\d),(\d{1,2})(?!\d)", r"\1.\2", texto or "")
    for trozo in re.split(r";|,\s| y ", texto):
        m = re.search(r"([A-Za-z_][\w.]*)\s*(>=|<=|!=|==|=|>|<)\s*(-?\d[\d.]*\s?[KMBT]?)(?![\w])", trozo)
        if m:
            valor = _num(m.group(3).replace(" ", ""))
            if valor is not None:
                out.append({"campo": m.group(1), "op": m.group(2), "valor": valor})
                continue
        m = re.search(r"([A-Za-z_][\w.]*)\s*(==|=|!=)\s*([\w .&-]+?)\s*$", trozo)
        if m:
            out.append({"campo": m.group(1), "op": m.group(2), "valor": m.group(3)})
    return out


# ------------------------------------------------------------------ fuentes para el servicio Mundo
def fuente_indices(ctx) -> list[dict]:
    m = get()
    simbolos = _lista(ctx.cfg("mercados_simbolos", SIMBOLOS_FABRICA)) or SIMBOLOS_FABRICA
    out = []
    for s in simbolos[:40]:
        try:
            q = m.cotizacion(s)
        except Exception as e:  # noqa: BLE001
            ctx.aviso(f"{s}: {e}")
            continue
        nombre = INDICES.get(s, (q.get("nombre") or s,))[0]
        cambio = q.get("cambio_pct")
        out.append({"uid": s, "pais": q.get("pais") or pais_de_simbolo(s), "ts": time.time(), "valor": cambio,
                    "titulo": f"{nombre} ({s}): {q['precio']:,.2f} {q.get('divisa') or ''}".strip() +
                              (f", {cambio:+.2f} % hoy" if cambio is not None else ""),
                    "detalle": f"Fuente: {q.get('fuente', '')}", "url": "", "lat": None, "lon": None,
                    "datos": {k: q.get(k) for k in ("precio", "cambio_pct", "divisa", "volumen", "fuente")}})
    if not out:
        raise SinDatos("ninguna cotización disponible")
    return out


def fuente_cripto(ctx) -> list[dict]:
    c = get().cripto(40)
    out = []
    dom = c.get("dominancia") or {}
    if dom:
        out.append({"uid": "dominancia", "pais": "", "ts": time.time(), "valor": dom.get("btc_pct"),
                    "titulo": f"Dominancia: Bitcoin {dom.get('btc_pct')} %, Ether {dom.get('eth_pct')} %",
                    "detalle": "", "url": "", "lat": None, "lon": None, "datos": dom})
    for x in c["monedas"]:
        if x.get("precio_usd") is None:
            continue
        cambio = x.get("cambio_24h_pct")
        out.append({"uid": x["simbolo"], "pais": "", "ts": time.time(), "valor": cambio, "lat": None, "lon": None,
                    "titulo": f"{x['nombre']} ({x['simbolo']}): {x['precio_usd']:,.4g} USD" +
                              (f", {cambio:+.2f} % en 24 h" if cambio is not None else ""),
                    "detalle": f"Fuente: {c['fuente']}", "url": "", "datos": x})
    return out


def fuente_macro(ctx) -> list[dict]:
    d = get().macro()
    out = [{"uid": c["plazo"], "pais": "US", "ts": time.time(), "valor": c["tipo_pct"], "lat": None, "lon": None,
            "titulo": f"Bono de Estados Unidos a {c['plazo']}: {c['tipo_pct']} %", "detalle": f"Dato del {c['fecha']}",
            "url": "https://fred.stlouisfed.org/", "datos": c} for c in d["curva"]]
    if d.get("diferencial_10a_2a") is not None:
        out.insert(0, {"uid": "diferencial", "pais": "US", "ts": time.time(), "valor": d["diferencial_10a_2a"],
                       "titulo": f"Diferencial 10 años - 2 años: {d['diferencial_10a_2a']} puntos" +
                                 (" (curva invertida)" if d["curva_invertida"] else ""), "detalle": "", "url": "",
                       "lat": None, "lon": None, "datos": {}})
    if d.get("vix") is not None:
        out.insert(0, {"uid": "vix", "pais": "US", "ts": time.time(), "valor": d["vix"], "lat": None, "lon": None,
                       "titulo": f"Índice de volatilidad VIX: {d['vix']}", "detalle": "", "url": "", "datos": {}})
    return out


def fuente_calendario(ctx) -> list[dict]:
    from ..mundo import paises
    out = []
    for e in get().calendario():
        if e["importancia"] == "baja":
            continue
        out.append({"uid": f"{e['ts']}-{e['moneda']}-{e['dato']}", "pais": e["pais"], "ts": e["ts"],
                    "valor": {"alta": 3, "media": 2}.get(e["importancia"], 1), "lat": None, "lon": None,
                    "titulo": f"{e['dato']} ({paises.nombre(e['pais']) if e['pais'] else e['moneda']}, "
                              f"importancia {e['importancia']})",
                    "detalle": ", ".join(x for x in (f"previsión {e['prevision']}" if e["prevision"] else "",
                                                     f"anterior {e['anterior']}" if e["anterior"] else "",
                                                     f"real {e['real']}" if e["real"] else "") if x),
                    "url": "", "datos": e})
    return out


def fuente_noticias(ctx) -> list[dict]:
    from ..mundo import paises
    m = get()
    out = []
    simbolos = [s for s in _lista(ctx.cfg("mercados_noticias", [])) if s][:6]
    for s in [""] + simbolos:
        try:
            for n in m.noticias(s, 15):
                nombrados = paises.en_texto(n["titulo"])
                out.append({"uid": n["url"] or n["titulo"], "pais": nombrados[0] if nombrados else pais_de_simbolo(s) if s else "",
                            "ts": n["ts"], "valor": None, "titulo": n["titulo"], "lat": None, "lon": None,
                            "detalle": (f"{s} · " if s else "") + (n.get("medio") or ""), "url": n["url"],
                            "datos": {"simbolo": s}})
        except Exception as e:  # noqa: BLE001
            ctx.aviso(f"noticias {s or 'mercado'}: {e}")
    if not out:
        raise SinDatos("sin noticias")
    return out


def fuente_sectores(ctx) -> list[dict]:
    from ..mundo import paises
    m = get()
    out = []
    for pais in (["US"] + [p for p in ctx.paises_interes() if p in MERCADOS_TV and p != "US"])[:4]:
        try:
            for s in m.sectores(MERCADOS_TV[pais]):
                out.append({"uid": f"{pais}-{s['sector']}", "pais": pais, "ts": time.time(), "valor": s["cambio_pct"],
                            "titulo": f"Sector {s['sector']} en {paises.nombre(pais)}: {s['cambio_pct']:+.2f} % hoy",
                            "detalle": f"{s['valores']} valores; mejor {s['mejor']}, peor {s['peor']}", "url": "",
                            "lat": None, "lon": None, "datos": s})
        except Exception as e:  # noqa: BLE001
            ctx.aviso(f"sectores {pais}: {e}")
    if not out:
        raise SinDatos("sin sectores")
    return out


def fuente_resultados(ctx) -> list[dict]:
    out = []
    for f in get().resultados_dia()[:120]:
        sym = f.get("symbol") or f.get("Symbol") or ""
        if not sym:
            continue
        out.append({"uid": sym, "pais": "US", "ts": time.time(), "valor": _num(f.get("marketCap") or f.get("Market Cap")),
                    "titulo": f"Resultados de {f.get('name') or f.get('Company Name') or sym} ({sym})", "lat": None,
                    "lon": None, "detalle": ", ".join(f"{k}: {v}" for k, v in f.items()
                                                      if k not in ("symbol", "name", "Symbol", "Company Name"))[:300],
                    "url": f"https://www.nasdaq.com/market-activity/stocks/{sym.lower()}/earnings", "datos": f})
    return out


_instancia: Mercados | None = None


def get() -> Mercados:
    global _instancia
    if _instancia is None:
        _instancia = Mercados()
    return _instancia


def reiniciar(m: Mercados | None = None) -> None:
    """Para las pruebas: sustituye la instancia compartida."""
    global _instancia
    _instancia = m
