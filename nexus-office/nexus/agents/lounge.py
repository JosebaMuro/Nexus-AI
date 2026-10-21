"""Salon virtual: sala de reuniones entre agentes.

* Cada tarea resuelta deja un "informe" en el salon y actualiza el directorio de habilidades
  (agente x tema -> exitos/fallos), alimentado tambien por la retroalimentacion del usuario.
* El directorio mejora el enrutado automatico y se inyecta en el prompt para que los agentes
  sepan en quien delegar.
* En las reuniones los agentes se presentan, comentan tareas recientes y el moderador detecta
  carencias (temas recurrentes sin especialista, fallos repetidos) y propone NUEVOS agentes,
  que quedan pendientes de aprobacion por un administrador.
Funciona con LLM (dialogo en personaje) y sin el (plantillas a partir de los datos).
"""
from __future__ import annotations

import json
import re
import threading
import time
from collections import Counter

from .. import log
from ..memory.embeddings import tokens
from ..memory.graph import KnowledgeGraph

L = log.get("lounge")
GENERIC_TOOLS = ["memory_search", "memory_save", "web_search", "web_fetch", "delegate", "calculator"]


class Lounge:
    def __init__(self, services) -> None:
        self.s = services
        self.db = services.db
        self.listeners: list = []
        self._meeting = threading.Lock()
        self.tasks_since_meeting = 0

    # ------------------------------------------------------------ mensajes
    def post(self, agent: str, content: str, kind: str = "chat", meta: dict | None = None) -> dict:
        ts = time.time()
        cur = self.db.execute("INSERT INTO lounge(ts, agent, kind, content, meta) VALUES(?,?,?,?,?)",
                              (ts, agent, kind, content[:2000], json.dumps(meta or {}, ensure_ascii=False)))
        msg = {"id": cur.lastrowid, "ts": ts, "agent": agent, "kind": kind, "content": content[:2000], "meta": meta or {}}
        for cb in list(self.listeners):
            try:
                cb({"type": "lounge", "message": msg})
            except Exception:  # noqa: BLE001
                pass
        return msg

    def history(self, limit: int = 120) -> list[dict]:
        rows = self.db.query("SELECT * FROM lounge ORDER BY id DESC LIMIT ?", (limit,))
        for r in rows:
            r["meta"] = json.loads(r["meta"] or "{}")
        return list(reversed(rows))

    # ------------------------------------------------------------ habilidades
    @staticmethod
    def topics(text: str, n: int = 4) -> list[str]:
        return [t.lower() for t in KnowledgeGraph.keywords(text, n)]

    def _bump(self, agent: str, topics: list[str], ok: float, fail: float) -> None:
        now = time.time()
        for t in topics:
            self.db.execute("INSERT INTO agent_skills(agent, topic, ok, fail, last) VALUES(?,?,?,?,?) ON CONFLICT(agent, topic) "
                            "DO UPDATE SET ok=ok+excluded.ok, fail=fail+excluded.fail, last=excluded.last",
                            (agent, t[:40], ok, fail, now))

    def record_task(self, agent: str, text: str, tool_log: list[dict], routed_by: str = "auto") -> None:
        topics = self.topics(text)
        failed = [t["tool"] for t in tool_log if not t["ok"]]
        ok = not failed
        self._bump(agent, topics, 1.0 if ok else 0.0, 0.0 if ok else 1.0)
        tools = ", ".join(t["tool"] for t in tool_log) or "sin herramientas"
        self.post(agent, f"Tarea sobre {', '.join(topics) or 'tema general'} · {tools}" + (f" · fallos: {', '.join(failed)}" if failed else ""),
                  "task", {"topics": topics, "ok": ok, "routed_by": routed_by})
        self.tasks_since_meeting += 1
        if self.tasks_since_meeting >= int(self.s.config.get("lounge.meet_every_tasks", 8)) and self.s.config.get("lounge.auto_meet", True):
            threading.Thread(target=self.meet, kwargs={"quick": True}, daemon=True).start()

    def on_feedback(self, agent: str, question: str, score: int) -> None:
        topics = self.topics(question)
        self._bump(agent, topics, 1.0 if score > 0 else 0.0, 0.0 if score > 0 else 1.5)
        self.post("usuario", f"{'👍' if score > 0 else '👎'} a {agent} en {', '.join(topics) or 'una tarea'}", "feedback",
                  {"agent": agent, "score": score, "topics": topics})

    def skills(self) -> dict[str, list[dict]]:
        out: dict[str, list[dict]] = {}
        for r in self.db.query("SELECT * FROM agent_skills ORDER BY agent, (ok - fail) DESC"):
            r["score"] = round((r["ok"] + 1) / (r["ok"] + r["fail"] + 2), 2)   # Laplace
            out.setdefault(r["agent"], []).append(r)
        return out

    def skill_score(self, agent: str, text: str) -> float:
        ts = set(self.topics(text, 6)) | set(tokens(text))
        if not ts:
            return 0.0
        marks = ",".join("?" * len(ts))
        rows = self.db.query(f"SELECT ok, fail FROM agent_skills WHERE agent=? AND topic IN ({marks})", (agent, *ts))
        return sum((r["ok"] - r["fail"]) / (r["ok"] + r["fail"] + 1) for r in rows)

    def directory(self, exclude: str = "") -> str:
        """Resumen 'quien sabe de que' para el prompt de los agentes."""
        lines = []
        for agent, rows in self.skills().items():
            if agent == exclude or agent == "usuario" or not self.s.agents.get(agent):
                continue
            good = [r["topic"] for r in rows if r["score"] >= 0.6][:6]
            if good:
                lines.append(f"- {agent}: buen historial en {', '.join(good)}")
        return "\n".join(lines)

    # ------------------------------------------------------------ carencias
    def detect_gaps(self, window: int = 60) -> list[dict]:
        tasks = [m for m in self.history(400) if m["kind"] in ("task", "feedback")][-window:]
        agent_kw = {a.id: {" ".join(tokens(k)) for k in a.keywords} for a in self.s.agents.list()}
        covered = set().union(*agent_kw.values()) if agent_kw else set()
        uncovered: Counter = Counter()
        struggling: Counter = Counter()
        for t in tasks:
            topics = t["meta"].get("topics", [])
            if t["kind"] == "task" and t["agent"] == "nexus":
                uncovered.update(x for x in topics if x not in covered)
            if (t["kind"] == "task" and not t["meta"].get("ok")) or (t["kind"] == "feedback" and t["meta"].get("score", 0) < 0):
                struggling.update(topics)
        existing = {json.loads(p["spec"]).get("id") for p in self.db.query("SELECT spec FROM agent_proposals")}
        gaps = []
        for topic, n in (uncovered + struggling).most_common(5):
            if n < 2 or any(topic in kw for kw in agent_kw.values()):
                continue
            gaps.append({"topic": topic, "count": n, "struggling": struggling.get(topic, 0),
                         "already_proposed": f"experto-{re.sub(r'[^a-z0-9]+', '-', topic)}"[:30] in existing})
        return gaps

    def _spec_for_gap(self, gap: dict, related: list[str]) -> dict:
        topic = gap["topic"]
        aid = f"experto-{re.sub(r'[^a-z0-9]+', '-', topic)}"[:30]
        spec = {"id": aid, "name": f"Experto en {topic}", "description": f"Especialista en {topic} y temas afines",
                "system_prompt": (f"Eres el agente experto en {topic} de {{assistant_name}}. Respondes con precision sobre "
                                  f"{topic}, investigas cuando falta informacion y delegas lo que no es de tu area."),
                "model": "default", "temperature": 0.4, "tools": GENERIC_TOOLS, "memory": True,
                "can_delegate_to": ["*"], "keywords": [topic] + related[:5], "color": "#9d7bff", "icon": "◈", "enabled": True}
        if self.s.llm.has_real_llm():
            try:
                raw = self.s.llm.complete(
                    "Disena un agente de IA especialista. Devuelve SOLO JSON con claves name, description, system_prompt "
                    f"(en espanol, 2-3 frases), keywords (lista de 6 palabras). Tema: {topic}. Temas relacionados: "
                    f"{', '.join(related)}. Herramientas disponibles: {', '.join(t['name'] for t in self.s.tools.list())}.",
                    max_tokens=350, temperature=0.4)
                data = json.loads(raw[raw.find("{"): raw.rfind("}") + 1])
                for k in ("name", "description", "system_prompt"):
                    if isinstance(data.get(k), str) and data[k].strip():
                        spec[k] = data[k].strip()[:600]
                if isinstance(data.get("keywords"), list):
                    spec["keywords"] = [str(k).lower()[:30] for k in data["keywords"]][:8] or spec["keywords"]
            except Exception as e:  # noqa: BLE001
                L.debug("Diseno de agente con LLM fallido: %s", e)
        return spec

    def propose_agent(self, spec: dict, reason: str, proposer: str = "nexus") -> dict:
        cur = self.db.execute("INSERT INTO agent_proposals(ts, spec, reason, status, proposer) VALUES(?,?,?,?,?)",
                              (time.time(), json.dumps(spec, ensure_ascii=False), reason, "pendiente", proposer))
        self.post(proposer, f"Propongo crear el agente «{spec['name']}»: {reason}", "proposal",
                  {"proposal_id": cur.lastrowid, "spec": spec})
        return {"id": cur.lastrowid, "spec": spec, "reason": reason, "status": "pendiente"}

    def proposals(self) -> list[dict]:
        rows = self.db.query("SELECT * FROM agent_proposals ORDER BY id DESC LIMIT 50")
        for r in rows:
            r["spec"] = json.loads(r["spec"])
        return rows

    def resolve_proposal(self, pid: int, approve: bool, note: str = "") -> dict:
        row = self.db.one("SELECT * FROM agent_proposals WHERE id=?", (pid,))
        if not row:
            raise KeyError(pid)
        spec = json.loads(row["spec"])
        self.db.execute("UPDATE agent_proposals SET status=? WHERE id=?", ("aprobada" if approve else "rechazada", pid))
        if approve:
            self.s.agents.save(spec)
            self.post(spec["id"], f"¡Hola a todos! Soy {spec['name']}. {spec['description']}. Podeis delegarme temas de "
                                  f"{', '.join(spec.get('keywords', [])[:4])}.", "intro")
        else:
            self.post("admin", f"Propuesta «{spec['name']}» rechazada. {note}".strip(), "chat")
        return {"ok": True, "spec": spec}

    # ------------------------------------------------------------ reuniones
    def _say(self, agent, prompt_ctx: str, fallback: str) -> str:
        if not self.s.llm.has_real_llm():
            return fallback
        try:
            text = self.s.llm.complete(
                f"Eres {agent.name} ({agent.description}) en una reunion breve con otros agentes de IA de tu equipo. "
                f"{prompt_ctx}\nResponde en espanol, en 1-2 frases, en primera persona, sin saludos largos.",
                agent.model, max_tokens=120, temperature=0.8, reasoning="off")   # charla breve: sin razonar, contesta ya
            return re.sub(r"<tool>.*?</tool>", "", text, flags=re.S).strip() or fallback
        except Exception:  # noqa: BLE001
            return fallback

    @property
    def busy(self) -> bool:
        return self._meeting.locked()

    def meet(self, quick: bool = False, topic: str = "") -> dict:
        """Celebra una reunion. Devuelve resumen con huecos detectados y propuestas."""
        if not self._meeting.acquire(blocking=False):
            return {"busy": True}
        try:
            self.tasks_since_meeting = 0
            agents = self.s.agents.list()
            skills = self.skills()
            self.post("nexus", f"Abro la reunion del salon{': ' + topic if topic else ''}. "
                               f"Somos {len(agents)} agentes.", "system", {"event": "meeting_start"})
            if not quick:  # ronda de presentaciones (con muchos agentes, solo los de mas actividad para no eternizarla)
                cap = int(self.s.config.get("lounge.max_intros", 8))
                speakers = agents
                if len(agents) > cap:
                    active = Counter(m["agent"] for m in self.history(300) if m["kind"] in ("task", "feedback"))
                    speakers = sorted(agents, key=lambda a: (a.id != "nexus", -active.get(a.id, 0)))[:cap]
                for a in speakers:
                    strong = [r["topic"] for r in skills.get(a.id, []) if r["score"] >= 0.6][:4]
                    tools = ", ".join(a.tools[:5]) if a.tools != ["*"] else "todas las herramientas"
                    fb = f"Soy {a.name}: {a.description}. Uso {tools}." + (f" Se me da bien: {', '.join(strong)}." if strong else "")
                    self.post(a.id, self._say(a, f"Presentate y di en que destacas. Datos: herramientas {tools}; "
                                                 f"temas con buen historial: {', '.join(strong) or 'aun ninguno'}.", fb), "intro")
                    time.sleep(0.05)
            # repaso de tareas recientes: quien deberia encargarse
            recent = [m for m in self.history(80) if m["kind"] == "task"][-6:]
            for t in recent:
                topics = t["meta"].get("topics", [])
                if not topics:
                    continue
                text = " ".join(topics)
                best = max(agents, key=lambda a: self.skill_score(a.id, text) + self.s.agents.keyword_score(a, text))
                if best.id != t["agent"] and self.skill_score(best.id, text) + self.s.agents.keyword_score(best, text) > 0:
                    self.post(best.id, f"Para temas como {', '.join(topics[:3])}, {t['agent']} puede delegarme: tengo "
                                       f"mejor encaje.", "insight", {"topics": topics, "from": t["agent"], "to": best.id})
                elif not t["meta"].get("ok"):
                    self.post(t["agent"], f"En la tarea sobre {', '.join(topics[:3])} hubo fallos; anoto la leccion.",
                              "insight", {"topics": topics})
            # opinion libre si se dio un tema
            if topic:
                for a in sorted(agents, key=lambda a: -self.s.agents.keyword_score(a, topic))[:4]:
                    self.post(a.id, self._say(a, f"El tema de la reunion es: {topic}. Aporta tu punto de vista.",
                                              f"Sobre «{topic}», puedo ayudar desde mi especialidad: {a.description.lower()}."), "chat")
            # carencias -> propuestas de agente
            proposals = []
            for gap in self.detect_gaps():
                if gap["already_proposed"]:
                    continue
                related = [w for w, _ in Counter(
                    w for m in self.history(200) if gap["topic"] in m["meta"].get("topics", [])
                    for w in m["meta"].get("topics", []) if w != gap["topic"]).most_common(5)]
                spec = self._spec_for_gap(gap, related)
                reason = (f"«{gap['topic']}» aparecio {gap['count']} veces sin especialista claro"
                          + (f" y hubo {gap['struggling']} resultados flojos" if gap["struggling"] else ""))
                proposals.append(self.propose_agent(spec, reason))
            directory = self.directory()
            self.post("nexus", ("Directorio actualizado:\n" + directory) if directory else
                      "Aun no hay historial suficiente para el directorio de habilidades.", "summary",
                      {"event": "meeting_end", "proposals": len(proposals)})
            return {"proposals": proposals, "directory": directory}
        finally:
            self._meeting.release()

    def user_says(self, text: str) -> None:
        """El usuario habla en el salon: responde el agente con mejor encaje."""
        self.post("usuario", text, "chat")
        target = self.s.agents.route(text)
        agent = self.s.agents.get(target)

        def work():
            fb = f"Me encargo yo ({agent.name}); para hacerlo, escribelo en el chat principal y lo resolvere."
            self.post(agent.id, self._say(agent, f"El usuario dice en el salon: «{text}». Responde o indica quien deberia encargarse.", fb), "chat")

        threading.Thread(target=work, daemon=True).start()
