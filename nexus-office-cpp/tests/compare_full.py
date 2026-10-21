"""Flujos de las funciones que la edicion C++ completa (2.1) trae respecto a la beta: mesa de bolsa, grafos en otros
formatos y paquetes de dominio, URL a PDF y rastreo, red COOP, acceso remoto, VPN y GIMP. Compara la forma de cada
respuesta (y el codigo HTTP) entre las dos ediciones.

    python tests/compare_full.py http://127.0.0.1:8790 http://127.0.0.1:8791 [solo_esta_seccion]

Las dos ediciones deben arrancar con carpetas de datos nuevas.
"""
import json
import sys

import httpx

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from compare_api import diff  # noqa: E402

GRAPHML = """<?xml version="1.0" encoding="UTF-8"?>
<graphml xmlns="http://graphml.graphdrawing.org/xmlns">
  <key id="label" for="node" attr.name="label" attr.type="string"/>
  <key id="rel" for="edge" attr.name="relation" attr.type="string"/>
  <graph edgedefault="directed">
    <node id="a"><data key="label">Factura</data></node>
    <node id="b"><data key="label">IVA</data></node>
    <node id="c"><data key="label">Cliente</data></node>
    <edge source="a" target="b"><data key="rel">incluye</data></edge>
    <edge source="a" target="c"><data key="rel">se emite a</data></edge>
  </graph>
</graphml>
"""


def flows(base, only=""):
    c = httpx.Client(base_url=base, timeout=120)
    out = {}

    def rec(name, r):
        ctype = r.headers.get("content-type", "")
        body = r.json() if "json" in ctype else {"_bytes": len(r.content) > 0, "_type": ctype.split(";")[0]}
        out[name] = (r.status_code, body)
        return body

    c.post("/api/admin/setup", json={"pin": "1234"})
    tok = c.post("/api/admin/login", json={"pin": "1234"}).json()["token"]
    h = {"X-Admin-Token": tok}

    if only in ("", "bolsa"):
        st = rec("bolsa", c.get("/api/bolsa"))
        rec("bolsa_inst", c.get("/api/bolsa/instrumentos?q=etf"))
        rec("bolsa_quote", c.get("/api/bolsa/cotizacion/BTC-EUR"))
        rec("bolsa_quote_bad", c.get("/api/bolsa/cotizacion/NOEXISTE"))
        rec("bolsa_costes", c.post("/api/bolsa/costes", json={"simbolo": "SAN.MC", "cantidad": 100}))
        rec("bolsa_costes_usd", c.post("/api/bolsa/costes", json={"simbolo": "AAPL", "cantidad": 3, "lado": "venta"}))
        rec("bolsa_buy", c.post("/api/bolsa/orden", json={"simbolo": "BTC-EUR", "lado": "compra", "cantidad": 0.01}))
        rec("bolsa_sell_short", c.post("/api/bolsa/orden", json={"simbolo": "ETH-EUR", "lado": "venta",
                                                                 "cantidad": 1}))
        rec("bolsa_sell", c.post("/api/bolsa/orden", json={"simbolo": "BTC-EUR", "lado": "venta", "cantidad": 0.005}))
        lim = rec("bolsa_limit", c.post("/api/bolsa/orden", json={"simbolo": "SAN.MC", "lado": "compra",
                                                                  "cantidad": 10, "tipo": "limitada", "limite": 1,
                                                                  "validez": "hasta_cancelar"}))
        rec("bolsa_cancel", c.post(f"/api/bolsa/orden/{lim.get('id', 0)}/cancelar"))
        rec("bolsa_cancel_again", c.post(f"/api/bolsa/orden/{lim.get('id', 0)}/cancelar"))
        rec("bolsa_bad_type", c.post("/api/bolsa/orden", json={"simbolo": "SAN.MC", "lado": "compra", "cantidad": 1,
                                                               "tipo": "rara"}))
        rec("bolsa_real_noadmin", c.post("/api/bolsa/cartera", json={"modo": "real"}))
        real = rec("bolsa_real", c.post("/api/bolsa/cartera", json={"modo": "real", "nombre": "Real", "capital": 5000},
                                        headers=h))
        o = rec("bolsa_real_order", c.post("/api/bolsa/orden", json={"simbolo": "KO", "lado": "compra", "cantidad": 2,
                                                                     "cartera": real.get("id")}))
        rec("bolsa_confirm_bad", c.post(f"/api/bolsa/orden/{o.get('id', 0)}/confirmar",
                                        json={"confirmacion": "si"}, headers=h))
        rec("bolsa_confirm", c.post(f"/api/bolsa/orden/{o.get('id', 0)}/confirmar",
                                    json={"confirmacion": "CONFIRMO"}, headers=h))
        rec("bolsa_fill", c.post(f"/api/bolsa/orden/{o.get('id', 0)}/ejecucion", json={"precio": 64.5,
                                                                                       "comision_eur": 1}, headers=h))
        rec("bolsa_inst_add", c.post("/api/bolsa/instrumento", json={"simbolo": "PYME.MC", "nombre": "Pyme SA",
                                                                    "mercado": "BME", "precio": "12,5"}))
        csv = "Date,Close\n" + "\n".join(f"2026-09-{d:02d},{10 + d * 0.1:.2f}" for d in range(1, 25))
        rec("bolsa_import", c.post("/api/bolsa/importar", json={"simbolo": "PYME.MC", "csv": csv}))
        rec("bolsa_fiscal", c.get("/api/bolsa/fiscal"))
        rec("bolsa_reset", c.post(f"/api/bolsa/cartera/{st['resumen']['cartera']}/reiniciar", json={"capital": 2000}))
        rec("bolsa_after", c.get(f"/api/bolsa?cartera={real.get('id', 0)}"))

    if only in ("", "grafo"):
        rec("kg_formats", c.get("/api/kg/formats"))
        rec("kg_import", c.post("/api/kg/import", files={"files": ("g.graphml", GRAPHML.encode(), "application/xml")},
                                data={"scope": ""}, headers=h))
        rec("kg_import_json", c.post("/api/kg/import", files={"files": ("g.json", json.dumps(
            {"nodes": [{"id": "x", "label": "Nómina"}, {"id": "y", "label": "IRPF"}],
             "edges": [{"source": "x", "target": "y", "label": "retiene"}]}).encode(), "application/json")},
            data={"scope": ""}, headers=h))
        for fmt in ("json", "graphml", "gexf", "csv", "dot", "ttl", "cypher", "md"):
            rec(f"kg_export_{fmt}", c.get(f"/api/kg/export?fmt={fmt}"))
        rec("kg_overview", c.get("/api/kg/overview"))
        rec("dk_list", c.get("/api/rag/domain-knowledge"))

    if only in ("", "red"):
        rec("coop", c.get("/api/coop"))
        rec("remote", c.get("/api/remote", headers=h))
        rec("remote_devices", c.get("/api/remote/devices", headers=h))
        rec("vpn", c.get("/api/vpn", headers=h))
        rec("lan", c.get("/api/lan"))

    if only in ("", "gimp"):
        rec("gimp_tools", c.get("/api/tools"))
    return out


def main():
    cpp, py = sys.argv[1], sys.argv[2]
    only = sys.argv[3] if len(sys.argv) > 3 else ""
    a, b = flows(cpp, only), flows(py, only)
    bad = 0
    for name in b:
        sa, ja = a.get(name, (0, None))
        sb, jb = b[name]
        d = ([f"estado {sa} en C++, {sb} en Python"] if sa != sb else []) + diff(ja, jb)
        print(("OK  " if not d else "DIF ") + name)
        for line in d[:20]:
            print("      " + line)
        bad += bool(d)
    print(f"\n{len(b) - bad}/{len(b)} flujos iguales")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
