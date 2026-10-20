"""Grafo de conocimiento propio: entidades y relaciones aprendidas de la conversacion.

La extraccion heuristica (ES/EN) funciona siempre; si hay LLM y esta activado, se puede
complementar con extraccion de tripletas por el modelo (ver ``extract_with_llm``).
El grafo alimenta el avatar 3D del webui.

Ambitos: el grafo general (``scope=""``) es comun a todos los agentes y guarda lo que se sabe del usuario y
lo importado "para todos". Dentro de el, cada agente tiene su propio subgrafo aislado (``scope=<id del agente>``)
donde acumula su conocimiento: lo que aprende en sus conversaciones y los documentos RAG importados solo para el.
Un agente lee el general y el suyo; nunca el de otro agente.

Organizacion (1.6): el general guarda conocimiento generico -como es el usuario y que le gusta, pautas de
comportamiento, relaciones e interacciones y conocimiento comun para la inferencia- y cada subgrafo de agente guarda
el saber hacer de su especialidad. Cada nodo lleva una categoria (``CATEGORIES``) y nexus.memory.organizer decide a
que ambito va cada cosa. Una relacion puede salir de un subgrafo de agente hacia un nodo del general (puente): es
del agente (su ``scope``) y solo la ve el.
"""
from __future__ import annotations

import json
import re
import time
from collections import Counter

from ..db import DB
from .embeddings import normalize, tokens

SELF = "Nexus-Office"
USER = "Usuario"
CAP = r"[A-ZÁÉÍÓÚÑ][\wáéíóúñü]+"
STOP = set(normalize("""
el la los las un una unos unas de del al a en y o u que se por para con sin sobre como mas pero sus su mi mis tu tus
es son fue era ser estar esta este estos estas eso esto ese esa aqui alli hay muy tambien ya no si le les lo me te nos
yo tu el ella ellos nosotros vosotros usted ustedes cual cuales quien donde cuando porque puedes puede quiero quieres
hola gracias favor algo nada todo todos cosa cosas hacer tiene tengo tienes
hablame dime cuentame explica explicame compon componer calcula crea genera dibuja escribe busca quiero
necesito ayuda ayudame sabes puedo podrias haz dame muestra
the a an of to in on and or for with is are was were be been this that these those it its i you he she we they my your
what which who when where why how can could would should will just about from into than then there here please thanks
""").split())

PATTERNS = [
    (re.compile(rf"\b(?:me llamo|mi nombre es|my name is|i am called|llamame)\s+({CAP}(?:\s+{CAP})?)", re.I), "se_llama", "persona"),
    (re.compile(r"\b(?:no me gusta(?:n)?|odio|detesto|i hate|i dislike|i don'?t like)\s+(?:el |la |los |las |the )?([^.,;!?\n]{2,40})", re.I), "no_le_gusta", "preferencia"),
    (re.compile(r"\b(?<!no )(?:me gusta(?:n)?|me encanta(?:n)?|adoro|i like|i love|i enjoy)\s+(?:el |la |los |las |the )?([^.,;!?\n]{2,40})", re.I), "le_gusta", "preferencia"),
    (re.compile(rf"\b(?:vivo en|soy de|resido en|i live in|i'?m from)\s+({CAP}(?:\s+{CAP})*)", re.I), "vive_en", "lugar"),
    (re.compile(r"\b(?:trabajo (?:en|para)|i work (?:at|for))\s+([^.,;!?\n]{2,40})", re.I), "trabaja_en", "organizacion"),
    (re.compile(r"\b(?:trabajo como|soy (?:un |una )?(?=programador|ingenier|disenador|diseñador|medic|profesor|estudiante|artista|music|desarrollador)|i work as (?:an? )?|i'?m an? (?=developer|engineer|designer|doctor|teacher|student|artist|musician))([^.,;!?\n]{2,40})", re.I), "es", "rol"),
    (re.compile(r"\b(?:tengo un|tengo una|i have an?)\s+([^.,;!?\n]{2,30})", re.I), "tiene", "objeto"),
    (re.compile(r"\b(?:estoy aprendiendo|aprendo|i'?m learning|i am learning)\s+([^.,;!?\n]{2,40})", re.I), "aprende", "concepto"),
]
IS_A = re.compile(rf"\b({CAP}(?:[ \t]+{CAP})*)\s+(?:es|is)\s+(?:un|una|el|la|an?|the)\s+([\wáéíóúñ ]{{3,40}})", re.U)


def _key(name: str) -> str:
    return " ".join(tokens(name))


def _clean(obj: str) -> str:
    obj = re.split(r"\b(?:y|and|pero|but|porque|because|que|that)\b", obj.strip(), maxsplit=1)[0]
    return obj.strip(" .,'\"").strip()


GENERAL = ""
# categorias de conocimiento: las primeras son del grafo general (comunes), las demas de cada agente (su especialidad)
GENERAL_CATEGORIES = {
    "perfil": "Datos del usuario", "preferencia": "Gustos y preferencias",
    "comportamiento": "Pautas de comportamiento", "relacion": "Relaciones entre personas y entidades",
    "interaccion": "Interacciones entre el usuario y los agentes", "comun": "Conocimiento general para la inferencia"}
AGENT_CATEGORIES = {
    "especialidad": "Saber hacer de su especialidad", "procedimiento": "Procedimientos y técnicas",
    "documento": "Documentos y fuentes", "experiencia": "Experiencia propia"}
CATEGORIES = {**GENERAL_CATEGORIES, **AGENT_CATEGORIES}
PERSONAL_CATEGORY = {"le_gusta": "preferencia", "no_le_gusta": "preferencia"}   # el resto de hechos: perfil
AGENT = "agente"            # tipo del nodo ancla de cada subgrafo de agente
LINK_REL = "subgrafo"       # enlace virtual nucleo -> ancla de agente en la vista completa
WEAK = ("menciona", "relacionado_con")


class KnowledgeGraph:
    def __init__(self, db: DB, scope: str = GENERAL, self_name: str = SELF, root: "KnowledgeGraph | None" = None) -> None:
        self.db, self.scope = db, scope
        self.self_name = self_name
        self.self_type = "self" if scope == GENERAL else AGENT
        self.root = root or self
        self._views: dict[str, KnowledgeGraph] = {}
        self.namer = None            # aid -> nombre visible del agente (lo inyecta core.Services)
        with db.transaction():
            if scope != GENERAL:   # agente renombrado: su ancla cambia de nombre en vez de duplicarse
                db.execute("UPDATE OR IGNORE entities SET key=?, name=? WHERE scope=? AND type=? AND key!=?",
                           (_key(self_name), self_name.strip()[:60], scope, AGENT, _key(self_name)))
                db.execute("UPDATE entities SET category='' WHERE scope=? AND key=? AND category!=''", (scope, _key(USER)))
            self.upsert_entity(self_name, self.self_type, weight=5)
            self.upsert_entity(USER, "persona", weight=4)

    # ------------------------------------------------------------ ambitos
    def for_agent(self, agent: str | None) -> "KnowledgeGraph":
        """Subgrafo aislado del agente (se crea al primer uso). Sin agente: el grafo general."""
        root = self.root
        agent = (agent or "").strip()
        if not agent:
            return root
        view = root._views.get(agent)
        if view is None:
            name = (root.namer(agent) if root.namer else "") or agent
            view = root._views[agent] = KnowledgeGraph(self.db, agent, name, root)
        return view

    def forget_view(self, agent: str) -> None:
        """El agente ha cambiado (p.ej. de nombre): la proxima vista se rehace."""
        self.root._views.pop(agent, None)

    def general(self) -> "KnowledgeGraph":
        return self.root

    def scopes(self) -> list[dict]:
        rows = self.db.query("SELECT scope, COUNT(*) n FROM entities GROUP BY scope")
        rels = {r["scope"]: r["n"] for r in self.db.query("SELECT scope, COUNT(*) n FROM relations GROUP BY scope")}
        return [{"scope": r["scope"], "entities": r["n"], "relations": rels.get(r["scope"], 0)} for r in rows]

    def drop_scope(self, agent: str) -> int:
        """Borra el subgrafo de un agente (al eliminar el agente). El general no se puede borrar."""
        if not agent:
            raise ValueError("el grafo general no se puede borrar")
        with self.db.transaction():
            self.db.execute("DELETE FROM relations WHERE scope=?", (agent,))
            n = self.db.execute("DELETE FROM entities WHERE scope=?", (agent,)).rowcount
        self.root._views.pop(agent, None)
        return n

    # ------------------------------------------------------------ escritura
    def default_category(self, etype: str = "concepto") -> str:
        if etype == "documento":
            return "documento"
        return "comun" if self.scope == GENERAL else "especialidad"

    def upsert_entity(self, name: str, etype: str = "concepto", weight: float = 1.0, category: str = "",
                      note: str = "", source: str = "") -> int:
        key = _key(name)
        if not key:
            return 0
        now = time.time()
        if etype in ("self", AGENT) or key == _key(USER):
            # el Usuario solo es "perfil" en el general; en un subgrafo de agente es un ancla, como el propio agente
            category = "" if self.scope != GENERAL else (category or ("perfil" if key == _key(USER) else ""))
        else:
            category = category or self.default_category(etype)
        # una sola sentencia (antes: SELECT + UPDATE/INSERT y una confirmacion por paso). La categoria solo cambia si
        # la nueva es mas especifica que la generica por defecto; la nota y la fuente se conservan si ya las hay.
        row = self.db.returning(
            "INSERT INTO entities(scope, key, name, type, weight, first_seen, last_seen, category, note, source) "
            "VALUES(?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(scope, key) DO UPDATE SET mentions=mentions+1, weight=weight+excluded.weight*0.5, "
            "last_seen=excluded.last_seen, type=CASE WHEN type IN ('concepto','') AND excluded.type!='concepto' "
            "THEN excluded.type ELSE type END, "
            "category=CASE WHEN category IN ('', 'comun', 'especialidad') AND excluded.category NOT IN ('', 'comun', "
            "'especialidad') THEN excluded.category WHEN category='' THEN excluded.category ELSE category END, "
            "note=CASE WHEN note='' THEN excluded.note ELSE note END, "
            "source=CASE WHEN source='' THEN excluded.source ELSE source END RETURNING id",
            (self.scope, key, name.strip()[:60], etype, weight, now, now, category, note[:500], source[:120]))
        return int(row["id"]) if row else 0

    def relate(self, src: str, rel: str, dst: str, src_type: str = "concepto", dst_type: str = "concepto",
               weight: float = 1.0, category: str = "", src_category: str = "") -> tuple[int, int] | None:
        """category: la del objeto (dst); src_category: la del sujeto. Vacias: la de este ambito por defecto."""
        with self.db.transaction():
            a = self.upsert_entity(src, src_type, category=src_category)
            b = self.upsert_entity(dst, dst_type, category=category)
            if not a or not b or a == b:
                return None
            self.db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                            "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
                            "last_seen=excluded.last_seen", (a, b, rel, weight, time.time(), self.scope))
        return a, b

    # ------------------------------------------------------------ extraccion
    def extract(self, text: str, speaker: str = "user") -> list[dict]:
        """Extrae y guarda hechos en este grafo; devuelve las tripletas aprendidas."""
        with self.db.transaction():   # decenas de escrituras -> una sola confirmacion
            learned = self.extract_personal(text) if speaker == "user" else []
            return learned + self.extract_topics(text, speaker)

    def extract_personal(self, text: str) -> list[dict]:
        """Hechos sobre el usuario (nombre, gustos, lugar...)."""
        learned: list[dict] = []
        with self.db.transaction():
            for pat, rel, otype in PATTERNS:
                for m in pat.finditer(text):
                    obj = _clean(m.group(1))
                    if len(obj) < 2 or _key(obj) in STOP:
                        continue
                    cat = PERSONAL_CATEGORY.get(rel, "perfil")
                    if self.relate(USER, rel, obj, "persona", otype, 1.5, category=cat):
                        learned.append({"s": USER, "r": rel, "o": obj, "type": otype, "category": cat})
        return learned

    def extract_topics(self, text: str, speaker: str = "user", category: str = "") -> list[dict]:
        """Definiciones ("X es un Y") y conceptos mencionados con sus co-ocurrencias. category: la que haya decidido
        el organizador (comun en el general, especialidad o procedimiento en un agente)."""
        learned: list[dict] = []
        cat = category or self.default_category()
        with self.db.transaction():
            for m in IS_A.finditer(text):
                subj, obj = m.group(1).strip(), _clean(m.group(2))
                if _key(subj) not in STOP and len(obj) > 2 and self.relate(subj, "es_un", obj, "concepto", "concepto",
                                                                           category=cat, src_category=cat):
                    learned.append({"s": subj, "r": "es_un", "o": obj, "type": "concepto", "scope": self.scope,
                                    "category": cat})
            concepts = self.keywords(text, 4)
            anchor, atype = (USER, "persona") if speaker == "user" else (self.self_name, self.self_type)
            for c in concepts:
                self.relate(anchor, "menciona", c, atype, "concepto", 0.3, category=cat)
            for i, a in enumerate(concepts):
                for b in concepts[i + 1:]:
                    self.relate(a, "relacionado_con", b, weight=0.2, category=cat, src_category=cat)
        return learned

    def link_document(self, title: str, text: str = "", kind: str = "documento") -> int:
        """Incorpora un documento RAG a este grafo: nodo del documento, quien lo ha leido y de que trata."""
        title = title.strip()[:60]
        if not _key(title):
            return 0
        n = 0
        with self.db.transaction():
            self.upsert_entity(title, "documento", weight=2, category="documento")
            self.relate(self.self_name, "ha_leido", title, self.self_type, "documento", 1.0, category="documento")
            for kw in self.keywords(f"{title}. {text[:6000]}", 8):
                if _key(kw) != _key(title) and self.relate(title, "trata_de", kw, "documento", "concepto", 0.6,
                                                           src_category="documento"):
                    n += 1
        return n

    def unlink_document(self, title: str) -> None:
        """Quita el nodo de un documento (al borrarlo o moverlo a otro grafo) y los conceptos que solo colgaban de el."""
        row = self.db.one("SELECT id FROM entities WHERE scope=? AND key=? AND type='documento'",
                          (self.scope, _key(title.strip()[:60])))
        if not row:
            return
        with self.db.transaction():
            orphans = [r["dst"] for r in self.db.query(
                "SELECT r.dst FROM relations r JOIN entities e ON e.id=r.dst WHERE r.src=? AND r.rel='trata_de' "
                "AND e.mentions<=1 AND (SELECT COUNT(*) FROM relations x WHERE x.src=r.dst OR x.dst=r.dst)=1", (row["id"],))]
            for eid in [row["id"], *orphans]:
                self.delete_entity(eid)

    @staticmethod
    def keywords(text: str, n: int = 4) -> list[str]:
        proper = [m.group(0) for m in re.finditer(rf"(?<![.!?]\s)(?<!^)(?<!\n)\b{CAP}(?:[ \t]+{CAP})*", text)]
        words = [w for w in tokens(text) if len(w) >= 5 and w not in STOP and not w.isdigit()]
        counts = Counter(words)
        out: list[str] = []
        for p in proper:
            if _key(p) not in STOP and _key(p) not in (_key(SELF), _key(USER)) and p not in out:
                out.append(p)
        for w, _ in counts.most_common(n):
            if all(_key(o) != w for o in out):
                out.append(w)
        return out[:n]

    def extract_with_llm(self, text: str, complete) -> list[dict]:
        """``complete(prompt)->str``: pide al LLM tripletas JSON y las guarda."""
        prompt = ("Extrae hechos como JSON: lista de objetos {\"s\":sujeto,\"r\":relacion,\"o\":objeto,\"type\":tipo}. "
                  "Usa 'Usuario' para el usuario. Solo JSON.\nTexto: " + text[:2000])
        try:
            raw = complete(prompt)
            data = json.loads(raw[raw.find("["): raw.rfind("]") + 1])
        except (ValueError, TypeError):
            return []
        out = []
        with self.db.transaction():
            for t in data[:15]:
                if isinstance(t, dict) and t.get("s") and t.get("o") and t.get("r"):
                    cat = str(t.get("category", "")) if t.get("category") in CATEGORIES else ""
                    self.relate(str(t["s"]), str(t["r"])[:30], str(t["o"]), dst_type=str(t.get("type", "concepto")),
                                category=cat, src_category=cat)
                    out.append(t)
        return out

    # ------------------------------------------------------------ lectura
    def facts_about(self, names: list[str], limit: int = 12) -> list[str]:
        keys = list(dict.fromkeys(k for k in (_key(n) for n in names) if k))
        if not keys:
            return []
        marks = ",".join("?" * len(keys))
        # ids primero (indice scope+key) y despues relaciones por los indices de src/dst
        rows = self.db.query(
            f"WITH ids AS (SELECT id FROM entities WHERE scope=? AND key IN ({marks})) "
            f"SELECT a.name AS s, r.rel, b.name AS o, r.weight FROM relations r JOIN entities a ON a.id=r.src "
            f"JOIN entities b ON b.id=r.dst WHERE (r.src IN ids OR r.dst IN ids) AND r.scope=? AND r.rel NOT IN "
            f"('menciona','relacionado_con','conversa_con','delega_en','pide') ORDER BY r.weight DESC LIMIT ?", (self.scope, *keys, self.scope, limit))
        return [f"{r['s']} {r['rel'].replace('_', ' ')} {r['o']}" for r in rows]

    def user_facts(self, limit: int = 15) -> list[str]:
        return self.facts_about([USER], limit)

    def to_json(self, limit: int = 220, scope: str | None = None) -> dict:
        """scope=None: este grafo. scope="*": el general con el subgrafo de cada agente colgando del nucleo."""
        if scope == "*":
            return self._to_json_all(limit)
        scope = self.scope if scope is None else scope
        nodes = self.db.query("SELECT id, name, type, weight, mentions, last_seen, scope, category FROM entities "
                              "WHERE scope=? ORDER BY weight DESC LIMIT ?", (scope, limit))
        if scope != GENERAL:   # vista aislada de un agente: su ancla es el nucleo
            for n in nodes:
                if n["type"] == AGENT:
                    n["type"], n["agent_core"] = "self", True
        return {"nodes": nodes, "links": self._links([n["id"] for n in nodes], limit * 3)}

    def _to_json_all(self, limit: int) -> dict:
        counts = {r["scope"]: r["n"] for r in self.db.query("SELECT scope, COUNT(*) n FROM entities GROUP BY scope")}
        total = sum(counts.values()) or 1
        nodes: list[dict] = []
        for sc, n in counts.items():   # reparto proporcional (minimo 12 por agente) para que todos se vean
            quota = max(12, round(limit * n / total)) if sc != GENERAL else max(limit // 2, round(limit * n / total))
            nodes += self.db.query("SELECT id, name, type, weight, mentions, last_seen, scope, category FROM entities "
                                   "WHERE scope=? ORDER BY weight DESC LIMIT ?", (sc, quota))
        links = self._links([n["id"] for n in nodes], limit * 3)
        core = next((n["id"] for n in nodes if n["scope"] == GENERAL and n["type"] == "self"), None)
        if core is not None:
            links += [{"source": core, "target": n["id"], "rel": LINK_REL, "weight": 3.0}
                      for n in nodes if n["type"] == AGENT]
        return {"nodes": nodes, "links": links}

    def _links(self, ids: list[int], limit: int) -> list[dict]:
        if not ids:
            return []
        marks = ",".join(str(int(i)) for i in ids)
        return self.db.query(f"SELECT src AS source, dst AS target, rel, weight FROM relations "
                             f"WHERE src IN ({marks}) AND dst IN ({marks}) ORDER BY weight DESC LIMIT ?", (limit,))

    def stats(self) -> dict:
        """Totales de todo el grafo (general + agentes) y desglose por ambito."""
        by = self.scopes()
        return {"entities": sum(r["entities"] for r in by), "relations": sum(r["relations"] for r in by),
                "scopes": {r["scope"] or "general": {"entities": r["entities"], "relations": r["relations"]} for r in by}}

    def delete_entity(self, eid: int) -> None:
        row = self.db.one("SELECT key, type FROM entities WHERE id=?", (eid,))
        if not row or row["type"] in ("self", AGENT) or row["key"] == _key(USER):
            return   # los nucleos (Nexus-Office, cada agente) y el usuario no se borran
        with self.db.transaction():
            self.db.execute("DELETE FROM relations WHERE src=? OR dst=?", (eid, eid))
            self.db.execute("DELETE FROM entities WHERE id=?", (eid,))

    # ------------------------------------------------------------ gestion (herramienta de Ajustes)
    def protected(self, row: dict) -> bool:
        """Nucleos (Nexus-Office y el ancla de cada agente) y el nodo del usuario: no se borran, mueven ni fusionan."""
        return row["type"] in ("self", AGENT) or row["key"] == _key(USER)

    def entity(self, eid: int) -> dict | None:
        return self.db.one("SELECT * FROM entities WHERE id=?", (eid,))

    def node(self, eid: int) -> dict | None:
        """Nodo con todas sus relaciones (entrantes y salientes, con el ambito de cada una)."""
        row = self.entity(eid)
        if not row:
            return None
        rels = self.db.query(
            "SELECT r.id, r.src, r.dst, r.rel, r.weight, r.scope, a.name AS src_name, b.name AS dst_name, "
            "a.scope AS src_scope, b.scope AS dst_scope FROM relations r JOIN entities a ON a.id=r.src "
            "JOIN entities b ON b.id=r.dst WHERE r.src=? OR r.dst=? ORDER BY r.weight DESC LIMIT 300", (eid, eid))
        return {**row, "protected": self.protected(row), "relations": rels}

    def search_nodes(self, scope: str | None = None, q: str = "", category: str = "", etype: str = "",
                     limit: int = 100, offset: int = 0) -> dict:
        where, params = [], []
        if scope is not None:
            where.append("scope=?")
            params.append(scope)
        if category:
            where.append("category=?")
            params.append(category)
        if etype:
            where.append("type=?")
            params.append(etype)
        if q.strip():
            where.append("(key LIKE ? OR name LIKE ? OR note LIKE ?)")
            like = f"%{_key(q) or q.strip()}%"
            params += [like, f"%{q.strip()}%", f"%{q.strip()}%"]
        sql = (" WHERE " + " AND ".join(where)) if where else ""
        total = self.db.one(f"SELECT COUNT(*) n FROM entities{sql}", params)["n"]
        rows = self.db.query(
            f"SELECT e.*, (SELECT COUNT(*) FROM relations r WHERE r.src=e.id OR r.dst=e.id) AS degree "
            f"FROM entities e{sql} ORDER BY weight DESC, id LIMIT ? OFFSET ?",
            (*params, limit, offset))
        return {"total": total, "items": rows}

    def update_entity(self, eid: int, name: str | None = None, etype: str | None = None, category: str | None = None,
                      note: str | None = None, weight: float | None = None) -> dict:
        row = self.entity(eid)
        if not row:
            raise KeyError(eid)
        if category is not None and category and category not in CATEGORIES:
            raise ValueError(f"categoria desconocida: {category}")
        with self.db.transaction():
            if name is not None and name.strip() and _key(name) != row["key"]:
                if self.protected(row):
                    raise ValueError("los nucleos y el usuario no se renombran aqui")
                other = self.db.one("SELECT id FROM entities WHERE scope=? AND key=?", (row["scope"], _key(name)))
                if other:   # ya existe ese nombre en el mismo ambito: se fusionan
                    self.merge_entities(eid, other["id"])
                    eid = other["id"]
                else:
                    self.db.execute("UPDATE entities SET key=?, name=? WHERE id=?", (_key(name), name.strip()[:60], eid))
            elif name is not None and name.strip():
                self.db.execute("UPDATE entities SET name=? WHERE id=?", (name.strip()[:60], eid))
            for col, val in (("type", etype), ("category", category), ("note", note)):
                if val is not None and not (col == "type" and self.protected(row)):
                    self.db.execute(f"UPDATE entities SET {col}=? WHERE id=?", (str(val)[:500], eid))
            if weight is not None:
                self.db.execute("UPDATE entities SET weight=? WHERE id=?", (max(0.0, float(weight)), eid))
        return self.entity(eid)

    def add_entity(self, scope: str, name: str, etype: str = "concepto", category: str = "", note: str = "",
                   source: str = "manual") -> dict:
        if category and category not in CATEGORIES:
            raise ValueError(f"categoria desconocida: {category}")
        eid = self.for_agent(scope).upsert_entity(name, etype or "concepto", 1.0, category, note, source)
        if not eid:
            raise ValueError("nombre vacio")
        return self.entity(eid)

    @staticmethod
    def relation_scope(a: str, b: str) -> str:
        """Ambito de una relacion entre dos nodos: el mismo, o el del agente si une su subgrafo con el general."""
        if a == b:
            return a
        if GENERAL in (a, b):
            return a or b
        raise ValueError("no se pueden relacionar los subgrafos de dos agentes distintos (estan aislados)")

    def add_relation(self, src: int, dst: int, rel: str, weight: float = 1.0) -> dict:
        a, b = self.entity(src), self.entity(dst)
        if not a or not b or src == dst:
            raise ValueError("relacion invalida")
        rel = re.sub(r"\s+", "_", rel.strip().lower())[:30] or "relacionado_con"
        scope = self.relation_scope(a["scope"], b["scope"])
        self.db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                        "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
                        "last_seen=excluded.last_seen", (src, dst, rel, float(weight), time.time(), scope))
        return self.db.one("SELECT * FROM relations WHERE src=? AND dst=? AND rel=?", (src, dst, rel))

    def delete_relation(self, rid: int) -> None:
        self.db.execute("DELETE FROM relations WHERE id=?", (rid,))

    def merge_entities(self, src: int, dst: int) -> dict:
        """Fusiona ``src`` en ``dst`` (duplicados): suma pesos y menciones y redirige todas sus relaciones."""
        a, b = self.entity(src), self.entity(dst)
        if not a or not b or src == dst:
            raise ValueError("fusion invalida")
        if self.protected(a):
            raise ValueError("los nucleos y el usuario no se pueden fusionar en otro nodo")
        with self.db.transaction():
            for r in self.db.query("SELECT * FROM relations WHERE src=? OR dst=?", (src, src)):
                s2 = dst if r["src"] == src else r["src"]
                d2 = dst if r["dst"] == src else r["dst"]
                self.db.execute("DELETE FROM relations WHERE id=?", (r["id"],))
                if s2 == d2:
                    continue
                ends = {e["id"]: e["scope"] for e in self.db.query("SELECT id, scope FROM entities WHERE id IN (?,?)", (s2, d2))}
                try:
                    scope = self.relation_scope(ends[s2], ends[d2])
                except (ValueError, KeyError):
                    continue
                self.db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                                "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight, "
                                "last_seen=MAX(last_seen, excluded.last_seen)",
                                (s2, d2, r["rel"], r["weight"], r["last_seen"], scope))
            self.db.execute(
                "UPDATE entities SET weight=weight+?, mentions=mentions+?, first_seen=MIN(first_seen, ?), "
                "last_seen=MAX(last_seen, ?), note=CASE WHEN note='' THEN ? ELSE note END, "
                "category=CASE WHEN category IN ('', 'comun', 'especialidad') AND ? NOT IN ('', 'comun', 'especialidad') "
                "THEN ? ELSE category END WHERE id=?",
                (a["weight"], a["mentions"], a["first_seen"] or time.time(), a["last_seen"] or time.time(), a["note"],
                 a["category"], a["category"], dst))
            self.db.execute("DELETE FROM entities WHERE id=?", (src,))
        return self.entity(dst)

    def move_entity(self, eid: int, scope: str, category: str = "") -> dict:
        """Mueve un nodo al grafo general ("") o al subgrafo de un agente. Si alli ya existe uno igual, se fusionan.

        Sus relaciones le acompanan: con nodos del mismo ambito siguen igual, con el general quedan como puente del
        agente y las que unirian dos agentes distintos se eliminan (los subgrafos estan aislados)."""
        row = self.entity(eid)
        if not row:
            raise KeyError(eid)
        if self.protected(row):
            raise ValueError("los nucleos y el usuario no se mueven")
        scope = scope or GENERAL
        if category and category not in CATEGORIES:
            raise ValueError(f"categoria desconocida: {category}")
        if scope != GENERAL:
            self.for_agent(scope)   # crea el ancla del agente si aun no existe
        if row["scope"] == scope:
            return self.update_entity(eid, category=category) if category else row
        new_cat = category or (row["category"] if (row["category"] in GENERAL_CATEGORIES) == (scope == GENERAL)
                               else ("comun" if scope == GENERAL else "especialidad"))
        with self.db.transaction():
            other = self.db.one("SELECT id FROM entities WHERE scope=? AND key=?", (scope, row["key"]))
            target = eid
            if other:   # mismo concepto ya presente en el destino: se fusiona alli
                self.db.execute("UPDATE entities SET scope=?, key=?, category=? WHERE id=?",
                                (scope, f"{row['key']}#{eid}", new_cat, eid))
                target = self.merge_entities(eid, other["id"])["id"]
            else:
                self.db.execute("UPDATE entities SET scope=?, category=? WHERE id=?", (scope, new_cat, eid))
            for r in self.db.query("SELECT r.id, a.scope AS sa, b.scope AS sb FROM relations r JOIN entities a ON "
                                   "a.id=r.src JOIN entities b ON b.id=r.dst WHERE r.src=? OR r.dst=?", (target, target)):
                try:
                    self.db.execute("UPDATE relations SET scope=? WHERE id=?", (self.relation_scope(r["sa"], r["sb"]), r["id"]))
                except ValueError:
                    self.db.execute("DELETE FROM relations WHERE id=?", (r["id"],))
        return self.entity(target)

    def categories_by_scope(self) -> dict:
        out: dict = {}
        for r in self.db.query("SELECT scope, category, COUNT(*) n FROM entities WHERE type NOT IN ('self', 'agente') "
                               "AND NOT (scope!='' AND key=?) GROUP BY scope, category", (_key(USER),)):
            out.setdefault(r["scope"] or "general", {})[r["category"] or "sin_categoria"] = r["n"]
        return out

    def prune(self, max_age_days: float = 45, min_weight: float = 0.5) -> int:
        """Olvido de lo trivial: co-ocurrencias debiles y antiguas, y conceptos que se quedan sin enlaces."""
        cutoff = time.time() - max_age_days * 86400
        with self.db.transaction():
            n = self.db.execute(f"DELETE FROM relations WHERE scope=? AND rel IN {WEAK} AND weight<? AND last_seen<?",
                                (self.scope, min_weight, cutoff)).rowcount
            n += self.db.execute(
                "DELETE FROM entities WHERE scope=? AND type='concepto' AND mentions<=2 AND last_seen<? AND NOT EXISTS "
                "(SELECT 1 FROM relations r WHERE r.src=entities.id OR r.dst=entities.id)", (self.scope, cutoff)).rowcount
        return n
