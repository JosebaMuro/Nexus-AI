"""IA de apoyo: el modelo de embeddings multilingue (Qwen3-Embedding, 100+ idiomas) ayuda a todas las herramientas.

El LLM decide y redacta; el embed, que es pequeno y rapido en CPU, hace el trabajo semantico de apoyo en cualquier
idioma:

* ``select_tools``: de las decenas de herramientas, solo las que vienen al caso pasan al prompt (menos tokens que
  procesar en CPU y menos confusion en los modelos pequenos). Las basicas se quedan siempre.
* ``route``: puntuacion semantica de cada agente para el enrutado automatico, aunque se escriba en otro idioma o sin
  las palabras clave del agente.
* ``fix_tool_name`` y ``fix_enum``: si el modelo pide una herramienta que no existe («search_web») o un valor fuera
  de la lista («blur» en vez de «desenfocar»), se corrige al mas parecido cuando el parecido es claro.
* ``condense``: los resultados largos (una pagina web, un fichero) se reducen a los fragmentos que responden a la
  peticion, en su orden, en vez de cortarlos a ciegas.
* ``rerank``: reordena resultados (busquedas web, listados) por relevancia.
* ``guess_tool``: sin LLM (modo basico), elige la herramienta de una sola entrada que mejor encaja.

Con el embedder de hashing (sin modelo) nada se filtra ni se corrige: solo se usa con un embedder semantico.
"""
from __future__ import annotations

import hashlib
import re
import threading
from collections import OrderedDict

import numpy as np

from .. import log

L = log.get("assist")
# siempre al alcance del agente, pida lo que pida (si las tiene permitidas)
CORE_TOOLS = ("memory_search", "memory_save", "delegate", "calculator", "datetime_now", "rag_search")


def _key(task: str, text: str) -> str:
    return task + ":" + hashlib.blake2b(text.encode("utf-8", "replace"), digest_size=12).hexdigest()


def chunks(text: str, size: int = 700) -> list[str]:
    """Trozos por parrafos de unos `size` caracteres (los parrafos enormes se parten por frases)."""
    out: list[str] = []
    cur = ""
    for para in re.split(r"\n\s*\n", text):
        pieces = [para] if len(para) <= size * 1.5 else re.split(r"(?<=[.!?])\s+", para)
        for p in pieces:
            if cur and len(cur) + len(p) > size:
                out.append(cur)
                cur = ""
            while len(p) > size * 1.5:     # sin puntos: corte duro
                out.append(p[:size])
                p = p[size:]
            cur = f"{cur}\n\n{p}" if cur else p
    if cur.strip():
        out.append(cur)
    return [c for c in out if c.strip()]


class SemanticAssist:
    def __init__(self, embedder_getter, config=None, max_cache: int = 4096) -> None:
        self._embedder = embedder_getter
        self.config = config
        self._cache: OrderedDict[str, np.ndarray] = OrderedDict()
        self._max = max_cache
        self._lock = threading.Lock()
        self.last: dict = {}           # ultima seleccion (para la interfaz y las pruebas)

    # ------------------------------------------------------------ base
    @property
    def embedder(self):
        return self._embedder()

    def semantic(self) -> bool:
        e = self.embedder
        if e is None or str(getattr(e, "id", "hash")).startswith("hash"):
            return False
        return bool(self._cfg("assist.enabled", True))

    def _cfg(self, key: str, default):
        try:
            return self.config.get(key, default) if self.config is not None else default
        except Exception:  # noqa: BLE001
            return default

    def info(self) -> dict:
        e = self.embedder
        return {"embedder": getattr(e, "id", ""), "semantic": self.semantic(), "last": self.last}

    def _docs(self, texts: list[str], task: str = "doc") -> np.ndarray:
        """Vectores de documentos (sin instruccion), con cache por embedder."""
        e = self.embedder
        tag = f"{getattr(e, 'id', '')}|{task}"
        keys = [_key(tag, t) for t in texts]
        with self._lock:
            missing = [i for i, k in enumerate(keys) if k not in self._cache]
        if missing:
            vecs = np.asarray(e.embed([texts[i][:2000] for i in missing]), dtype=np.float32)
            with self._lock:
                for i, v in zip(missing, vecs):
                    self._cache[keys[i]] = v
                while len(self._cache) > self._max:
                    self._cache.popitem(last=False)
        with self._lock:
            return np.stack([self._cache[k] for k in keys]) if keys else np.zeros((0, 1), np.float32)

    def _query(self, text: str, task: str) -> np.ndarray:
        e = self.embedder
        k = _key(f"{getattr(e, 'id', '')}|q|{task}", text)
        with self._lock:
            v = self._cache.get(k)
        if v is None:
            v = np.asarray(e.embed_query([text[:2000]], task), dtype=np.float32)[0]
            with self._lock:
                self._cache[k] = v
        return v

    def scores(self, query: str, candidates: list[str], task: str = "search") -> np.ndarray:
        if not candidates or not query.strip():
            return np.zeros(len(candidates), np.float32)
        return self._docs(candidates) @ self._query(query, task)

    # ------------------------------------------------------------ herramientas
    @staticmethod
    def tool_text(t) -> str:
        p = t.parameters.get("properties", {}) if hasattr(t, "parameters") else t.get("parameters", {}).get("properties", {})
        name = t.name if hasattr(t, "name") else t["name"]
        desc = t.description if hasattr(t, "description") else t.get("description", "")
        args = ", ".join(f"{k}: {v.get('description', '')}"[:120] for k, v in p.items())
        return f"{name.replace('_', ' ')}: {desc} ({args})"

    def select_tools(self, text: str, tools: list, k: int | None = None) -> list:
        """Las herramientas que vienen al caso (orden original). Con pocas herramientas o sin embedder semantico,
        todas."""
        limit = int(k or self._cfg("assist.max_tools", 10))
        if len(tools) <= limit or not self.semantic() or not text.strip():
            return tools
        try:
            sims = self.scores(text, [self.tool_text(t) for t in tools], "tools")
        except Exception as e:  # noqa: BLE001
            L.warning("Seleccion semantica de herramientas no disponible: %s", e)
            return tools
        low = text.lower()
        keep = {t.name for t in tools if t.name in CORE_TOOLS or t.name in low}
        ranked = [tools[i].name for i in np.argsort(-sims)]
        for name in ranked:
            if len(keep) >= limit + len([n for n in keep if n in CORE_TOOLS]):
                break
            keep.add(name)
        chosen = [t for t in tools if t.name in keep]
        self.last = {"tools": [t.name for t in chosen], "of": len(tools),
                     "top": [(tools[i].name, round(float(sims[i]), 3)) for i in np.argsort(-sims)[:5]]}
        return chosen

    def fix_tool_name(self, name: str, available: list) -> str | None:
        """Nombre de herramienta inventado por el modelo -> el existente mas parecido (si el parecido es claro)."""
        names = [t.name if hasattr(t, "name") else t for t in available]
        if not names or name in names:
            return name if name in names else None
        norm = re.sub(r"[^a-z0-9]", "", name.lower())
        for n in names:   # mismas letras con otros separadores: search-web -> web_search no, rag.search -> rag_search
            if re.sub(r"[^a-z0-9]", "", n.lower()) == norm:
                return n
        if not self.semantic():
            return None
        try:
            texts = [self.tool_text(t) if hasattr(t, "name") else t for t in available]
            sims = self.scores(name.replace("_", " "), texts, "tools")
        except Exception:  # noqa: BLE001
            return None
        order = np.argsort(-sims)
        best, second = float(sims[order[0]]), float(sims[order[1]]) if len(order) > 1 else 0.0
        return names[order[0]] if best >= 0.45 and best - second >= 0.04 else None

    def fix_enum(self, value: str, options: list) -> str | None:
        """Valor fuera de la lista -> la opcion equivalente (en cualquier idioma) si es clara."""
        opts = [str(o) for o in options]
        low = str(value).strip().lower()
        for o in opts:
            if o.lower() == low:
                return o
        if not self.semantic() or not low:
            return None
        try:
            sims = self.scores(low, opts, "dedupe")
        except Exception:  # noqa: BLE001
            return None
        order = np.argsort(-sims)
        best, second = float(sims[order[0]]), float(sims[order[1]]) if len(order) > 1 else 0.0
        return opts[order[0]] if best >= 0.5 and best - second >= 0.05 else None

    def repair_args(self, schema: dict, args: dict) -> tuple[dict, list[str]]:
        """Corrige los valores fuera de un enum. Devuelve (args, cambios)."""
        changes = []
        out = dict(args or {})
        for key, spec in (schema.get("properties") or {}).items():
            if key in out and "enum" in spec and out[key] not in spec["enum"] and isinstance(out[key], str):
                fixed = self.fix_enum(out[key], spec["enum"])
                if fixed is not None:
                    changes.append(f"{key}: {out[key]} -> {fixed}")
                    out[key] = fixed
        return out, changes

    def guess_tool(self, text: str, tools: list) -> tuple[str, dict] | None:
        """Modo basico (sin LLM): herramienta de una sola entrada de texto que encaja claramente con la peticion."""
        single = []
        for t in tools:
            props = t.parameters.get("properties", {})
            req = t.parameters.get("required", [])
            if len(req) == 1 and props.get(req[0], {}).get("type", "string") == "string" and t.risk == "low":
                single.append((t, req[0]))
        if len(single) < 2 or not self.semantic():
            return None
        sims = self.scores(text, [self.tool_text(t) for t, _ in single], "tools")
        order = np.argsort(-sims)
        best, second = float(sims[order[0]]), float(sims[order[1]])
        if best < 0.5 or best - second < 0.06:
            return None
        t, arg = single[order[0]]
        return t.name, {arg: text}

    # ------------------------------------------------------------ agentes
    @staticmethod
    def agent_text(a) -> str:
        return f"{a.name}: {a.description}. {a.specialty}. {', '.join(a.keywords[:20])}"

    def route_bonus(self, text: str, agents: list) -> dict[str, float]:
        """Bono 0..3 por agente segun el parecido semantico, relativo al resto (no depende de la escala del modelo)."""
        if len(agents) < 2 or not self.semantic() or not text.strip():
            return {}
        try:
            sims = self.scores(text, [self.agent_text(a) for a in agents], "route")
        except Exception as e:  # noqa: BLE001
            L.warning("Enrutado semantico no disponible: %s", e)
            return {}
        order = np.argsort(-sims)
        top, second, med = float(sims[order[0]]), float(sims[order[1]]), float(np.median(sims))
        if top - med < 0.06:     # nadie destaca sobre el resto: no se toca el enrutado
            return {}
        # solo el que destaca, y tanto mas cuanto mas claro saca ventaja al segundo
        return {agents[int(order[0])].id: round(min(3.0, 40.0 * (top - second)), 3)}

    # ------------------------------------------------------------ resultados
    def condense(self, query: str, text: str, budget: int = 5000) -> str:
        """Texto largo -> los trozos mas relevantes para la peticion, en su orden original, hasta `budget`."""
        if len(text) <= budget or not self.semantic() or not query.strip():
            return text
        parts = chunks(text, max(150, min(700, budget // 4)))
        if len(parts) < 3:
            return text
        try:
            sims = self.scores(query, parts, "search")
        except Exception:  # noqa: BLE001
            return text
        chosen, used = {0}, len(parts[0])           # el principio da contexto (titulo, cabecera)
        for i in np.argsort(-sims):
            if used + len(parts[i]) > budget:
                continue
            chosen.add(int(i))
            used += len(parts[i])
        out, prev = [], -1
        for i in sorted(chosen):
            if prev >= 0 and i != prev + 1:
                out.append("[…]")
            out.append(parts[i])
            prev = i
        return "\n\n".join(out) + f"\n\n[resumen semántico: {len(chosen)} de {len(parts)} fragmentos]"

    def condense_result(self, query: str, result, budget: int = 5000):
        """Aplica condense a los textos largos de un resultado de herramienta (dict o str)."""
        if isinstance(result, str):
            return self.condense(query, result, budget)
        if isinstance(result, dict):
            return {k: (self.condense(query, v, budget) if isinstance(v, str) and len(v) > budget else v)
                    for k, v in result.items()}
        return result

    def rerank(self, query: str, items: list, key=lambda x: str(x)) -> list:
        if len(items) < 2 or not self.semantic() or not query.strip():
            return items
        try:
            sims = self.scores(query, [key(x) for x in items], "search")
        except Exception:  # noqa: BLE001
            return items
        return [items[i] for i in np.argsort(-sims, kind="stable")]

    def prewarm(self, tools: list, agents: list) -> None:
        """Calcula en segundo plano los vectores de herramientas y agentes (el primer mensaje no espera)."""
        if not self.semantic():
            return

        def work():
            try:
                self._docs([self.tool_text(t) for t in tools])
                self._docs([self.agent_text(a) for a in agents])
            except Exception as e:  # noqa: BLE001
                L.debug("Precalculo de la IA de apoyo fallido: %s", e)
        threading.Thread(target=work, name="nexus-assist-prewarm", daemon=True).start()
