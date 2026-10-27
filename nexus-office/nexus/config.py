"""Configuracion en dos niveles.

* Nivel Usuario (modo sencillo): solo las claves de ``USER_KEYS``.
* Nivel Desarrollador/Administrador: todo el arbol, protegido por PIN.

La configuracion se guarda en ``data/config.yaml``. Los valores ausentes se
completan con ``DEFAULTS`` para que actualizar el proyecto nunca rompa
configuraciones antiguas.
"""
from __future__ import annotations

import copy
import hashlib
import hmac
import os
import threading
from typing import Any

import yaml

from . import paths

DEFAULTS: dict[str, Any] = {
    "user": {
        "assistant_name": "Nexus-Office",
        "language": "es",
        "response_style": "equilibrado",   # breve | equilibrado | detallado
        "voice_enabled": True,
        "voice_name": "",
        "speech_rate": 1.0,
        "auto_speak": False,
        "default_agent": "auto",
        "memory_enabled": True,
        "accent_color": "#00f0ff",
        "avatar_enabled": True,
        "avatar_style": "geisha",      # geisha (3D) | grafo (2D) | glb (modelo propio)
        "avatar_model": "",            # URL del modelo GLB/GLTF/VRM subido
        "theme": {},                   # tema del editor ({} = plantilla "neon")
    },
    "llm": {
        # orden de preferencia; se usa el primer backend disponible
        "backend_order": ["llamaserver", "llamacpp", "ollama", "openai", "transformers", "builtin"],
        "engine": "auto",               # motor elegido al arrancar: auto | llamaserver | llamacpp | ollama
        "default_model": "",            # id del catalogo, nombre ollama o modelo del endpoint
        "ollama_url": "http://127.0.0.1:11434",
        "openai_base_url": "http://127.0.0.1:1234/v1",   # LM Studio / vLLM / llama-server
        "openai_api_key": "",
        "openai_model": "",
        "temperature": 0.7,
        "max_tokens": 1024,
        # modelos que razonan (Qwen3.5, Qwen3-VL Thinking...): auto = razonan antes de responder; off = responden directo
        "reasoning": "auto",
        "reasoning_budget": 2048,       # tokens extra reservados para razonar en el chat
        "reasoning_budget_tasks": 768,  # en tareas internas (resumenes, extraccion de conocimiento)
        "max_tool_steps": 5,
        "request_timeout": 180,
    },
    "memory": {
        "embedder": "auto",             # auto | gguf | hashing | ollama | sentence-transformers
        "embed_model": "",              # modelo GGUF de embeddings activo (Modelos): p.ej. qwen3-embedding-0.6b
        "embed_threads": 0,             # hilos del servidor de embeddings (0 = los del LLM)
        "embed_dims": 0,                # recorte Matryoshka (p.ej. 512) para ahorrar RAM; 0 = completo
        "ollama_embed_model": "nomic-embed-text",
        # que conocimiento es comun y cual es de cada agente (nexus/memory/organizer.py)
        "organizer": {"min_affinity": 0.35, "margin": 0.03, "min_keyword_hits": 1},
        "zip_reason_docs": 12,          # documentos de un ZIP que el LLM razona uno a uno (el resto, por reglas)
        "top_k": 6,
        "llm_graph_extraction": False,  # extraccion de tripletas con el LLM (mas lento)
        "decay_days": 30,
        "consolidate_every": 12,        # mensajes entre consolidaciones
    },
    "tools": {
        # politica de supervision por nivel de riesgo: auto | ask | admin | deny
        "policy": {"low": "auto", "medium": "ask", "high": "admin"},
        "timeout_s": 30,
        "allow_agent_proposals": True,
        "sandbox_community_tools": True,
    },
    "hardware": {
        "profile_override": "",         # forzar un acelerador: cuda | rocm | directml | xpu | openvino | mps | cpu
        "accel": "auto",                # eleccion del selector de inicio: auto | cpu | gpu | npu
        "llama_variant": "",            # binario de llama.cpp preferido (lo fija el selector: cpu, cuda, vulkan...)
        "adaptive_tuning": True,
        "monitor_interval_s": 2.0,
    },
    "models": {
        "huggingface_token": "",
        "civitai_token": "",
        "kaggle_username": "",
        "kaggle_key": "",
        "modelscope_token": "",
    },
    "media": {
        "tts_engine": "auto",           # auto | pyttsx3 | edge-tts | piper | browser
        "stt_engine": "auto",           # auto | faster-whisper | vosk | browser
        "image_model": "",              # id del catalogo (sd.cpp) o vacio = el primero; sin modelo, compositor de escenas
        "music_engine": "auto",         # auto | musicgen | procedural
        "vision_model": "",             # modelo multimodal (ollama/openai) para analizar imagenes
        "auto_enhance_prompts": True,   # mejorar automaticamente los prompts de imagen (estilo, calidad, traduccion)
    },
    "coop": {
        "enabled": False,               # arrancar el nodo de la LAN virtual al iniciar
        "node_name": "",
        "network_id": "",
        "passphrase": "",               # clave de red (se comparte con el codigo de invitacion)
        "listen": False,                # aceptar conexiones entrantes (requiere puerto accesible / Tailscale / ZeroTier)
        "listen_host": "0.0.0.0",
        "listen_port": 8766,
        "public_urls": [],              # ws://IP:puerto por las que otros pueden alcanzarte (van en la invitacion)
        "peers": [],                    # ws://... de nodos a los que conectar
        "max_hops": 6,
        "allow_remote_inference": True,
        "allow_remote_agents": False,
        "share_memory": False,
        "share_tools": True,
    },
    # perfil de oficina activo (nexus/office): la instalacion como oficina de una pyme con agentes de su gremio
    "office": {"profile": ""},
    "lounge": {"auto_meet": True, "meet_every_tasks": 8},
    "startup": {"ask_accel": True},     # mostrar al abrir el selector de motor llama y CPU/GPU/NPU
    "server": {"host": "127.0.0.1", "port": 8775, "open_browser": True,
               "lan_key": ""},                 # clave para dispositivos de la red (movil, tablet, consola); se genera sola
    # acceso remoto por internet: HTTPS (TLS 1.3) en un puerto propio, solo para dispositivos emparejados
    "remote": {"enabled": False, "port": 8443, "host": "0.0.0.0"},
    # VPN cifrada (WireGuard) entre la app Android y este equipo: ver docs/VPN.md
    "vpn": {"enabled": False, "port": 51820, "subnet": "10.78.0.0/24", "endpoints": []},
    # paralelismo de rastreo, importacion y embeddings (0 = automatico: todos los nucleos)
    "performance": {"workers": 0, "crawl_connections": 0},
    # rastreo web por lotes (0 = automatico segun la RAM) y acceso a webs de la red local (intranet, wiki interna)
    "rag": {"crawl_batch_pages": 0, "allow_private_urls": False},
    # mesa de bolsa: SIEMPRE simulada (sin dinero ni ordenes reales); precios reales con conexion
    "bolsa": {"datos": "reales",            # reales (con conexion; sin ella, simulados) | simulados
              "refresco_s": 20,             # cada cuanto se piden cotizaciones reales
              # agente autonomo: gestiona su propia cartera simulada con los datos que recopila
              "agente": {"activo": True, "intervalo_s": 60, "capital": 10000.0, "riesgo_pct": 1.0,
                         "max_posiciones": 8, "max_peso_pct": 20.0, "reserva_pct": 10.0, "universo": []}},
    # IA online para el agente Profesor: nada sale a internet hasta que un administrador pone una clave
    "nube": {"proveedor": "", "modelo": "", "base_url": "", "esfuerzo": "medium",
             "clave_gemini": "",
             "clave_openai": "",
             "clave_perplexity": "",
             "clave_claude": "",
             "clave_mistral": "",
             "clave_deepseek": "",
             "clave_groq": "",
             "clave_openrouter": "",
             "clave_compatible": ""},
    "admin": {"pin_hash": "", "pin_salt": ""},
}

# claves editables en modo usuario (seccion.clave)
USER_KEYS = {f"user.{k}" for k in DEFAULTS["user"]}
SECRET_KEYS = {"admin.pin_hash", "admin.pin_salt", "llm.openai_api_key", "models.huggingface_token",
               "models.civitai_token", "models.kaggle_key", "models.modelscope_token", "coop.passphrase", "server.lan_key",
               "nube.clave_gemini", "nube.clave_openai", "nube.clave_perplexity", "nube.clave_claude",
               "nube.clave_mistral", "nube.clave_deepseek", "nube.clave_groq", "nube.clave_openrouter", "nube.clave_compatible"}


def _deep_merge(base: dict, extra: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in (extra or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


class Config:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.data: dict[str, Any] = {}
        self.load()

    def load(self) -> None:
        f = paths.config_file()
        raw = {}
        if f.exists():
            raw = yaml.safe_load(f.read_text(encoding="utf-8")) or {}
        with self._lock:
            self.data = _deep_merge(DEFAULTS, raw)
        if not f.exists():
            self.save()

    def save(self) -> None:
        with self._lock:
            txt = yaml.safe_dump(self.data, allow_unicode=True, sort_keys=False)
        tmp = paths.config_file().with_suffix(".tmp")
        tmp.write_text(txt, encoding="utf-8")
        os.replace(tmp, paths.config_file())

    def get(self, dotted: str, default: Any = None) -> Any:
        node: Any = self.data
        for part in dotted.split("."):
            if not isinstance(node, dict) or part not in node:
                return default
            node = node[part]
        return node

    def set(self, dotted: str, value: Any, save: bool = True) -> None:
        with self._lock:
            node = self.data
            parts = dotted.split(".")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = value
        if save:
            self.save()

    def update(self, changes: dict[str, Any], admin: bool) -> dict[str, Any]:
        """Aplica cambios ``{"seccion.clave": valor}`` respetando el nivel de acceso."""
        applied = {}
        for key, value in changes.items():
            if key.startswith("admin."):
                continue  # el PIN solo se cambia via set_pin
            if not admin and key not in USER_KEYS:
                raise PermissionError(f"'{key}' requiere modo administrador")
            if key in SECRET_KEYS and value == "********":
                continue  # la vista enmascarada de vuelta: se conserva el secreto guardado
            self.set(key, value, save=False)
            applied[key] = value
        self.save()
        return applied

    def view(self, admin: bool) -> dict[str, Any]:
        """Configuracion visible para el nivel indicado (sin secretos en claro)."""
        with self._lock:
            d = copy.deepcopy(self.data)
        if not admin:
            return {"user": d["user"]}
        for key in SECRET_KEYS:
            sec, k = key.split(".")
            if d.get(sec, {}).get(k):
                d[sec][k] = "********"
        d.pop("admin", None)
        return d

    # ---- PIN de administrador -------------------------------------------------
    def has_pin(self) -> bool:
        return bool(self.get("admin.pin_hash"))

    def set_pin(self, pin: str) -> None:
        if len(pin) < 4:
            raise ValueError("El PIN debe tener al menos 4 caracteres")
        salt = os.urandom(16).hex()
        self.set("admin.pin_salt", salt, save=False)
        self.set("admin.pin_hash", _hash_pin(pin, salt))

    def check_pin(self, pin: str) -> bool:
        if not self.has_pin():
            return False
        return hmac.compare_digest(_hash_pin(pin, self.get("admin.pin_salt")), self.get("admin.pin_hash"))


def _hash_pin(pin: str, salt: str) -> str:
    return hashlib.pbkdf2_hmac("sha256", pin.encode(), bytes.fromhex(salt), 200_000).hex()


_config: Config | None = None


def get_config() -> Config:
    global _config
    if _config is None:
        _config = Config()
    return _config


def reset_config() -> None:
    """Solo para tests: fuerza recargar desde disco."""
    global _config
    _config = None
