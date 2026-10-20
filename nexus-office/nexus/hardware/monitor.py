"""Monitorizacion en vivo + resumen de sesion que alimenta al autoajuste del siguiente arranque."""
from __future__ import annotations

import collections
import json
import subprocess
import threading
import time
import uuid

import psutil

from .. import log, paths
from .detect import find_nvidia_smi

L = log.get("monitor")
HISTORY_MAX = 50


def history_file():
    return paths.data_dir() / "perf_history.json"


def running_marker():
    return paths.data_dir() / ".running"


def load_history() -> list[dict]:
    f = history_file()
    if f.exists():
        try:
            return json.loads(f.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return []
    return []


class Monitor:
    def __init__(self, interval: float = 2.0, settings: dict | None = None) -> None:
        self.interval = interval
        self.settings = settings or {}           # ajustes LLM en uso (para atribuir el rendimiento)
        self.samples: collections.deque[dict] = collections.deque(maxlen=300)
        self.session_id = uuid.uuid4().hex[:10]
        self.started = time.time()
        self.llm_calls: list[dict] = []
        self.errors: list[dict] = []
        self.peak_ram_pct = 0.0
        self.peak_vram_pct = 0.0
        self._cpu_acc = 0.0
        self._n = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._proc = psutil.Process()
        self._nvsmi = find_nvidia_smi()
        self.previous_unclean = running_marker().exists()
        self.previous_info: dict = {}
        if self.previous_unclean:  # ultimo latido de la sesion que no cerro bien
            try:
                self.previous_info = json.loads(running_marker().read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                self.previous_info = {}

    # ---------------------------------------------------------------- ciclo de vida
    def start(self) -> None:
        self._heartbeat()
        psutil.cpu_percent(None)
        self._thread = threading.Thread(target=self._loop, name="nexus-monitor", daemon=True)
        self._thread.start()

    def stop(self) -> dict:
        self._stop.set()
        summary = self.summary(final=True)
        hist = load_history()
        hist.append(summary)
        history_file().write_text(json.dumps(hist[-HISTORY_MAX:], indent=1), encoding="utf-8")
        running_marker().unlink(missing_ok=True)
        L.info("Sesion %s guardada (%.1f tok/s medio)", self.session_id, summary["avg_tps"])
        return summary

    def _gpu_sample(self) -> dict:
        if not self._nvsmi:
            return {}
        try:
            out = subprocess.run([self._nvsmi, "--query-gpu=utilization.gpu,memory.used,memory.total",
                                  "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=3,
                                 creationflags=0x08000000 if hasattr(subprocess, "CREATE_NO_WINDOW") else 0)
            util, used, total = (float(x) for x in out.stdout.strip().splitlines()[0].split(","))
            return {"gpu_util": util, "vram_used_gb": round(used / 1024, 2), "vram_pct": round(100 * used / total, 1)}
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            return {}

    def _loop(self) -> None:
        while not self._stop.wait(self.interval):
            try:
                vm = psutil.virtual_memory()
                s = {"t": round(time.time(), 1), "cpu": psutil.cpu_percent(None), "ram_pct": vm.percent,
                     "ram_used_gb": round(vm.used / 1024**3, 2),
                     "proc_rss_gb": round(self._proc.memory_info().rss / 1024**3, 3)}
                s.update(self._gpu_sample())
                self.samples.append(s)
                self.peak_ram_pct = max(self.peak_ram_pct, vm.percent)
                self.peak_vram_pct = max(self.peak_vram_pct, s.get("vram_pct", 0.0))
                self._cpu_acc += s["cpu"]
                self._n += 1
                if self._n % 5 == 0:
                    self._heartbeat()
            except Exception as e:  # noqa: BLE001
                L.debug("muestra fallida: %s", e)

    def _heartbeat(self) -> None:
        """Latido en disco: si la sesion muere de golpe, el siguiente arranque sabe si hubo presion de memoria."""
        try:
            running_marker().write_text(json.dumps({"session": self.session_id, "peak_ram_pct": self.peak_ram_pct,
                                                    "peak_vram_pct": self.peak_vram_pct, "settings": self.settings,
                                                    "oom_errors": sum(1 for e in self.errors if e["kind"] == "oom"),
                                                    "t": time.time()}), encoding="utf-8")
        except OSError:
            pass

    # ---------------------------------------------------------------- eventos
    def record_llm(self, backend: str, model: str, tokens: int, seconds: float, first_token_s: float) -> None:
        tps = tokens / seconds if seconds > 0 else 0.0
        self.llm_calls.append({"backend": backend, "model": model, "tokens": tokens, "seconds": round(seconds, 3),
                               "tps": round(tps, 2), "ttft": round(first_token_s, 3), "t": time.time()})

    def record_error(self, kind: str, message: str) -> None:
        self.errors.append({"kind": kind, "message": message[:300], "t": time.time()})
        L.warning("Error registrado [%s]: %s", kind, message[:200])

    # ---------------------------------------------------------------- informes
    def live(self) -> dict:
        last = self.samples[-1] if self.samples else {}
        recent_calls = self.llm_calls[-20:]
        return {"session": self.session_id, "uptime_s": round(time.time() - self.started), "current": last,
                "samples": list(self.samples)[-90:], "llm_calls": recent_calls,
                "errors": self.errors[-20:], "summary": self.summary()}

    def summary(self, final: bool = False) -> dict:
        calls = [c for c in self.llm_calls if c["backend"] != "builtin"]
        tok = sum(c["tokens"] for c in calls)
        secs = sum(c["seconds"] for c in calls)
        return {
            "session": self.session_id, "started": self.started, "duration_s": round(time.time() - self.started),
            "clean_exit": final, "llm_calls": len(calls), "avg_tps": round(tok / secs, 2) if secs else 0.0,
            "avg_ttft": round(sum(c["ttft"] for c in calls) / len(calls), 2) if calls else 0.0,
            "peak_ram_pct": self.peak_ram_pct, "peak_vram_pct": self.peak_vram_pct,
            "avg_cpu": round(self._cpu_acc / self._n, 1) if self._n else 0.0,
            "oom_errors": sum(1 for e in self.errors if e["kind"] == "oom"),
            "errors": len(self.errors), "settings": self.settings,
        }
