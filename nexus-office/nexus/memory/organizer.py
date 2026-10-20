"""Organizador del conocimiento: decide que es comun y que es saber hacer de cada agente.

* Grafo general (comun a todos): datos y gustos del usuario, pautas de comportamiento («respondeme breve», «no me
  hables de usted»), relaciones entre personas y entidades, interacciones entre el usuario y los agentes, y el
  conocimiento general que sirve para razonar sobre cualquier cosa.
* Subgrafo de cada agente (solo suyo): el saber hacer de su especialidad, sus procedimientos y documentos.

Las pautas, relaciones y preferencias se detectan con reglas (ES/EN). Para los temas se compara el texto con el
perfil de especialidad de cada agente (descripcion, ``specialty`` y palabras clave): con un modelo de embeddings
semantico (Qwen3-Embedding, instruccion «clasifica por area de conocimiento») y, sin el, por palabras clave.
Gana el especialista que encaje con claridad; si ninguno lo hace, va al general como conocimiento comun.
"""
from __future__ import annotations

import re
import time

import numpy as np

from .. import log
from .embeddings import normalize, tokens
from .graph import AGENT, AGENT_CATEGORIES, GENERAL, GENERAL_CATEGORIES, USER, KnowledgeGraph, _key

L = log.get("organizer")

# ------------------------------------------------------------------ reglas: comportamiento y relaciones
# verbos de trato en 2a persona (subjuntivo/indicativo): «no me hables...», «prefiero que respondas...»
_V = (r"(?:respond|contest|habl|escrib|explic|us|utilic|trat|llam|dig|pong|hag|mand|formate|resum|corrij|pregunt|repit|"
      r"invent|tute|ignor|traduzc|cit|alarg|extiend|incluy|emple|met|dej|insist|enroll|interrump|repas|ofrezc|propong|"
      r"sugier|recomiend|pid)\w*?(?:as|es)")
BEHAVIOR = [
    re.compile(rf"\b(?:prefiero|quiero|me gusta(?:r[ií]a)?|necesito|te pido) que (?:me |le |nos )?{_V}\b[^.!?\n]{{0,140}}", re.I),
    re.compile(rf"\b(?:no|nunca|jam[aá]s) (?:me |le |nos |lo |la |los |las )?{_V}\b[^.!?\n]{{0,140}}", re.I),
    re.compile(r"\b(?:a partir de ahora|de ahora en adelante|en adelante|por defecto)\b[ ,:]+[^.!?\n]{5,140}", re.I),
    re.compile(rf"\b(?:siempre) (?:me |le |nos )?(?:{_V}|\w+(?:me|nos))\b[^.!?\n]{{0,120}}", re.I),
    re.compile(r"\b(?:s[eé]|sed) (?:m[aá]s |menos )?(?:breve|conciso|concisa|directo|directa|detallado|detallada|formal|"
               r"informal|claro|clara|t[eé]cnico|t[eé]cnica|amable|sincero|sincera)\b[^.!?\n]{0,80}", re.I),
    re.compile(r"\b(?:h[aá]blame|tr[aá]tame|dir[ií]gete a m[ií]) de (?:t[uú]|usted)\b", re.I),
    re.compile(r"\b\w+(?:me|nos)\s+(?:siempre|nunca)\b[^.!?\n]{3,120}", re.I),          # respondeme siempre con...
    re.compile(r"\bll[aá]mame\s+[^.!?\n]{2,40}", re.I),
    re.compile(r"\bcuando (?:te pida|te diga|te pregunte|te hable|te mande)\b[^.!?\n]{5,140}", re.I),
    re.compile(r"\b(?:i prefer|i want|i'?d like) (?:you|that you) (?:to )?\w+[^.!?\n]{2,120}", re.I),
    re.compile(r"\b(?:always|never|from now on)\b[ ,]+(?:answer|reply|respond|use|write|call|explain|cite|include|"
               r"speak|talk|be|keep|give|show|ask)\b[^.!?\n]{0,120}", re.I),
    re.compile(r"\b(?:be|keep it) (?:more |less )?(?:brief|concise|short|detailed|formal|informal|direct)\b[^.!?\n]{0,80}", re.I),
]
KIN = (r"hermano|hermana|madre|padre|mam[aá]|pap[aá]|hijo|hija|mujer|marido|esposa|esposo|pareja|novio|novia|jefe|jefa|"
       r"amigo|amiga|compa[nñ]ero|compa[nñ]era|socio|socia|cliente|vecino|vecina|abuelo|abuela|t[ií]o|t[ií]a|primo|prima|"
       r"perro|perra|gato|gata|brother|sister|mother|father|son|daughter|wife|husband|partner|boss|friend|colleague")
CAP = r"[A-ZÁÉÍÓÚÑ][\wáéíóúñü]+"
RELATIONS = [
    re.compile(rf"\b(?i:mis?) ((?i:{KIN}))\s+(?:se llama\s+|es\s+|,\s*)?({CAP}(?:\s+{CAP})?)"),
    re.compile(rf"\b({CAP}(?:\s+{CAP})?) es (?i:mi) ((?i:{KIN}))\b"),
    re.compile(rf"\b(?i:my) ((?i:{KIN}))(?:'s name is|\s+is called|\s+is)?\s+({CAP}(?:\s+{CAP})?)"),
]
# prototipos del conocimiento comun (para comparar con los perfiles de los especialistas)
GENERAL_PROTOTYPES = [
    "Conversación cotidiana, cultura general, actualidad, ocio, viajes, comida, salud, deportes y temas comunes",
    "Personas, familia, amigos, lugares, fechas y acontecimientos de la vida diaria del usuario",
    "Gustos, preferencias, opiniones, costumbres y forma de ser del usuario",
    "Cómo debe comportarse y responder el asistente: tono, estilo, longitud, idioma y normas de trato",
    "Conocimiento del mundo y sentido común útil para razonar sobre cualquier tema",
]
GENERALIST = {"nexus"}   # el agente principal coordina: lo suyo es comun


def _clip(text: str, n: int = 140) -> str:
    text = re.sub(r"\s+", " ", text).strip(" .,;:")
    return text[:n]


class Organizer:
    def __init__(self, graph: KnowledgeGraph, embedder_getter, agents_getter=None, config=None) -> None:
        self.graph = graph
        self.embedder_getter = embedder_getter          # siempre el embedder activo (se cambia en caliente)
        self.agents_getter = agents_getter or (lambda: [])
        self.config = config
        self._profiles: tuple | None = None             # (clave, ids, matriz) cacheado por embedder y agentes

    # ------------------------------------------------------------ ajustes
    def _cfg(self, key: str, default):
        return self.config.get(f"memory.organizer.{key}", default) if self.config is not None else default

    @property
    def embedder(self):
        return self.embedder_getter()

    def semantic(self) -> bool:
        return not str(getattr(self.embedder, "id", "hash")).startswith("hash")

    # ------------------------------------------------------------ especialistas
    def specialists(self) -> list:
        out = []
        for a in self.agents_getter():
            if a.id in GENERALIST or not getattr(a, "enabled", True):
                continue
            if getattr(a, "specialty", "") or a.keywords or a.description:
                out.append(a)
        return out

    @staticmethod
    def profile_text(a) -> str:
        return (f"{a.name}. {a.description}. {getattr(a, 'specialty', '')}. "
                f"Palabras clave: {', '.join(a.keywords)}").strip()

    @staticmethod
    def keyword_hits(a, text: str) -> float:
        words = set(tokens(text))
        low = " ".join(tokens(text))
        terms = list(a.keywords) + [t.strip() for t in re.split(r"[,;\n]", getattr(a, "specialty", "") or "")
                                    if 0 < len(t.strip()) <= 30]
        score = 0.0
        for k in dict.fromkeys(terms):
            kt = " ".join(tokens(k))
            if not kt:
                continue
            if " " in kt and kt in low:
                score += 2
            elif kt in words or (len(kt) >= 6 and any(w.startswith(kt) for w in words)):
                score += 1
        return score

    def _profile_matrix(self, agents: list):
        emb = self.embedder
        key = (emb.id, tuple((a.id, self.profile_text(a)) for a in agents))
        if self._profiles is None or self._profiles[0] != key:
            texts = [self.profile_text(a) for a in agents] + GENERAL_PROTOTYPES
            self._profiles = (key, emb.embed(texts))
        mat = self._profiles[1]
        return mat[: len(agents)], mat[len(agents):]

    def affinities(self, texts: list[str], current: str = "") -> list[dict]:
        """Para cada texto: puntuacion de cada especialista y del conocimiento comun, y el destino elegido."""
        agents = self.specialists()
        if not agents or not texts:
            return [{"scope": "", "category": "comun", "score": 0.0, "scores": {}} for _ in texts]
        kws = [[self.keyword_hits(a, t) for a in agents] for t in texts]
        out = []
        if self.semantic():
            prof, gen = self._profile_matrix(agents)
            q = self.embedder.embed_query([t[:2000] for t in texts], "classify")
            sims, gsims = q @ prof.T, (q @ gen.T).max(axis=1)
            min_aff, margin = float(self._cfg("min_affinity", 0.35)), float(self._cfg("margin", 0.03))
            for i in range(len(texts)):
                scores = {a.id: float(sims[i, j]) + 0.04 * min(kws[i][j], 3) + (0.03 if a.id == current else 0)
                          for j, a in enumerate(agents)}
                best = max(scores, key=scores.get)
                j = [a.id for a in agents].index(best)
                ok = sims[i, j] >= min_aff and scores[best] - float(gsims[i]) >= margin
                out.append({"scope": best if ok else "", "category": "especialidad" if ok else "comun",
                            "score": round(scores[best], 3), "general": round(float(gsims[i]), 3),
                            "scores": {k: round(v, 3) for k, v in scores.items()}})
            return out
        for i in range(len(texts)):   # sin modelo semantico: palabras clave de cada especialidad
            scores = {a.id: kws[i][j] + (0.5 if a.id == current and kws[i][j] else 0) for j, a in enumerate(agents)}
            best = max(scores, key=scores.get)
            ok = scores[best] >= float(self._cfg("min_keyword_hits", 1))
            out.append({"scope": best if ok else "", "category": "especialidad" if ok else "comun",
                        "score": float(scores[best]), "scores": scores})
        return out

    def route(self, text: str, current: str = "") -> tuple[str, str]:
        """(ambito, categoria) para los temas de un texto. Sin especialistas conocidos (p.ej. en pruebas sin agentes),
        lo aprendido sigue siendo del agente que conversa, como antes de la 1.6."""
        if not self.specialists():
            return (current or "", "especialidad" if current else "comun")
        try:
            r = self.affinities([text], current)[0]
        except Exception as e:  # noqa: BLE001
            L.warning("Clasificacion semantica no disponible (%s): se usan palabras clave", e)
            agents = self.specialists()
            hits = {a.id: self.keyword_hits(a, text) for a in agents}
            best = max(hits, key=hits.get)
            return (best, "especialidad") if hits[best] >= 1 else ("", "comun")
        if r["scope"] and re.search(r"\b(paso \d|primero|despu[eé]s|luego|finalmente|c[oó]mo se hace|procedimiento|"
                                    r"instala|configura|ejecuta|step \d|how to)\b", text, re.I):
            return r["scope"], "procedimiento"
        return r["scope"], r["category"]

    # ------------------------------------------------------------ lo comun: pautas y relaciones
    @staticmethod
    def strip_behaviors(text: str) -> str:
        """El texto sin las pautas de comportamiento («respondeme siempre con ejemplos»): sus palabras no son temas."""
        for pat in BEHAVIOR:
            text = pat.sub(" ", text)
        return text

    def extract_general(self, text: str) -> list[dict]:
        """Pautas de comportamiento y relaciones del usuario -> grafo general. Devuelve lo aprendido."""
        g = self.graph.general()
        learned: list[dict] = []
        with g.db.transaction():
            rules = list(dict.fromkeys(_clip(m.group(0)) for pat in BEHAVIOR for m in pat.finditer(text)))
            rules = [r for r in rules if len(r) >= 6 and not any(r != o and r.lower() in o.lower() for o in rules)]
            for rule in rules:
                if g.relate(USER, "pide", rule, "persona", "pauta", 1.2, category="comportamiento"):
                    learned.append({"s": USER, "r": "pide", "o": rule, "type": "pauta", "category": "comportamiento"})
            for pat in RELATIONS:
                for m in pat.finditer(text):
                    a, b = m.group(1), m.group(2)
                    kin, name = (a, b) if re.fullmatch(KIN, a, re.I) else (b, a)
                    rel = "su_" + normalize(kin).replace(" ", "_")
                    if _key(name) and g.relate(USER, rel, name, "persona", "persona", 1.3, category="relacion"):
                        learned.append({"s": USER, "r": rel, "o": name, "type": "persona", "category": "relacion"})
        return learned

    def record_interaction(self, who: str, rel: str, whom: str, weight: float = 0.2) -> None:
        """Interacciones usuario-agente y entre agentes (conversa_con, delega_en): conocimiento comun."""
        if not who or not whom or _key(who) == _key(whom):
            return
        g = self.graph.general()
        wtype = "persona" if _key(who) == _key(USER) else "interlocutor"
        g.relate(who, rel, whom, wtype, "interlocutor", weight, category="interaccion", src_category=(
            "perfil" if wtype == "persona" else "interaccion"))

    # ------------------------------------------------------------ vectores de nodos (busqueda y duplicados)
    def node_vectors(self, rows: list[dict]) -> np.ndarray:
        """Vector de cada nodo (nombre + nota), guardado en entity_vecs por embedder: solo se calcula lo nuevo."""
        emb = self.embedder
        db = self.graph.db
        if not rows:
            return np.zeros((0, 1), dtype=np.float32)
        ids = [r["id"] for r in rows]
        have = {r["entity_id"]: r for r in db.query(
            f"SELECT entity_id, key, vec FROM entity_vecs WHERE embedder=? AND entity_id IN ({','.join(map(str, ids))})",
            (emb.id,))}
        todo = [r for r in rows if r["id"] not in have or have[r["id"]]["key"] != r["key"]]
        if todo:
            vecs = emb.embed([f"{r['name']}. {r.get('note') or ''}".strip(" .") for r in todo])
            db.executemany("INSERT OR REPLACE INTO entity_vecs(entity_id, embedder, key, vec) VALUES(?,?,?,?)",
                           ((r["id"], emb.id, r["key"], v.astype(np.float32).tobytes()) for r, v in zip(todo, vecs)))
            for r, v in zip(todo, vecs):
                have[r["id"]] = {"vec": v.astype(np.float32).tobytes()}
        return np.vstack([np.frombuffer(have[i]["vec"], dtype=np.float32) for i in ids])

    def semantic_search(self, q: str, scope: str | None = None, limit: int = 30, pool: int = 3000) -> list[dict]:
        where, params = ("WHERE scope=?", (scope,)) if scope is not None else ("", ())
        rows = self.graph.db.query(f"SELECT id, key, name, type, scope, category, weight, note FROM entities {where} "
                                   f"ORDER BY weight DESC LIMIT ?", (*params, pool))
        if not rows or not q.strip():
            return []
        mat = self.node_vectors(rows)
        qv = self.embedder.embed_query([q], "graph")[0]
        if mat.shape[1] != qv.shape[0]:
            return []
        sims = mat @ qv
        order = np.argsort(-sims)[:limit]
        return [{**rows[i], "similarity": round(float(sims[i]), 3)} for i in order]

    def duplicates(self, scope: str | None = None, threshold: float | None = None, pool: int = 2000,
                   limit: int = 60) -> list[dict]:
        """Parejas de nodos del mismo ambito que parecen el mismo concepto (para fusionarlos)."""
        thr = threshold if threshold is not None else (0.9 if self.semantic() else 0.82)
        scopes = [scope] if scope is not None else [r["scope"] for r in self.graph.db.query(
            "SELECT DISTINCT scope FROM entities")]
        out = []
        for sc in scopes:
            rows = [r for r in self.graph.db.query(
                "SELECT id, key, name, type, scope, category, weight, note FROM entities WHERE scope=? "
                "ORDER BY weight DESC LIMIT ?", (sc, pool)) if r["type"] not in ("self", AGENT) and r["key"] != _key(USER)]
            if len(rows) < 2:
                continue
            mat = self.node_vectors(rows)
            sims = mat @ mat.T
            np.fill_diagonal(sims, -1)
            ii, jj = np.where(np.triu(sims, 1) >= thr)
            for i, j in zip(ii, jj):
                a, b = rows[int(i)], rows[int(j)]
                keep, drop = (a, b) if a["weight"] >= b["weight"] else (b, a)
                out.append({"keep": keep, "drop": drop, "similarity": round(float(sims[i, j]), 3)})
        out.sort(key=lambda d: -d["similarity"])
        return out[:limit]

    # ------------------------------------------------------------ reorganizacion de lo ya aprendido
    def plan(self, progress=None, limit: int = 20000) -> list[dict]:
        """Propuesta de movimientos para que cada cosa quede en su sitio (no cambia nada)."""
        db = self.graph.db
        rows = db.query("SELECT id, key, name, type, scope, category, note FROM entities ORDER BY weight DESC LIMIT ?",
                        (limit,))
        moves: list[dict] = []
        topical: list[dict] = []
        for r in rows:
            if r["type"] in ("self", AGENT) or r["key"] == _key(USER):
                continue
            if r["type"] == "documento" or r["category"] == "documento":
                continue   # los documentos siguen a su biblioteca (Biblioteca > mover a otro grafo)
            personal = r["category"] in GENERAL_CATEGORIES and r["category"] != "comun"
            if not personal and r["scope"] != GENERAL:   # hechos del usuario que acabaron en un agente
                tie = db.one("SELECT r.rel FROM relations r JOIN entities u ON u.id=r.src WHERE r.dst=? AND u.key=? "
                             "AND r.rel NOT IN ('menciona','relacionado_con') LIMIT 1", (r["id"], _key(USER)))
                personal = bool(tie)
            if personal:
                if r["scope"] != GENERAL:
                    cat = r["category"] if r["category"] in GENERAL_CATEGORIES else "perfil"
                    moves.append({**r, "to": GENERAL, "to_category": cat, "reason": "conocimiento del usuario (comun)"})
                continue
            topical.append(r)
        if self.specialists():
            for b in range(0, len(topical), 64):
                block = topical[b:b + 64]
                texts = []
                for r in block:
                    near = db.query("SELECT e.name FROM relations x JOIN entities e ON e.id = CASE WHEN x.src=? THEN x.dst "
                                    "ELSE x.src END WHERE (x.src=? OR x.dst=?) AND e.type NOT IN ('self','agente') "
                                    "ORDER BY x.weight DESC LIMIT 8", (r["id"], r["id"], r["id"]))
                    texts.append(f"{r['name']}. {r['note'] or ''} " + ", ".join(n["name"] for n in near))
                for r, aff in zip(block, self.affinities(texts)):
                    if aff["scope"] != r["scope"]:
                        moves.append({**r, "to": aff["scope"], "to_category": aff["category"], "score": aff["score"],
                                      "reason": "especialidad de un agente" if aff["scope"] else "conocimiento comun"})
                if progress:
                    progress(min(b + 64, len(topical)), len(topical))
        return moves

    def apply(self, moves: list[dict], memories: bool = True, store=None) -> dict:
        moved = 0
        for m in moves:
            try:
                self.graph.move_entity(m["id"], m["to"], m.get("to_category", ""))
                moved += 1
            except (KeyError, ValueError) as e:
                L.info("No se movio %s: %s", m.get("name"), e)
        fixed = 0
        if memories and store is not None:   # recuerdos del usuario siempre comunes
            fixed = store.db.execute("UPDATE memories SET agent='' WHERE agent!='' AND kind IN "
                                     "('fact','preference','behavior')").rowcount
            store._cache = None
        return {"moved": moved, "memories_shared": fixed, "ts": time.time()}

    def overview(self) -> dict:
        db = self.graph.db
        cats = self.graph.categories_by_scope()
        return {"general_categories": GENERAL_CATEGORIES, "agent_categories": AGENT_CATEGORIES, "by_scope": cats,
                "semantic": self.semantic(), "embedder": getattr(self.embedder, "id", ""),
                "specialists": [{"id": a.id, "name": a.name, "profile": self.profile_text(a)} for a in self.specialists()],
                "relations": db.one("SELECT COUNT(*) n FROM relations")["n"],
                "bridges": db.one("SELECT COUNT(*) n FROM relations r JOIN entities a ON a.id=r.src JOIN entities b "
                                  "ON b.id=r.dst WHERE a.scope!=b.scope")["n"]}
