"""Arranque de Nexus-Office dentro de la app Android autonoma (android-autonomo/).

La APK lleva su propio Python 3.12 para ARM64 (musl) con todas las dependencias, y llama.cpp. El servicio
de la app lo lanza asi:

    libnexusld.so --library-path <runtime>/py/lib:<libs> <runtime>/py/bin/python3.12 \
        -m nexus.android_main <datos> <libs> <puerto>

Nexus-Office corre entero en el movil: servidor web en 127.0.0.1, memoria, RAG, agentes y modelos GGUF locales con
``llama-server`` (tres variantes segun la CPU: ARMv8, con dotprod y con i8mm, y una con GPU por Vulkan/OpenCL que
se usa si ``nexus.hardware.android_accel`` encuentra una GPU que llama.cpp pueda usar). Sin procesos hijo de Python
para el paralelismo: cae a hilos (nexus.parallel).
"""
from __future__ import annotations

import os
import threading
from pathlib import Path

_server = None


def _harden_psutil() -> None:
    """Android limita /proc (stat, cpuinfo...): las lecturas que fallan devuelven un valor neutro en vez de romper."""
    try:
        import psutil
    except ImportError:
        return

    def safe(fn, default):
        def wrapper(*a, **k):
            try:
                return fn(*a, **k)
            except (PermissionError, OSError, NotImplementedError, RuntimeError):
                return default() if callable(default) else default
        return wrapper

    psutil.cpu_percent = safe(psutil.cpu_percent, 0.0)
    if hasattr(psutil, "cpu_freq"):
        psutil.cpu_freq = safe(psutil.cpu_freq, None)
    psutil.net_if_addrs = safe(psutil.net_if_addrs, dict)
    psutil.cpu_count = safe(psutil.cpu_count, lambda: os.cpu_count() or 4)


def cpu_features(cpuinfo: str = "/proc/cpuinfo") -> set[str]:
    try:
        text = Path(cpuinfo).read_text(errors="replace")
    except OSError:
        return set()
    return {f for line in text.splitlines() if line.lower().startswith("features") for f in line.split(":", 1)[1].split()}


def llama_binary(native_lib_dir: str, features: set[str] | None = None) -> Path | None:
    """La variante de llama-server mas rapida que admite esta CPU (i8mm > dotprod > ARMv8 basico)."""
    if not native_lib_dir:
        return None
    feats = cpu_features() if features is None else features
    names = (["libllama-server-i8mm.so"] if "i8mm" in feats and "asimddp" in feats else []) + \
            (["libllama-server-dotprod.so"] if "asimddp" in feats else []) + ["libllama-server.so"]
    return next((Path(native_lib_dir) / n for n in names if (Path(native_lib_dir) / n).is_file()), None)


def _certificates() -> None:
    """El OpenSSL propio no conoce el almacen de Android: se usan los certificados raiz de certifi."""
    if os.environ.get("SSL_CERT_FILE"):
        return
    try:
        import certifi
        os.environ["SSL_CERT_FILE"] = certifi.where()
    except ImportError:
        pass


def prepare(data_dir: str, native_lib_dir: str = "") -> dict:
    """Variables de entorno y configuracion por defecto para el movil. Devuelve lo que se ha detectado."""
    os.environ["NEXUS_DATA_DIR"] = str(Path(data_dir) / "nexus")
    os.environ["NEXUS_ANDROID"] = "1"
    os.environ.setdefault("HOME", data_dir)
    os.environ.setdefault("TMPDIR", str(Path(data_dir) / "tmp"))
    Path(os.environ["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    llama = llama_binary(native_lib_dir)
    if llama:
        os.environ["NEXUS_RUNTIME_LLAMA"] = os.environ["NEXUS_RUNTIME_LLAMA_CPU"] = str(llama)
    gpu = Path(native_lib_dir) / "libllama-server-gpu.so" if native_lib_dir else None
    if gpu and gpu.is_file():   # llama.cpp con Vulkan/OpenCL: lo usa si la sonda de hardware encuentra GPU
        os.environ["NEXUS_RUNTIME_LLAMA_GPU"] = str(gpu)
    if native_lib_dir:
        os.environ["NEXUS_ANDROID_LIBS"] = native_lib_dir
    from . import android_dns
    dns = android_dns.install()
    _certificates()
    _harden_psutil()
    from .config import get_config
    cfg = get_config()
    first = not cfg.get("android.configured", False)
    if first:   # valores pensados para un movil; despues el usuario los cambia en Ajustes
        cfg.set("server.host", "127.0.0.1")
        cfg.set("server.open_browser", False)
        cfg.set("llm.max_tokens", 768)
        cfg.set("memory.llm_graph_extraction", False)
        cfg.set("android.configured", True)
    return {"llama_server": os.environ.get("NEXUS_RUNTIME_LLAMA", ""),
            "llama_server_gpu": os.environ.get("NEXUS_RUNTIME_LLAMA_GPU", ""), "first_run": first,
            "data_dir": os.environ["NEXUS_DATA_DIR"], "own_dns": dns}


def main(data_dir: str, native_lib_dir: str = "", port: int = 8775, embedded: bool = True) -> None:
    """Bloquea mientras el servidor esta en marcha. ``embedded``: llamado desde un hilo (sin manejar senales);
    como proceso propio (``python -m nexus.android_main``), SIGTERM de la app lo para limpiamente."""
    global _server
    import uvicorn

    from . import log
    from .server import create_app
    info = prepare(data_dir, native_lib_dir)
    log.get("android").info("Nexus-Office autonomo en Android: %s", info)
    app = create_app()
    _server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=int(port), log_level="warning",
                                            timeout_graceful_shutdown=3, loop="asyncio", http="h11", ws="auto"))
    if embedded:
        _server.install_signal_handlers = lambda: None   # no es el hilo principal
    _server.run()


def stop() -> None:
    if _server is not None:
        _server.should_exit = True


def start_in_thread(data_dir: str, native_lib_dir: str = "", port: int = 8775) -> threading.Thread:
    """Para probarlo fuera de Android: arranca main() en un hilo."""
    t = threading.Thread(target=main, args=(data_dir, native_lib_dir, port), name="nexus-android", daemon=True)
    t.start()
    return t


if __name__ == "__main__":
    import sys
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "", int(sys.argv[3]) if len(sys.argv) > 3 else 8775,
         embedded=False)
