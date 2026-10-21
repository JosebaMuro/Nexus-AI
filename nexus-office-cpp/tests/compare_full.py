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
        import time

        def wait(job):
            for _ in range(200):
                j = c.get(f"/api/kg/jobs/{job.get('id', '')}").json()
                if j.get("status") != "en curso":
                    return j
                time.sleep(0.1)
            return j

        rec("kg_formats", c.get("/api/kg/formats"))
        j = rec("kg_import", c.post("/api/kg/import", files={"files": ("g.graphml", GRAPHML.encode(), "application/xml")},
                                    headers=h))
        out["kg_import_done"] = (200, wait(j))
        j = rec("kg_import_json", c.post("/api/kg/import", files={"file": ("g.json", json.dumps(
            {"nodes": [{"id": "x", "label": "Nómina"}, {"id": "y", "label": "IRPF"}],
             "edges": [{"source": "x", "target": "y", "label": "retiene"}]}).encode(), "application/json")},
            headers=h))
        out["kg_import_json_done"] = (200, wait(j))
        j = rec("kg_import_many", c.post("/api/kg/import", files=[
            ("files", ("nodes.csv", b"id:ID,name,:LABEL\n1,Proveedor,Empresa\n2,Pedido,Documento\n", "text/csv")),
            ("files", ("edges.csv", b":START_ID,:END_ID,:TYPE\n1,2,RECIBE\n", "text/csv"))], headers=h))
        out["kg_import_many_done"] = (200, wait(j))
        rec("kg_import_bad", c.post("/api/kg/import", files={"file": ("x.bin", b"\x00\x01\x02", "application/x")},
                                    headers=h))
        rec("kg_import_noadmin", c.post("/api/kg/import", files={"file": ("g.graphml", GRAPHML.encode(), "text/xml")}))
        rec("kg_import_empty", c.post("/api/kg/import", data={"target": ""}, headers=h))
        rec("kg_import_badtarget", c.post("/api/kg/import", files={"file": ("g.graphml", GRAPHML.encode(), "text/xml")},
                                          data={"target": "nadie"}, headers=h))
        import io
        import zipfile
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("red/g.graphml", GRAPHML)
            z.writestr("notas/Ana.md", "# Ana\nTrabaja con [[Luis]] en [[Proyecto X]].\n")
            z.writestr("notas/Luis.md", "Luis conoce a [[Ana]].\n")
            z.writestr("docs/leeme.txt", "Manual de facturacion. La factura incluye el IVA y se emite al cliente.")
        j = rec("kg_archive", c.post("/api/kg/archive", files={"file": ("paquete.zip", buf.getvalue(), "application/zip")},
                                     data={"target": "general", "reason": "false"}, headers=h))
        out["kg_archive_done"] = (200, wait(j))
        rec("kg_archive_badtarget", c.post("/api/kg/archive", files={"file": ("p.zip", buf.getvalue(), "application/zip")},
                                           data={"target": "nadie"}, headers=h))
        for fmt in ("json", "graphml", "csv", "gexf"):
            rec(f"kg_export_{fmt}", c.get(f"/api/kg/export?format={fmt}"))
        rec("kg_export_general", c.get("/api/kg/export?format=json&scope=general"))
        rec("kg_overview", c.get("/api/kg/overview"))
        rec("dk_list", c.get("/api/rag/domain-knowledge"))

    if only in ("", "web"):   # necesita el sitio de pruebas en 127.0.0.1:8777 y rag.allow_private_urls
        import time
        site = "http://127.0.0.1:8777"
        rec("crawl", c.post("/api/rag/crawl", json={"url": site + "/docs/index.html", "max_pages": 10}))
        rec("crawl_again", c.post("/api/rag/crawl", json={"url": site + "/docs/", "max_pages": 10}))
        rec("crawl_bad_scope", c.post("/api/rag/crawl", json={"url": site + "/docs/", "scope": "todo"}))
        rec("crawl_bad_url", c.post("/api/rag/crawl", json={"url": "ftp://x"}))
        rec("crawl_404", c.post("/api/rag/crawl", json={"url": site + "/nada/", "max_pages": 3}))
        rec("url_pdf", c.post("/api/rag/url", json={"url": site + "/docs/guia/facturas.html"}))
        rec("url_pdf_short", c.post("/api/rag/url", json={"url": site + "/robots.txt"}))
        j = rec("crawl_domain", c.post("/api/rag/crawl", json={"url": site + "/docs/", "scope": "domain"}))
        jid = (j.get("job") or {}).get("id", "")
        for _ in range(300):
            st = [x for x in c.get("/api/rag/crawls").json() if x["id"] == jid]
            if st and not st[0]["running"] and st[0]["status"] != "en curso":
                break
            time.sleep(0.2)
        out["crawl_domain_done"] = (200, st[0] if st else {})
        rec("crawls", c.get("/api/rag/crawls"))
        rec("kg_build", c.post(f"/api/rag/crawls/{jid}/knowledge"))
        kz = c.get(f"/api/rag/crawls/{jid}/knowledge")
        out["kg_file"] = (kz.status_code, {"_type": kz.headers.get("content-type", "").split(";")[0],
                                           "_name": "conocimiento" in kz.headers.get("content-disposition", "")})
        rec("kg_import_pkg", c.post(f"/api/rag/crawls/{jid}/knowledge/import", json={"agent": ""}))
        rec("dk_import", c.post("/api/rag/domain-knowledge/import", files={"file": ("k.zip", kz.content, "application/zip")}))
        rec("dk_import_bad", c.post("/api/rag/domain-knowledge/import", files={"file": ("k.zip", b"nada", "application/zip")}))
        rec("crawl_resume_404", c.post("/api/rag/crawls/noexiste99/resume"))
        rec("crawl_pause", c.post(f"/api/rag/crawls/{jid}/pause"))
        rec("crawl_delete", c.delete(f"/api/rag/crawls/{jid}"))
        rec("crawl_delete_again", c.delete(f"/api/rag/crawls/{jid}"))

    if only in ("", "red"):
        rec("coop", c.get("/api/coop"))
        rec("coop_start_unconf", c.post("/api/coop/start"))
        rec("coop_create_noadmin", c.post("/api/coop/create", json={"name": "x"}))
        rec("coop_create", c.post("/api/coop/create", headers=h, json={"name": "oficina-prueba"}))
        inv = rec("coop_invite", c.get("/api/coop/invite", headers=h))
        rec("coop_join_bad", c.post("/api/coop/join", json={"code": "NEXUS1:no-vale"}))
        rec("coop_join_other", c.post("/api/coop/join", json={"code": "OTRO:abc"}))
        rec("coop_join", c.post("/api/coop/join", json={"code": inv.get("code", "")}))
        rec("coop_chat_off", c.post("/api/coop/chat", json={"text": "hola"}))
        rec("coop_start", c.post("/api/coop/start"))
        rec("coop_chat", c.post("/api/coop/chat", json={"text": "hola red"}))
        rec("coop_ping_unknown", c.post("/api/coop/ping/nadie"))
        rec("coop_agent_unknown", c.post("/api/coop/agent", json={"node": "nadie", "text": "hola"}))
        rec("coop_share_facts", c.post("/api/coop/share-facts", headers=h))
        rec("coop_status_on", c.get("/api/coop"))
        rec("coop_stop", c.post("/api/coop/stop"))
        rec("remote", c.get("/api/remote", headers=h))
        rec("remote_noadmin", c.get("/api/remote"))
        rec("remote_enable_bad", c.post("/api/remote/enable", headers=h, json={"enabled": True, "port": 80}))
        rec("remote_enable", c.post("/api/remote/enable", headers=h, json={"enabled": True, "port": 8443}))
        p = rec("remote_pairing", c.post("/api/remote/pairing", headers=h, json={"name": "Movil"}))
        rec("remote_pair_bad", c.post("/api/remote/pair", json={"code": "AAAA-BBBB-CCCC"}))
        dev = rec("remote_pair", c.post("/api/remote/pair", json={"code": p.get("code", ""), "name": "Pixel"}))
        rec("remote_pair_reuse", c.post("/api/remote/pair", json={"code": p.get("code", "")}))
        rec("remote_with_dev", c.get("/api/remote", headers=h))
        did = (dev.get("device") or {}).get("id", "x")
        rec("remote_revoke", c.delete(f"/api/remote/devices/{did}", headers=h))
        rec("remote_revoke_again", c.delete(f"/api/remote/devices/{did}", headers=h))
        rec("remote_cert", c.post("/api/remote/certificate", headers=h))
        rec("remote_upnp", c.post("/api/remote/upnp", headers=h, json={"open": True}))
        rec("remote_disable", c.post("/api/remote/enable", headers=h, json={"enabled": False}))
        rec("vpn", c.get("/api/vpn", headers=h))
        rec("vpn_peer_off", c.post("/api/vpn/peers", headers=h, json={"name": "Movil"}))
        rec("vpn_enable_bad", c.post("/api/vpn/enable", headers=h, json={"enabled": True, "port": 0}))
        rec("vpn_enable", c.post("/api/vpn/enable", headers=h, json={"enabled": True}))
        peer = rec("vpn_peer", c.post("/api/vpn/peers", headers=h, json={"name": "Movil de Ana"}))
        rec("vpn_conf", c.get("/api/vpn/server.conf", headers=h))
        rec("vpn_status_peers", c.get("/api/vpn", headers=h))
        pid = (peer.get("peer") or {}).get("id", "x")
        rec("vpn_apply", c.post("/api/vpn/apply", headers=h))
        rec("vpn_del", c.delete(f"/api/vpn/peers/{pid}", headers=h))
        rec("vpn_del_again", c.delete(f"/api/vpn/peers/{pid}", headers=h))
        rec("vpn_disable", c.post("/api/vpn/enable", headers=h, json={"enabled": False}))
        rec("lan", c.get("/api/lan"))
        rec("status_coop", c.get("/api/status"))

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
