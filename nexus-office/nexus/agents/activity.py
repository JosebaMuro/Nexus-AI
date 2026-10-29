"""Que esta haciendo cada agente ahora mismo (para los despachos animados, el reloj y el salon).

Estados: ``idle`` (sin tarea), ``thinking`` (pensando/razonando), ``writing`` (escribiendo la respuesta), ``tool``
(usando una herramienta), ``meeting`` (en una reunion del salon), ``delegating`` (pasando trabajo a otro agente).
"""
from __future__ import annotations

import threading
import time

TOOL_SCENES = {   # herramienta -> escena del despacho
    "web_search": "browsing", "web_fetch": "browsing", "rag_search": "reading", "rag_import_url": "reading",
    "generate_image": "drawing", "imagen_editar": "drawing", "gimp_script": "drawing", "compose_music": "music",
    "calculator": "calculating", "python": "coding", "run_code": "coding", "delegate": "delegating",
    "graph_import": "reading", "consultar_profesor": "phone",
}


class Activity:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.state: dict[str, dict] = {}
        self.listeners: list = []

    def set(self, agent: str, estado: str, tarea: str | None = None, herramienta: str = "") -> None:
        now = time.time()
        with self._lock:
            cur = self.state.get(agent) or {"agente": agent, "tareas": 0, "desde": now, "tarea": ""}
            if cur.get("estado") == estado and cur.get("herramienta") == herramienta and tarea is None:
                return
            if estado == "idle" and cur.get("estado") not in (None, "idle"):
                cur["tareas"] = cur.get("tareas", 0) + 1
                cur["ultima"] = now
            cur.update(estado=estado, herramienta=herramienta, desde=now,
                       escena=TOOL_SCENES.get(herramienta, "working") if estado == "tool" else estado)
            if tarea is not None:
                cur["tarea"] = tarea[:140]
            self.state[agent] = cur
            snap = dict(cur)
        for fn in list(self.listeners):
            try:
                fn({"type": "agent_activity", "activity": snap})
            except Exception:   # noqa: BLE001
                pass

    def on_event(self, agent: str, e: dict, task: str = "") -> None:
        t = e.get("type")
        if t == "state":
            st = e.get("state")
            if st == "thinking":
                self.set(agent, "thinking", task if self.get(agent).get("estado") in (None, "idle") else None)
            elif st == "idle":
                self.set(agent, "idle")
        elif t == "token":
            self.set(agent, "writing")
        elif t == "tool_call":
            name = e.get("name", "")
            self.set(agent, "delegating" if name == "delegate" else "tool", herramienta=name)

    def get(self, agent: str) -> dict:
        with self._lock:
            return dict(self.state.get(agent) or {})

    def all(self) -> dict[str, dict]:
        now = time.time()
        with self._lock:
            out = {a: dict(v) for a, v in self.state.items()}
        for v in out.values():
            v["segundos"] = round(now - v.get("desde", now))
        return out


ACTIVITY = Activity()
