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
    print("sin dato real:", fallos)
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
    for fn, sym in ((d._quote_stooq, "AAPL"), (d._quote_stooq, "SAP.DE"), (d._quote_crypto, "BTC-EUR")):
        try:
            ok[f"{fn.__name__}:{sym}"] = fn(sym)["precio"]
        except Exception as e:  # noqa: BLE001
            ok[f"{fn.__name__}:{sym}"] = f"FALLO {e}"
    print(ok)
    assert isinstance(ok["_quote_crypto:BTC-EUR"], float)
