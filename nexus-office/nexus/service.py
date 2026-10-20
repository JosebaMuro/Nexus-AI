"""Nexus-Office como servicio: arranca solo al encender el equipo, sin ventana, y se reinicia si falla.

    python -m nexus servicio instalar [--red] [--puerto P]
    python -m nexus servicio quitar
    python -m nexus servicio estado

Linux    systemd: del sistema si se ejecuta como root (corre como el usuario de la carpeta o el de sudo),
         si no de usuario (+ loginctl enable-linger para que arranque sin iniciar sesion). Sin systemd: cron @reboot.
macOS    launchd: LaunchDaemon como root, LaunchAgent (al iniciar sesion) si no.
Windows  Programador de tareas: como administrador arranca con el equipo (cuenta SYSTEM), si no al iniciar sesion.
         Reinicio automatico ante fallos y sin limite de tiempo de ejecucion.
Termux   termux-services (sv) y, si esta instalado Termux:Boot, arranque al encender el movil.
         (La app Nexus-Office autonomo tiene su propio servicio en primer plano: ver docs/SERVICIO.md.)
La carpeta sigue siendo portable: el servicio apunta a esta carpeta; si la mueves, vuelve a instalarlo.
"""
from __future__ import annotations

import argparse
import getpass
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from xml.sax.saxutils import escape

from . import paths

NAME = "nexus-office"                 # systemd / termux
LABEL = "ai.nexus.office.server"         # launchd
TASK = "Nexus-Office"                 # Programador de tareas de Windows
MARK = "# nexus-office-servicio"      # linea de crontab


def kind() -> str:
    if os.environ.get("TERMUX_VERSION") or Path("/data/data/com.termux").is_dir():
        return "termux"
    s = platform.system()
    if s == "Windows":
        return "windows"
    if s == "Darwin":
        return "launchd"
    return "systemd" if Path("/run/systemd/system").is_dir() and shutil.which("systemctl") else "cron"


def is_admin() -> bool:
    if os.name == "nt":
        try:
            import ctypes
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:  # noqa: BLE001
            return False
    return os.geteuid() == 0


def run_user() -> str:
    """Usuario con el que corre el servicio del sistema: el que hizo sudo o el dueno de la carpeta."""
    if os.environ.get("SUDO_USER") and os.environ["SUDO_USER"] != "root":
        return os.environ["SUDO_USER"]
    try:
        import pwd
        return pwd.getpwuid(paths.ROOT.stat().st_uid).pw_name
    except Exception:  # noqa: BLE001
        return getpass.getuser()


def _home(user: str) -> Path:
    try:
        import pwd
        return Path(pwd.getpwnam(user).pw_dir)
    except Exception:  # noqa: BLE001
        return Path.home()


def data_dir_for(user: str) -> str:
    """NEXUS_DATA_DIR que vera el servicio ('' = la carpeta data/ portable)."""
    d = os.environ.get("NEXUS_DATA_DIR", "")
    if d and is_admin() and os.name != "nt" and user != "root" and d.startswith("/root"):
        d = str(_home(user) / ".local/share/nexus-office")      # sudo conserva HOME=/root: datos del usuario real
    return d


def python_exe(windowless: bool = True) -> str:
    exe = Path(os.path.abspath(sys.executable))
    if os.name == "nt" and windowless and (exe.parent / "pythonw.exe").exists():
        return str(exe.parent / "pythonw.exe")
    return str(exe)


def bundle() -> str:
    """'appimage' o 'flatpak' si Nexus-Office corre empaquetado asi (carpeta de solo lectura y ruta temporal)."""
    return "appimage" if os.environ.get("APPIMAGE") else "flatpak" if os.environ.get("FLATPAK_ID") else ""


def workdir() -> Path:
    return Path.home() if bundle() else paths.ROOT


def command(host: str, port: int, windowless: bool = True) -> list[str]:
    tail = ["run", "--no-browser", "--host", host, "--port", str(port)]
    if bundle() == "appimage":                 # el AppImage se monta en una ruta temporal: se arranca el fichero
        return [os.environ["APPIMAGE"], *tail]
    if bundle() == "flatpak":
        return ["/usr/bin/flatpak", "run", os.environ["FLATPAK_ID"], *tail]
    return [python_exe(windowless), "-m", "nexus", *tail]


# ------------------------------------------------------------------ ficheros de cada sistema
def systemd_unit(cmd: list[str], system: bool, user: str = "", data: str = "") -> str:
    env = "Environment=PYTHONUNBUFFERED=1\n" + (f'Environment="NEXUS_DATA_DIR={data}"\n' if data else "")
    who = f"User={user}\n" if system and user else ""
    return f"""[Unit]
Description=Nexus-Office (oficina de IA local)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
{who}WorkingDirectory={workdir()}
{env}ExecStart={" ".join(_q(c) for c in cmd)}
Restart=on-failure
RestartSec=5
TimeoutStopSec=20

[Install]
WantedBy={"multi-user.target" if system else "default.target"}
"""


def _q(s: str) -> str:
    return f'"{s}"' if any(c in s for c in " \t'\"") else s


def launchd_plist(cmd: list[str], user: str = "", data: str = "") -> str:
    args = "".join(f"<string>{escape(c)}</string>" for c in cmd)
    logs = paths.logs_dir()
    env = f"<key>NEXUS_DATA_DIR</key><string>{escape(data)}</string>" if data else ""
    who = f"<key>UserName</key><string>{escape(user)}</string>" if user else ""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>{LABEL}</string>
<key>ProgramArguments</key><array>{args}</array>
<key>WorkingDirectory</key><string>{escape(str(workdir()))}</string>
<key>EnvironmentVariables</key><dict><key>PYTHONUNBUFFERED</key><string>1</string>{env}</dict>
{who}<key>RunAtLoad</key><true/>
<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
<key>ThrottleInterval</key><integer>10</integer>
<key>StandardOutPath</key><string>{escape(str(logs / "servicio.log"))}</string>
<key>StandardErrorPath</key><string>{escape(str(logs / "servicio.log"))}</string>
</dict></plist>
"""


def windows_task_xml(cmd: list[str], system: bool) -> str:
    args = " ".join(_q(c) for c in cmd[1:])
    if system:
        trigger = "<BootTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></BootTrigger>"
        principal = "<Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal>"
    else:
        user = os.environ.get("USERDOMAIN", "") + "\\" + getpass.getuser() if os.environ.get("USERDOMAIN") else getpass.getuser()
        trigger = f"<LogonTrigger><Enabled>true</Enabled><UserId>{escape(user)}</UserId></LogonTrigger>"
        principal = (f"<Principal id=\"Author\"><UserId>{escape(user)}</UserId><LogonType>InteractiveToken</LogonType>"
                     "<RunLevel>LeastPrivilege</RunLevel></Principal>")
    return f"""<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>Nexus-Office: oficina de IA local ({escape(str(paths.ROOT))})</Description></RegistrationInfo>
  <Triggers>{trigger}</Triggers>
  <Principals>{principal}</Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>true</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <Priority>6</Priority>
    <RestartOnFailure><Interval>PT1M</Interval><Count>999</Count></RestartOnFailure>
  </Settings>
  <Actions Context="Author">
    <Exec><Command>{escape(cmd[0])}</Command><Arguments>{escape(args)}</Arguments><WorkingDirectory>{escape(str(paths.ROOT))}</WorkingDirectory></Exec>
  </Actions>
</Task>
"""


def termux_run(cmd: list[str]) -> str:
    return "#!/data/data/com.termux/files/usr/bin/sh\nexec 2>&1\ncd " + _q(str(paths.ROOT)) + "\nexec " + " ".join(_q(c) for c in cmd) + "\n"


# ------------------------------------------------------------------ acciones
def _sh(args: list[str], check: bool = True, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=check, text=True, capture_output=True, **kw)


def _say(msg: str) -> None:
    print(f"[Nexus-Office] {msg}", flush=True)


def install(host: str, port: int) -> str:
    k = kind()
    cmd = command(host, port)
    if bundle() == "flatpak":                  # desde el sandbox no se puede llamar a systemctl: se deja la unidad lista
        dest = Path.home() / ".config/systemd/user" / f"{NAME}.service"
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(systemd_unit(cmd, False), encoding="utf-8")
        _say("Activalo en una terminal del sistema con: systemctl --user enable --now nexus-office  "
             "(y loginctl enable-linger para que arranque sin iniciar sesion)")
        return f"unidad systemd de usuario (Flatpak): {dest}"
    if k == "systemd":
        system = is_admin()
        user = run_user() if system else ""
        unit = systemd_unit(cmd, system, user, data_dir_for(user) if system else os.environ.get("NEXUS_DATA_DIR", ""))
        if system:
            dest = Path("/etc/systemd/system") / f"{NAME}.service"
            ctl = ["systemctl"]
        else:
            dest = Path.home() / ".config/systemd/user" / f"{NAME}.service"
            ctl = ["systemctl", "--user"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(unit, encoding="utf-8")
        _sh(ctl + ["daemon-reload"])
        _sh(ctl + ["enable", "--now", f"{NAME}.service"])
        if not system:
            r = _sh(["loginctl", "enable-linger", getpass.getuser()], check=False)
            if r.returncode:
                _say("Aviso: sin 'loginctl enable-linger' Nexus-Office arrancara al iniciar sesion, no al encender. "
                     "Para que arranque con el equipo: sudo venv/bin/python3 -m nexus servicio instalar")
        return f"systemd {'del sistema (usuario ' + user + ')' if system else 'de usuario'}: {dest}"
    if k == "cron":
        line = " ".join(_q(c) for c in ["cd", str(workdir())]) + " && " + " ".join(_q(c) for c in cmd) + \
            f" >> {_q(str(paths.logs_dir() / 'servicio.log'))} 2>&1 {MARK}"
        cur = _sh(["crontab", "-l"], check=False).stdout if shutil.which("crontab") else None
        if cur is None:
            raise RuntimeError("este sistema no tiene systemd ni cron: arranca Nexus-Office con ./start.sh --no-browser")
        lines = [ln for ln in cur.splitlines() if MARK not in ln] + [f"@reboot {line}"]
        subprocess.run(["crontab", "-"], input="\n".join(lines) + "\n", text=True, check=True)
        subprocess.Popen(cmd, cwd=workdir(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        return "cron @reboot (sin systemd)"
    if k == "launchd":
        system = is_admin()
        user = run_user() if system else ""
        dest = (Path("/Library/LaunchDaemons") if system else Path.home() / "Library/LaunchAgents") / f"{LABEL}.plist"
        dest.parent.mkdir(parents=True, exist_ok=True)
        _launchd_unload(dest, system)
        dest.write_text(launchd_plist(cmd, user, data_dir_for(user) if system else os.environ.get("NEXUS_DATA_DIR", "")),
                        encoding="utf-8")
        if system:
            os.chmod(dest, 0o644)
        domain = "system" if system else f"gui/{os.getuid()}"
        if _sh(["launchctl", "bootstrap", domain, str(dest)], check=False).returncode:
            _sh(["launchctl", "load", "-w", str(dest)], check=False)
        return f"launchd {'LaunchDaemon (arranca con el equipo)' if system else 'LaunchAgent (al iniciar sesion)'}: {dest}"
    if k == "windows":
        system = is_admin()
        xml = windows_task_xml(cmd, system)
        with tempfile.NamedTemporaryFile("w", suffix=".xml", delete=False, encoding="utf-16") as f:
            f.write(xml)
        try:
            r = _sh(["schtasks", "/Create", "/TN", TASK, "/XML", f.name, "/F"], check=False)
            if r.returncode:
                raise RuntimeError((r.stdout + r.stderr).strip() or "schtasks fallo")
        finally:
            os.unlink(f.name)
        _sh(["schtasks", "/Run", "/TN", TASK], check=False)
        if system and host not in ("127.0.0.1", "localhost"):
            _sh(["netsh", "advfirewall", "firewall", "delete", "rule", f"name={TASK}"], check=False)
            _sh(["netsh", "advfirewall", "firewall", "add", "rule", f"name={TASK}", "dir=in", "action=allow",
                 "protocol=TCP", f"localport={port}"], check=False)
        return ("tarea programada al iniciar el equipo (SYSTEM)" if system
                else "tarea programada al iniciar sesion (ejecuta como administrador para que arranque con el equipo)")
    # termux
    prefix = Path(os.environ.get("PREFIX", "/data/data/com.termux/files/usr"))
    svdir = prefix / "var/service" / NAME
    (svdir / "log").mkdir(parents=True, exist_ok=True)
    (svdir / "run").write_text(termux_run(cmd))
    (svdir / "log/run").write_text(f"#!/data/data/com.termux/files/usr/bin/sh\nexec svlogd -tt {_q(str(paths.logs_dir()))}\n")
    for f in (svdir / "run", svdir / "log/run"):
        f.chmod(0o755)
    boot = Path.home() / ".termux/boot"
    boot.mkdir(parents=True, exist_ok=True)
    (boot / f"{NAME}.sh").write_text("#!/data/data/com.termux/files/usr/bin/sh\ntermux-wake-lock\n"
                                     f". $PREFIX/etc/profile.d/start-services.sh 2>/dev/null\nsv up {NAME}\n")
    (boot / f"{NAME}.sh").chmod(0o755)
    if shutil.which("sv-enable"):
        _sh(["sv-enable", NAME], check=False)
        _sh(["sv", "up", NAME], check=False)
        return "termux-services (+ Termux:Boot para arrancar al encender el movil)"
    _say("Falta termux-services: pkg install termux-services y reinicia Termux; luego: sv-enable nexus-office")
    return "termux-services preparado (pendiente: pkg install termux-services)"


def _launchd_unload(dest: Path, system: bool) -> None:
    if dest.exists():
        domain = "system" if system else f"gui/{os.getuid()}"
        if _sh(["launchctl", "bootout", f"{domain}/{LABEL}"], check=False).returncode:
            _sh(["launchctl", "unload", "-w", str(dest)], check=False)


def uninstall() -> list[str]:
    k, done = kind(), []
    if bundle() == "flatpak":
        dest = Path.home() / ".config/systemd/user" / f"{NAME}.service"
        if dest.exists():
            dest.unlink()
            done.append(str(dest))
            _say("Desactivalo tambien en el sistema: systemctl --user disable --now nexus-office")
        return done
    if k == "systemd":
        for ctl, dest in ((["systemctl"], Path("/etc/systemd/system") / f"{NAME}.service"),
                          (["systemctl", "--user"], Path.home() / ".config/systemd/user" / f"{NAME}.service")):
            if dest.exists():
                if ctl == ["systemctl"] and not is_admin():
                    raise RuntimeError(f"el servicio del sistema ({dest}) se quita con sudo")
                _sh(ctl + ["disable", "--now", f"{NAME}.service"], check=False)
                dest.unlink()
                _sh(ctl + ["daemon-reload"], check=False)
                done.append(str(dest))
    elif k == "cron" and shutil.which("crontab"):
        cur = _sh(["crontab", "-l"], check=False).stdout
        if MARK in cur:
            subprocess.run(["crontab", "-"], input="\n".join(ln for ln in cur.splitlines() if MARK not in ln) + "\n",
                           text=True, check=True)
            done.append("crontab @reboot")
    elif k == "launchd":
        for dest, system in ((Path("/Library/LaunchDaemons") / f"{LABEL}.plist", True),
                             (Path.home() / "Library/LaunchAgents" / f"{LABEL}.plist", False)):
            if dest.exists():
                _launchd_unload(dest, system)
                dest.unlink()
                done.append(str(dest))
    elif k == "windows":
        if _sh(["schtasks", "/Query", "/TN", TASK], check=False).returncode == 0:
            _sh(["schtasks", "/End", "/TN", TASK], check=False)
            r = _sh(["schtasks", "/Delete", "/TN", TASK, "/F"], check=False)
            if r.returncode:
                raise RuntimeError("no se pudo borrar la tarea (si se creo como administrador, quitala como administrador)")
            done.append(f"tarea '{TASK}'")
        _sh(["netsh", "advfirewall", "firewall", "delete", "rule", f"name={TASK}"], check=False)
    else:
        prefix = Path(os.environ.get("PREFIX", "/data/data/com.termux/files/usr"))
        if shutil.which("sv-disable"):
            _sh(["sv", "down", NAME], check=False)
            _sh(["sv-disable", NAME], check=False)
        for p in (prefix / "var/service" / NAME, Path.home() / ".termux/boot" / f"{NAME}.sh"):
            if p.exists():
                shutil.rmtree(p) if p.is_dir() else p.unlink()
                done.append(str(p))
    return done


def registered() -> str:
    """Donde esta registrado el servicio ('' si no lo esta)."""
    k = kind()
    if k == "systemd" or bundle() == "flatpak":
        for p in (Path("/etc/systemd/system") / f"{NAME}.service", Path.home() / ".config/systemd/user" / f"{NAME}.service"):
            if p.exists():
                return str(p)
    elif k == "cron":
        if shutil.which("crontab") and MARK in _sh(["crontab", "-l"], check=False).stdout:
            return "crontab @reboot"
    elif k == "launchd":
        for p in (Path("/Library/LaunchDaemons") / f"{LABEL}.plist", Path.home() / "Library/LaunchAgents" / f"{LABEL}.plist"):
            if p.exists():
                return str(p)
    elif k == "windows":
        if _sh(["schtasks", "/Query", "/TN", TASK], check=False).returncode == 0:
            return f"tarea '{TASK}'"
    else:
        p = Path(os.environ.get("PREFIX", "/data/data/com.termux/files/usr")) / "var/service" / NAME
        if p.exists():
            return str(p)
    return ""


def running(port: int, timeout: float = 1.5) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/ping", timeout=timeout) as r:
            return r.status == 200
    except Exception:  # noqa: BLE001
        return False


def main(argv: list[str]) -> int:
    from .config import get_config
    cfg = get_config()
    ap = argparse.ArgumentParser(prog="nexus servicio", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("accion", choices=["instalar", "quitar", "estado"])
    ap.add_argument("--red", action="store_true", help="escucha en la red local (0.0.0.0) para entrar desde moviles y otros PC")
    ap.add_argument("--host", help="direccion de escucha (por defecto la de config.yaml)")
    ap.add_argument("--puerto", type=int, default=None)
    a = ap.parse_args(argv)
    port = a.puerto or int(cfg.get("server.port", 8775))
    host = "0.0.0.0" if a.red else (a.host or cfg.get("server.host", "127.0.0.1"))
    try:
        if a.accion == "instalar":
            where = install(host, port)
            _say(f"Servicio instalado: {where}")
            _say(f"Nexus-Office arrancara solo; abre http://127.0.0.1:{port}" + (" (o http://<ip-del-equipo>:" + str(port) + " desde la red)" if host == "0.0.0.0" else ""))
        elif a.accion == "quitar":
            done = uninstall()
            _say("Servicio quitado: " + ", ".join(done) if done else "No habia servicio instalado")
        else:
            reg = registered()
            _say(f"Tipo: {kind()} | registrado: {reg or 'no'} | en marcha: {'si' if running(port) else 'no'} (puerto {port})")
    except (RuntimeError, subprocess.CalledProcessError, PermissionError, OSError) as e:
        detail = getattr(e, "stderr", "") or ""
        _say(f"Error: {e} {detail}".strip())
        return 1
    return 0
