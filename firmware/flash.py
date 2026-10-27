"""Graba el firmware de los dispositivos de Nexus-Office y les pone la Wi-Fi y la direccion de Nexus.

    python firmware/flash.py puertos                       # que hay conectado
    python firmware/flash.py luces  [--puerto COM5] [--modelo esp32s3] [--wifi RED CLAVE] [--leds 60 --pin 16]
    python firmware/flash.py reloj  [--puerto /dev/ttyACM0] [--wifi RED CLAVE] [--nexus 192.168.1.10:8775 CLAVE]
    python firmware/flash.py pico   [--wifi RED CLAVE] [--nexus 192.168.1.10:8775 CLAVE]
    python firmware/flash.py config reloj|luces|pico ...   # solo la configuracion, sin regrabar

* luces: NexusLED en cualquier ESP32 (ESP32, S2, S3, C3, C6). El modelo se detecta solo.
* reloj: NexusWatch en la ESP32-2424S012C (ESP32-C3 con pantalla redonda).
* pico : NexusPico en el Pico W con la pantalla de tinta del pwnagotchi. Conecta el Pico manteniendo pulsado
  BOOTSEL: aparece la unidad RPI-RP2 y se copia el .uf2; despues se le graba config.json por el puerto serie.

Funciona sin Internet: los binarios estan en firmware/bin. Necesita ``esptool`` y ``pyserial`` (vienen con
Nexus-Office). Nexus lo usa desde Dispositivos > Grabar firmware (``nexus/devices/firmware.py``).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

AQUI = Path(__file__).resolve().parent
BIN = AQUI / "bin"
MODELOS_LUCES = ("esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6")
# fabricantes USB: Espressif nativo, CP210x, CH340/CH9102, FTDI, Raspberry Pi (Pico)
VID_ESP = {0x303A, 0x10C4, 0x1A86, 0x0403}
VID_PICO = 0x2E8A


def binario(tipo: str, modelo: str = "") -> Path:
    if tipo == "luces":
        return BIN / f"nexusled-{modelo or 'esp32'}.bin"
    if tipo == "reloj":
        return BIN / "nexuswatch-esp32c3.bin"
    if tipo == "pico":
        return BIN / "nexuspico-picow.uf2"
    raise ValueError(f"tipo desconocido: {tipo}")


def binarios() -> list[dict]:
    """Los firmwares disponibles (con su version y huella si se compilaron en CI)."""
    info = {}
    try:
        info = json.loads((BIN / "firmware.json").read_text(encoding="utf-8")).get("archivos", {})
    except (OSError, ValueError):
        pass
    out = []
    for p in sorted(BIN.glob("*.bin")) + sorted(BIN.glob("*.uf2")):
        out.append({"archivo": p.name, "bytes": p.stat().st_size, **info.get(p.name, {})})
    return out


def puertos() -> list[dict]:
    """Puertos serie con su tipo probable, y unidades RPI-RP2 (Pico en modo BOOTSEL)."""
    out = []
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            tipo = "pico" if p.vid == VID_PICO else "esp32" if p.vid in VID_ESP else ""
            out.append({"puerto": p.device, "descripcion": p.description or "", "tipo": tipo,
                        "vid": f"{p.vid:04x}" if p.vid else "", "pid": f"{p.pid:04x}" if p.pid else ""})
    except ImportError:
        pass
    for d in unidades_pico():
        out.append({"puerto": str(d), "descripcion": "Pico en modo BOOTSEL (RPI-RP2)", "tipo": "pico-bootsel"})
    return out


def unidades_pico() -> list[Path]:
    cand: list[Path] = []
    if os.name == "nt":
        cand = [Path(f"{c}:/") for c in "DEFGHIJKLMNOPQRSTUVWXYZ"]
    else:
        user = os.environ.get("USER", "")
        for base in ("/media", f"/media/{user}", f"/run/media/{user}", "/Volumes", "/mnt"):
            b = Path(base)
            if b.is_dir():
                cand += [p for p in b.iterdir() if p.is_dir()]
    return [p for p in cand if (p / "INFO_UF2.TXT").exists()]


def _esptool(args: list[str], log=print) -> str:
    cmd = [sys.executable, "-m", "esptool", *args]
    log("$ esptool " + " ".join(args))
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    salida = []
    for linea in p.stdout:   # type: ignore[union-attr]
        linea = linea.rstrip()
        salida.append(linea)
        if linea:
            log(linea)
    if p.wait() != 0:
        raise RuntimeError("esptool fallo:\n" + "\n".join(salida[-8:]))
    return "\n".join(salida)


def detectar_chip(puerto: str, log=print) -> str:
    out = _esptool(["--port", puerto, "chip_id"], log)
    m = re.search(r"(?:Chip is|Chip type:)\s*(ESP32(?:-[A-Z0-9]+)?)", out)
    if not m:
        raise RuntimeError("no se reconoce el chip")
    chip = m.group(1).lower().replace("-", "")
    # el ESP32 clasico aparece como ESP32-D0WD-V3, ESP32-PICO-D4...
    return chip if chip in ("esp32s2", "esp32s3", "esp32c2", "esp32c3", "esp32c6", "esp32h2", "esp32p4") else "esp32"


def puerto_auto(tipo: str) -> str:
    for p in puertos():
        if p["tipo"] == tipo:
            return p["puerto"]
    raise RuntimeError("no encuentro el dispositivo: conectalo por USB (o indica --puerto)")


def flashear_esp32(tipo: str, puerto: str = "", modelo: str = "", log=print) -> dict:
    puerto = puerto or puerto_auto("esp32")
    chip = detectar_chip(puerto, log)
    if tipo == "reloj" and chip != "esp32c3":
        raise RuntimeError(f"el reloj ESP32-2424S012C lleva un ESP32-C3 y este es un {chip}")
    modelo = modelo or chip
    if tipo == "luces" and modelo not in MODELOS_LUCES:
        raise RuntimeError(f"no hay NexusLED para {modelo} (hay: {', '.join(MODELOS_LUCES)})")
    fw = binario(tipo, modelo)
    if not fw.exists():
        raise RuntimeError(f"falta {fw.name} en firmware/bin")
    _esptool(["--chip", modelo, "--port", puerto, "--baud", "460800", "write_flash", "0x0", str(fw)], log)
    log(f"{fw.name} grabado en {puerto}")
    return {"ok": True, "puerto": puerto, "chip": chip, "archivo": fw.name}


def _serie(puerto: str, baud: int = 115200):
    import serial
    return serial.Serial(puerto, baud, timeout=0.3)


def configurar_esp32(tipo: str, puerto: str, cfg: dict, log=print, espera_s: float = 8) -> dict:
    """Manda la configuracion por el puerto serie (una linea JSON) y espera la respuesta del firmware."""
    puerto = puerto or puerto_auto("esp32")
    if tipo == "luces":
        msg = {"config": {k: v for k, v in cfg.items() if k != "nexus"}}
    else:
        msg = {k: v for k, v in cfg.items() if k in ("wifi", "nexus")}
    linea = (json.dumps(msg, ensure_ascii=False) + "\n").encode()
    with _serie(puerto) as s:
        fin = time.time() + espera_s
        while time.time() < fin:       # tras grabar, el ESP32 tarda un poco en arrancar
            s.write(linea)
            t = time.time() + 1.5
            while time.time() < t:
                r = s.readline().decode(errors="replace").strip()
                if r.startswith("{"):
                    log("respuesta: " + r)
                    try:
                        return json.loads(r)
                    except ValueError:
                        return {"respuesta": r}
    raise RuntimeError("el dispositivo no responde por el puerto serie")


def flashear_pico(unidad: str = "", log=print, espera_s: float = 30) -> dict:
    fw = binario("pico")
    if not fw.exists():
        raise RuntimeError(f"falta {fw.name} en firmware/bin")
    destinos = [Path(unidad)] if unidad else unidades_pico()
    if not destinos:
        raise RuntimeError("no veo la unidad RPI-RP2: conecta el Pico W manteniendo pulsado BOOTSEL")
    shutil.copyfile(fw, destinos[0] / fw.name)
    log(f"{fw.name} copiado a {destinos[0]}; el Pico se reinicia")
    fin = time.time() + espera_s
    while time.time() < fin:
        try:
            return {"ok": True, "puerto": puerto_auto("pico")}
        except RuntimeError:
            time.sleep(1)
    return {"ok": True, "puerto": ""}


def configurar_pico(puerto: str, cfg: dict, log=print) -> dict:
    """Escribe /config.json en el Pico por el REPL en bruto de MicroPython y lo reinicia."""
    puerto = puerto or puerto_auto("pico")
    datos = json.dumps(cfg, ensure_ascii=False)
    codigo = f"f=open('/config.json','w')\nf.write({datos!r})\nf.close()\nprint('NEXUS-OK')\n"
    with _serie(puerto) as s:
        s.write(b"\r\x03\x03")          # interrumpe el programa
        time.sleep(0.3)
        s.write(b"\r\x01")              # REPL en bruto
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(codigo.encode() + b"\x04")
        fin, salida = time.time() + 6, b""
        while time.time() < fin and b"NEXUS-OK" not in salida:
            salida += s.read(256)
        s.write(b"\x02")                # REPL normal
        if b"NEXUS-OK" not in salida:
            raise RuntimeError("el Pico no acepto la configuracion: " + salida.decode(errors="replace")[-200:])
        s.write(b"import machine; machine.reset()\r")
    log("config.json grabado en el Pico; reiniciando")
    return {"ok": True, "puerto": puerto}


def montar_config(a) -> dict:
    cfg: dict = {}
    if a.wifi:
        cfg["wifi"] = {"ssid": a.wifi[0], "pass": a.wifi[1] if len(a.wifi) > 1 else ""}
    if a.nexus:
        cfg["nexus"] = {"host": a.nexus[0], "key": a.nexus[1] if len(a.nexus) > 1 else ""}
    if getattr(a, "leds", None):
        cfg["outputs"] = [{"pin": a.pin, "count": a.leds}]
    if getattr(a, "nombre", None):
        cfg["name"] = a.nombre
    return cfg


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Graba el firmware de las luces, el reloj y el Pico W de Nexus")
    ap.add_argument("accion", choices=["puertos", "binarios", "luces", "reloj", "pico", "config"])
    ap.add_argument("tipo", nargs="?", choices=["luces", "reloj", "pico"], help="con 'config'")
    ap.add_argument("--puerto", default="")
    ap.add_argument("--modelo", default="", choices=["", *MODELOS_LUCES])
    ap.add_argument("--wifi", nargs="+", metavar=("RED", "CLAVE"))
    ap.add_argument("--nexus", nargs="+", metavar=("HOST", "CLAVE"))
    ap.add_argument("--leds", type=int, help="luces: numero de LED de la tira")
    ap.add_argument("--pin", type=int, default=16, help="luces: GPIO de datos (16 por defecto)")
    ap.add_argument("--nombre", help="luces: nombre de la tira")
    a = ap.parse_args(argv)
    try:
        if a.accion == "puertos":
            for p in puertos():
                print(f"{p['puerto']:<24} {p['tipo'] or '-':<13} {p['descripcion']}")
            return 0
        if a.accion == "binarios":
            for b in binarios():
                print(f"{b['archivo']:<28} {b['bytes']:>9} bytes  {b.get('version', '')}")
            return 0
        cfg = montar_config(a)
        tipo = a.tipo if a.accion == "config" else a.accion
        if not tipo:
            ap.error("indica luces, reloj o pico")
        if tipo == "pico":
            puerto = a.puerto
            if a.accion != "config":
                puerto = flashear_pico(a.puerto if a.puerto and Path(a.puerto).is_dir() else "")["puerto"] or ""
            if cfg:
                configurar_pico(puerto, cfg)
        else:
            puerto = a.puerto
            if a.accion != "config":
                puerto = flashear_esp32(tipo, a.puerto, a.modelo)["puerto"]
            if cfg:
                configurar_esp32(tipo, puerto, cfg)
        print("Listo.")
        return 0
    except (RuntimeError, ValueError, OSError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
