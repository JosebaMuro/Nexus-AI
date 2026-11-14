"""Hilo de las conversaciones largas: que el agente no pierda el hilo aunque la charla no quepa en el contexto.

Cada conversacion guarda un RESUMEN PROGRESIVO de lo ya hablado (se pliega por tandas, con el LLM o, sin el, de
forma extractiva) y unas NOTAS FIJAS que escribe el usuario y entran siempre en el prompt. En cada turno el contexto
se arma con un presupuesto en tokens sacado de n_ctx: notas + resumen + trozos antiguos que vienen al caso + los
ultimos mensajes literales.
"""
from __future__ import annotations

import queue
import re
import threading
import time
from dataclasses import dataclass, field

from .. import log
from .embeddings import tokens as palabras

L = log.get("hilo")
SCHEMA = """
CREATE TABLE IF NOT EXISTS hilos (
    session_id TEXT PRIMARY KEY, notas TEXT NOT NULL DEFAULT '', resumen TEXT NOT NULL DEFAULT '',
    hasta INTEGER NOT NULL DEFAULT 0, plegados INTEGER NOT NULL DEFAULT 0, actualizado REAL);
"""


def tokens(s: str) -> int:
    """Estimacion barata: unos 3,6 caracteres por token en espanol."""
    return (len(s or "") * 10 + 35) // 36


def recortar(s: str, max_tokens: int) -> str:
    """Recorta por el medio (cabeza y cola) para que quepa en max_tokens."""
    if tokens(s) <= max_tokens:
        return s
    chars = max(40, int(max_tokens * 3.6))
    head = chars * 2 // 3
    return s[:head].rstrip() + " [...] " + s[-(chars - head):].lstrip()


def resumen_extractivo(previo: str, mensajes: list[dict], resumen_tokens: int) -> str:
    """Sin LLM: la primera frase de cada mensaje, con las mas antiguas recortadas primero."""
    lineas = [x for x in (previo or "").split("\n") if x.strip()]
    for m in mensajes:
        txt = re.sub(r"\s+", " ", m.get("content") or "").strip()
        if not txt:
            continue
        frase = re.split(r"(?<=[.!?])\s", txt, maxsplit=1)[0][:220]
        lineas.append(("- Usuario: " if m.get("role") == "user" else "- Agente: ") + frase)
    while len(lineas) > 2 and tokens("\n".join(lineas)) > resumen_tokens:
        lineas.pop(0)
    return recortar("\n".join(lineas), resumen_tokens)


@dataclass
class HiloPlan:
    notas: str = ""
    resumen: str = ""
    recuerdos: list[str] = field(default_factory=list)
    recientes: list[dict] = field(default_factory=list)
    plegados: int = 0
    omitidos: int = 0
    tokens: int = 0

    def texto(self) -> str:
        partes = []
        if self.notas:
            partes.append("Notas fijas de esta conversacion (tenlas siempre presentes):\n" + self.notas)
        if self.resumen:
            partes.append("Resumen de lo hablado antes en esta conversacion:\n" + self.resumen)
        if self.recuerdos:
            partes.append("Fragmentos anteriores de esta conversacion que vienen al caso:\n" +
                          "\n".join(f"- {r}" for r in self.recuerdos))
        return "\n\n".join(partes)


class Hilo:
    def __init__(self, db, busy=None) -> None:
        self.db = db
        self.busy = busy or (lambda: False)
        self.llm_complete = None          # (prompt, max_tokens) -> texto; None = extractivo
        self.llm_real = lambda: True
        self._mu = threading.Lock()
        self._cola: queue.Queue = queue.Queue()
        self._hilo: threading.Thread | None = None
        with db._lock:
            db.conn.executescript(SCHEMA)
            db.conn.commit()

    # ------------------------------------------------------------ estado
    def _fila(self, sid: str) -> dict:
        return self.db.one("SELECT * FROM hilos WHERE session_id=?", (sid,)) or \
            {"session_id": sid, "notas": "", "resumen": "", "hasta": 0, "plegados": 0, "actualizado": 0}

    def estado(self, sid: str) -> dict:
        f = self._fila(sid)
        total = (self.db.one("SELECT COUNT(*) n FROM messages WHERE session_id=?", (sid,)) or {"n": 0})["n"]
        return {"sesion": sid, "notas": f["notas"], "resumen": f["resumen"], "plegados": f["plegados"],
                "mensajes": total, "tokens_notas": tokens(f["notas"]), "tokens_resumen": tokens(f["resumen"]),
                "actualizado": f["actualizado"]}

    def guardar(self, sid: str, cambios: dict) -> None:
        f = self._fila(sid)
        for k in ("notas", "resumen"):
            if k in cambios:
                f[k] = str(cambios[k] or "")[:20000]
        self._escribir(f)

    def _escribir(self, f: dict) -> None:
        self.db.execute(
            "INSERT INTO hilos(session_id, notas, resumen, hasta, plegados, actualizado) VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(session_id) DO UPDATE SET notas=excluded.notas, resumen=excluded.resumen, "
            "hasta=excluded.hasta, plegados=excluded.plegados, actualizado=excluded.actualizado",
            (f["session_id"], f["notas"], f["resumen"], f["hasta"], f["plegados"], time.time()))

    def olvidar(self, sid: str) -> None:
        self.db.execute("DELETE FROM hilos WHERE session_id=?", (sid,))

    def _pendientes(self, sid: str, hasta: int) -> list[dict]:
        return self.db.query("SELECT id, role, content FROM messages WHERE session_id=? AND id>? AND "
                             "role IN ('user','assistant') AND content<>'' ORDER BY id", (sid, hasta))

    # ------------------------------------------------------------ contexto del turno
    def plan(self, sid: str, query: str, budget: int) -> HiloPlan:
        f = self._fila(sid)
        p = HiloPlan(notas=recortar(f["notas"], max(64, budget // 5)), plegados=f["plegados"])
        p.resumen = recortar(f["resumen"], max(64, budget * 3 // 10))
        usado = tokens(p.notas) + tokens(p.resumen)
        vivos = self._pendientes(sid, f["hasta"])
        resto = max(128, budget - usado)
        elegidos: list[dict] = []
        gasto = 0
        for m in reversed(vivos):   # los ultimos, literales, hasta llenar lo que queda (minimo 2)
            txt = recortar(m["content"], max(96, resto // 3))
            t = tokens(txt) + 4
            if elegidos and len(elegidos) >= 2 and gasto + t > resto * 4 // 5:
                break
            elegidos.append({"role": m["role"], "content": txt, "id": m["id"]})
            gasto += t
        elegidos.reverse()
        fuera = vivos[:len(vivos) - len(elegidos)]
        p.omitidos = f["plegados"] + len(fuera)
        libre = resto - gasto
        if fuera and query and libre > 60:   # de lo que no entra literal, lo que tiene que ver con la pregunta
            q = set(palabras(query))
            puntuados = []
            for m in fuera:
                w = set(palabras(m["content"]))
                sc = len(q & w) / (len(q) or 1)
                if sc >= 0.34:
                    puntuados.append((sc, m))
            for _sc, m in sorted(puntuados, key=lambda x: -x[0])[:4]:
                trozo = recortar(re.sub(r"\s+", " ", m["content"]), 90)
                if tokens(trozo) > libre:
                    break
                p.recuerdos.append(("Usuario: " if m["role"] == "user" else "Agente: ") + trozo)
                libre -= tokens(trozo)
        p.recientes = [{"role": m["role"], "content": m["content"]} for m in elegidos]
        p.tokens = usado + gasto + sum(tokens(r) for r in p.recuerdos)
        return p

    # ------------------------------------------------------------ plegado
    def plegar(self, sid: str, recent_budget: int, resumen_tokens: int, llm=None) -> int:
        """Pasa al resumen los mensajes mas antiguos cuando lo literal pasa de recent_budget tokens. Se dejan
        siempre los dos ultimos. Devuelve cuantos mensajes ha plegado."""
        with self._mu:
            f = self._fila(sid)
            vivos = self._pendientes(sid, f["hasta"])
            if len(vivos) <= 2:
                return 0
            total = sum(tokens(m["content"]) + 4 for m in vivos)
            if total <= recent_budget:
                return 0
            objetivo = recent_budget // 2
            tanda = []
            for m in vivos[:-2]:
                if total <= objetivo:
                    break
                tanda.append(m)
                total -= tokens(m["content"]) + 4
            if not tanda:
                return 0
            f["resumen"] = self._resumir(f["resumen"], tanda, resumen_tokens, llm)
            f["hasta"] = tanda[-1]["id"]
            f["plegados"] += len(tanda)
            self._escribir(f)
            return len(tanda)

    def _resumir(self, previo: str, tanda: list[dict], resumen_tokens: int, llm) -> str:
        if llm is not None and self.llm_real():
            charla = "\n".join(("Usuario: " if m["role"] == "user" else "Agente: ") + recortar(m["content"], 400)
                               for m in tanda)
            prompt = ("Actualiza el resumen de una conversacion. Conserva decisiones, datos, nombres, cifras y tareas "
                      "pendientes; quita saludos y repeticiones. Escribe solo el resumen, en vinetas breves.\n\n"
                      f"Resumen hasta ahora:\n{previo or '(vacio)'}\n\nMensajes nuevos:\n{charla}\n\nResumen actualizado:")
            try:
                out = (llm(prompt, max(128, resumen_tokens)) or "").strip()
                if len(out) > 20:
                    return recortar(out, resumen_tokens)
            except Exception as e:  # noqa: BLE001
                L.warning("Resumen con el LLM fallido, se usa el extractivo: %s", e)
        return resumen_extractivo(previo, tanda, resumen_tokens)

    def encolar(self, sid: str, recent_budget: int, resumen_tokens: int) -> None:
        """Plegado en segundo plano: no retrasa la respuesta y espera a que el LLM quede libre."""
        self._cola.put((sid, recent_budget, resumen_tokens))
        if self._hilo is None or not self._hilo.is_alive():
            self._hilo = threading.Thread(target=self._bucle, name="nexus-hilo", daemon=True)
            self._hilo.start()

    def _bucle(self) -> None:
        while True:
            try:
                sid, rb, rt = self._cola.get(timeout=30)
            except queue.Empty:
                return
            for _ in range(120):
                if not self.busy():
                    break
                time.sleep(0.5)
            try:
                self.plegar(sid, rb, rt, self.llm_complete)
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo plegar la conversacion %s: %s", sid, e)
