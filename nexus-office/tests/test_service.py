"""Servicio del sistema: ficheros generados para cada plataforma e instalacion/borrado sin tocar el sistema real."""
import plistlib
import xml.dom.minidom

import pytest

from nexus import paths, service


def test_command_no_abre_navegador():
    cmd = service.command("0.0.0.0", 9000)
    assert cmd[1:] == ["-m", "nexus", "run", "--no-browser", "--host", "0.0.0.0", "--port", "9000"]


def test_systemd_unit_sistema_y_usuario():
    cmd = service.command("127.0.0.1", 8775)
    sysu = service.systemd_unit(cmd, True, "pepe", "/home/pepe/.local/share/nexus-office")
    assert "User=pepe" in sysu and "multi-user.target" in sysu and "Restart=on-failure" in sysu
    assert 'NEXUS_DATA_DIR=/home/pepe/.local/share/nexus-office' in sysu and f"WorkingDirectory={paths.ROOT}" in sysu
    user = service.systemd_unit(cmd, False)
    assert "User=" not in user and "default.target" in user and "NEXUS_DATA_DIR" not in user


def test_launchd_plist_valido():
    d = plistlib.loads(service.launchd_plist(service.command("127.0.0.1", 8775), "pepe", "/datos").encode())
    assert d["Label"] == service.LABEL and d["RunAtLoad"] and d["UserName"] == "pepe"
    assert d["ProgramArguments"][-1] == "8775" and d["EnvironmentVariables"]["NEXUS_DATA_DIR"] == "/datos"


@pytest.mark.parametrize("system", [True, False])
def test_tarea_windows_valida(system):
    cmd = [r"C:\Mis programas\Nexus-Office\venv\pythonw.exe", "-m", "nexus", "run", "--no-browser", "--host", "127.0.0.1", "--port", "8775"]
    txt = service.windows_task_xml(cmd, system)
    doc = xml.dom.minidom.parseString(txt.replace('encoding="UTF-16"', 'encoding="UTF-8"').encode())
    assert doc.getElementsByTagName("ExecutionTimeLimit")[0].firstChild.data == "PT0S"   # sin limite de 72 h
    assert bool(doc.getElementsByTagName("BootTrigger")) == system
    assert bool(doc.getElementsByTagName("LogonTrigger")) != system
    assert doc.getElementsByTagName("Command")[0].firstChild.data == cmd[0]


def test_systemd_usuario_instala_y_quita(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(service, "kind", lambda: "systemd")
    monkeypatch.setattr(service, "is_admin", lambda: False)
    monkeypatch.setattr(service.Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.setattr(service, "_sh", lambda args, check=True, **kw: calls.append(args) or
                        type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())
    where = service.install("127.0.0.1", 8775)
    unit = tmp_path / ".config/systemd/user/nexus-office.service"
    assert unit.exists() and "de usuario" in where
    assert ["systemctl", "--user", "enable", "--now", "nexus-office.service"] in calls
    assert service.registered() == str(unit)
    assert service.uninstall() == [str(unit)] and not unit.exists()


def test_termux_instala_y_quita(tmp_path, monkeypatch):
    monkeypatch.setattr(service, "kind", lambda: "termux")
    monkeypatch.setenv("PREFIX", str(tmp_path / "usr"))
    monkeypatch.setattr(service.Path, "home", classmethod(lambda cls: tmp_path / "home"))
    monkeypatch.setattr(service.shutil, "which", lambda name: None)
    service.install("127.0.0.1", 8775)
    run = tmp_path / "usr/var/service/nexus-office/run"
    assert run.exists() and "nexus run --no-browser" in run.read_text()
    assert (tmp_path / "home/.termux/boot/nexus-office.sh").exists()
    assert len(service.uninstall()) == 2 and not run.exists()


def test_cli_estado(capsys):
    assert service.main(["estado", "--puerto", "1"]) == 0
    assert "registrado" in capsys.readouterr().out


def test_descargas_clasifica_y_sirve(services, data_dir):
    from fastapi.testclient import TestClient

    from nexus import downloads
    from nexus.server import create_app
    base = downloads.folder()
    for rel in ("windows/Nexus-Office-Setup-1.7.0-windows-x64.exe", "linux/nexus-office_1.7.0_arm64.deb",
                "linux/Nexus-Office-1.7.0-linux-aarch64.run", "android/Nexus-Office-1.7.0-android-vpn.apk", "Nexus-Office-1.7.0-codigo-fuente.zip"):
        (base / rel).parent.mkdir(parents=True, exist_ok=True)
        (base / rel).write_bytes(b"x" * 10)
    d = downloads.listing()
    kinds = {f["name"]: f["platform"] for f in d["files"]}
    assert kinds["nexus-office_1.7.0_arm64.deb"] == "linux-aarch64" and kinds["Nexus-Office-1.7.0-android-vpn.apk"] == "android"
    assert kinds["Nexus-Office-Setup-1.7.0-windows-x64.exe"] == "windows-x64" and kinds["Nexus-Office-1.7.0-codigo-fuente.zip"] == "codigo"
    assert d["host"] in {p for p, _, _ in downloads.PLATFORMS}
    with TestClient(create_app(services)) as c:
        assert len(c.get("/api/downloads").json()["files"]) == 5
        r = c.get("/descargas/android/Nexus-Office-1.7.0-android-vpn.apk")
        assert r.status_code == 200 and r.content == b"x" * 10 and "android.package-archive" in r.headers["content-type"]
        assert c.get("/descargas/../config.yaml").status_code == 404
        st = c.get("/api/service").json()
        assert st["supported"] and "registered" in st
        assert c.post("/api/service", json={"action": "instalar"}).status_code in (401, 403)   # solo administrador


def test_appimage_y_flatpak_arrancan_el_paquete(monkeypatch, tmp_path):
    monkeypatch.setenv("APPIMAGE", "/home/pepe/Nexus-Office-x86_64.AppImage")
    assert service.command("127.0.0.1", 8775)[:2] == ["/home/pepe/Nexus-Office-x86_64.AppImage", "run"]
    assert service.workdir() != paths.ROOT
    monkeypatch.delenv("APPIMAGE")
    monkeypatch.setenv("FLATPAK_ID", "ai.nexus.NexusOffice")
    monkeypatch.setattr(service.Path, "home", classmethod(lambda cls: tmp_path))
    assert service.command("127.0.0.1", 8775)[:3] == ["/usr/bin/flatpak", "run", "ai.nexus.NexusOffice"]
    service.install("127.0.0.1", 8775)
    unit = tmp_path / ".config/systemd/user/nexus-office.service"
    assert "flatpak run ai.nexus.NexusOffice run --no-browser" in unit.read_text()
    assert service.uninstall() == [str(unit)]
