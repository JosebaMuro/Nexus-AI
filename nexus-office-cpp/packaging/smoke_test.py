"""Prueba de humo de un Nexus-Office C++ ya instalado desde su instalador: con modelos reales descargados de internet,
comprueba el selector de motor y acelerador, el chat por WebSocket, la voz (Piper y Whisper), la imagen
(stable-diffusion.cpp) y el servicio del sistema.

    python packaging/smoke_test.py <carpeta instalada> [--exe ruta/al/nexus-office] [--modelo qwen3.5-0.8b]
                                   [--sin-imagen] [--sin-servicio] [--acelerador auto|cpu|gpu|npu]

Solo usa la biblioteca estandar (y websockets si esta, para el chat). Sale con error si algo falla.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path

PORT = 8799


def call(path: str, body: dict | None = None, timeout: float = 900, raw: bool = False):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method="POST" if body is not None else "GET",
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
        return (r.status, dict(r.headers), data) if raw else json.loads(data or b"null")


def upload(path: str, field: str, name: str, content: bytes, timeout: float = 600) -> dict:
    b = uuid.uuid4().hex
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"{field}\"; filename=\"{name}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + content + f"\r\n--{b}--\r\n".encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=body, method="POST",
                                 headers={"Content-Type": f"multipart/form-data; boundary={b}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


async def ws_chat(text: str) -> str:
    import websockets
    async with websockets.connect(f"ws://127.0.0.1:{PORT}/ws", max_size=None) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "chat", "text": text, "agent": "auto"}))
        out = ""
        while True:
            e = json.loads(await asyncio.wait_for(ws.recv(), 600))
            if e.get("type") == "token":
                out += e.get("text", "")
            if e.get("type") in ("done", "error"):
                return out or e.get("text", "") or e.get("error", "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("home", type=Path)
    ap.add_argument("--exe", default="")
    ap.add_argument("--modelo", default="qwen3.5-0.8b")
    ap.add_argument("--sin-imagen", action="store_true")
    ap.add_argument("--sin-servicio", action="store_true")
    ap.add_argument("--acelerador", default="auto", help="acelerador para el chat (cpu en maquinas virtuales con GPU emulada)")
    a = ap.parse_args()
    home = a.home.resolve()
    exe = a.exe or str(next(p for p in (home / "nexus-office.exe", home / "nexus-office") if p.exists()))
    data = Path(os.environ.get("NEXUS_DATA_DIR") or tempfile.mkdtemp(prefix="nexus-humo-"))
    env = {**os.environ, "NEXUS_DATA_DIR": str(data)}
    fails: list[str] = []

    def check(ok, what: str) -> bool:
        print(("OK    " if ok else "FALLO ") + what, flush=True)
        if not ok:
            fails.append(what)
        return bool(ok)

    def cli(*args: str, timeout: float = 3600) -> subprocess.CompletedProcess:
        r = subprocess.run([exe, *args], cwd=home, env=env, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
        print(f"$ nexus-office {' '.join(args)} -> {r.returncode}\n{r.stdout[-1500:]}{r.stderr[-1500:]}", flush=True)
        return r

    check(cli("--version").returncode == 0, "el programa arranca (--version)")
    check(cli("detect").returncode == 0, "deteccion del hardware")
    r = cli("pull", a.modelo)
    check(r.returncode == 0 and "completado" in r.stdout, f"modelo {a.modelo} descargado")
    voice = cli("pull", "piper-es-davefx")
    check(voice.returncode == 0, "voz de Piper descargada")
    if not a.sin_imagen:
        check(cli("pull", "sdxs-512").returncode == 0, "modelo de imagen SDXS-512 descargado")

    log = open(data / "servidor.log", "w", encoding="utf-8")
    srv = subprocess.Popen([exe, "run", "--no-browser", "--port", str(PORT)], cwd=home, env=env, stdout=log,
                           stderr=subprocess.STDOUT)
    try:
        opts = None
        for _ in range(120):
            try:
                opts = call("/api/accel", timeout=5)
                break
            except OSError:
                if srv.poll() is not None:
                    break
                time.sleep(1)
        if not check(bool(opts), "servidor arrancado"):
            return 1
        rt = call("/api/runtimes")
        print(json.dumps(rt["bundled"]), flush=True)
        check(bool(rt["bundled"].get("llama")), f"llama-server incluido: {rt['bundled'].get('llama')}")
        check(bool(rt["bundled"].get("sd")) or a.sin_imagen, f"sd-cli incluido: {rt['bundled'].get('sd')}")
        engines = {e["id"]: e for e in opts["engines"]}
        check(engines["llamaserver"]["available"], f"selector: llama-server disponible ({engines['llamaserver']['note']})")
        res = call("/api/accel", {"engine": "llamaserver", "accel": "cpu", "remember": True})
        t = call("/api/accel/test", {})
        print(json.dumps({k: t.get(k) for k in ("ok", "backend", "text", "seconds", "tokens_s", "error", "effective")},
                         ensure_ascii=False), flush=True)
        check(t.get("ok") and t.get("backend") == "llamaserver" and bool(t.get("text")),
              f"llama-server en CPU genera texto ({t.get('tokens_s')} tok/s): {res.get('note') or 'sin avisos'}")
        for acc in ("gpu", "npu"):     # sin GPU ni NPU en el equipo de pruebas: debe decirlo y seguir funcionando
            res = call("/api/accel", {"engine": "auto", "accel": acc, "remember": True})
            t = call("/api/accel/test", {})
            check(t.get("ok"), f"{acc} pedida en un equipo sin {acc.upper()}: responde igual ({res.get('note')}; "
                               f"{t.get('backend')}: {t.get('error') or t.get('text')})")
        call("/api/accel", {"engine": "auto", "accel": a.acelerador, "remember": True})

        try:
            answer = asyncio.run(ws_chat("Responde con una sola palabra: ¿cual es la capital de Francia?"))
            # Un modelo de 0.8B puede equivocarse de capital: lo que se comprueba es que genera texto coherente
            # (palabras reales, no simbolos sueltos como los que salen con un acelerador roto).
            limpio = re.sub(r"[*_.,:;¡!¿?\"'()\-]", "", answer).strip()
            letras = sum(ch.isalpha() or ch.isspace() for ch in limpio)
            coherente = bool(re.search(r"[a-záéíóúñ]{3,}", limpio.lower())) and letras >= 0.8 * len(limpio)
            if coherente and not re.search(r"par[ií]s", answer.lower()):
                print(f"AVISO el modelo responde con texto coherente pero no acierta: {answer[:120]!r}")
            check(coherente, f"chat por WebSocket con el modelo: {answer[:120]!r}")
            calc = asyncio.run(ws_chat("Cuanto es 12*7?"))
            check("84" in calc, f"chat con herramienta (calculadora): {calc[:120]!r}")
        except ImportError:
            print("(sin el paquete websockets: no se prueba el chat por WebSocket)")

        status, headers, audio = call("/api/voice/tts", {"text": "Hola, soy Nexus-Office."}, raw=True)
        engine = headers.get("X-TTS-Engine") or headers.get("x-tts-engine")
        check(status == 200 and engine == "piper" and audio[:4] == b"RIFF",
              f"voz con Piper: {status} {engine} {len(audio)} bytes")
        if status == 200 and audio[:4] == b"RIFF":
            stt = upload("/api/voice/stt", "file", "voz.wav", audio)
            print(json.dumps(stt, ensure_ascii=False)[:300], flush=True)
            check(bool((stt.get("text") or "").strip()) and not stt.get("error"),
                  f"dictado con Whisper: {stt.get('text') or stt.get('error')!r}")

        if not a.sin_imagen:
            img = call("/api/image/generate", {"prompt": "un faro al atardecer", "seed": 3, "steps": 1}, timeout=1800)
            print(json.dumps({k: img.get(k) for k in ("engine", "url", "note", "error")}, ensure_ascii=False), flush=True)
            check(img.get("engine") not in (None, "procedural") and img.get("url"),
                  f"imagen con stable-diffusion.cpp: {img.get('engine')} {img.get('note') or ''}")
    finally:
        srv.terminate()
        try:
            srv.wait(30)
        except subprocess.TimeoutExpired:
            srv.kill()
        log.close()
        print((data / "servidor.log").read_text(encoding="utf-8", errors="replace")[-4000:])

    if not a.sin_servicio:
        r = cli("servicio", "instalar", "--puerto", "8798")
        if check(r.returncode == 0, "servicio del sistema instalado"):
            up = False
            for _ in range(60):
                try:
                    with urllib.request.urlopen("http://127.0.0.1:8798/api/ping", timeout=3):
                        up = True
                        break
                except OSError:
                    time.sleep(2)
            print("servicio respondiendo en 8798" if up else "el servicio no responde (el sistema de pruebas puede no "
                                                             "arrancarlo sin sesion)", flush=True)
            cli("servicio", "estado")
            check(cli("servicio", "quitar").returncode == 0, "servicio quitado")
    print(f"\n{len(fails)} fallos" + (": " + "; ".join(fails) if fails else ""))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
