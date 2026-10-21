"""Recorre los flujos principales (chat por WebSocket, administrador, memoria, agentes, biblioteca, multimedia,
oficina y gestor de conocimiento) en las dos ediciones y compara la forma de cada respuesta.

    python tests/compare_flows.py http://127.0.0.1:8790 http://127.0.0.1:8791

Necesita httpx y websockets. Las dos ediciones deben arrancar con carpetas de datos nuevas.
"""
import asyncio
import json
import sys

import httpx
import websockets

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from compare_api import diff  # noqa: E402


async def chat(base, text, agent="auto"):
    events = []
    async with websockets.connect(base.replace("http", "ws") + "/ws") as ws:
        events.append(json.loads(await ws.recv()))
        await ws.send(json.dumps({"type": "chat", "text": text, "agent": agent}))
        while True:
            e = json.loads(await asyncio.wait_for(ws.recv(), 120))
            events.append(e)
            if e.get("type") == "done" or (e.get("type") == "state" and e.get("state") == "idle" and len(events) > 3):
                break
    return events


def flows(base):
    c = httpx.Client(base_url=base, timeout=120)
    out = {}

    def rec(name, r):
        ctype = r.headers.get("content-type", "")
        out[name] = (r.status_code, r.json() if "json" in ctype else {"_bytes": len(r.content), "_type": ctype,
                                                                     "_engine": r.headers.get("x-tts-engine")})
        return out[name][1]

    rec("setup", c.post("/api/admin/setup", json={"pin": "1234"}))
    rec("setup_again", c.post("/api/admin/setup", json={"pin": "1234"}))
    rec("login_bad", c.post("/api/admin/login", json={"pin": "9999"}))
    tok = rec("login", c.post("/api/admin/login", json={"pin": "1234"}))["token"]
    h = {"X-Admin-Token": tok}
    rec("status_admin", c.get("/api/status", headers=h))
    rec("config_admin", c.get("/api/config", headers=h))
    rec("config_put", c.put("/api/config", json={"user.assistant_name": "Prueba"}))
    rec("config_put_forbidden", c.put("/api/config", json={"llm.n_ctx": 4096}))
    rec("missing_body", c.post("/api/memory", json={}))
    rec("memory_add", c.post("/api/memory", json={"content": "Me llamo Ana y trabajo en una carpinteria de Bilbao"}))
    rec("memory_list", c.get("/api/memory"))
    rec("memory_search", c.get("/api/memory/search", params={"q": "carpinteria"}))
    rec("agent_put_forbidden", c.put("/api/agents", json={"id": "x", "name": "X"}))
    rec("agent_put", c.put("/api/agents", headers=h, json={"id": "probador", "name": "Probador", "description": "Pruebas",
                                                            "keywords": ["prueba"], "tools": ["calculator"]}))
    rec("agent_bad", c.put("/api/agents", headers=h, json={"name": "sin id"}))
    rec("agents", c.get("/api/agents"))
    rec("rag_upload", c.post("/api/rag/upload", files={"file": ("nota.md", b"# Presupuestos\nEl IVA general es del 21 por ciento.\n" * 20)},
                             data={"agent": ""}))
    rec("rag_import", c.post("/api/rag/import", files=[("files", ("a.txt", b"Horario de la tienda: de 9 a 14 horas.")),
                                                        ("files", ("b.csv", b"producto,precio\nmesa,120\nsilla,45\n"))]))
    rec("rag_bad", c.post("/api/rag/upload", files={"file": ("x.exe", b"MZ")}))
    rec("rag_search", c.get("/api/rag/search", params={"q": "IVA"}))
    rec("prompt", c.post("/api/prompt/enhance", json={"text": "un gato en la playa al atardecer", "use_llm": False}))
    rec("image", c.post("/api/image/generate", json={"prompt": "un gato en la playa", "seed": 7}))
    rec("music", c.post("/api/music/generate", json={"prompt": "musica alegre", "seconds": 4}))
    rec("tts", c.post("/api/voice/tts", json={"text": "hola"}))
    rec("outputs", c.get("/api/outputs"))
    rec("kg_node_add", c.post("/api/kg/nodes", headers=h, json={"name": "Barniz al agua", "type": "producto"}))
    rec("kg_node_bad", c.post("/api/kg/nodes", headers=h, json={"name": "x", "category": "inventada"}))
    rec("kg_missing", c.get("/api/kg/nodes/999999"))
    rec("graph_unknown", c.get("/api/graph", params={"scope": "nadie"}))
    rec("feedback", c.post("/api/feedback", json={"message_id": 1, "score": 1}))
    rec("approval_unknown", c.post("/api/approvals/nada", json={"approve": True}))
    rec("lounge_say", c.post("/api/lounge/say", json={"text": "hola equipo"}))
    rec("office_profiles", c.get("/api/office/profiles"))
    rec("office_apply", c.post("/api/office/profile", headers=h, json={"id": "carpinteria"}))
    rec("office_bad", c.post("/api/office/profile", headers=h, json={"id": "no-existe"}))
    rec("agents_after", c.get("/api/agents"))
    rec("dl_missing", c.get("/descargas/nada.apk"))
    rec("logout", c.post("/api/admin/logout", headers=h))
    rec("after_logout", c.post("/api/admin/pin", headers=h, json={"pin": "1"}))
    out["chat"] = (200, asyncio.run(chat(base, "Cuanto es 12*7?")))
    out["chat2"] = (200, asyncio.run(chat(base, "hola, que puedes hacer?")))
    return out


def main():
    cpp, py = sys.argv[1], sys.argv[2]
    a, b = flows(cpp), flows(py)
    bad = 0
    for k in b:
        sa, va = a[k]
        sb, vb = b[k]
        d = ([f"estado {sa} en C++, {sb} en Python"] if sa != sb else []) + diff(va, vb)
        print(("OK  " if not d else "DIF ") + k)
        for line in d[:12]:
            print("      " + line)
        bad += bool(d)
    print("\nchat C++:", [e.get("type") for e in a["chat"][1]])
    print("chat Py :", [e.get("type") for e in b["chat"][1]])
    print("respuesta C++:", "".join(e.get("text", "") for e in a["chat"][1] if e.get("type") == "token")[:200])
    print("respuesta Py :", "".join(e.get("text", "") for e in b["chat"][1] if e.get("type") == "token")[:200])
    print(f"\n{len(b) - bad}/{len(b)} flujos iguales")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
