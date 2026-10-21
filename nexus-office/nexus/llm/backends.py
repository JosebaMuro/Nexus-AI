"""Backends de LLM intercambiables. Todos exponen ``stream_chat(messages, **opts) -> Iterator[str]``.

Mensajes: ``{"role": "system|user|assistant", "content": str, "images": [base64...]}``.
Al terminar cada stream, ``self.last_stats`` contiene tokens y tiempos para el monitor.
"""
from __future__ import annotations

import base64
import importlib.util
import json
import re
import threading
import time
from typing import Iterator

import httpx

from .. import log

L = log.get("llm")


class ToolsUnsupported(RuntimeError):
    """El endpoint rechazo el parametro 'tools'; se reintenta con el protocolo de texto."""


def merge_roles(messages: list[dict]) -> list[dict]:
    """Une mensajes consecutivos del mismo rol (algunas plantillas exigen alternancia)."""
    out: list[dict] = []
    for m in messages:
        if out and out[-1]["role"] == m["role"] and m["role"] != "system":
            out[-1] = {**out[-1], "content": out[-1]["content"] + "\n\n" + m["content"],
                       "images": (out[-1].get("images") or []) + (m.get("images") or [])}
        else:
            out.append(dict(m))
    return out


REASONING_NAME = re.compile(r"think|reason|\br1\b|-r1|qwq|magistral|deepseek-r|cogito|phi-4-reasoning", re.I)


def is_reasoning_model(entry_or_name) -> bool:
    """Modelo que razona antes de responder (Qwen3-VL Thinking, QwQ, DeepSeek-R1...)."""
    if isinstance(entry_or_name, dict):
        if "reasoning" in entry_or_name:
            return bool(entry_or_name["reasoning"])
        entry_or_name = f"{entry_or_name.get('id', '')} {entry_or_name.get('name', '')} {entry_or_name.get('path', '')}"
    return bool(REASONING_NAME.search(str(entry_or_name or "")))


class ThinkSplitter:
    """Separa el razonamiento (<think>...</think>) de la respuesta visible, en streaming.

    mode "tags": solo bloques <think> explicitos. mode "detect" (modelos de razonamiento): si el servidor no lo
    separa, la plantilla suele abrir <think> en el prompt y la salida trae el razonamiento y luego </think>;
    se retiene la salida hasta ver </think> (o hasta `hold` caracteres sin el: entonces era respuesta normal)."""
    OPEN, CLOSE = "<think>", "</think>"

    def __init__(self, mode: str = "tags", hold: int = 6000) -> None:
        self.mode, self.hold = mode, hold
        self.inside = False
        self.pending = mode == "detect"     # aun no sabemos si lo retenido es razonamiento
        self.buf = ""
        self.thought: list[str] = []
        self.visible = 0
        self.raw = 0          # trozos generados (razonamiento incluido): para medir tokens/s reales

    def server_split(self) -> None:
        """El servidor ya separa el razonamiento (reasoning_content): el contenido es todo visible."""
        self.pending = False

    def feed(self, piece: str) -> tuple[str, str]:
        self.raw += 1
        self.buf += piece
        vis, th = [], []
        while self.buf:
            if self.pending:
                low = self.buf.lstrip()
                if low.startswith(self.OPEN):
                    self.buf, self.inside, self.pending = low[len(self.OPEN):], True, False
                    continue
                i = self.buf.find(self.CLOSE)
                if i >= 0:
                    th.append(self.buf[:i])
                    self.buf, self.pending = self.buf[i + len(self.CLOSE):].lstrip("\n"), False
                    continue
                if len(self.buf) > self.hold:
                    self.pending = False
                    continue
                break
            if self.inside:
                i = self.buf.find(self.CLOSE)
                if i >= 0:
                    th.append(self.buf[:i])
                    self.buf, self.inside = self.buf[i + len(self.CLOSE):].lstrip("\n"), False
                    continue
                keep = len(self.CLOSE) - 1
                if len(self.buf) > keep:
                    th.append(self.buf[:-keep])
                    self.buf = self.buf[-keep:]
                break
            i = self.buf.find(self.OPEN)
            if i >= 0:
                vis.append(self.buf[:i])
                self.buf, self.inside = self.buf[i + len(self.OPEN):], True
                continue
            j = self.buf.rfind("<")
            if j >= 0 and self.OPEN.startswith(self.buf[j:]) and len(self.buf) - j < len(self.OPEN):
                vis.append(self.buf[:j])          # posible "<thi" partido entre trozos
                self.buf = self.buf[j:]
            else:
                vis.append(self.buf)
                self.buf = ""
            break
        return self._out(vis, th)

    def flush(self) -> tuple[str, str]:
        rest, self.buf = self.buf, ""
        if self.inside:
            return self._out([], [rest])
        return self._out([rest], [])

    def _out(self, vis: list[str], th: list[str]) -> tuple[str, str]:
        v, t = "".join(vis), "".join(th)
        if t:
            self.thought.append(t)
        self.visible += len(v.strip())
        return v, t

    @property
    def reasoning(self) -> str:
        return "".join(self.thought)


def split_stream(pieces: Iterator[str], mode: str, on_reasoning=None, splitter: ThinkSplitter | None = None) -> Iterator[str]:
    """Aplica ThinkSplitter a un stream de texto: devuelve lo visible y manda el razonamiento a on_reasoning."""
    sp = splitter or ThinkSplitter(mode)
    for piece in pieces:
        vis, th = sp.feed(piece)
        if th and on_reasoning:
            on_reasoning(th)
        if vis:
            yield vis
    vis, th = sp.flush()
    if th and on_reasoning:
        on_reasoning(th)
    if vis:
        yield vis


def reasoning_fallback(reasoning: str) -> str:
    """Si el modelo agoto los tokens razonando y no llego a responder, se muestra el final de lo que penso."""
    tail = reasoning.strip()[-1500:]
    return ("_(El modelo ha agotado el límite de tokens mientras razonaba y no ha llegado a escribir la respuesta. "
            "Sube «llm.reasoning_budget» en Ajustes o usa un modelo mayor o una GPU. Lo último que estaba pensando:)_\n\n"
            + "> " + tail.replace("\n", "\n> ")) if tail else ""


class Backend:
    name = "base"

    def __init__(self) -> None:
        self.last_stats = {"tokens": 0, "seconds": 0.0, "ttft": 0.0}
        self.last_reasoning = ""

    def available(self) -> bool:
        return False

    def models(self) -> list[str]:
        return []

    def stream_chat(self, messages: list[dict], model: str = "", **opts) -> Iterator[str]:
        raise NotImplementedError

    def complete(self, prompt: str, model: str = "", **opts) -> str:
        return "".join(self.stream_chat([{"role": "user", "content": prompt}], model=model, **opts))

    def _finish(self, gen: Iterator[str], sp: ThinkSplitter) -> Iterator[str]:
        """Tras el stream: guarda el razonamiento y, si no hubo respuesta visible, muestra lo pensado."""
        yield from gen
        self.last_reasoning = sp.reasoning
        if not sp.visible and sp.reasoning.strip():
            yield reasoning_fallback(sp.reasoning)

    def _timed(self, gen: Iterator[str], token_counter=None) -> Iterator[str]:
        t0 = time.time()
        first = 0.0
        n = 0
        for piece in gen:
            if not first:
                first = time.time() - t0
            n += 1
            yield piece
        self.last_stats = {"tokens": token_counter() if token_counter else n, "seconds": time.time() - t0,
                           "ttft": first}


# --------------------------------------------------------------------------- Ollama
class OllamaBackend(Backend):
    name = "ollama"

    def __init__(self, url: str, timeout: float = 180) -> None:
        super().__init__()
        self.url, self.timeout = url.rstrip("/"), timeout

    def available(self) -> bool:
        try:
            return httpx.get(f"{self.url}/api/version", timeout=1.5).status_code == 200
        except httpx.HTTPError:
            return False

    def models(self) -> list[str]:
        try:
            return [m["name"] for m in httpx.get(f"{self.url}/api/tags", timeout=3).json().get("models", [])]
        except (httpx.HTTPError, ValueError):
            return []

    def stream_chat(self, messages, model="", **opts):
        model = model or (self.models() or [""])[0]
        msgs = [{"role": m["role"], "content": m["content"], **({"images": m["images"]} if m.get("images") else {})}
                for m in merge_roles(messages)]
        options = {"temperature": opts.get("temperature", 0.7), "num_predict": opts.get("max_tokens", 1024)}
        if opts.get("n_ctx"):
            options["num_ctx"] = opts["n_ctx"]
        if opts.get("n_threads"):
            options["num_thread"] = opts["n_threads"]
        if opts.get("gpu_layers", -1) >= 0:
            options["num_gpu"] = opts["gpu_layers"]
        stats = {}

        def gen():
            with httpx.stream("POST", f"{self.url}/api/chat", timeout=self.timeout,
                              json={"model": model, "messages": msgs, "stream": True, "options": options}) as r:
                r.raise_for_status()
                for line in r.iter_lines():
                    if not line:
                        continue
                    data = json.loads(line)
                    if data.get("error"):
                        raise RuntimeError(data["error"])
                    msg = data.get("message", {})
                    if msg.get("thinking"):   # Ollama >= 0.9 separa el razonamiento
                        sp.server_split()
                        sp.thought.append(msg["thinking"])
                        if opts.get("on_reasoning"):
                            opts["on_reasoning"](msg["thinking"])
                    piece = msg.get("content", "")
                    if piece:
                        yield piece
                    if data.get("done"):
                        stats["n"] = data.get("eval_count", 0)

        sp = ThinkSplitter(opts.get("think_mode", "tags"))
        yield from self._finish(self._timed(split_stream(gen(), "", opts.get("on_reasoning"), sp),
                                            lambda: stats.get("n", 0)), sp)

    def pull(self, name: str) -> Iterator[dict]:
        with httpx.stream("POST", f"{self.url}/api/pull", json={"name": name, "stream": True}, timeout=None) as r:
            for line in r.iter_lines():
                if line:
                    yield json.loads(line)


# --------------------------------------------------------------------------- OpenAI compatible
class OpenAICompatBackend(Backend):
    """LM Studio, vLLM, llama-server, LocalAI, Jan, OpenRouter, OpenAI..."""
    name = "openai"

    def __init__(self, base_url: str, api_key: str = "", default_model: str = "", timeout: float = 180) -> None:
        super().__init__()
        self.base_url, self.api_key, self.default_model, self.timeout = base_url.rstrip("/"), api_key, default_model, timeout

    def _headers(self) -> dict:
        return {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}

    def available(self) -> bool:
        if not self.base_url:
            return False
        try:
            return httpx.get(f"{self.base_url}/models", headers=self._headers(), timeout=1.5).status_code == 200
        except httpx.HTTPError:
            return False

    def models(self) -> list[str]:
        try:
            return [m["id"] for m in httpx.get(f"{self.base_url}/models", headers=self._headers(), timeout=3).json()["data"]]
        except (httpx.HTTPError, ValueError, KeyError):
            return []

    def stream_chat(self, messages, model="", **opts):
        msgs = []
        for m in merge_roles(messages):
            if m.get("images"):
                parts = [{"type": "text", "text": m["content"]}] + [
                    {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b}"}} for b in m["images"]]
                msgs.append({"role": m["role"], "content": parts})
            else:
                msgs.append({"role": m["role"], "content": m["content"]})
        body = {"model": model or self.default_model or (self.models() or ["default"])[0], "messages": msgs,
                "stream": True, "temperature": opts.get("temperature", 0.7), "max_tokens": opts.get("max_tokens", 1024)}
        if opts.get("reasoning") == "off":   # plantillas Qwen3 y similares: responder sin razonar
            body["chat_template_kwargs"] = {"enable_thinking": False}
        body.update(opts.get("extra_body") or {})
        sp = ThinkSplitter(opts.get("think_mode", "tags"))
        on_reasoning = opts.get("on_reasoning")
        if opts.get("tool_specs"):  # llamadas nativas (OpenAI tools): mas fiables que el protocolo de texto
            body["tools"] = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                                                               "parameters": t.get("parameters") or {"type": "object", "properties": {}}}}
                             for t in opts["tool_specs"]]

        def gen():
            calls: dict[int, dict] = {}
            with httpx.stream("POST", f"{self.base_url}/chat/completions", json=body, headers=self._headers(),
                              timeout=self.timeout) as r:
                if r.status_code >= 400 and "tools" in body:
                    r.read()
                    raise ToolsUnsupported(r.text[:300])
                r.raise_for_status()
                for line in r.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    payload = line[5:].strip()
                    if payload == "[DONE]":
                        break
                    try:
                        delta = json.loads(payload)["choices"][0].get("delta", {})
                    except (ValueError, KeyError, IndexError):
                        continue
                    thought = delta.get("reasoning_content") or delta.get("reasoning")
                    if thought:   # llama-server / vLLM / LM Studio ya separan el razonamiento
                        sp.server_split()
                        sp.raw += 1
                        sp.thought.append(thought)
                        if on_reasoning:
                            on_reasoning(thought)
                    for tc in delta.get("tool_calls") or []:
                        slot = calls.setdefault(tc.get("index", 0), {"name": "", "arguments": ""})
                        fn = tc.get("function") or {}
                        slot["name"] += fn.get("name") or ""
                        slot["arguments"] += fn.get("arguments") or ""
                    if delta.get("content"):
                        yield delta["content"]
            for c in calls.values():  # se traduce al protocolo comun para el bucle del agente
                try:
                    args = json.loads(c["arguments"] or "{}")
                except json.JSONDecodeError:
                    args = {"input": c["arguments"]}
                sp.visible += 1   # una llamada a herramienta cuenta como respuesta
                yield f'<tool>{json.dumps({"name": c["name"], "args": args}, ensure_ascii=False)}</tool>'
                break  # una herramienta por paso

        yield from self._finish(self._timed(split_stream(gen(), "", on_reasoning, sp), lambda: sp.raw), sp)


# --------------------------------------------------------------------------- llama.cpp
class LlamaCppBackend(Backend):
    name = "llamacpp"

    def __init__(self, resolve_path) -> None:
        super().__init__()
        self.resolve_path = resolve_path   # model_id -> (ruta .gguf, metadatos) o None
        self._llm = None
        self._key = None
        self._lock = threading.Lock()

    def available(self) -> bool:
        return importlib.util.find_spec("llama_cpp") is not None

    def _load(self, path: str, opts: dict):
        key = (path, opts.get("n_ctx"), opts.get("n_threads"), opts.get("gpu_layers"), opts.get("n_batch"))
        if self._llm is not None and self._key == key:
            return self._llm
        from llama_cpp import Llama
        self._llm = None
        kwargs = dict(model_path=path, n_ctx=opts.get("n_ctx", 4096), n_threads=opts.get("n_threads") or None,
                      n_batch=opts.get("n_batch", 256), n_gpu_layers=opts.get("gpu_layers", 0), verbose=False)
        if opts.get("flash_attn"):
            kwargs["flash_attn"] = True
        L.info("Cargando GGUF %s (ctx=%s, hilos=%s, gpu_layers=%s)", path, kwargs["n_ctx"], kwargs["n_threads"],
               kwargs["n_gpu_layers"])
        try:
            self._llm = Llama(**kwargs)
        except TypeError:
            kwargs.pop("flash_attn", None)
            self._llm = Llama(**kwargs)
        self._key = key
        return self._llm

    def unload(self) -> None:
        with self._lock:
            self._llm, self._key = None, None

    def stream_chat(self, messages, model="", **opts):
        resolved = self.resolve_path(model)
        if not resolved:
            raise RuntimeError("No hay modelo GGUF activo. Descarga o importa uno en el Gestor de Modelos.")
        path = resolved[0]
        msgs = [{"role": m["role"], "content": m["content"]} for m in merge_roles(messages)]

        def gen():
            with self._lock:
                llm = self._load(path, opts)
                for chunk in llm.create_chat_completion(messages=msgs, stream=True,
                                                        temperature=opts.get("temperature", 0.7),
                                                        max_tokens=opts.get("max_tokens", 1024)):
                    piece = chunk["choices"][0].get("delta", {}).get("content")
                    if piece:
                        yield piece

        sp = ThinkSplitter(opts.get("think_mode", "tags"))
        yield from self._finish(self._timed(split_stream(gen(), "", opts.get("on_reasoning"), sp), lambda: sp.raw), sp)


# --------------------------------------------------------------------------- Transformers
class TransformersBackend(Backend):
    name = "transformers"

    def __init__(self, resolve_path, device: str = "cpu") -> None:
        super().__init__()
        self.resolve_path, self.device = resolve_path, device
        self._model = self._tok = self._key = None
        self._lock = threading.Lock()

    def available(self) -> bool:
        return all(importlib.util.find_spec(m) for m in ("transformers", "torch"))

    def stream_chat(self, messages, model="", **opts):
        resolved = self.resolve_path(model)
        if not resolved:
            raise RuntimeError("No hay modelo Transformers activo")
        path = resolved[0]
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer, TextIteratorStreamer

        def gen():
            with self._lock:
                if self._key != path:
                    dev = self.device
                    if dev == "dml":
                        import torch_directml
                        dev = torch_directml.device()
                    self._tok = AutoTokenizer.from_pretrained(path)
                    dtype = torch.float16 if self.device in ("cuda", "xpu", "mps") else torch.float32
                    self._model = AutoModelForCausalLM.from_pretrained(path, torch_dtype=dtype).to(dev)
                    self._key = path
                msgs = [{"role": m["role"], "content": m["content"]} for m in merge_roles(messages)]
                ids = self._tok.apply_chat_template(msgs, add_generation_prompt=True, return_tensors="pt").to(self._model.device)
                streamer = TextIteratorStreamer(self._tok, skip_prompt=True, skip_special_tokens=True)
                th = threading.Thread(target=self._model.generate, kwargs=dict(
                    input_ids=ids, streamer=streamer, max_new_tokens=opts.get("max_tokens", 1024),
                    do_sample=True, temperature=max(0.05, opts.get("temperature", 0.7))))
                th.start()
                yield from streamer
                th.join()

        sp = ThinkSplitter(opts.get("think_mode", "tags"))
        yield from self._finish(self._timed(split_stream(gen(), "", opts.get("on_reasoning"), sp), lambda: sp.raw), sp)


def image_b64(data: bytes) -> str:
    return base64.b64encode(data).decode()
