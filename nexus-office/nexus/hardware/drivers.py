"""Gestor de drivers de la GPU.

Detecta la version y fecha del driver instalado, la compara con los minimos que necesitan los runtimes de
Nexus-Office (CUDA 12.x, Vulkan, OpenVINO...) y con su antiguedad, y ofrece el metodo OFICIAL de actualizacion de
cada fabricante y sistema. La actualizacion siempre la confirma el usuario: se abre el actualizador del
fabricante (con el aviso de permisos de administrador del sistema) o su pagina oficial; Nexus-Office nunca instala
drivers en silencio.
"""
from __future__ import annotations

import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import webbrowser

from .. import log

L = log.get("drivers")
MAX_AGE_DAYS = 365
NVIDIA_MIN = {"win32": (551, 61), "linux": (550, 54)}      # CUDA 12.4 (runtime cuda de llama.cpp)
INTEL_MIN_BUILD = 5000                                       # 31.0.101.5xxx (2024+): Vulkan/OpenVINO estables
PAGES = {
    "nvidia": "https://www.nvidia.com/es-es/software/nvidia-app/",
    "amd": "https://www.amd.com/es/support/download/drivers.html",
    "intel": "https://www.intel.es/content/www/es/es/support/detect.html",
}
NOWIN = 0x08000000 if os.name == "nt" else 0


def _run(cmd: list[str], timeout: float = 30) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, creationflags=NOWIN, encoding="utf-8", errors="replace")
        return r.stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def installed() -> list[dict]:
    """Drivers de las GPU presentes: nombre, fabricante, version y fecha."""
    out: list[dict] = []
    if os.name == "nt":
        raw = _run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,"
                    "@{n='Date';e={$_.DriverDate.ToString('yyyy-MM-dd')}},PNPDeviceID | ConvertTo-Json -Compress"])
        try:
            data = json.loads(raw) if raw.strip() else []
        except json.JSONDecodeError:
            data = []
        for v in data if isinstance(data, list) else [data]:
            name = v.get("Name") or ""
            if "basic display" in name.lower():
                continue
            out.append({"name": name, "vendor": _vendor(f"{v.get('PNPDeviceID', '')} {name}"),
                        "version": v.get("DriverVersion") or "", "date": v.get("Date") or ""})
    elif sys.platform.startswith("linux"):
        from .detect import find_nvidia_smi, linux_gpus
        smi = find_nvidia_smi()
        if smi:
            for line in _run([smi, "--query-gpu=name,driver_version", "--format=csv,noheader"]).splitlines():
                parts = [x.strip() for x in line.split(",")]
                if len(parts) >= 2:
                    out.append({"name": parts[0], "vendor": "nvidia", "version": parts[1], "date": ""})
        mesa = re.search(r"Mesa (\d+\.\d+(?:\.\d+)?)", _mesa_info())
        if mesa:
            out.append({"name": "Mesa (AMD/Intel)", "vendor": "mesa", "version": mesa.group(1), "date": ""})
        try:
            pci = linux_gpus()
        except OSError:
            pci = []
        for g in pci:
            # GPUs que ni nvidia-smi ni Mesa han reportado: se listan para ofrecer el driver/actualizacion
            if (g["vendor"] == "nvidia" and any(d["vendor"] == "nvidia" for d in out)) or \
                    (g["vendor"] in ("amd", "intel") and mesa):
                continue
            name = g["name"] or f"{g['vendor'].upper()} GPU"
            out.append({"name": f"{name} ({g['driver'] or 'sin driver'})", "vendor": g["vendor"], "version": "",
                        "date": ""})
    return out


def _mesa_info() -> str:
    """Version de Mesa: glxinfo (mesa-utils) o, si no esta, vulkaninfo / eglinfo."""
    for cmd in (["glxinfo", "-B"], ["vulkaninfo", "--summary"], ["eglinfo", "-B"]):
        if shutil.which(cmd[0]):
            out = _run(cmd)
            if "Mesa" in out:
                return out
    return ""


def _vendor(text: str) -> str:
    t = text.lower()
    if "ven_10de" in t or "nvidia" in t:
        return "nvidia"
    if "ven_1002" in t or "amd" in t or "radeon" in t:
        return "amd"
    if "ven_8086" in t or "intel" in t:
        return "intel"
    return "otro"


def _nvidia_version(v: str, windows: bool) -> tuple[int, int] | None:
    """Windows reporta 31.0.15.5161 -> 551.61 (ultimos 5 digitos); Linux 550.54.14."""
    digits = re.sub(r"\D", "", v.replace(".", ""))
    if windows and len(digits) >= 5:
        tail = digits[-5:]
        return int(tail[:3]), int(tail[3:])
    m = re.match(r"(\d+)\.(\d+)", v)
    return (int(m.group(1)), int(m.group(2))) if m else None


def evaluate(d: dict, accelerator: str, windows: bool | None = None) -> dict:
    """Estado: ok | actualizar | desconocido, con motivos.

    `windows` indica el formato de la version (por defecto, el del sistema actual): Windows numera los
    drivers a su manera (31.0.15.5161, 31.0.101.4146) y Linux da la del fabricante (550.54.14)."""
    if windows is None:
        windows = os.name == "nt"
    reasons, status = [], "ok"
    if d.get("date"):
        try:
            age = (dt.date.today() - dt.date.fromisoformat(d["date"])).days
            d["age_days"] = age
            if age > MAX_AGE_DAYS:
                status = "actualizar"
                reasons.append(f"El driver tiene {age // 30} meses; los recientes mejoran rendimiento y compatibilidad")
        except ValueError:
            pass
    if d["vendor"] == "nvidia":
        ver = _nvidia_version(d.get("version", ""), windows)
        need = NVIDIA_MIN["win32" if windows else "linux"]
        if ver and ver < need:
            status = "actualizar"
            reasons.append(f"CUDA 12.4 necesita driver {need[0]}.{need[1]} o superior (instalado {ver[0]}.{ver[1]})")
    elif d["vendor"] == "intel" and windows:
        build = d.get("version", "").split(".")[-1]
        if build.isdigit() and int(build) < INTEL_MIN_BUILD:
            status = "actualizar"
            reasons.append("Los drivers Intel anteriores a 31.0.101.5000 tienen problemas con Vulkan/OpenVINO")
    if not d.get("version"):
        status = "desconocido"
    if status == "ok":
        reasons.append("Driver al dia para los runtimes de Nexus-Office")
    d.update(status=status, reasons=reasons, options=options(d["vendor"]))
    return d


def options(vendor: str) -> list[dict]:
    """Metodos oficiales de actualizacion para este sistema."""
    opts: list[dict] = []
    if os.name == "nt":
        if vendor == "intel" and shutil.which("winget"):
            opts.append({"id": "intel-dsa", "label": "Intel Driver & Support Assistant (oficial, via winget)",
                         "detail": "Instala el asistente oficial de Intel, que detecta y actualiza los drivers"})
        if vendor in PAGES:
            opts.append({"id": f"{vendor}-web", "label": f"Pagina oficial de drivers de {vendor.upper()}", "detail": PAGES[vendor]})
        opts.append({"id": "windows-update", "label": "Windows Update (actualizaciones opcionales de drivers)",
                     "detail": "Abre Configuracion > Windows Update > Actualizaciones opcionales"})
    elif sys.platform.startswith("linux"):
        if shutil.which("ubuntu-drivers") and vendor == "nvidia":
            opts.append({"id": "ubuntu-drivers", "label": "ubuntu-drivers install (driver NVIDIA recomendado)",
                         "detail": "pkexec ubuntu-drivers install"})
        for pm, cmd in (("apt-get", "pkexec sh -c 'apt-get update && apt-get install -y --only-upgrade mesa-vulkan-drivers libgl1-mesa-dri'"),
                        ("dnf", "pkexec dnf upgrade -y 'mesa*'"), ("pacman", "pkexec pacman -Syu --noconfirm mesa vulkan-icd-loader")):
            if shutil.which(pm) and vendor in ("amd", "intel", "mesa"):
                opts.append({"id": f"mesa-{pm}", "label": f"Actualizar Mesa/Vulkan ({pm})", "detail": cmd})
                break
        if vendor in PAGES:
            opts.append({"id": f"{vendor}-web", "label": f"Pagina oficial de drivers de {vendor.upper()}", "detail": PAGES[vendor]})
    elif sys.platform == "darwin":
        opts.append({"id": "macos-update", "label": "Actualizacion de software de macOS", "detail": "Los drivers de GPU llegan con macOS"})
    else:
        opts.append({"id": "system", "label": "Actualizacion del sistema",
                     "detail": "En Android los drivers de GPU llegan con las actualizaciones del sistema"})
    return opts


def report(accelerator: str) -> dict:
    gpus = [evaluate(d, accelerator) for d in installed()]
    return {"gpus": gpus, "needs_update": any(g["status"] == "actualizar" for g in gpus),
            "note": "Las actualizaciones las confirma el usuario en el actualizador oficial (requieren permisos de administrador)."}


def launch(option_id: str) -> dict:
    """Abre el metodo de actualizacion elegido (siempre visible e interactivo para el usuario)."""
    L.info("Actualizacion de drivers solicitada por el usuario: %s", option_id)
    if option_id == "intel-dsa":
        # consola visible: winget muestra los acuerdos y el UAC; el usuario decide
        subprocess.Popen(["cmd", "/c", "start", "Nexus-Office - actualizar drivers Intel", "cmd", "/k",
                          "winget install --id Intel.IntelDriverAndSupportAssistant -e --source winget"])
        return {"launched": True, "how": "winget (consola visible)"}
    if option_id.endswith("-web"):
        webbrowser.open(PAGES[option_id.split("-")[0]])
        return {"launched": True, "how": "navegador"}
    if option_id == "windows-update":
        os.startfile("ms-settings:windowsupdate-optionalupdates")  # type: ignore[attr-defined]
        return {"launched": True, "how": "Configuracion de Windows"}
    if option_id == "macos-update":
        subprocess.Popen(["open", "x-apple.systempreferences:com.apple.Software-Update-Settings.extension"])
        return {"launched": True, "how": "Ajustes del sistema"}
    for o in options("nvidia") + options("amd") + options("mesa"):
        if o["id"] == option_id and o["detail"].startswith("pkexec"):
            term = shutil.which("x-terminal-emulator") or shutil.which("gnome-terminal") or shutil.which("konsole")
            cmd = o["detail"] + "; echo; read -p 'Pulsa Enter para cerrar'"
            if term:
                subprocess.Popen([term, "-e", "bash", "-c", cmd])
            else:
                subprocess.Popen(["bash", "-c", o["detail"]])
            return {"launched": True, "how": "terminal (pkexec pide la contrasena)"}
    raise ValueError(f"opcion desconocida: {option_id}")
