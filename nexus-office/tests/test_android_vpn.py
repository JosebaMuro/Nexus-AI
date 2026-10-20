"""La configuracion WireGuard que genera la app Android (WgConfig.java) a partir del codigo del PC.
Se ejecuta si hay javac/java y org.json (.cache/android/json.jar o NEXUS_JSON_JAR); si no, se omite.
La prueba completa con tuneles reales esta en packaging/test_vpn_e2e.sh (Linux, root)."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from nexus.remote import vpn

ROOT = Path(__file__).resolve().parent.parent
JAR = Path(os.environ.get("NEXUS_JSON_JAR", ROOT / ".cache" / "android" / "json.jar"))


@pytest.mark.skipif(not (shutil.which("javac") and shutil.which("java") and JAR.exists()), reason="sin Java u org.json")
def test_app_builds_uapi_from_pc_code(services, tmp_path):
    services.config.set("vpn.endpoints", ["127.0.0.1"])
    r = services.vpn.add_peer("Movil", "clave")
    src = [ROOT / "android/src/ai/nexus/office/client/WgConfig.java", ROOT / "android/test/ai/nexus/office/client/WgConfigCheck.java"]
    subprocess.run(["javac", "--release", "8", "-Xlint:-options", "-nowarn", "-cp", str(JAR), "-d", str(tmp_path),
                    *map(str, src)], check=True, capture_output=True)
    out = subprocess.run(["java", "-cp", f"{tmp_path}{os.pathsep}{JAR}", "ai.nexus.office.client.WgConfigCheck", r["code"]],
                         check=True, capture_output=True, text=True).stdout
    code = vpn.decode_code(r["code"])
    hexkey = lambda b: __import__("base64").b64decode(b).hex()   # noqa: E731
    assert f"private_key={hexkey(code['k'])}" in out and f"public_key={hexkey(code['s'])}" in out
    assert f"preshared_key={hexkey(code['p'])}" in out and "endpoint=127.0.0.1:51820" in out
    assert "allowed_ip=10.78.0.1/32" in out and "#url=http://10.78.0.1:8775/?key=clave" in out
