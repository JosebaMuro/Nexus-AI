"""IA online para el agente Profesor (y para quien la elija): Gemini, ChatGPT, Perplexity, Claude, Mistral...

Es lo unico de Nexus-Office que sale a internet, y solo si un administrador pone la clave de un proveedor: sin
clave el Profesor responde con el modelo local. Especificacion de modelo: ``nube:`` (el proveedor elegido en
Ajustes), ``nube:<proveedor>`` o ``nube:<proveedor>:<modelo>``.

Claude va por el SDK oficial de Anthropic (Messages API); el resto por su API compatible con OpenAI
(``/chat/completions`` en streaming). Las claves se guardan en la configuracion como secretos (``nube.clave_<id>``)
y la API nunca las devuelve.
"""
from __future__ import annotations

import base64
import json
import time
from typing import Iterator

import httpx

from .. import log
from .backends import Backend, ThinkSplitter, merge_roles, split_stream

L = log.get("nube")

# id -> nombre, estilo de API, URL base, modelo recomendado y donde se saca la clave
PROVIDERS: dict[str, dict] = {
    "gemini": {"nombre": "Google Gemini", "estilo": "openai",
               "base_url": "https://generativelanguage.googleapis.com/v1beta/openai",
               "modelo": "gemini-2.5-flash", "clave_en": "https://aistudio.google.com/apikey"},
    "openai": {"nombre": "ChatGPT (OpenAI)", "estilo": "openai", "base_url": "https://api.openai.com/v1",
               "modelo": "gpt-4.1-mini", "clave_en": "https://platform.openai.com/api-keys"},
    "perplexity": {"nombre": "Perplexity", "estilo": "openai", "base_url": "https://api.perplexity.ai",
                   "modelo": "sonar", "clave_en": "https://www.perplexity.ai/settings/api"},
    "claude": {"nombre": "Claude (Anthropic)", "estilo": "anthropic", "base_url": "https://api.anthropic.com",
               "modelo": "claude-opus-5-5", "clave_en": "https://platform.claude.com/settings/keys"},
    "mistral": {"nombre": "Mistral", "estilo": "openai", "base_url": "https://api.mistral.ai/v1",
                "modelo": "mistral-small-latest", "clave_en": "https://console.mistral.ai/api-keys"},
    "deepseek": {"nombre": "DeepSeek", "estilo": "openai", "base_url": "https://api.deepseek.com/v1",
                 "modelo": "deepseek-chat", "clave_en": "https://platform.deepseek.com/api_keys"},
    "groq": {"nombre": "Groq", "estilo": "openai", "base_url": "https://api.groq.com/openai/v1",
             "modelo": "llama-3.3-70b-versatile", "clave_en": "https://console.groq.com/keys"},
    "openrouter": {"nombre": "OpenRouter", "estilo": "openai", "base_url": "https://openrouter.ai/api/v1",
                   "modelo": "openrouter/auto", "clave_en": "https://openrouter.ai/keys"},
    "compatible": {"nombre": "Otro compatible con OpenAI", "estilo": "openai", "base_url": "", "modelo": "",
                   "clave_en": ""},
}
EFFORTS = ("low", "medium", "high", "xhigh", "max")


class CloudError(RuntimeError):
    pass


class CloudBackend(Backend):
    name = "nube"

    def __init__(self, config, timeout: float = 180) -> None:
        super().__init__()
        self.config, self.timeout = config, timeout

    # ---------------------------------------------------------------- configuracion
    def key(self, prov: str) -> str:
        return str(self.config.get(f"nube.clave_{prov}", "") or "") if self.config else ""

    def resolve(self, spec: str = "") -> tuple[str, str]:
        """(proveedor, modelo) para ``<proveedor>[:<modelo>]``; vacio = lo elegido en Ajustes."""
        prov, _, model = (spec or "").partition(":")
        chosen = str(self.config.get("nube.proveedor", "") or "") if self.config else ""
        prov = prov or chosen
        if not model and prov == chosen:
            model = str(self.config.get("nube.modelo", "") or "")
        info = PROVIDERS.get(prov)
        return prov, model or (info["modelo"] if info else "")

    def base_url(self, prov: str) -> str:
        if prov == "compatible":
            return str(self.config.get("nube.base_url", "") or "").rstrip("/")
        return PROVIDERS[prov]["base_url"]

    def ready(self, spec: str = "") -> bool:
        prov, model = self.resolve(spec)
        return prov in PROVIDERS and bool(self.key(prov) or prov == "compatible") and bool(model) \
            and bool(self.base_url(prov))

    def available(self) -> bool:
        return self.ready()

    def models(self) -> list[str]:
        prov, model = self.resolve()
        return [f"{prov}:{model}"] if self.ready() else []

    # ---------------------------------------------------------------- chat
    def stream_chat(self, messages, model="", **opts) -> Iterator[str]:
        prov, mdl = self.resolve(model)
        if not self.ready(model):
            raise CloudError(f"IA online sin configurar ({prov or 'ningun proveedor'}): pon la clave en Ajustes > "
                             "Profesor")
        on_reasoning = opts.get("on_reasoning")
        sp = ThinkSplitter("tags")
        gen = self._anthropic(messages, mdl, opts, sp) if PROVIDERS[prov]["estilo"] == "anthropic" else \
            self._openai(prov, messages, mdl, opts, sp)
        yield from self._finish(self._timed(split_stream(gen, "", on_reasoning, sp), lambda: sp.raw), sp)

    def _openai(self, prov: str, messages, model: str, opts: dict, sp: ThinkSplitter) -> Iterator[str]:
        msgs = []
        for m in merge_roles(messages):
            if m.get("images"):
                parts = [{"type": "text", "text": m["content"]}] + [
                    {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b}"}} for b in m["images"]]
                msgs.append({"role": m["role"], "content": parts})
            else:
                msgs.append({"role": m["role"], "content": m["content"]})
        body = {"model": model, "messages": msgs, "stream": True, "temperature": opts.get("temperature", 0.7),
                "max_tokens": max(int(opts.get("max_tokens") or 0), 2048)}
        headers = {"Authorization": f"Bearer {self.key(prov)}"} if self.key(prov) else {}
        if prov == "openrouter":
            headers.update({"HTTP-Referer": "https://github.com/JosebaMuro/Nexus-AI", "X-Title": "Nexus-Office"})
        citations: list[str] = []
        try:
            with httpx.stream("POST", f"{self.base_url(prov)}/chat/completions", json=body, headers=headers,
                              timeout=self.timeout) as r:
                if r.status_code >= 400:
                    r.read()
                    raise CloudError(_http_error(PROVIDERS[prov]["nombre"], r.status_code, r.text))
                for line in r.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    payload = line[5:].strip()
                    if payload == "[DONE]":
                        break
                    try:
                        data = json.loads(payload)
                    except ValueError:
                        continue
                    citations = data.get("citations") or citations   # Perplexity: fuentes de la respuesta
                    try:
                        delta = data["choices"][0].get("delta", {})
                    except (KeyError, IndexError):
                        continue
                    thought = delta.get("reasoning_content") or delta.get("reasoning")
                    if thought:
                        sp.server_split()
                        sp.thought.append(thought)
                        cb = opts.get("on_reasoning")
                        if cb:
                            cb(thought)
                    if delta.get("content"):
                        yield delta["content"]
        except httpx.HTTPError as e:
            raise CloudError(f"No se puede conectar con {PROVIDERS[prov]['nombre']}: {e}") from e
        if citations:
            yield "\n\nFuentes:\n" + "\n".join(f"[{i}] {u}" for i, u in enumerate(citations, 1))

    def _anthropic(self, messages, model: str, opts: dict, sp: ThinkSplitter) -> Iterator[str]:
        try:
            import anthropic
        except ImportError as e:
            raise CloudError("Falta el paquete 'anthropic' (va en requirements.txt): reinstala Nexus-Office") from e
        client = anthropic.Anthropic(api_key=self.key("claude"), base_url=self.base_url("claude"), timeout=self.timeout,
                                     max_retries=2)
        system = "\n\n".join(m["content"] for m in messages if m["role"] == "system")
        msgs = []
        for m in merge_roles([m for m in messages if m["role"] != "system"]):
            content: list[dict] = [{"type": "image", "source": {"type": "base64", "media_type": _mime(b), "data": b}}
                                   for b in m.get("images") or []]
            content.append({"type": "text", "text": m["content"] or "."})
            msgs.append({"role": m["role"], "content": content})
        if not msgs or msgs[0]["role"] != "user":
            msgs.insert(0, {"role": "user", "content": [{"type": "text", "text": "."}]})
        effort = str(self.config.get("nube.esfuerzo", "medium") or "medium")
        kw = {"model": model, "max_tokens": max(int(opts.get("max_tokens") or 0), 16000), "messages": msgs,
              "thinking": {"type": "adaptive", "display": "summarized"},
              "output_config": {"effort": effort if effort in EFFORTS else "medium"},
              # si el modelo declina por sus filtros de seguridad, Anthropic reintenta con su modelo recomendado
              "betas": ["server-side-fallback-2026-07-01"], "fallbacks": "default"}
        if system:
            kw["system"] = system
        cb = opts.get("on_reasoning")
        try:
            with client.beta.messages.stream(**kw) as stream:
                for event in stream:
                    if event.type != "content_block_delta":
                        continue
                    if event.delta.type == "thinking_delta":
                        sp.server_split()
                        sp.thought.append(event.delta.thinking)
                        if cb:
                            cb(event.delta.thinking)
                    elif event.delta.type == "text_delta":
                        yield event.delta.text
                final = stream.get_final_message()
            if final.stop_reason == "refusal":
                yield "\n\n_(Claude no ha querido responder a esta petición.)_"
            elif final.stop_reason == "max_tokens":
                yield "\n\n_(Respuesta cortada por el límite de tokens.)_"
        except anthropic.AuthenticationError as e:
            raise CloudError("Claude: la clave no es válida") from e
        except anthropic.PermissionDeniedError as e:
            raise CloudError("Claude: la clave no tiene permiso para este modelo") from e
        except anthropic.NotFoundError as e:
            raise CloudError(f"Claude: el modelo {model} no existe") from e
        except anthropic.RateLimitError as e:
            raise CloudError("Claude: límite de uso alcanzado, prueba en un rato") from e
        except anthropic.APIStatusError as e:
            raise CloudError(f"Claude: error {e.status_code}: {e.message}"[:400]) from e
        except anthropic.APIConnectionError as e:
            raise CloudError("No se puede conectar con Claude (¿hay internet?)") from e

    # ---------------------------------------------------------------- estado y prueba
    def status(self) -> dict:
        prov, model = self.resolve()
        return {"proveedor": prov, "modelo": model, "listo": self.ready(),
                "esfuerzo": self.config.get("nube.esfuerzo", "medium") if self.config else "medium",
                "base_url": self.config.get("nube.base_url", "") if self.config else "",
                "proveedores": [{"id": k, "nombre": v["nombre"], "modelo": v["modelo"], "clave_en": v["clave_en"],
                                 "con_clave": bool(self.key(k))} for k, v in PROVIDERS.items()]}

    def test(self, spec: str = "") -> dict:
        t0 = time.time()
        try:
            out = "".join(self.stream_chat([{"role": "user", "content": "Responde solo: OK"}], model=spec,
                                           temperature=0, max_tokens=64))
            return {"ok": True, "respuesta": out.strip()[:200], "segundos": round(time.time() - t0, 1)}
        except CloudError as e:
            return {"ok": False, "error": str(e)}


def _mime(b64: str) -> str:
    head = base64.b64decode(b64[:16] + "=" * (-len(b64[:16]) % 4))
    return "image/jpeg" if head.startswith(b"\xff\xd8") else "image/webp" if head[8:12] == b"WEBP" else \
        "image/gif" if head.startswith(b"GIF") else "image/png"


def _http_error(name: str, code: int, text: str) -> str:
    try:
        err = json.loads(text)
        err = err[0] if isinstance(err, list) else err
        msg = (err.get("error") or {}).get("message") if isinstance(err.get("error"), dict) else err.get("error")
        msg = msg or err.get("message") or text
    except (ValueError, AttributeError):
        msg = text
    if code in (401, 403):
        return f"{name}: la clave no es válida ({code})"
    if code == 429:
        return f"{name}: límite de uso alcanzado, prueba en un rato"
    return f"{name}: error {code}: {str(msg)[:300]}"
