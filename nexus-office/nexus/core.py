"""Ensamblado de todos los servicios de Nexus-Office."""
from __future__ import annotations

import os
import threading

from . import motores, hardware, log
from .agents import AgentManager
from .agents.lounge import Lounge
from .config import get_config
from .coop import CoopNode
from .db import DB
from .llm import LLMManager
from .media import ImageEngine, MusicEngine, VoiceEngine
from .memory import KnowledgeGraph, Learner, MemoryStore, get_embedder
from .memory.embed_worker import EmbedWorker
from .memory.organizer import Organizer
from .models import ModelManager
from .rag import RAGStore
from .remote import RemoteAccess
from .remote.vpn import VPN
from .runtimes import RuntimeManager
from .tools import ProposalManager, Supervisor, ToolRegistry

L = log.get("core")


class Services:
    def __init__(self, detect_hardware: bool = True) -> None:
        log.setup()
        self.config = cfg = get_config()
        if cfg.get("hardware.prioridad_alta", True):
            from .proc import alto_rendimiento
            alto_rendimiento()   # Windows: el servicio sin ventana no pasa a los nucleos de eficiencia
        self.monitor = hardware.Monitor(cfg.get("hardware.monitor_interval_s", 2.0))
        if self.monitor.previous_unclean:
            L.warning("La sesion anterior no se cerro correctamente; se aplicara un perfil mas conservador")
        from .hardware import hw_profiles
        hw_profiles.al_arrancar(cfg)   # GPU elegida antes de calcular el perfil
        if detect_hardware:
            self.hw, self.profile, self.tuning_reasons = hardware.startup(
                cfg.get("hardware.profile_override", ""), cfg.get("hardware.adaptive_tuning", True),
                cfg.get("llm.ollama_url"), self.monitor.previous_unclean, self.monitor.previous_info,
                rapida=bool(cfg.get("hardware.deteccion_rapida", True)))
        else:  # modo rapido para tests
            self.hw = hardware.load_cached() or hardware.detect(probe_runtimes=False)
            self.profile, self.tuning_reasons = hardware.build_profile(self.hw), []
        if detect_hardware and cfg.get("hardware.accel", "auto") != "auto":   # CPU/GPU/NPU elegido a mano al arrancar
            from .hardware.accel_choice import build_for
            self.profile, _plan = build_for(self.hw, cfg.get("hardware.accel"))
            if _plan["note"]:
                L.warning("Acelerador %s: %s", cfg.get("hardware.accel"), _plan["note"])
        hw_profiles.aplicar_al_perfil(self)
        self.monitor.settings = {k: self.profile["llm"][k] for k in ("n_ctx", "n_threads", "n_batch", "gpu_layers")}
        self.runtimes = RuntimeManager()
        self.db = DB()
        self.models = ModelManager(cfg, lambda: self.profile)
        self.embedder = self._make_embedder()
        self.memory = MemoryStore(self.db, self.embedder, cfg.get("memory.decay_days", 30))
        self.graph = KnowledgeGraph(self.db)
        from .memory.hilo import Hilo
        self.hilo = Hilo(self.db)   # conversaciones largas: resumen progresivo y notas fijas
        # que es comun (grafo general) y que es saber hacer de cada agente (su subgrafo): ver nexus/memory/organizer.py
        self.organizer = Organizer(self.graph, lambda: self.embedder, lambda: self.agents.list(), cfg)
        self.learner = Learner(self.db, self.memory, self.graph, self.organizer)
        self.rag = RAGStore(self.db, self.embedder)
        self.rag.graph = self.graph   # cada documento se enlaza en el grafo general o en el de su agente
        self.embed_worker = EmbedWorker(self)   # vectores semanticos en segundo plano con modelos lentos en CPU
        self.rag.on_pending = self.memory.on_pending = self.embed_worker.wake
        self.models.on_activate.append(self._on_model_activated)
        self.models.hw_getter = lambda: self.hw.to_dict()
        motores.revisar_marcas()   # caidas de motores en el arranque anterior (driver de NPU/GPU)
        self.llm = LLMManager(cfg, self.models.catalog, self.profile, self.monitor, self.runtimes, self.hw)
        self.models.ollama_pull = lambda name: self.llm.backends["ollama"].pull(name)   # falla -> descarga directa
        self.learner.llm_complete = lambda p: self.llm.complete(p, max_tokens=300, temperature=0.1)
        self.images = ImageEngine(cfg, self.models.catalog, self.runtimes, lambda: self.profile, self.llm)
        self.music = MusicEngine(cfg, lambda: self.profile)
        self.voice = VoiceEngine(cfg, self.models.catalog, lambda: self.profile)
        # memoria justa: los modelos se cargan por turnos segun la etapa del flujo (nexus/hardware/turnos.py)
        from .hardware.turnos import Turnos
        self.turnos = Turnos(cfg, lambda: self.profile, self.monitor)
        self._registrar_turnos()
        self.tools = ToolRegistry()
        self.supervisor = Supervisor(self.tools, self.db, cfg)
        # IA de apoyo: el embed multilingue ayuda a todas las herramientas (nexus/memory/assist.py)
        from .memory.assist import SemanticAssist
        self.assist = SemanticAssist(lambda: self.embedder, cfg)
        self.supervisor.assist = self.assist
        self.proposals = ProposalManager(self.tools, self.supervisor, self.learner)
        self.agents = AgentManager(self)
        self.graph.namer = lambda aid: getattr(self.agents.get(aid), "name", "") or aid   # ancla de cada subgrafo
        self.learner.namer = self.graph.namer
        try:   # oficina de pyme (nexus/office): cada agente de la oficina estrena su propio subgrafo sembrado
            from . import office
            office.ensure(self)
        except Exception as e:  # noqa: BLE001
            L.warning("No se pudo sembrar el grafo de los agentes de la oficina: %s", e)
        from .memory.kg_jobs import KnowledgeJobs
        self.kg_jobs = KnowledgeJobs(self)   # importaciones de grafos, ZIP razonados, reorganizacion
        self.lounge = Lounge(self)
        self.coop = CoopNode(self)
        self.remote = RemoteAccess(cfg)
        self.vpn = VPN(cfg)
        self.llm.remote = lambda node, messages, opts: self.coop.remote_llm(node, messages, opts)
        self.hilo.llm_complete = lambda p, n: self.llm.complete(p, max_tokens=n, temperature=0.1)
        self.hilo.llm_real = self.llm.has_real_llm
        from .mcp import Gestor
        self.mcp = Gestor(self)     # herramientas de servidores MCP externos (mcp.servidores)
        from .mundo import Mundo
        self.mundo = Mundo(cfg)     # informacion publica al dia por categorias y paises (nexus/mundo)
        from .terminal import Terminal
        self.terminal = Terminal(self)   # consola, interprete y ficheros para personas y agentes
        from .freellmapi import FreeLLMAPI
        self.freellmapi = FreeLLMAPI(cfg)   # pasarela FreeLLMAPI incluida (endpoint LLM «freellmapi:auto»)
        self.freellmapi.al_cambiar = self.llm.actualizar_freellmapi

    # ------------------------------------------------------------ embeddings
    def _make_embedder(self):
        """El modelo GGUF de embeddings activo (Modelos > Embeddings, p.ej. Qwen3-Embedding) o, si no hay, lo que
        haya disponible (sentence-transformers, Ollama) y en ultimo caso hashing, que funciona siempre sin modelo."""
        cfg = self.config
        entry = self.models.catalog.get(cfg.get("memory.embed_model", "")) if cfg.get("memory.embed_model") else None
        if entry is not None and entry.get("kind") != "embed":
            entry = None
        threads = int(cfg.get("memory.embed_threads", 0) or 0) or max(1, int(self.profile["llm"].get("n_threads") or 2))
        return get_embedder(cfg.get("memory.embedder", "auto"), cfg.get("llm.ollama_url"),
                            cfg.get("memory.ollama_embed_model"), self.profile["embeddings"]["device"],
                            gguf_entry=entry, threads=threads, dims=int(cfg.get("memory.embed_dims", 0) or 0),
                            exe_getter=lambda: self.runtimes.exe("llama", self.profile, "cpu"))

    # ------------------------------------------------------------ turnos de modelos
    def _registrar_turnos(self) -> None:
        t = self.turnos
        self.llm.turnos = self.images.turnos = self.voice.turnos = t
        self.embedder.turnos = t
        t.registrar("llm", "modelo de texto", self.llm.huella_turno, self.llm.llm_cargado, self.llm.liberar_llm)
        t.registrar("embed", "embeddings", self._huella_embed,
                    lambda: bool(getattr(self.embedder, "running", lambda: False)()),
                    lambda: getattr(self.embedder, "close", lambda: None)())
        t.registrar("stt", "voz a texto (whisper)",
                    lambda: self.voice.huella_stt() if "faster-whisper" in self.voice.stt_engines() else (0.0, 0.0),
                    lambda: self.voice._whisper is not None, lambda: self.voice.liberar("stt"))
        t.registrar("tts", "texto a voz (Piper)", self._huella_tts,
                    lambda: self.voice._piper is not None, lambda: self.voice.liberar("tts"))
        t.registrar("imagen", "imagen (stable-diffusion.cpp)", self._huella_imagen)

    def _huella_embed(self) -> tuple[float, float]:
        path = getattr(self.embedder, "path", "")
        if not path:
            return 0.0, 0.0
        from .motores.selector import peso_mb
        return peso_mb({"path": path}) * 1.2 + 250, 0.0

    def _huella_tts(self) -> tuple[float, float]:
        entry = self.voice._piper_model() if hasattr(self.voice, "_piper_model") else None
        if not entry:
            return 0.0, 0.0
        from .motores.selector import peso_mb
        return peso_mb(entry) * 1.5 + 100, 0.0

    def _huella_imagen(self) -> tuple[float, float]:
        model = self.images._model()
        sd = self.runtimes.exe("sd", self.profile) if model else None
        if not (model and sd):
            return 0.0, 0.0
        gen = model.get("gen") or {}
        return self.images.huella(model, sd[1], int(gen.get("size") or self.profile["image"]["resolution"]))

    def _turnos_al_arrancar(self) -> None:
        try:
            motivo = self.turnos.motivo_arranque()
            if motivo:
                self.tuning_reasons.append(motivo)
                L.info("Autoajuste: %s", motivo)
                if hasattr(self.embedder, "idle_s"):   # el modelo de embeddings no se queda ocupando memoria
                    self.embedder.idle_s = min(self.embedder.idle_s or 90, 90)
        except Exception as e:  # noqa: BLE001
            L.debug("turnos al arrancar: %s", e)

    def set_embedder(self, embedder=None):
        """Cambia el modelo de embeddings en caliente: nada se bloquea; lo ya indexado se recalcula en segundo plano."""
        old = self.embedder
        self.embedder = embedder or self._make_embedder()
        for store in (self.memory, self.rag):
            store.embedder = self.embedder
            store._cache = None
        self.embedder.turnos = getattr(self, "turnos", None)
        if old is not self.embedder and hasattr(old, "close"):
            old.close()
        L.info("Modelo de embeddings: %s", self.embedder.id)
        self.embed_worker.start()
        return self.embedder

    def _on_model_activated(self, entry: dict) -> None:
        if entry.get("kind") == "embed":
            self.set_embedder()

    def start(self) -> None:
        self.monitor.start()
        threading.Thread(target=self._turnos_al_arrancar, name="nexus-turnos", daemon=True).start()
        self.embed_worker.start()
        self.assist.prewarm(self.tools.for_agent(["*"]), self.agents.list())
        if hardware.profiles.android() and self.config.get("hardware.adaptive_tuning", True):
            from .android_tune import LiveTuner
            self.android_tuner = LiveTuner(self)
            self.android_tuner.start()
            from .android_tune import after_start
            threading.Thread(target=after_start, args=(self, self.android_tuner), name="nexus-tras-arranque",
                             daemon=True).start()
        if self.config.get("mcp.servidores"):
            self.mcp.conectar()
        if not os.environ.get("NEXUS_SIN_MUNDO"):
            self.mundo.iniciar()
        threading.Thread(target=self._freellmapi_al_iniciar, name="nexus-freellmapi", daemon=True).start()
        if self.config.get("coop.enabled", False):
            try:
                self.coop.start()
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo iniciar el modo cooperativo: %s", e)

    def _freellmapi_al_iniciar(self) -> None:
        self.freellmapi.al_iniciar()

    def shutdown(self) -> None:
        L.info("Apagando Nexus-Office...")
        try:
            self.mcp.cerrar()
        except Exception:  # noqa: BLE001
            pass
        for cierre in (self.mundo.cerrar, self.terminal.cerrar, self.freellmapi.parar):
            try:
                cierre()
            except Exception:  # noqa: BLE001
                pass
        try:
            self.coop.stop()
        except Exception:  # noqa: BLE001
            pass
        ls = self.llm.backends.get("llamaserver")
        if ls:
            ls.stop()
        self.embed_worker.stop()
        if hasattr(self.embedder, "close"):
            self.embedder.close()
        if getattr(self, "android_tuner", None):
            self.android_tuner.stop()
        self.monitor.stop()
        self.db.close()
