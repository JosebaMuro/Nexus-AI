"""Sistema de agentes configurable.

Cada agente es un YAML en data/agents (se siembran los de nexus/agents/defaults en el primer
arranque). Campos: id, name, description, system_prompt, model ("default" o "backend:modelo"),
temperature, max_tokens, tools (lista o ["*"]), memory, can_delegate_to, keywords, specialty, color, icon, enabled.

``specialty`` describe el saber hacer del agente (areas y terminos separados por comas). El organizador de la
memoria lo usa para decidir que conocimiento es suyo (su subgrafo aislado) y que es comun (el grafo general).
"""
from __future__ import annotations

import datetime as dt
import json
import re
import shutil
from dataclasses import asdict, dataclass, field
from pathlib import Path

import yaml

from .. import log, paths
from ..llm import format_result, parse_tool_call, tool_instructions
from ..llm.backends import ToolsUnsupported
from ..llm.builtin import BuiltinBackend, detect_intent
from ..memory.embeddings import tokens
from ..tools import ToolContext

L = log.get("agents")
DEFAULTS_DIR = Path(__file__).with_name("defaults")
STYLE = {"breve": "Responde de forma breve y directa (1-3 frases salvo que se pida mas).",
         "equilibrado": "Responde con claridad y la extension justa.",
         "detallado": "Responde de forma detallada, con ejemplos y estructura."}
LANG = {"es": "espanol", "en": "ingles", "ca": "catalan", "eu": "euskera", "gl": "gallego", "fr": "frances",
        "pt": "portugues", "de": "aleman", "it": "italiano"}


@dataclass
class AgentSpec:
    id: str
    name: str
    description: str = ""
    system_prompt: str = "Eres un asistente util."
    model: str = "default"
    temperature: float | None = None
    max_tokens: int | None = None
    tools: list[str] = field(default_factory=lambda: ["*"])
    memory: bool = True
    can_delegate_to: list[str] = field(default_factory=list)
    keywords: list[str] = field(default_factory=list)
    specialty: str = ""
    color: str = "#00f0ff"
    icon: str = "◆"
    enabled: bool = True

    @staticmethod
    def from_dict(d: dict) -> "AgentSpec":
        known = {k: v for k, v in d.items() if k in AgentSpec.__dataclass_fields__}
        return AgentSpec(**known)


def store_images(images: list[str]) -> list[str]:
    """Guarda las imagenes adjuntas (base64) en data/sandbox/adjuntos para que los agentes las editen con sus
    herramientas. Devuelve sus rutas dentro de la carpeta de trabajo."""
    import base64
    import io
    import time
    out = []
    folder = paths.sub("sandbox", "adjuntos")
    for i, b64 in enumerate(images or []):
        try:
            data = base64.b64decode(b64.split(",", 1)[-1])
            from PIL import Image
            with Image.open(io.BytesIO(data)) as im:
                ext = {"JPEG": "jpg", "PNG": "png", "WEBP": "webp", "GIF": "gif", "BMP": "bmp"}.get(im.format, "png")
            name = f"{time.strftime('%Y%m%d-%H%M%S')}_{i + 1}.{ext}"
            (folder / name).write_bytes(data)
            out.append(f"adjuntos/{name}")
        except Exception as e:  # noqa: BLE001
            L.warning("No se pudo guardar la imagen adjunta %d: %s", i + 1, e)
    return out


class AgentManager:
    def __init__(self, services) -> None:
        self.s = services
        self.dir = paths.agents_dir()
        self.agents: dict[str, AgentSpec] = {}
        self._seed()
        self.reload()

    # ------------------------------------------------------------ persistencia
    def _seed(self) -> None:
        for f in DEFAULTS_DIR.glob("*.yaml"):
            if not (self.dir / f.name).exists():
                shutil.copy(f, self.dir / f.name)

    def reload(self) -> None:
        agents = {}
        for f in sorted(self.dir.glob("*.yaml")):
            try:
                spec = AgentSpec.from_dict(yaml.safe_load(f.read_text(encoding="utf-8")))
                if not spec.specialty and (DEFAULTS_DIR / f.name).exists():   # agentes sembrados antes de la 1.6
                    spec.specialty = (yaml.safe_load((DEFAULTS_DIR / f.name).read_text(encoding="utf-8")) or {}).get(
                        "specialty", "") or ""
                agents[spec.id] = spec
            except Exception as e:  # noqa: BLE001
                L.error("Agente invalido %s: %s", f.name, e)
        self.agents = agents

    def save(self, data: dict) -> AgentSpec:
        data["id"] = re.sub(r"[^a-z0-9_\-]", "", str(data.get("id", "")).lower())[:30]
        if not data["id"]:
            raise ValueError("id invalido")
        spec = AgentSpec.from_dict(data)
        (self.dir / f"{spec.id}.yaml").write_text(yaml.safe_dump(asdict(spec), allow_unicode=True, sort_keys=False),
                                                  encoding="utf-8")
        self.agents[spec.id] = spec
        graph = getattr(self.s, "graph", None)
        if graph is not None:
            graph.forget_view(spec.id)   # si cambia el nombre, el ancla de su subgrafo lo sigue
        return spec

    def delete(self, aid: str) -> None:
        if aid == "nexus":
            raise ValueError("El agente principal no se puede borrar")
        (self.dir / f"{aid}.yaml").unlink(missing_ok=True)
        self.agents.pop(aid, None)
        # su biblioteca pasa al grafo general (no se pierden documentos) y su subgrafo propio se elimina
        rag, graph = getattr(self.s, "rag", None), getattr(self.s, "graph", None)
        if rag is not None:
            rag.reassign_agent(aid, "")
        if graph is not None and aid:
            graph.drop_scope(aid)

    def get(self, aid: str) -> AgentSpec | None:
        return self.agents.get(aid)

    def ids(self) -> list[str]:
        return [a.id for a in self.list()]

    def list(self) -> list[AgentSpec]:
        return [a for a in self.agents.values() if a.enabled]

    # ------------------------------------------------------------ enrutado
    @staticmethod
    def keyword_score(agent: AgentSpec, text: str) -> float:
        words = set(tokens(text))
        low = " ".join(tokens(text))
        score = 0.0
        for k in agent.keywords:
            kt = " ".join(tokens(k))
            if " " in kt and kt in low:
                score += 2
            elif kt in words:
                score += 1
        return score

    def route(self, text: str, has_images: bool = False) -> str:
        """Palabras clave + habilidades aprendidas en el salon virtual."""
        if has_images:
            return "nexus"
        lounge = getattr(self.s, "lounge", None)
        best, score = "nexus", 0.5
        for a in self.list():
            sc = self.keyword_score(a, text) + (0.8 * lounge.skill_score(a.id, text) if lounge and a.id != "nexus" else 0)
            if sc > score:
                best, score = a.id, sc
        return best

    # ------------------------------------------------------------ prompt
    def build_system(self, spec: AgentSpec, info: dict, tools: list, text_tools: bool = True) -> str:
        cfg = self.s.config
        lang = LANG.get(cfg.get("user.language", "es"), cfg.get("user.language", "es"))
        style = STYLE.get(cfg.get("user.response_style", "equilibrado"), "")
        bias = float(info["profile"].get("length_bias", 0))
        if bias <= -0.3:
            style += " El usuario prefiere respuestas cortas."
        elif bias >= 0.3:
            style += " El usuario valora respuestas completas."
        # Orden pensado para la cache de prompt de llama.cpp: primero lo estable (persona, estilo,
        # herramientas, companeros) y al final lo variable (recuerdos, fecha). En CPU ahorra segundos por turno.
        parts = [spec.system_prompt.replace("{assistant_name}", cfg.get("user.assistant_name", "Nexus-Office")),
                 f"Responde siempre en {lang}. {style} Usa Markdown sencillo (sin LaTeX)."]
        if info.get("behaviors"):   # pautas comunes a todos los agentes (cambian poco: parte estable del prompt)
            parts.append("Pautas de comportamiento que el usuario te ha pedido (respetalas siempre):\n" +
                         "\n".join(f"- {b[:200]}" for b in info["behaviors"]))
        dynamic: list[str] = []
        if tools:
            parts.append(tool_instructions([t.public() for t in tools]) if text_tools else
                         "Dispones de herramientas: usalas cuando aporten algo (crear imagenes o musica, calcular, "
                         "buscar, recordar...). Nunca inventes resultados, enlaces ni imagenes: usa la herramienta.")
            if any(t.name == "delegate" for t in tools):
                others = [a for a in self.list() if a.id != spec.id and
                          (not spec.can_delegate_to or "*" in spec.can_delegate_to or a.id in spec.can_delegate_to)]
                if others:
                    parts.append("Agentes en los que puedes delegar: " +
                                 "; ".join(f"{a.id} ({a.description})" for a in others))
                    lounge = getattr(self.s, "lounge", None)
                    directory = lounge.directory(exclude=spec.id) if lounge else ""
                    if directory:
                        dynamic.append("Lo aprendido en el salon sobre tus companeros:\n" + directory)
        if info["facts"]:
            dynamic.append("Datos que has aprendido (\"Usuario\" es la persona con la que hablas, no tu):\n" +
                           "\n".join(f"- {f}" for f in info["facts"][:12]))
        if info["memories"]:
            dynamic.append("Recuerdos relevantes:\n" + "\n".join(f"- {m['content'][:300]}" for m in info["memories"]))
        if info["lessons"]:
            dynamic.append("Lecciones aprendidas (evita repetir estos errores):\n" +
                           "\n".join(f"- {m['content'][:250]}" for m in info["lessons"]))
        if info["examples"]:
            dynamic.append("Ejemplos de respuestas que el usuario valoro:\n" +
                           "\n---\n".join(m["content"][:600] for m in info["examples"]))
        if info.get("documents"):
            dynamic.append("Fragmentos de tu biblioteca de documentos (RAG). Usalos si son relevantes y cita la fuente "
                           "como [n]:\n" + "\n".join(f"[{i + 1}] «{d['title'][:80]}» p.{d['page']}: {d['text'][:450]}"
                                                    for i, d in enumerate(info["documents"])))
        dynamic.append(f"Fecha y hora actuales: {dt.datetime.now():%d/%m/%Y %H:%M}.")
        return "\n\n".join(parts + dynamic)

    # ------------------------------------------------------------ ejecucion
    def run(self, agent_id: str, text: str, session_id: str, images: list[str] | None = None, emit=None,
            remote: bool = False, depth: int = 0, save: bool = True) -> dict:
        s = self.s
        emit = emit or (lambda e: None)
        routed_by = "usuario"
        if agent_id in ("", "auto"):
            agent_id = self.route(text, bool(images))
            routed_by = "auto"
        spec = self.get(agent_id) or self.get("nexus") or next(iter(self.agents.values()))
        emit({"type": "agent", "agent": spec.id, "name": spec.name, "color": spec.color, "icon": spec.icon})
        use_memory = spec.memory and s.config.get("user.memory_enabled", True)
        if save and use_memory and not remote:
            learned = s.learner.on_user_message(text, session_id, spec.id, s.config.get("memory.llm_graph_extraction", False))
            if learned:
                emit({"type": "learned", "items": learned[:8]})
        info = s.learner.build_context(text, spec.id, s.config.get("memory.top_k", 6)) if use_memory else \
            {"memories": [], "examples": [], "lessons": [], "facts": [], "profile": {}}
        rag = getattr(s, "rag", None)
        if rag is not None and use_memory:
            try:
                info["documents"] = rag.search(text, k=3, min_score=0.3, agent=spec.id)   # general + los suyos
            except Exception as e:  # noqa: BLE001
                L.warning("Busqueda RAG fallida: %s", e)
        tools = s.tools.for_agent(spec.tools)
        native = bool(tools) and s.llm.native_tools(spec.model)
        msgs = [{"role": "system", "content": self.build_system(spec, info, tools, text_tools=not native)}]
        if save:
            for m in s.memory.history(session_id, 12):
                if m["role"] in ("user", "assistant") and m["content"]:
                    msgs.append({"role": m["role"], "content": m["content"][:3000]})
            s.memory.add_message(session_id, "user", text + (f"\n[{len(images)} imagen(es) adjunta(s)]" if images else ""),
                                 spec.id)
        stored = store_images(images) if images else []
        note = (f"\n\n[Imagenes adjuntas guardadas en tu carpeta de trabajo: {', '.join(stored)}. Para editarlas usa "
                "imagen_editar con esa ruta.]" if stored else "")
        msgs.append({"role": "user", "content": text + note, "images": images or []})
        ctx = ToolContext(services=s, agent=spec.id, session_id=session_id, emit=emit, remote=remote, depth=depth)
        max_steps = int(s.config.get("llm.max_tool_steps", 5))
        answer_parts, media, tool_log = [], [], []
        # via rapida: intenciones inequivocas se ejecutan sin esperar al LLM (fiable y rapido en CPU)
        intent = detect_intent(text, {t.name for t in tools}) if tools and not images else None
        for step in range(max_steps + 1):
            emit({"type": "state", "state": "thinking"})
            buf = ""
            if step == 0 and intent:
                buf = f'<tool>{json.dumps({"name": intent[0], "args": intent[1]}, ensure_ascii=False)}</tool>'
            else:
                opts = {"temperature": spec.temperature, "max_tokens": spec.max_tokens, "tools": [t.name for t in tools],
                        "on_reasoning": lambda t, a=spec.id: emit({"type": "reasoning", "text": t, "agent": a})}
                if native:
                    opts["tool_specs"] = [t.public() for t in tools]
                try:
                    for piece in s.llm.stream(msgs, spec.model, **opts):
                        buf += piece
                        emit({"type": "token", "text": piece, "agent": spec.id})
                except ToolsUnsupported:
                    native = False  # el endpoint no acepta 'tools': protocolo de texto
                    s.llm.native_off.add(s.llm.select(spec.model)[0])
                    msgs[0]["content"] = self.build_system(spec, info, tools, text_tools=True)
                    opts.pop("tool_specs", None)
                    for piece in s.llm.stream(msgs, spec.model, **opts):
                        buf += piece
                        emit({"type": "token", "text": piece, "agent": spec.id})
            call = parse_tool_call(buf) if tools else None
            if not call or step == max_steps:
                answer_parts.append(re.sub(r"<tool>.*?</tool>", "", buf, flags=re.S).strip())
                break
            name, args, pre = call
            if pre:
                answer_parts.append(pre)
            emit({"type": "tool_call", "name": name, "args": args})
            emit({"type": "state", "state": "tool"})
            result = s.supervisor.execute(name, args, ctx)
            if isinstance(result, dict) and result.get("url") and name in ("generate_image", "compose_music",
                                                                                    "imagen_editar", "gimp_script"):
                media.append((name, result["url"]))
            tool_log.append({"tool": name, "ok": not (isinstance(result, dict) and result.get("error"))})
            emit({"type": "tool_result", "name": name, "result": json.dumps(result, ensure_ascii=False, default=str)[:1500]})
            if step == 0 and intent and not (isinstance(result, dict) and result.get("error")):
                # via rapida: respuesta inmediata con plantilla (evita reprocesar todo el prompt en CPU)
                final = BuiltinBackend._after_tool(name, json.dumps(result, ensure_ascii=False, default=str))
                emit({"type": "token", "text": final, "agent": spec.id})
                answer_parts.append(final)
                break
            msgs.append({"role": "assistant", "content": buf})
            msgs.append({"role": "user", "content": format_result(name, result)})
        answer = "\n\n".join(p for p in answer_parts if p).strip() or "(sin respuesta)"
        produced = {u for _, u in media}
        # los modelos pequenos a veces "inventan" imagenes con URLs externas: se eliminan
        answer = re.sub(r"!\[[^\]]*\]\(([^)\s]+)\)", lambda m: m.group(0) if m.group(1) in produced or
                        m.group(1).startswith("/outputs/") else "", answer).strip() or "(sin respuesta)"
        for name, url in media:
            if url not in answer:
                answer += f"\n\n![imagen]({url})" if name != "compose_music" else f"\n\n[audio]({url})"
        mid = None
        if save:
            meta = json.dumps({"memories": [m["id"] for m in info["memories"]], "tools": tool_log})
            mid = s.memory.add_message(session_id, "assistant", answer, spec.id, meta)
            if use_memory and not remote:
                s.learner.on_exchange(session_id, text, answer, spec.id, s.config.get("memory.consolidate_every", 12))
                emit({"type": "graph_update"})
        if depth == 0 and getattr(s, "lounge", None):
            try:
                s.lounge.record_task(spec.id, text, tool_log, routed_by)
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo registrar la tarea en el salon: %s", e)
        emit({"type": "done", "message_id": mid, "agent": spec.id, "text": answer, "session_id": session_id})
        emit({"type": "state", "state": "idle"})
        return {"answer": answer, "message_id": mid, "agent": spec.id}

    def run_subtask(self, target: str, task: str, ctx: ToolContext) -> str:
        parent_emit = ctx.emit or (lambda e: None)

        def sub_emit(e: dict) -> None:
            if e["type"] in ("tool_call", "tool_result", "approval_required", "media", "agent"):
                parent_emit({**e, "sub": target})

        lounge = getattr(self.s, "lounge", None)
        if lounge:
            lounge.post(ctx.agent, f"→ {target}: {task[:200]}", "delegation", {"from": ctx.agent, "to": target})
        organizer = getattr(self.s, "organizer", None)
        if organizer is not None:   # interaccion entre agentes: conocimiento comun
            try:
                src, dst = self.get(ctx.agent), self.get(target)
                organizer.record_interaction(src.name if src else ctx.agent, "delega_en", dst.name if dst else target, 0.5)
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo anotar la delegacion: %s", e)
        res = self.run(target, task, ctx.session_id, emit=sub_emit, remote=ctx.remote, depth=ctx.depth + 1, save=False)
        if lounge:
            lounge._bump(target, lounge.topics(task), 1.0, 0.0)
            lounge.post(target, f"← {ctx.agent}: hecho. {res['answer'][:160]}", "delegation", {"from": target, "to": ctx.agent})
        return res["answer"]
