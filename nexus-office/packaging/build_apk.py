"""Compila la app Android «Nexus-Office VPN» (APK firmado) sin Gradle ni Android Studio.

    venv/python packaging/build_apk.py            # -> dist/Nexus-Office-<version>-android-vpn.apk

Herramientas (se usa lo primero que haya):
  1. Android SDK en .cache/android/: jdk/ (JDK 17), build-tools/ (aapt2, d8, zipalign, apksigner) y
     platform/android.jar (android-34). Ver docs/PLATAFORMAS.md.
  2. Paquetes de Debian/Ubuntu/WSL:  sudo apt install aapt zipalign apksigner dalvik-exchange
     android-sdk-platform-23 openjdk-17-jdk-headless   (+ android.jar de API 34 en .cache/android/platform/
     o en la variable NEXUS_ANDROID_JAR; vale el android-all de Robolectric).

WireGuard: se descarga el paquete oficial com.wireguard.android:tunnel de Maven Central (huella SHA-256
fijada) y se incluye su libwg-go.so para cada arquitectura (arm64, arm, x86_64, x86).
Pasos: iconos -> aapt2 compile/link -> javac -> d8/dx -> classes.dex + libs -> zipalign -> apksigner.

La app autonoma (Nexus-Office completo dentro del movil) es android-autonomo/: packaging/build_apk_autonomo.py.
"""
from __future__ import annotations

import hashlib
import os
import secrets
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "android"
SDK = ROOT / ".cache" / "android"
BT, JDK = SDK / "build-tools", SDK / "jdk"
OUT = ROOT / "build" / "apk"
sys.path.insert(0, str(ROOT))
from nexus import __version__ as VERSION  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
BAT = ".bat" if os.name == "nt" else ""
ENV = {**os.environ}
if JDK.exists():
    ENV.update({"JAVA_HOME": str(JDK), "PATH": f"{JDK / 'bin'}{os.pathsep}{os.environ.get('PATH', '')}"})

WG_VERSION = "1.0.20230706"
WG_SHA256 = "df8f5358d3a350d707b5614a493fc3e4dbd0e60a8aae9d9a0cd3fe10d94a2f64"
WG_URL = f"https://repo.maven.apache.org/maven2/com/wireguard/android/tunnel/{WG_VERSION}/tunnel-{WG_VERSION}.aar"
ABIS = ("arm64-v8a", "armeabi-v7a", "x86_64", "x86")


def run(cmd: list, **kw) -> None:
    print("[apk]", " ".join(Path(str(c)).name if i == 0 else str(c) for i, c in enumerate(cmd[:3])), "...")
    subprocess.run([str(c) for c in cmd], check=True, env=ENV, **kw)


def tool(name: str, *alts: str, bat: bool = False) -> Path | None:
    """Herramienta del SDK (.cache/android/build-tools) o, si no, del sistema."""
    p = BT / f"{name}{BAT if bat else EXE}"
    if p.exists():
        return p
    for n in (name, *alts):
        found = shutil.which(n)
        if found:
            return Path(found)
    return None


def android_jar() -> Path:
    for p in (os.environ.get("NEXUS_ANDROID_JAR", ""), SDK / "platform" / "android.jar"):
        if p and Path(p).exists():
            return Path(p)
    sys.exit("Falta android.jar (API 34): copia platforms/android-34/android.jar del SDK a .cache/android/platform/ "
             "o indica su ruta en NEXUS_ANDROID_JAR (ver docs/PLATAFORMAS.md).")


def link_jar(api_jar: Path) -> Path:
    """Para enlazar recursos vale el android.jar de la plataforma; si es el de Robolectric y el aapt2 es antiguo,
    se usa el android-23 del sistema (los recursos del marco que usamos existen desde siempre)."""
    deb = Path("/usr/lib/android-sdk/platforms/android-23/android.jar")
    return deb if "android-all" in api_jar.name and deb.exists() else api_jar


def icons(res_dir: Path | None = None, shield: bool = True) -> None:
    """Icono del lanzador: nucleo luminoso sobre fondo oscuro, con escudo en la app VPN, en todas las densidades.
    La app autonoma usa el mismo sin escudo:  python -c "import build_apk; build_apk.icons(<res>, False)"."""
    for dpi, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        S = size * 4
        img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([0, 0, S - 1, S - 1], radius=S // 5, fill=(4, 6, 12, 255))
        glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        g = ImageDraw.Draw(glow)
        g.ellipse([S * .2, S * .2, S * .8, S * .8], fill=(0, 240, 255, 150))
        glow = glow.filter(ImageFilter.GaussianBlur(S // 14))
        img.alpha_composite(glow)
        d.ellipse([S * .3, S * .3, S * .7, S * .7], fill=(0, 240, 255, 255))
        d.ellipse([S * .38, S * .34, S * .52, S * .48], fill=(220, 255, 255, 255))
        d.arc([S * .16, S * .16, S * .84, S * .84], 200, 330, fill=(255, 61, 242, 255), width=S // 28)
        d.arc([S * .16, S * .16, S * .84, S * .84], 20, 150, fill=(0, 240, 255, 255), width=S // 28)
        try:
            font = ImageFont.truetype("segoeuib.ttf" if os.name == "nt" else "DejaVuSans-Bold.ttf", int(S * .22))
            d.text((S / 2, S / 2), "N", fill=(0, 18, 26, 255), font=font, anchor="mm")
        except OSError:
            pass
        # escudo pequeno abajo a la derecha: distingue la app VPN de la autonoma
        if shield:
            sx, sy, w = S * .66, S * .64, S * .26
            d.polygon([(sx, sy), (sx + w, sy), (sx + w, sy + w * .55), (sx + w / 2, sy + w * 1.05), (sx, sy + w * .55)],
                      fill=(255, 61, 242, 255), outline=(4, 6, 12, 255), width=max(2, S // 90))
        dest = (res_dir or SRC / "res") / f"mipmap-{dpi}" / "ic_launcher.png"
        dest.parent.mkdir(parents=True, exist_ok=True)
        img.resize((size, size), Image.LANCZOS).save(dest)


def wireguard_libs() -> dict[str, bytes]:
    """{abi: libwg-go.so} del paquete oficial de WireGuard para Android (Apache 2.0)."""
    aar = SDK / f"wireguard-tunnel-{WG_VERSION}.aar"
    if not aar.exists():
        aar.parent.mkdir(parents=True, exist_ok=True)
        print(f"[apk] descargando WireGuard {WG_VERSION} de Maven Central ...")
        with urllib.request.urlopen(WG_URL, timeout=120) as r:
            aar.write_bytes(r.read())
    digest = hashlib.sha256(aar.read_bytes()).hexdigest()
    if digest != WG_SHA256:
        aar.unlink()
        sys.exit(f"La huella de {aar.name} no coincide ({digest}); descarga cancelada por seguridad.")
    with zipfile.ZipFile(aar) as z:
        return {abi: z.read(f"jni/{abi}/libwg-go.so") for abi in ABIS}


# revision de las APK dentro de una misma version de Nexus-Office (sube el versionCode para que Android las instale
# como actualizacion): 1.6.0 revision 1 = llama.cpp con GPU (Vulkan/OpenCL) y deteccion de CPU/GPU/NPU
APK_REVISION = 1


def version_code() -> int:
    return int("".join(f"{int(x):02d}" for x in VERSION.split("."))) + APK_REVISION   # 1.6.0 rev 1 -> 10601


def keystore() -> tuple[Path, str]:
    ks, pw_file = SDK / "nexus-release.jks", SDK / "nexus-release.pass"
    if not ks.exists():
        ks.parent.mkdir(parents=True, exist_ok=True)
        pw = secrets.token_urlsafe(16)
        keytool = JDK / "bin" / f"keytool{EXE}" if JDK.exists() else Path(shutil.which("keytool") or "keytool")
        run([keytool, "-genkeypair", "-keystore", ks, "-alias", "nexus", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-storepass", pw, "-keypass", pw, "-dname", "CN=Nexus-Office, O=Nexus-Office, C=ES"])
        pw_file.write_text(pw)
    return ks, pw_file.read_text().strip()


def build() -> Path:
    aapt2, zipalign = tool("aapt2"), tool("zipalign")
    apksigner = tool("apksigner", bat=True)
    d8, dx = tool("d8", bat=True), tool("dalvik-exchange", "dx")
    javac = (JDK / "bin" / f"javac{EXE}") if JDK.exists() else Path(shutil.which("javac") or "javac")
    missing = [n for n, t in (("aapt2", aapt2), ("zipalign", zipalign), ("apksigner", apksigner), ("d8 o dx", d8 or dx)) if not t]
    if missing:
        sys.exit(f"Faltan herramientas de Android: {', '.join(missing)}. Ver docs/PLATAFORMAS.md (SDK o paquetes de Ubuntu).")
    api_jar = android_jar()
    shutil.rmtree(OUT, ignore_errors=True)
    for d in ("gen", "classes", "dex"):
        (OUT / d).mkdir(parents=True)
    icons()
    libs = wireguard_libs()
    code = version_code()
    run([aapt2, "compile", "--dir", SRC / "res", "-o", OUT / "res.zip"])
    run([aapt2, "link", "-o", OUT / "base.apk", "-I", link_jar(api_jar), "--manifest", SRC / "AndroidManifest.xml",
         "-A", SRC / "assets", "--java", OUT / "gen", "--min-sdk-version", "24", "--target-sdk-version", "34",
         "--version-code", str(code), "--version-name", VERSION, OUT / "res.zip"])
    sources = [str(p) for p in (SRC / "src").rglob("*.java")] + [str(p) for p in (OUT / "gen").rglob("*.java")]
    run([javac, "-encoding", "UTF-8", "--release", "8", "-Xlint:-options", "-nowarn", "-cp", api_jar,
         "-d", OUT / "classes", *sources])
    classes = [str(p) for p in (OUT / "classes").rglob("*.class")]
    if d8:
        run([d8, "--release", "--min-api", "24", "--lib", api_jar, "--output", OUT / "dex", *classes])
    else:   # dx (dalvik-exchange) de las distribuciones: el codigo de la app no usa lambdas, no hace falta desugar
        run([dx, "--dex", "--min-sdk-version=24", f"--output={OUT / 'dex' / 'classes.dex'}", OUT / "classes"])
    with zipfile.ZipFile(OUT / "base.apk", "a", zipfile.ZIP_DEFLATED) as z:
        z.write(OUT / "dex" / "classes.dex", "classes.dex")
        for abi, data in libs.items():
            z.writestr(f"lib/{abi}/libwg-go.so", data)
        z.writestr("assets/LICENSES/WireGuard-Apache-2.0.txt",
                   "libwg-go.so: WireGuard para Android (https://git.zx2c4.com/wireguard-android), "
                   "Copyright (c) 2017-2023 WireGuard LLC. Apache License 2.0.\n")
    run([zipalign, "-f", "-p", "4", OUT / "base.apk", OUT / "aligned.apk"])
    ks, pw = keystore()
    dest = ROOT / "dist" / f"Nexus-Office-{VERSION}-android-vpn.apk"
    dest.parent.mkdir(exist_ok=True)
    run([apksigner, "sign", "--ks", ks, "--ks-key-alias", "nexus", "--ks-pass", f"pass:{pw}", "--key-pass", f"pass:{pw}",
         "--min-sdk-version", "24", "--out", dest, OUT / "aligned.apk"])
    run([apksigner, "verify", "--verbose", "--min-sdk-version", "24", dest])
    print(f"[apk] -> {dest} ({dest.stat().st_size / 1e6:.2f} MB)")
    return dest


if __name__ == "__main__":
    build()
