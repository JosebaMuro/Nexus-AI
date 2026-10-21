"""Gestor de LLM: elige el backend activo, resuelve el modelo y mide el rendimiento.

Especificacion de modelo por agente: ``"default"`` o ``"backend:modelo"``
(p.ej. ``ollama:qwen2.5:3b``, ``llamacpp:<id-catalogo>``, ``openai:gpt-4o-mini``, ``peer:<nodo>``).
"""
from __future__ import annotations

import re
import time
from typing import Iterator

from .. import log
from .backends import LlamaCppBackend, OllamaBackend, OpenAICompatBackend, TransformersBackend, is_reasoning_model
from .builtin import BuiltinBackend
from .llamaserver import LlamaServerBackend

L = log.get("llm")
REASONING_STOP = "\n\nYa he pensado bastante: respondo ahora con lo que tengo.\n"
OOM = re.compile(r"out of memory|failed to allocate|cudaMalloc|insufficient memory|bad_alloc|OOM", re.I)


def _tokens(m: dict) -> float:
    return len(m.get("content") or "") / 3.2 + 8 + 400 * len(m.get("images") or [])


def fit_context(messages: list[dict], n_ctx: int, reply_tokens: int) -> list[dict]:
    """Recorta el historial (lo mas antiguo primero) para que prompt + respuesta quepan en el contexto."""
    budget = n_ctx - reply_tokens - 48
    msgs = list(messages)
    while sum(map(_tokens, msgs)) > budget and len(msgs) > 2:
        msgs.pop(1)  # el [0] es el prompt de sistema y el ultimo la peticion actual
    over = sum(map(_tokens, msgs)) - budget
    if over > 0 and msgs and msgs[0]["role"] == "system":
        keep = max(400, int((_tokens(msgs[0]) - over) * 3.2))
        msgs[0] = {**msgs[0], "content": msgs[0]["content"][:keep]}
    return msgs


class LLMManager:
    def __init__(self, config, catalog, profile: dict, monitor=None, runtimes=None) -> None:
        self.config, self.catalog, self.profile, self.monitor, self.runtimes = config, catalog, profile, monitor, runtimes
        self.remote = None   # se inyecta desde coop para "peer:<nodo>"
        self._avail_cache: dict[str, tuple[float, bool]] = {}
        self.backends: dict = {}
        self.inflight = 0    # respuestas en curso: el autoajuste de Android solo cambia hilos entre respuestas
        self.rebuild()

    def rebuild(self) -> None:
        c = self.config
        timeout = c.get("llm.request_timeout", 180)
        old = self.backends.get("llamaserver")
        if old:
            old.stop()
        self.backends = {
            "llamaserver": LlamaServerBackend(self.runtimes, self._resolve("gguf"), lambda: self.profile,
                                              lambda: c.get("hardware.llama_variant", "")),
            "llamacpp": LlamaCppBackend(self._resolve("gguf")),
            "ollama": OllamaBackend(c.get("llm.ollama_url"), timeout),
            "openai": OpenAICompatBackend(c.get("llm.openai_base_url"), c.get("llm.openai_api_key"),
                                          c.get("llm.openai_model"), timeout),
            "transformers": TransformersBackend(self._resolve("transformers"), self.profile["image"]["device"]),
            "builtin": BuiltinBackend(),
        }
        self._avail_cache.clear()

    def _resolve(self, fmt: str):
        def resolve(model_id: str):
            entry = self.catalog.get(model_id) if model_id else None
            if entry is None:
                default = self.config.get("llm.default_model", "")
                entry = self.catalog.get(default.split(":", 1)[-1]) if default else None
            if entry is None:
                entry = next((m for m in self.catalog.list() if m.get("format") == fmt and m.get("kind", "llm") == "llm"), None)
            if entry and entry.get("format") == fmt:
                return entry["path"], entry
            return None
        return resolve

    def is_available(self, name: str) -> bool:
        now = time.time()
        cached = self._avail_cache.get(name)
        if cached and now - cached[0] < 15:
            return cached[1]
        b = self.backends.get(name)
        ok = bool(b and b.available())
        if ok and name == "llamacpp":
            ok = self._resolve("gguf")("") is not None
        if ok and name == "transformers":
            ok = self._resolve("transformers")("") is not None
        if ok and name == "ollama":
            ok = bool(b.models())
        self._avail_cache[name] = (now, ok)
        return ok

    def select(self, spec: str = "default") -> tuple[str, str]:
        """Devuelve (backend, modelo) para una especificacion."""
        if spec and spec != "default" and ":" in spec:
            backend, model = spec.split(":", 1)
            if backend == "peer" or self.is_available(backend):
                return backend, model
        default = self.config.get("llm.default_model", "")
        if default and ":" in default:
            backend, model = default.split(":", 1)
            if self.is_available(backend):
                return backend, model
        entry = self.catalog.get(default) if default else None
        if entry:
            b = "llamacpp" if entry.get("format") == "gguf" else "transformers"
            if entry.get("format") == "gguf":
                engine = self.config.get("llm.engine", "auto")
                if engine in ("llamaserver", "llamacpp"):
                    b = engine
                elif self.is_available("llamaserver"):   # automatico: llama-server (sus variantes usan GPU y NPU)
                    b = "llamaserver"
            if self.is_available(b):
                return b, entry["id"]
        order = list(self.config.get("llm.backend_order", []))
        engine = self.config.get("llm.engine", "auto")
        if engine in self.backends and engine != "auto":   # motor elegido al arrancar: primero (si no esta, el siguiente)
            order = [engine] + [n for n in order if n != engine]
        for name in self.backends:  # backends nuevos tras actualizar el proyecto
            if name not in order:
                order.insert(0 if name == "llamaserver" else len(order) - 1, name)
        for name in order:
            if self.is_available(name):
                return name, ""
        return "builtin", ""

    def status(self) -> dict:
        backend, model = self.select()
        if backend == "llamacpp" and not model:
            r = self._resolve("gguf")("")
            model = r[1]["id"] if r else ""
        if backend == "ollama" and not model:
            model = (self.backends["ollama"].models() or [""])[0]
        if backend == "builtin":
            model = "nexus-core"
        return {"backend": backend, "model": model, "available": {n: self.is_available(n) for n in self.backends}}

    def options(self, agent_opts: dict | None = None) -> dict:
        llm = self.profile["llm"]
        opts = {"n_ctx": llm["n_ctx"], "n_threads": llm["n_threads"], "n_batch": llm["n_batch"],
                "gpu_layers": llm["gpu_layers"], "flash_attn": llm.get("flash_attn", False), "device": llm.get("device", ""),
                "temperature": self.config.get("llm.temperature", 0.7), "max_tokens": self.config.get("llm.max_tokens", 1024)}
        opts.update({k: v for k, v in (agent_opts or {}).items() if v is not None})
        return opts

    def reasoning_model(self, backend_name: str, model: str) -> bool:
        """El modelo que va a responder razona antes (Qwen3-VL Thinking, QwQ, R1...)."""
        if backend_name in ("llamaserver", "llamacpp"):
            r = self._resolve("gguf")(model)
            return bool(r and is_reasoning_model(r[1]))
        if backend_name == "transformers":
            r = self._resolve("transformers")(model)
            return bool(r and is_reasoning_model(r[1]))
        if backend_name == "ollama":
            return is_reasoning_model(model or (self.backends["ollama"].models() or [""])[0])
        if backend_name == "openai":
            return is_reasoning_model(model or self.config.get("llm.openai_model", ""))
        return False

    def thinking_only(self, backend_name: str, model: str) -> bool:
        """Modelo que no sabe responder sin razonar (no tiene modo hibrido como Qwen3.5 o Qwen3)."""
        ident = model or ""
        if backend_name in ("llamaserver", "llamacpp", "transformers"):
            r = self._resolve("transformers" if backend_name == "transformers" else "gguf")(model)
            ident = f"{r[1].get('id', '')} {r[1].get('name', '')} {r[1].get('path', '')}" if r else ident
        return bool(re.search(r"thinking|\br1\b|-r1|qwq|deepseek-r", ident, re.I))

    def stream(self, messages: list[dict], spec: str = "default", **opts) -> Iterator[str]:
        backend_name, model = self.select(spec)
        if backend_name == "peer":
            if not self.remote:
                raise RuntimeError("Modo cooperativo no activo")
            opts.pop("on_reasoning", None)
            yield from self.remote(model, messages, opts)
            return
        backend = self.backends[backend_name]
        full = self.options(opts)
        budget = int(full.pop("reasoning_budget", 0) or self.config.get("llm.reasoning_budget", 2048) or 0)
        thinking = self.reasoning_model(backend_name, model)
        off = self.config.get("llm.reasoning", "auto") == "off" or full.get("reasoning") == "off"
        if thinking and off and self.thinking_only(backend_name, model):
            # variantes «Thinking» (Qwen3-VL Thinking, R1...): siempre razonan; se acorta el presupuesto en vez de apagarlo
            full.pop("reasoning", None)
            budget, off = min(budget or 256, 256), False
        if thinking and off:
            full["reasoning"] = "off"
        elif thinking:
            # el razonamiento gasta tokens antes de la respuesta: se le reserva un presupuesto aparte
            full["think_mode"] = "detect"
            full["max_tokens"] = (full["max_tokens"] or 1024) + max(0, budget)
            if backend_name == "llamaserver" and budget > 0:
                # llama-server corta el razonamiento al agotar el presupuesto y obliga a responder
                full["extra_body"] = {"reasoning_budget_tokens": budget,
                                      "reasoning_budget_message": REASONING_STOP}
        if backend_name in ("llamaserver", "llamacpp", "ollama"):
            cap = full["n_ctx"] // 2 if thinking else full["n_ctx"] // 3
            full["max_tokens"] = min(full["max_tokens"] or 1024, max(256, cap))
            messages = fit_context(messages, full["n_ctx"], full["max_tokens"])
        self.inflight += 1
        try:
            yield from backend.stream_chat(messages, model=model, **full)
        except Exception as e:
            if self.monitor:
                self.monitor.record_error("oom" if OOM.search(str(e)) else "llm", f"{backend_name}: {e}")
            raise
        finally:
            self.inflight -= 1
            st = backend.last_stats
            if self.monitor and st.get("tokens"):
                self.monitor.record_llm(backend_name, model, st["tokens"], st["seconds"], st["ttft"])
        self.last_used = (backend_name, model)

    def complete(self, prompt: str, spec: str = "default", **opts) -> str:
        """Tareas internas (resumenes, extraccion...): poco razonamiento y, si no llega a responder, cadena vacia
        para que quien llama use su alternativa sin LLM."""
        opts.setdefault("reasoning_budget", self.config.get("llm.reasoning_budget_tasks", 768))
        out = "".join(self.stream([{"role": "user", "content": prompt}], spec, **opts))
        return "" if out.startswith("_(El modelo ha agotado") else out

    native_off: set = set()

    def native_tools(self, spec: str = "default") -> bool:
        """Backends con llamadas a herramientas nativas (API OpenAI 'tools' + plantilla jinja)."""
        backend = self.select(spec)[0]
        return backend in ("llamaserver", "openai") and backend not in self.native_off

    def has_real_llm(self) -> bool:
        return self.select()[0] != "builtin"
