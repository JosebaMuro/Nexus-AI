"""Autoajuste continuo de Nexus-Office en la app Android autonoma.

Al arrancar y mientras Nexus-Office esta en marcha, se adapta solo al movil:

* **Nucleos**: cuenta los nucleos rapidos (los grandes y el principal) por el modelo de CPU de /proc/cpuinfo o,
  si no, por la frecuencia maxima de cada nucleo. Los nucleos pequenos frenan a llama.cpp, asi que se empieza
  con los rapidos.
* **Hilos medidos**: prueba unos pocos numeros de hilos con las respuestas reales (velocidad de generacion de
  llama-server) y se queda con el mas rapido. Lo guarda por modelo, variante de llama.cpp y version de la app,
  asi que vuelve a medir al cambiar cualquiera de ellos, y repasa de vez en cuando.
* **Temperatura y bateria**: la app escribe el estado termico, la bateria y el ahorro de energia en
  ``device_state.json``. Con el movil caliente o la bateria baja usa menos hilos, y los recupera despues.
* **Memoria**: si Android se queda sin RAM libre, reduce el contexto para el siguiente arranque de llama-server.
* **NPU, GPU o CPU**: si el movil tiene una NPU (Hexagon) o una GPU que llama.cpp puede usar
  (``nexus.hardware.android_accel``), mide unas respuestas con ella y otras con la CPU y se queda con lo mas rapido;
  despues ajusta los hilos. Si la NPU no arranca, se mide con la GPU.
* **Modelo**: si el modelo va muy lento propone uno mas pequeno; si sobra velocidad y RAM, uno mayor
  (``advice``, que la app ofrece al abrirse).

Los cambios se aplican entre respuestas: llama-server se reinicia con los nuevos valores en la siguiente.
"""
from __future__ import annotations

import json
import os
import statistics
import threading
import time
from pathlib import Path

# CPU part de los nucleos pequenos de ARM (Cortex-A32/A35/A53/A55/A510/A520): el resto son grandes o principales
LITTLE_PARTS = {"0xd01", "0xd03", "0xd04", "0xd05", "0xd46", "0xd80"}
MIN_SAMPLES = 3          # respuestas por numero de hilos antes de decidir
MIN_TOKENS = 16          # respuestas mas cortas no dan una velocidad fiable
REEXPLORE_AFTER = 80     # respuestas con el mejor numero de hilos antes de volver a medir los demas
SLOW_TPS, FAST_TPS = 4.0, 14.0
ANDROID_MODELS = ("qwen3-vl-2b-thinking", "qwen3-vl-4b-thinking")   # de menor a mayor (vision + razonamiento)
STATE_MAX_AGE = 300      # segundos: un estado del movil mas viejo no se tiene en cuenta


# ------------------------------------------------------------------ deteccion de nucleos
def core_layout(cpuinfo: str = "/proc/cpuinfo", sys_cpu: str = "/sys/devices/system/cpu") -> dict:
    """{"total", "fast", "source"}: cuantos nucleos hay y cuantos son rapidos."""
    total = os.cpu_count() or 4
    parts: list[str] = []
    try:
        for line in Path(cpuinfo).read_text(errors="replace").splitlines():
            if line.lower().startswith("cpu part"):
                parts.append(line.split(":", 1)[1].strip().lower())
    except OSError:
        pass
    if len(parts) >= 2:
        fast = sum(1 for p in parts if p not in LITTLE_PARTS)
        if fast:
            return {"total": len(parts), "fast": fast, "source": "cpuinfo"}
    freqs: list[int] = []
    for d in sorted(Path(sys_cpu).glob("cpu[0-9]*")):
        try:
            freqs.append(int((d / "cpufreq" / "cpuinfo_max_freq").read_text().strip()))
        except (OSError, ValueError):
            pass
    if len(freqs) >= 2:
        low = min(freqs)
        fast = sum(1 for f in freqs if f > low)
        if fast >= 2:   # un solo nucleo por encima del resto no basta: se usan tambien los del grupo siguiente
            return {"total": len(freqs), "fast": fast, "source": "cpufreq"}
        return {"total": len(freqs), "fast": min(len(freqs), 4), "source": "cpufreq"}
    return {"total": total, "fast": min(total, 4), "source": "none"}


def thread_candidates(layout: dict) -> list[int]:
    fast, total = max(1, layout["fast"]), max(1, layout["total"])
    return sorted({max(2, fast - 1) if fast > 2 else fast, fast, min(total, fast + 2, 8)})


# ------------------------------------------------------------------ estado del movil
def device_state(path: Path, now: float | None = None) -> dict:
    try:
        st = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if (now or time.time()) - st.get("t", 0) / (1000 if st.get("t", 0) > 1e11 else 1) > STATE_MAX_AGE:
        return {}
    return st


def throttle(threads: int, st: dict) -> tuple[int, str]:
    """Hilos permitidos segun temperatura y bateria (estado termico de Android: 0 nada ... 6 apagado)."""
    thermal = int(st.get("thermal", 0) or 0)
    battery = int(st.get("battery", 100) if st.get("battery") is not None else 100)
    if thermal >= 3:
        return max(1, (threads + 1) // 2), f"movil caliente (estado termico {thermal})"
    if st.get("power_save") or (battery <= 15 and not st.get("charging")):
        return max(1, (threads + 1) // 2), "ahorro de energia" if st.get("power_save") else f"bateria al {battery}%"
    if thermal == 2:
        return max(1, threads - 1), "movil templado"
    return threads, ""


# ------------------------------------------------------------------ eleccion de hilos y de modelo
def gen_tps(call: dict) -> float:
    gen = call.get("seconds", 0) - call.get("ttft", 0)
    return call["tokens"] / gen if gen > 0.05 else call.get("tps", 0.0)


def choose(cands: list[int], trials: dict[str, list[float]]) -> tuple[int, bool]:
    """(hilos, explorando): primero los candidatos sin medir bastante, despues el de mejor mediana."""
    for c in cands:
        if len(trials.get(str(c), [])) < MIN_SAMPLES:
            return c, True
    return max(cands, key=lambda c: statistics.median(trials[str(c)])), False


def model_advice(model: str, best_tps: float, ram_gb: float, catalog: list[dict]) -> dict | None:
    from .models.recommended import LEGACY_LLM
    by_id = {r["id"]: r for r in catalog}
    if model in LEGACY_LLM and best_tps > 0:   # modelo sin razonamiento de una version anterior
        target = LEGACY_LLM[model] if LEGACY_LLM[model] in ANDROID_MODELS else ANDROID_MODELS[0]
        if best_tps < SLOW_TPS:
            target = ANDROID_MODELS[0]
        r = by_id.get(target, {"id": target})
        return {"from": model, "id": target, "name": r.get("name", target), "size_mb": r.get("size_mb", 0),
                "installed": bool(r.get("installed")),
                "reason": "tu modelo actual no razona: este ve imagenes y razona paso a paso antes de responder"}
    if model not in ANDROID_MODELS or best_tps <= 0:
        return None
    i = ANDROID_MODELS.index(model)
    if best_tps < SLOW_TPS and i > 0:
        target, why = ANDROID_MODELS[i - 1], f"va a {best_tps:.1f} tokens/s: uno mas pequeno respondera mas rapido"
    elif best_tps > FAST_TPS and i + 1 < len(ANDROID_MODELS) and \
            ram_gb >= by_id.get(ANDROID_MODELS[i + 1], {}).get("min_ram_gb", 99):
        target, why = ANDROID_MODELS[i + 1], f"va a {best_tps:.1f} tokens/s y hay RAM: uno mayor dara mejores respuestas"
    else:
        return None
    r = by_id.get(target, {"id": target})
    return {"from": model, "id": target, "name": r.get("name", target), "size_mb": r.get("size_mb", 0),
            "installed": bool(r.get("installed")), "reason": why}


class LiveTuner:
    """Hilo que revisa el rendimiento cada ``interval`` segundos y ajusta el perfil de llama.cpp en vivo."""

    def __init__(self, services, interval: float = 15.0, layout: dict | None = None) -> None:
        from . import paths
        self.s = services
        self.interval = interval
        self.layout = layout or core_layout()
        self.cands = thread_candidates(self.layout)
        self.file = paths.data_dir() / "android_tune.json"
        self.state_file = Path(os.environ.get("NEXUS_DEVICE_STATE", paths.data_dir().parent / "device_state.json"))
        self.state = self._load()
        self.gpu_device = services.profile["llm"].get("device", "") if services.profile["llm"].get("gpu_layers") else ""
        self._seen = 0
        self._applied = 0      # hilos sin limitar que se aplicaron por ultima vez (los de las respuestas medidas)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.last_reason = ""

    # ---------------------------------------------------------------- estado persistente
    def _load(self) -> dict:
        try:
            return json.loads(self.file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def _save(self) -> None:
        self.state["log"] = self.state.get("log", [])[-30:]
        tmp = self.file.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.state, indent=1, ensure_ascii=False), encoding="utf-8")
        tmp.replace(self.file)

    def _variant(self) -> str:
        """llama-server en uso: la GPU (con su dispositivo) o la variante de CPU."""
        llm = self.s.profile["llm"]
        if llm.get("gpu_layers") and llm.get("device") and os.environ.get("NEXUS_RUNTIME_LLAMA_GPU"):
            return f"gpu:{llm['device']}"
        return Path(os.environ.get("NEXUS_RUNTIME_LLAMA_CPU") or os.environ.get("NEXUS_RUNTIME_LLAMA", "")).name

    def _key(self) -> str:
        from . import __version__
        model = self.s.models.active().get("llm") or ""
        return f"{model}|{self._variant()}|{__version__}|{self.cands}"

    # ---------------------------------------------------------------- GPU o CPU
    def _set_gpu(self, on: bool) -> bool:
        """Cambia entre GPU y CPU (llama-server se reinicia en la siguiente respuesta). Devuelve si cambio."""
        llm = self.s.profile["llm"]
        want = (-1, self.gpu_device) if on else (0, "")
        if (llm.get("gpu_layers", 0), llm.get("device", "")) == want:
            return False
        llm["gpu_layers"], llm["device"] = want
        if isinstance(getattr(self.s.monitor, "settings", None), dict):
            self.s.monitor.settings["gpu_layers"] = want[0]
        return True

    def accel_step(self, new: list[dict]) -> bool:
        """Mide unas respuestas con la GPU y otras con la CPU y se queda con lo mas rapido (hay GPU que en
        llama.cpp van peor que los nucleos grandes). Devuelve True mientras esta midiendo."""
        from . import __version__
        llm = self.s.profile["llm"]
        if llm.get("gpu_layers") and llm.get("device") and llm["device"] != self.gpu_device:
            self.gpu_device = llm["device"]   # la NPU fallo al arrancar y llamaserver paso a la GPU: se mide esta
        if not self.gpu_device:
            return False
        model = self.s.models.active().get("llm") or ""
        key = f"{model}|{self.gpu_device}|{__version__}"
        acc = self.state.get("accel") or {}
        if acc.get("key") != key:
            acc = {"key": key, "gpu": [], "cpu": [], "choice": None}
        llm = self.s.profile["llm"]
        if not llm.get("device") and llm.get("gpu_layers", 0) == 0 and acc.get("mode") == "gpu":
            acc["choice"] = "cpu"     # la GPU fallo al arrancar (android_accel.mark_gpu_broken)
            self._note("La GPU fallo con llama.cpp en este movil: el modelo sigue en la CPU")
        if acc["choice"] is None:
            mode = acc.get("mode")
            if mode and new and not self.state.get("throttled"):
                acc[mode].extend(round(gen_tps(c), 2) for c in new)
            if len(acc["gpu"]) < MIN_SAMPLES:
                mode = "gpu"
            elif len(acc["cpu"]) < MIN_SAMPLES:
                mode = "cpu"
            else:
                g, c = statistics.median(acc["gpu"]), statistics.median(acc["cpu"])
                acc["choice"] = "gpu" if g >= c else "cpu"
                self._note(f"{'GPU' if g >= c else 'CPU'} mas rapida: {max(g, c):.1f} tokens/s "
                           f"(GPU {g:.1f}, CPU {c:.1f})")
        if acc["choice"] is not None:
            mode = acc["choice"]
        if getattr(self.s.llm, "inflight", 0) == 0 and self._set_gpu(mode == "gpu") and acc["choice"] is None:
            self._note(f"Midiendo la velocidad con la {'GPU (' + self.gpu_device + ')' if mode == 'gpu' else 'CPU'}")
        acc["mode"] = mode
        self.state["accel"] = acc
        return acc["choice"] is None

    def _note(self, msg: str) -> None:
        from . import log
        self.last_reason = msg
        self.state.setdefault("log", []).append({"t": round(time.time()), "msg": msg})
        log.get("android").info("Autoajuste: %s", msg)
        reasons = getattr(self.s, "tuning_reasons", None)
        if isinstance(reasons, list):
            reasons.append(msg)
            del reasons[:-12]

    # ---------------------------------------------------------------- ciclo
    def step(self) -> dict:
        llm = self.s.profile["llm"]
        calls = list(getattr(self.s.monitor, "llm_calls", []))
        new = [c for c in calls[self._seen:] if c.get("backend") in ("llamaserver", "llamacpp")
               and c.get("tokens", 0) >= MIN_TOKENS]
        self._seen = len(calls)

        # 0) GPU o CPU: mientras se comparan, los hilos no se tocan
        if self.accel_step(new):
            self._applied = 0
            self._save()
            return self.report()

        key = self._key()
        if self.state.get("key") != key:
            if self.state.get("key"):
                self._note("Modelo, variante de llama.cpp o version nuevos: se vuelven a medir los hilos")
            self.state = {"key": key, "trials": {}, "since_best": 0, "log": self.state.get("log", []),
                          "pending_switch": self.state.get("pending_switch"), "accel": self.state.get("accel")}
        trials: dict[str, list[float]] = self.state.setdefault("trials", {})
        st = device_state(self.state_file)

        # 1) velocidades de las respuestas nuevas con los hilos que habia (si no estaban limitados)
        if new and self._applied and not self.state.get("throttled"):
            lst = trials.setdefault(str(self._applied), [])
            lst.extend(round(gen_tps(c), 2) for c in new)
            del lst[:-8]
            self.state["since_best"] = self.state.get("since_best", 0) + len(new)

        # 2) hilos: explorar, despues el mejor; repasar los demas de vez en cuando
        target, exploring = choose(self.cands, trials)
        if not exploring and self.state.get("since_best", 0) >= REEXPLORE_AFTER:
            for c in self.cands:
                if c != target:
                    trials.pop(str(c), None)
            self.state["since_best"] = 0
            self._note(f"Repaso periodico: se vuelve a medir con {', '.join(str(c) for c in self.cands if c != target)} hilos")
            target, exploring = choose(self.cands, trials)
        if not exploring and self.state.get("best") != target:
            self._note(f"Mas rapido con {target} hilos ({statistics.median(trials[str(target)]):.1f} tokens/s)")
            self.state["best"] = target

        # 3) temperatura y bateria
        limited, why = throttle(target, st)
        if bool(why) != bool(self.state.get("throttled")):
            self._note(f"Menos hilos ({limited}) por {why}" if why else f"Vuelve a {target} hilos: movil en condiciones normales")
        self.state["throttled"] = why

        # 4) memoria libre de Android: menos contexto para el siguiente arranque
        try:
            import psutil
            avail = psutil.virtual_memory().available / 2**30
        except Exception:  # noqa: BLE001
            avail = 99.0
        if avail < 0.6 and llm["n_ctx"] > 2048:
            llm["n_ctx"] = max(2048, llm["n_ctx"] // 2)
            self._note(f"Poca RAM libre ({avail:.1f} GB): contexto reducido a {llm['n_ctx']}")

        # 5) aplicar entre respuestas (llama-server se reinicia en la siguiente con los nuevos valores)
        if llm["n_threads"] != limited and getattr(self.s.llm, "inflight", 0) == 0:
            if exploring and not why:
                self._note(f"Midiendo la velocidad con {limited} hilos")
            llm["n_threads"] = limited
            if isinstance(getattr(self.s.monitor, "settings", None), dict):
                self.s.monitor.settings["n_threads"] = limited
        if llm["n_threads"] == limited:
            self._applied = target

        # 6) modelo: cambiar al aceptado cuando termine de descargarse; proponer otro si va lento o sobra velocidad
        pending = self.state.get("pending_switch")
        if pending and self.s.models.catalog.get(pending) and getattr(self.s.llm, "inflight", 0) == 0:
            self._activate(pending)
        self.state["advice"] = None
        if not exploring and trials.get(str(target)):
            self.state["advice"] = model_advice(self.s.models.active().get("llm") or "",
                                                statistics.median(trials[str(target)]),
                                                self.s.profile.get("ram_gb", 0), self.s.models.recommended())
        self._save()
        return self.report()

    def _activate(self, mid: str) -> None:
        self.s.models.activate(mid)
        self.s.llm.rebuild()
        self.state["pending_switch"] = None
        self._note(f"Modelo cambiado a {mid}")

    def switch_model(self, mid: str) -> dict:
        """Acepta el consejo: activa el modelo si ya esta, o lo descarga y lo activa al terminar."""
        if mid not in ANDROID_MODELS:
            raise ValueError(f"Modelo no previsto para Android: {mid}")
        if self.s.models.catalog.get(mid):
            self._activate(mid)
            self._save()
            return {"activated": mid}
        self.state["pending_switch"] = mid
        self._save()
        return {"job": self.s.models.download_recommended(mid)}

    def report(self) -> dict:
        trials = self.state.get("trials", {})
        return {"layout": self.layout, "candidates": self.cands, "n_threads": self.s.profile["llm"]["n_threads"],
                "n_ctx": self.s.profile["llm"]["n_ctx"], "best": self.state.get("best"),
                "exploring": choose(self.cands, trials)[1],
                "tps": {k: round(statistics.median(v), 2) for k, v in trials.items() if v},
                "throttled": self.state.get("throttled") or "", "device": device_state(self.state_file),
                "llama_variant": self._variant(), "gpu": self.gpu_device,
                "accel": {k: v for k, v in (self.state.get("accel") or {}).items() if k != "key"},
                "advice": self.state.get("advice"), "pending_switch": self.state.get("pending_switch"), "last": self.last_reason, "log": self.state.get("log", [])[-10:]}

    def _loop(self) -> None:
        while not self._stop.wait(self.interval):
            try:
                self.step()
            except Exception as e:  # noqa: BLE001
                from . import log
                log.get("android").warning("Autoajuste: %s", e)

    def start(self) -> None:
        try:
            self.step()   # aplica ya lo aprendido en sesiones anteriores, antes de la primera respuesta
        except Exception:  # noqa: BLE001
            pass
        self._thread = threading.Thread(target=self._loop, name="nexus-autoajuste", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()


# ------------------------------------------------------------------ optimizacion tras cada arranque
def _clean_tmp(max_age_s: float = 86400) -> int:
    """Borra temporales viejos (importaciones a medias, descargas cortadas...). Devuelve cuantos."""
    tmp = Path(os.environ.get("TMPDIR", ""))
    if not tmp.is_dir():
        return 0
    n, limit = 0, time.time() - max_age_s
    for f in tmp.rglob("*"):
        try:
            if f.is_file() and f.stat().st_mtime < limit:
                f.unlink()
                n += 1
        except OSError:
            pass
    return n


def _trim_logs(max_bytes: int = 4 << 20) -> None:
    from . import paths
    for f in paths.logs_dir().glob("*.log"):
        try:
            if f.stat().st_size > max_bytes:
                data = f.read_bytes()[-max_bytes // 4:]
                f.write_bytes(data)
        except OSError:
            pass


def warmup_ok(st: dict, avail_gb: float, model_gb: float) -> tuple[bool, str]:
    """Precargar el modelo solo si no gasta de mas: sin calor, con bateria (o cargando) y con RAM libre."""
    if int(st.get("thermal", 0) or 0) >= 2:
        return False, "movil caliente"
    battery = st.get("battery")
    if battery is not None and int(battery) < 30 and not st.get("charging"):
        return False, f"bateria al {battery}%"
    if st.get("power_save"):
        return False, "ahorro de energia"
    if avail_gb < model_gb + 0.5:
        return False, f"poca RAM libre ({avail_gb:.1f} GB)"
    return True, ""


def after_start(services, tuner: "LiveTuner | None" = None) -> list[str]:
    """Se ejecuta tras cada arranque, con Nexus-Office ya respondiendo:
    limpia temporales y registros, optimiza la base de datos, aplica lo aprendido (hilos) y precarga el modelo
    para que la primera respuesta no espere a cargarlo. Devuelve lo hecho."""
    from . import log
    L = log.get("android")
    done: list[str] = []
    t0 = time.time()
    n = _clean_tmp()
    if n:
        done.append(f"{n} temporales borrados")
    _trim_logs()
    try:
        services.db.execute("PRAGMA optimize")
        services.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        done.append("base de datos optimizada")
    except Exception as e:  # noqa: BLE001
        L.debug("PRAGMA optimize: %s", e)
    if tuner is not None:
        r = tuner.step()
        done.append(f"{r['n_threads']} hilos" + (f" (limitado: {r['throttled']})" if r["throttled"] else "")
                    + f", contexto {r['n_ctx']}, {r['llama_variant'] or 'sin llama.cpp'}")
    try:
        backend, model = services.llm.select()
    except Exception:  # noqa: BLE001
        backend, model = "", ""
    if backend == "llamaserver":
        try:
            import psutil
            avail = psutil.virtual_memory().available / 2**30
        except Exception:  # noqa: BLE001
            avail = 99.0
        resolved = services.llm.backends["llamaserver"].resolve(model)
        size = Path(resolved[0]).stat().st_size / 2**30 if resolved else 0.0
        ok, why = warmup_ok(device_state(tuner.state_file) if tuner else {}, avail, size)
        if ok:
            t1 = time.time()
            services.llm.inflight += 1     # el autoajuste no cambia hilos mientras tanto
            try:
                import httpx
                ls = services.llm.backends["llamaserver"]
                ls.ensure(model, services.llm.options())
                # una peticion de 1 token lee los pesos del disco (mmap): la primera respuesta real no lo paga
                httpx.post(ls.base_url.rsplit("/v1", 1)[0] + "/completion", json={"prompt": "Hola", "n_predict": 1},
                           timeout=300)
                done.append(f"modelo precargado en {time.time() - t1:.0f} s")
            except Exception as e:  # noqa: BLE001
                done.append(f"no se pudo precargar el modelo: {str(e)[:120]}")
            finally:
                services.llm.inflight -= 1
        else:
            done.append(f"modelo sin precargar ({why})")
    msg = f"Optimizacion tras el arranque ({time.time() - t0:.0f} s): " + "; ".join(done)
    L.info(msg)
    reasons = getattr(services, "tuning_reasons", None)
    if isinstance(reasons, list):
        reasons.append(msg)
    return done
