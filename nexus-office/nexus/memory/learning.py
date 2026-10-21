"""Aprendizaje continuo:

* Hechos, preferencias, pautas de comportamiento y relaciones extraidos de lo que dice el usuario, e interacciones
  entre el usuario y los agentes: memoria + grafo general, comun a todos.
* Los temas van a donde los coloca el organizador (nexus.memory.organizer): al subgrafo aislado del especialista
  cuyo saber hacer encaja, o al general como conocimiento comun para la inferencia.
* Retroalimentacion (pulgar arriba/abajo): ejemplos positivos se reutilizan como few-shot,
  los negativos generan "lecciones" y ajustan la importancia de los recuerdos usados.
* Preferencia de longitud de respuesta aprendida de la retroalimentacion.
* Consolidacion periodica de conversaciones en resumenes episodicos.
"""
from __future__ import annotations

import json
import re
import time

from .. import log
from ..db import DB
from .embeddings import tokens
from .graph import USER, KnowledgeGraph
from .organizer import Organizer
from .store import MemoryStore

L = log.get("learning")
REL_TEXT = {"se_llama": "El usuario se llama {o}", "le_gusta": "Al usuario le gusta {o}",
            "no_le_gusta": "Al usuario no le gusta {o}", "vive_en": "El usuario vive en {o}",
            "trabaja_en": "El usuario trabaja en {o}", "es": "El usuario es {o}", "tiene": "El usuario tiene {o}",
            "aprende": "El usuario esta aprendiendo {o}"}
EXPLICIT = re.compile(r"^\s*(?:recuerda(?: que)?|apunta(?: que)?|no olvides(?: que)?|remember(?: that)?)\s*[:,]?\s*(.+)", re.I | re.S)


class Learner:
    def __init__(self, db: DB, memory: MemoryStore, graph: KnowledgeGraph, organizer: Organizer | None = None) -> None:
        self.db, self.memory, self.graph = db, memory, graph
        self.organizer = organizer or Organizer(graph, lambda: memory.embedder)
        self.llm_complete = None          # se inyecta si se activa la extraccion con LLM
        self.namer = lambda aid: aid      # id de agente -> nombre visible (lo inyecta core.Services)
        self._since_consolidation: dict[str, int] = {}

    def on_user_message(self, text: str, session_id: str, agent: str = "", llm_extraction: bool = False) -> list[dict]:
        learned: list[dict] = []
        m = EXPLICIT.match(text)
        if m:
            self.memory.remember(m.group(1).strip(), "fact", 0.9, source="explicito", agent="")
            learned.append({"s": "Usuario", "r": "pidio_recordar", "o": m.group(1).strip()[:80], "type": "nota"})
        return learned + self.learn_text(text, agent, llm_extraction)

    def learn_text(self, text: str, agent: str = "", llm_extraction: bool = False) -> list[dict]:
        """Incorpora un texto dicho o ensenado por el usuario: lo comun al grafo general y los temas a donde los
        coloque el organizador. Tambien lo usan «Ensenar algo» (Nucleo) y la herramienta memory_save."""
        learned: list[dict] = []
        topics = self.organizer.strip_behaviors(text)
        scope, category = self.organizer.route(topics, agent)
        dest = self.graph.for_agent(scope)
        with self.db.transaction():
            triples = self.graph.general().extract_personal(text)   # lo que se sabe del usuario: para todos
            general = self.organizer.extract_general(text)           # pautas de comportamiento y relaciones: idem
            triples += general
            triples += dest.extract_topics(topics, "user", category)   # temas: especialidad de un agente o comun
        if llm_extraction and self.llm_complete:
            triples += dest.extract_with_llm(text, self.llm_complete)
        for t in triples:
            tmpl = REL_TEXT.get(t["r"])
            if tmpl:
                kind = "preference" if "gusta" in t["r"] else "fact"
                self.memory.remember(tmpl.format(o=t["o"]), kind, 0.8, source="conversacion")
                if t["r"] == "se_llama":
                    self.db.set_profile("user_name", t["o"])
            elif t.get("category") == "comportamiento":
                self.memory.remember(t["o"], "behavior", 0.85, source="conversacion")
            elif t.get("category") == "relacion":
                self.memory.remember(f"{t['o']} es {t['r'][3:].replace('_', ' ')} del usuario", "fact", 0.75,
                                     source="conversacion")
        learned += triples
        return learned

    def on_exchange(self, session_id: str, user: str, assistant: str, agent: str, consolidate_every: int = 12) -> None:
        scope, category = self.organizer.route(f"{user[:600]}\n{assistant[:1500]}", agent)
        self.graph.for_agent(scope).extract_topics(assistant[:1500], "assistant", category)
        if agent:
            try:   # interaccion usuario <-> agente: conocimiento comun
                self.organizer.record_interaction(USER, "conversa_con", self.namer(agent) or agent)
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo anotar la interaccion: %s", e)
        n = self._since_consolidation.get(session_id, 0) + 1
        self._since_consolidation[session_id] = n
        if n >= consolidate_every:
            self.consolidate(session_id)
            try:
                self.graph.for_agent(agent).prune()
                self.graph.general().prune()
            except Exception as e:  # noqa: BLE001
                L.warning("Poda del grafo fallida: %s", e)
            self._since_consolidation[session_id] = 0

    # ------------------------------------------------------------ retroalimentacion
    def feedback(self, message_id: int, score: int, comment: str = "") -> dict:
        score = 1 if score > 0 else -1
        self.db.execute("INSERT INTO feedback(message_id, score, comment, ts) VALUES(?,?,?,?)",
                        (message_id, score, comment, time.time()))
        msg = self.db.one("SELECT * FROM messages WHERE id=?", (message_id,))
        if not msg:
            return {"ok": False}
        prev = self.db.one("SELECT content FROM messages WHERE session_id=? AND id<? AND role='user' "
                           "ORDER BY id DESC LIMIT 1", (msg["session_id"], message_id))
        question = prev["content"] if prev else ""
        meta = json.loads(msg["meta"] or "{}")
        for mid in meta.get("memories", []):
            self.memory.adjust(mid, 0.1 * score)
        if score > 0 and question:
            self.memory.remember(f"P: {question[:400]}\nR: {msg['content'][:900]}", "example", 0.8,
                                 source="feedback+", agent=msg["agent"])
        elif score < 0:
            lesson = f"Una respuesta a '{question[:120]}' no gusto al usuario"
            if comment:
                lesson += f": {comment[:200]}"
            self.memory.remember(lesson, "lesson", 0.85, source="feedback-", agent=msg["agent"])
        # preferencia de longitud: positivo en respuestas largas => le gustan largas, y viceversa
        long_answer = len(msg["content"]) > 700
        prof = self.db.get_profile()
        bias = float(prof.get("length_bias", 0.0))
        bias += 0.15 * score * (1 if long_answer else -1)
        self.db.set_profile("length_bias", max(-1.0, min(1.0, round(bias, 2))))
        return {"ok": True, "length_bias": bias}

    # ------------------------------------------------------------ consolidacion
    def consolidate(self, session_id: str, summarize=None) -> str:
        msgs = self.memory.history(session_id, 30)
        if len(msgs) < 4:
            return ""
        convo = "\n".join(f"{m['role']}: {m['content'][:400]}" for m in msgs)
        summary = ""
        if summarize:
            try:
                summary = summarize("Resume en 3 frases los temas y datos importantes de esta conversacion:\n" + convo)
            except Exception as e:  # noqa: BLE001
                L.warning("Resumen con LLM fallido: %s", e)
        if not summary:  # resumen extractivo
            topics = self.graph.keywords(" ".join(m["content"] for m in msgs if m["role"] == "user"), 6)
            firsts = [m["content"][:120] for m in msgs if m["role"] == "user"][:3]
            summary = f"Conversacion sobre: {', '.join(topics)}. Peticiones: " + " | ".join(firsts)
        self.memory.remember(summary, "summary", 0.6, source=f"sesion:{session_id}")
        self.memory.decay()
        return summary

    # ------------------------------------------------------------ contexto para el prompt
    def behaviors(self, limit: int = 8) -> list[str]:
        """Pautas de comportamiento que el usuario ha pedido (comunes a todos los agentes), las mas recientes primero."""
        rows = self.db.query("SELECT content FROM memories WHERE kind='behavior' AND agent='' ORDER BY importance DESC, "
                             "created DESC LIMIT ?", (limit,))
        return [r["content"] for r in rows]

    def build_context(self, query: str, agent: str = "", k: int = 6) -> dict:
        mems = self.memory.recall(query, k=k, kinds=("fact", "preference", "summary", "note", "episode", "skill"),
                                  agent=agent or None)   # sin los recuerdos privados de otros agentes
        examples = self.memory.recall(query, k=2, kinds=("example",), agent=agent or None, min_score=0.3)
        lessons = self.memory.recall(query, k=2, kinds=("lesson",), min_score=0.25)
        prof = self.db.get_profile()
        names = self.graph.keywords(query, 4)
        general = self.graph.general()
        own = self.graph.for_agent(agent) if agent else None
        facts = general.user_facts(10)
        if own is not None:   # primero lo que el agente ha aprendido por su cuenta, despues lo comun
            facts += own.facts_about(names + [own.self_name], 6)
        facts += general.facts_about(names, 6)
        if own is not None:   # atributos de lo nombrado (p. ej. propiedades de un grafo importado)
            facts += own.notes_about(names, 3)
        facts += general.notes_about(names, 3)
        return {"memories": mems, "examples": examples, "lessons": lessons, "facts": list(dict.fromkeys(facts)),
                "profile": prof, "query_terms": tokens(query)[:12], "behaviors": self.behaviors()}
