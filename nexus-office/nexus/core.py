"""Ensamblado de todos los servicios de Nexus-Office."""
from __future__ import annotations

import threading

from . import hardware, log
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
        self.monitor = hardware.Monitor(cfg.get("hardware.monitor_interval_s", 2.0))
        if self.monitor.previous_unclean:
            L.warning("La sesion anterior no se cerro correctamente; se aplicara un perfil mas conservador")
        if detect_hardware:
            self.hw, self.profile, self.tuning_reasons = hardware.startup(
                cfg.get("hardware.profile_override", ""), cfg.get("hardware.adaptive_tuning", True),
                cfg.get("llm.ollama_url"), self.monitor.previous_unclean, self.monitor.previous_info)
        else:  # modo rapido para tests
            self.hw = hardware.load_cached() or hardware.detect(probe_runtimes=False)
            self.profile, self.tuning_reasons = hardware.build_profile(self.hw), []
        self.monitor.settings = {k: self.profile["llm"][k] for k in ("n_ctx", "n_threads", "n_batch", "gpu_layers")}
        self.runtimes = RuntimeManager()
        self.db = DB()
        self.models = ModelManager(cfg, lambda: self.profile)
        self.embedder = self._make_embedder()
        self.memory = MemoryStore(self.db, self.embedder, cfg.get("memory.decay_days", 30))
        self.graph = KnowledgeGraph(self.db)
        # que es comun (grafo general) y que es saber hacer de cada agente (su subgrafo): ver nexus/memory/organizer.py
        self.organizer = Organizer(self.graph, lambda: self.embedder, lambda: self.agents.list(), cfg)
        self.learner = Learner(self.db, self.memory, self.graph, self.organizer)
        self.rag = RAGStore(self.db, self.embedder)
        self.rag.graph = self.graph   # cada documento se enlaza en el grafo general o en el de su agente
        self.embed_worker = EmbedWorker(self)   # vectores semanticos en segundo plano con modelos lentos en CPU
        self.rag.on_pending = self.memory.on_pending = self.embed_worker.wake
        self.models.on_activate.append(self._on_model_activated)
        self.llm = LLMManager(cfg, self.models.catalog, self.profile, self.monitor, self.runtimes)
        self.models.ollama_pull = lambda name: self.llm.backends["ollama"].pull(name)   # falla -> descarga directa
        self.learner.llm_complete = lambda p: self.llm.complete(p, max_tokens=300, temperature=0.1)
        self.images = ImageEngine(cfg, self.models.catalog, self.runtimes, lambda: self.profile, self.llm)
        self.music = MusicEngine(cfg, lambda: self.profile)
        self.voice = VoiceEngine(cfg, self.models.catalog, lambda: self.profile)
        self.tools = ToolRegistry(cfg.get("tools.sandbox_community_tools", True))
        self.supervisor = Supervisor(self.tools, self.db, cfg)
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

    def set_embedder(self, embedder=None):
        """Cambia el modelo de embeddings en caliente: nada se bloquea; lo ya indexado se recalcula en segundo plano."""
        old = self.embedder
        self.embedder = embedder or self._make_embedder()
        for store in (self.memory, self.rag):
            store.embedder = self.embedder
            store._cache = None
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
        self.embed_worker.start()
        if hardware.profiles.android() and self.config.get("hardware.adaptive_tuning", True):
            from .android_tune import LiveTuner
            self.android_tuner = LiveTuner(self)
            self.android_tuner.start()
            from .android_tune import after_start
            threading.Thread(target=after_start, args=(self, self.android_tuner), name="nexus-tras-arranque",
                             daemon=True).start()
        if self.config.get("coop.enabled", False):
            try:
                self.coop.start()
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo iniciar el modo cooperativo: %s", e)

    def shutdown(self) -> None:
        L.info("Apagando Nexus-Office...")
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
