"""Cotizaciones reales contra las fuentes de verdad (solo con NEXUS_ONLINE_TESTS=1, p. ej. en GitHub Actions)."""
import os

import pytest

pytestmark = pytest.mark.skipif(os.environ.get("NEXUS_ONLINE_TESTS") != "1", reason="necesita internet")


def test_cotizaciones_reales_del_catalogo(tmp_path):
    from nexus.office import bolsa as B
    from nexus.office import bolsa_datos as D
    b = B.Bolsa(tmp_path / "b.db")
    b.datos = D.DatosMercado()
    reales, fallos = {}, []
    for inst in b.instrumentos(""):
        q = b.precio(inst["simbolo"])
        (reales.__setitem__(inst["simbolo"], q) if q["real"] else fallos.append(inst["simbolo"]))
    for s, q in reales.items():
        print(f"{s:10} {q['precio']:>12} {q['divisa']} {q['variacion_pct']:+.2f}% {q['fuente']} ({q['retraso']})")
        ref = b.instrumento(s)["precio_ref"]
        assert ref / 20 < q["precio"] < ref * 20, f"{s}: {q['precio']} muy lejos de la referencia {ref}"
    print("sin dato real:", fallos, "estado:", b.datos.estado())
    assert len(reales) >= len(b.instrumentos("")) - 2
    assert b.datos.fx("USD") and 0.7 < b.datos.fx("USD") < 1.6
    s = b.serie("SAN.MC", "6mo")
    assert s["real"] and len(s["puntos"]) > 60
    d = b.serie("BTC-EUR", "1d")
    assert d["real"] and len(d["puntos"]) > 30
    b.close()


def test_fuentes_de_respaldo():
    from nexus.office import bolsa_datos as D
    d = D.DatosMercado()
    ok = {}
    for fn, sym in ((d._quote_yahoo, "AAPL"), (d._quote_google, "SAN.MC"), (d._quote_google, "AAPL"), (d._quote_google, "KO"),
                    (d._quote_google, "SHEL.L"), (d._quote_stooq, "AAPL"), (d._quote_crypto, "BTC-EUR")):
        d.fuentes = {n: D.Fuente(n) for n in d.fuentes}
        try:
            ok[f"{fn.__name__}:{sym}"] = fn(sym)["precio"]
        except Exception as e:  # noqa: BLE001
            ok[f"{fn.__name__}:{sym}"] = f"FALLO {e}"
    print(ok, d.ultimo_http)
    import httpx
    r = httpx.get("https://query1.finance.yahoo.com/v8/finance/chart/AAPL?range=1d&interval=5m", headers={"User-Agent": D.UA})
    print("yahoo directo:", r.status_code, r.text[:300])
    assert isinstance(ok["_quote_crypto:BTC-EUR"], float)


def test_diagnostico_de_fuentes():
    """Solo informa: que devuelve cada servicio desde aqui (para elegir fuentes que funcionen en cualquier red)."""
    import re

    import httpx
    from nexus.office import bolsa_datos as D
    h = {"User-Agent": D.UA, "Accept-Language": "en-US,en;q=0.9"}
    urls = {
        "google": "https://www.google.com/finance/quote/AAPL:NASDAQ?hl=en",
        "google_bme": "https://www.google.com/finance/quote/SAN:BME?hl=en",
        "yahoo2": "https://query2.finance.yahoo.com/v8/finance/chart/SAN.MC?range=1d&interval=5m",
        "stooq": "https://stooq.com/q/l/?s=aapl.us&f=sd2t2ohlcv&h&e=csv",
        "stooq_pl": "https://stooq.pl/q/l/?s=aapl.us&f=sd2t2ohlcv&h&e=csv",
        "stooq_d": "https://stooq.com/q/d/l/?s=aapl.us&i=d",
        "nasdaq": "https://api.nasdaq.com/api/quote/AAPL/info?assetclass=stocks",
        "cnbc": "https://quote.cnbc.com/quote-html-webservice/restQuote/symbolType/symbol?symbols=AAPL|SAN-ES&requestMethod=itv&noform=1&partnerId=2&fund=1&exthrs=1&output=json",
        "cnbc_eu": "https://quote.cnbc.com/quote-html-webservice/restQuote/symbolType/symbol?symbols=SAN-ES|ASML-NL|MC-FR|SHEL-GB|SAP-DE|IWDA-NL|VWCE-DE|IBGL-NL&requestMethod=itv&noform=1&partnerId=2&fund=1&exthrs=1&output=json",
        "cnbc_chart": "https://ts-api.cnbc.com/harmony/app/charts/1M.json?symbol=AAPL",
        "nasdaq_hist": "https://api.nasdaq.com/api/quote/AAPL/chart?assetclass=stocks",
        "marketwatch": "https://www.marketwatch.com/investing/stock/aapl",
        "investing": "https://api.investing.com/api/financialdata/6408/historical/chart/?period=P1D&interval=PT5M&pointscount=60",
        "bme": "https://www.bolsasymercados.es/bme-exchange/es/Mercados-y-Cotizaciones/Acciones/Mercado-Continuo/Precios/ibex-35-ES0SI0000005",
        "euronext": "https://live.euronext.com/en/ajax/getDetailedQuote/NL0010273215-XAMS",
        "boerse_frankfurt": "https://api.boerse-frankfurt.de/v1/data/quote_box/single?isin=DE0007164600&mic=XETR",
        "lse": "https://api.londonstockexchange.com/api/gw/lse/instruments/alldata/SHEL",
    }
    with httpx.Client(headers=h, timeout=10, follow_redirects=True) as c:
        for k, u in urls.items():
            try:
                r = c.get(u)
                t = r.text
                m = re.search(r'data-last-price="[^"]*"|"last"\s*:\s*"?[0-9.]+|"regularMarketPrice":[0-9.]+|"lastSalePrice":"[^"]+"', t)
                if k == "cnbc_eu":
                    for q in (r.json().get("FormattedQuoteResult") or {}).get("FormattedQuote") or []:
                        print("DIAG cnbc_eu", q.get("symbol"), q.get("last"), q.get("currencyCode"), q.get("code"))
                print(f"DIAG {k}: {r.status_code} {len(t)}B {r.headers.get('content-type', '')[:30]} "
                      f"{m.group(0) if m else repr(t[:160])}")
            except Exception as e:  # noqa: BLE001
                print(f"DIAG {k}: ERROR {e}")
