"""FreeLLMAPI dentro de Agente IA: arrancar la pasarela incluida y usarla como endpoint LLM.

FreeLLMAPI (https://github.com/tashfeenahmed/freellmapi, MIT) es el servidor Node que usa el proyecto «9000 tokens»:
junta los planes gratis de muchos proveedores (Groq, Gemini, Mistral, OpenRouter, Cerebras...) detras de un endpoint
/v1 compatible con OpenAI, elige el mejor modelo disponible («auto») y salta al siguiente cuando uno llega a su
limite.

Los instaladores de escritorio lo llevan en bundled/runtimes/freellmapi (Node.js + servidor + panel web, ver
packaging/fetch_freellmapi.py), asi que se instala sin conexion. Para responder necesita internet y las claves
gratis de los proveedores, que se ponen en su panel web (Keys). Tambien vale una FreeLLMAPI que ya este en marcha
(la app de escritorio en el puerto 31415, Docker u otro equipo de la red): basta con cambiar la URL en Ajustes.

El motor «freellmapi» de nexus/llm/manager.py usa config freellmapi.url / clave / modelo; esta clase arranca y para
la incluida, y copia su clave unica (la genera FreeLLMAPI en su base de datos) a freellmapi.clave.
"""
from __future__ import annotations

import json
import os
import secrets
import sqlite3
import subprocess
import threading
import time
from pathlib import Path
from urllib.parse import urlparse

from . import log, paths

L = log.get("freellmapi")
BUNDLED = paths.ROOT / "bundled" / "runtimes" / "freellmapi"
PUERTO = 3001
ORIGEN = "https://github.com/tashfeenahmed/freellmapi"


def _es_local(host: str) -> bool:
    return host in ("127.0.0.1", "localhost", "::1", "0.0.0.0", "")


class FreeLLMAPI:
    def __init__(self, config, raiz: Path | None = None) -> None:
        self.config = config
        self.raiz = Path(raiz).resolve() if raiz else BUNDLED
        self._proc: subprocess.Popen | None = None
        self._log_f = None
        self._lock = threading.Lock()
        self._error = ""
        self._inicio = 0.0
        self.puerto: int | None = None   # fijo (pruebas); si no, el de freellmapi.url
        self._clave_local = ""           # sin config (pruebas): la clave copiada se guarda aqui
        self.al_cambiar = None           # se llama al copiar la clave (core: actualiza el motor «freellmapi»)

    # ------------------------------------------------------------ configuracion
    def _cfg(self, clave: str, defecto=None):
        return self.config.get(f"freellmapi.{clave}", defecto) if self.config is not None else defecto

    def _set(self, clave: str, valor) -> None:
        if self.config is not None:
            self.config.set(f"freellmapi.{clave}", valor)
        elif clave == "clave":
            self._clave_local = valor
        if clave == "clave" and self.al_cambiar:
            try:
                self.al_cambiar()
            except Exception:  # noqa: BLE001
                pass

    def url(self) -> str:
        if self.config is None:
            return f"http://127.0.0.1:{self.puerto or PUERTO}/v1"
        u = (self._cfg("url", "") or f"http://127.0.0.1:{PUERTO}/v1").rstrip("/")
        return u if u.endswith("/v1") else u + "/v1"

    def panel(self) -> str:
        return self.url()[: -len("/v1")]

    def _puerto(self) -> int:
        if self.puerto:
            return self.puerto
        p = urlparse(self.url())
        return p.port or PUERTO

    def clave(self) -> str:
        return (self._cfg("clave", "") or self._clave_local or "").strip()

    def datos(self) -> Path:
        return paths.sub("freellmapi")

    # ------------------------------------------------------------ la incluida
    def node(self) -> Path | None:
        for rel in ("node/node.exe", "node/bin/node"):
            p = self.raiz / rel
            if p.exists():
                if os.name != "nt" and not os.access(p, os.X_OK):
                    try:
                        p.chmod(0o755)
                    except OSError:
                        pass
                return p
        return None

    def entrada(self) -> Path:
        return self.raiz / "app" / "server" / "dist" / "index.js"

    def instalada(self) -> bool:
        return self.node() is not None and self.entrada().exists()

    def version(self) -> dict:
        try:
            return json.loads((self.raiz / "freellmapi.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def en_marcha(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def _ruta_log(self) -> Path:
        return paths.logs_dir() / "freellmapi.log"

    def arrancar(self) -> dict:
        with self._lock:
            if self.en_marcha():
                return self.estado()
            if not self.instalada():
                raise RuntimeError("FreeLLMAPI no viene en este paquete (solo en los instaladores de escritorio). "
                                   "Usa una FreeLLMAPI que ya tengas en marcha poniendo su URL.")
            host = urlparse(self.url()).hostname or "127.0.0.1"
            if self.config is not None and not _es_local(host):
                raise RuntimeError(f"La URL apunta a otro equipo ({host}): esa FreeLLMAPI se arranca alli. "
                                   f"Para usar la incluida pon http://127.0.0.1:{PUERTO}/v1.")
            cifrado = self._cfg("clave_cifrado", "") or ""
            if len(cifrado) != 64:
                cifrado = secrets.token_hex(32)
                self._set("clave_cifrado", cifrado)
            d = self.datos()
            env = {**os.environ,
                   "NODE_ENV": "production",
                   "HOST": "0.0.0.0" if self._cfg("red", False) else "127.0.0.1",
                   "PORT": str(self._puerto()),
                   "ENCRYPTION_KEY": cifrado,
                   "FREEAPI_ENV_PATH": str(d / ".env"),
                   "FREEAPI_DB_PATH": str(d / "freellmapi.db"),
                   "NODE_NO_WARNINGS": "1"}
            node = self.node()
            env["PATH"] = str(node.parent) + os.pathsep + env.get("PATH", "")
            self._log_f = open(self._ruta_log(), "wb")  # noqa: SIM115 - se cierra al parar
            flags = 0
            if os.name == "nt":
                flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            self._proc = subprocess.Popen([str(node), str(self.entrada())], cwd=str(self.entrada().parent.parent),
                                          env=env, stdout=self._log_f, stderr=subprocess.STDOUT,
                                          stdin=subprocess.DEVNULL, creationflags=flags,
                                          start_new_session=os.name != "nt")
            self._inicio, self._error = time.time(), ""
            L.info("FreeLLMAPI arrancada (pid %s) en %s", self._proc.pid, self.url())
            threading.Thread(target=self._copiar_clave, args=(self._proc,), name="freellmapi-clave",
                             daemon=True).start()
        return self.estado()

    def parar(self) -> dict:
        with self._lock:
            p, self._proc = self._proc, None
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(8)
                except subprocess.TimeoutExpired:
                    p.kill()
            if self._log_f:
                self._log_f.close()
                self._log_f = None
        return self.estado()

    def _leer_clave_bd(self) -> str:
        db = self.datos() / "freellmapi.db"
        if not db.exists():
            return ""
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=2)
            try:
                fila = con.execute("SELECT value FROM settings WHERE key='unified_api_key'").fetchone()
            finally:
                con.close()
        except sqlite3.Error:
            return ""
        return str(fila[0]) if fila and fila[0] else ""

    def _copiar_clave(self, proc) -> None:
        """FreeLLMAPI genera su clave unica al crear la base de datos: se copia a freellmapi.clave."""
        for _ in range(90):
            time.sleep(1.5)
            if proc.poll() is not None:
                self._error = "FreeLLMAPI se ha cerrado: mira el registro"
                return
            k = self._leer_clave_bd()
            if k.startswith("freellmapi-"):
                if k != self.clave():
                    self._set("clave", k)
                    L.info("Clave unica de FreeLLMAPI copiada a Ajustes")
                return

    def sincronizar_clave(self) -> bool:
        k = self._leer_clave_bd()
        if k.startswith("freellmapi-") and k != self.clave():
            self._set("clave", k)
            return True
        return False

    def registro(self, n: int = 60) -> list[str]:
        try:
            lineas = self._ruta_log().read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return []
        return lineas[-n:]

    # ------------------------------------------------------------ estado y pruebas
    def _headers(self) -> dict:
        k = self.clave()
        return {"Authorization": f"Bearer {k}"} if k else {}

    def modelos(self, timeout: float = 4) -> list[str]:
        import httpx  # noqa: PLC0415
        r = httpx.get(f"{self.url()}/models", headers=self._headers(), timeout=timeout)
        r.raise_for_status()
        return [m.get("id", "") for m in (r.json().get("data") or []) if m.get("id")]

    def responde(self) -> bool:
        import httpx  # noqa: PLC0415
        try:
            r = httpx.get(f"{self.panel()}/api/health", timeout=1.5)
            if r.status_code < 500:
                return True
        except httpx.HTTPError:
            pass
        try:
            return httpx.get(f"{self.url()}/models", headers=self._headers(), timeout=1.5).status_code < 500
        except httpx.HTTPError:
            return False

    def probar(self, texto: str = "Responde solo: OK") -> dict:
        """Una peticion corta a /v1/chat/completions con el modelo elegido."""
        import httpx  # noqa: PLC0415
        t0 = time.time()
        body = {"model": self._cfg("modelo", "auto") or "auto", "max_tokens": 32, "stream": False,
                "messages": [{"role": "user", "content": texto}]}
        r = httpx.post(f"{self.url()}/chat/completions", json=body, headers=self._headers(), timeout=60)
        try:
            data = r.json()
        except ValueError:
            data = {}
        if r.status_code >= 400:
            msg = (data.get("error") or {}).get("message") if isinstance(data.get("error"), dict) else data.get("error")
            raise RuntimeError(f"HTTP {r.status_code}: {msg or r.text[:300]}")
        ch = (data.get("choices") or [{}])[0]
        return {"respuesta": ((ch.get("message") or {}).get("content") or "").strip(), "modelo": data.get("model", ""),
                "segundos": round(time.time() - t0, 2)}

    def estado(self) -> dict:
        activo = self.config.get("llm.default_model", "") if self.config is not None else ""
        return {"instalada": self.instalada(), "en_marcha": self.en_marcha(), "responde": self.responde(),
                "pid": self._proc.pid if self.en_marcha() else None, "url": self.url(), "panel": self.panel(),
                "clave_puesta": bool(self.clave()), "modelo": self._cfg("modelo", "auto") or "auto",
                "arrancar": bool(self._cfg("arrancar", False)), "red": bool(self._cfg("red", False)),
                "es_principal": str(activo).startswith("freellmapi:"), "version": self.version(),
                "error": self._error, "origen": ORIGEN}

    def al_iniciar(self) -> None:
        """Arranque automatico si se pidio en Ajustes (freellmapi.arrancar)."""
        if not self._cfg("arrancar", False) or not self.instalada():
            return
        try:
            if not self.responde():
                self.arrancar()
            else:
                self.sincronizar_clave()
        except Exception as e:  # noqa: BLE001
            self._error = str(e)
            L.warning("No se pudo arrancar FreeLLMAPI: %s", e)
