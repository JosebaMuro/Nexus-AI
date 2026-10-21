"""APK «Nexus-Office autonomo» de la edicion C++ (ARM64, Android 9+): la misma app que la de la edicion Python
(nexus-office/android-autonomo), pero el servidor es nexus-office compilado con el NDK en vez de Python.

    python packaging/build_apk_autonomo.py --exe <nexus-office para Android> --libs <carpeta con libllama-server*.so>
                                           --recursos <carpeta instalada con nexus/>

La APK lleva:
  lib/arm64-v8a/libnexusoffice.so          el ejecutable de Nexus-Office C++ (Android solo deja ejecutar lo que va aqui)
  lib/arm64-v8a/libllama-server*.so        llama-server estatico en 3 variantes de CPU (de la APK autonoma 2.0.0)
  assets/runtime.zip + runtime.json        la carpeta nexus/ (interfaz web, perfiles, agentes, certificados)
Se instala junto a la app de la edicion Python (identificador ai.nexus.office.cpp). Usa las herramientas de Android y
la firma de nexus-office/packaging/build_apk.py; las fuentes Java se toman de la app Python y se adaptan al compilar.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY_EDITION = ROOT.parent / "nexus-office"
sys.path.insert(0, str(PY_EDITION / "packaging"))
import build_apk as base  # noqa: E402

SRC = PY_EDITION / "android-autonomo"
OUT = ROOT / "build-paquetes" / "apk-autonomo"
APP_ID = "ai.nexus.office.cpp"
MIN_SDK = 28
LLAMA = ("libllama-server.so", "libllama-server-dotprod.so", "libllama-server-i8mm.so")


def _version() -> str:
    return re.search(r'set\(NEXUS_VERSION "([^"]+)"\)', (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)


VERSION = _version()
VERSION_CODE = 20000 + 50          # por encima de la 2.0.0 de Python (20000 + revision); es otra app, no la sustituye

LAUNCH_OLD_START = '        detail = "Arrancando Python...";'
LAUNCH_OLD_END = '        env.put("NEXUS_DEVICE_INFO", info.getAbsolutePath());'
LAUNCH_NEW = '''        detail = "Arrancando Nexus-Office...";
        String libs = getApplicationInfo().nativeLibraryDir;
        File data = getFilesDir();
        File tmp = new File(getCacheDir(), "tmp");
        tmp.mkdirs();
        // edicion C++: el servidor es un ejecutable nativo (libnexusoffice.so) y los recursos estan en el runtime
        List<String> cmd = new ArrayList<String>();
        cmd.add(new File(libs, "libnexusoffice.so").getAbsolutePath());
        cmd.add("run");
        cmd.add("--no-browser");
        cmd.add("--host");
        cmd.add("127.0.0.1");
        cmd.add("--port");
        cmd.add(String.valueOf(PORT));
        ProcessBuilder pb = new ProcessBuilder(cmd);
        Map<String, String> env = pb.environment();
        env.put("HOME", data.getAbsolutePath());
        env.put("TMPDIR", tmp.getAbsolutePath());
        env.put("LANG", "C.UTF-8");
        env.put("NEXUS_ROOT", runtime.getAbsolutePath());
        env.put("NEXUS_DATA_DIR", new File(data, "nexus").getAbsolutePath());
        env.put("NEXUS_ANDROID", "1");
        String llama = llamaBinary(libs);
        if (llama != null) env.put("NEXUS_RUNTIME_LLAMA", llama);
        env.put("NEXUS_DNS", dnsServers());
        env.put("NEXUS_DEVICE_STATE", new File(data, "device_state.json").getAbsolutePath());
        File info = new File(data, "device_info.json");
        writeDeviceInfo(info);
        env.put("NEXUS_DEVICE_INFO", info.getAbsolutePath());'''
LLAMA_JAVA = '''    /** La variante de llama-server mas rapida que admite esta CPU (i8mm > dotprod > ARMv8 basico). */
    private static String llamaBinary(String libs) {
        String feats = "";
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/proc/cpuinfo"));
            String l;
            while ((l = r.readLine()) != null) {
                if (l.toLowerCase().startsWith("features")) feats += " " + l.substring(l.indexOf(':') + 1) + " ";
            }
            r.close();
        } catch (IOException e) {
            // sin /proc/cpuinfo: la variante basica
        }
        List<String> names = new ArrayList<String>();
        if (feats.contains(" i8mm ") && feats.contains(" asimddp ")) names.add("libllama-server-i8mm.so");
        if (feats.contains(" asimddp ")) names.add("libllama-server-dotprod.so");
        names.add("libllama-server.so");
        for (String n : names) {
            File f = new File(libs, n);
            if (f.isFile()) return f.getAbsolutePath();
        }
        return null;
    }

    private Result launch(File runtime) throws Exception {'''


def _replace(text: str, old: str, new: str, what: str) -> str:
    if old not in text:
        sys.exit(f"no encuentro {what} en las fuentes de la app Python: revisa build_apk_autonomo.py")
    return text.replace(old, new)


def sources(dest: Path) -> None:
    """Copia la app de la edicion Python y la adapta al ejecutable C++."""
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(SRC, dest, ignore=shutil.ignore_patterns("native", "README.md"))
    pkg = dest / "src" / "ai" / "nexus" / "office" / "autonomo"
    svc = pkg / "NexusServerService.java"
    s = svc.read_text(encoding="utf-8")
    a, b = s.index(LAUNCH_OLD_START), s.index(LAUNCH_OLD_END) + len(LAUNCH_OLD_END)
    s = s[:a] + LAUNCH_NEW + s[b:]
    s = _replace(s, "    private Result launch(File runtime) throws Exception {", LLAMA_JAVA, "launch()")
    s = re.sub(r"(private void linkMusl\(File dir\) throws Exception \{).*?\n    \}",
               r"\1\n        // la edicion C++ no usa la libc de musl\n    }", s, count=1, flags=re.S)
    svc.write_text(s, encoding="utf-8")
    ri = pkg / "RuntimeInstaller.java"
    r = ri.read_text(encoding="utf-8")
    r = re.sub(r'String\[\] must = \{[^}]*\};', 'String[] must = {".completo", "nexus/web/index.html", "nexus/certs/cacert.pem"};',
               r, count=1, flags=re.S)
    r = r.replace("Descomprimiendo Python y Nexus-Office", "Descomprimiendo Nexus-Office")
    ri.write_text(r, encoding="utf-8")
    strings = dest / "res" / "values" / "strings.xml"
    strings.write_text(strings.read_text(encoding="utf-8").replace(
        '<string name="app_name">Nexus-Office</string>', '<string name="app_name">Nexus-Office C++</string>'), encoding="utf-8")


def runtime_zip(resources: Path, dest: Path) -> dict:
    """La carpeta nexus/ comprimida y {"files", "bytes"}: la app lo comprueba tras descomprimir."""
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted((resources / "nexus").rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(resources).as_posix())
        infos = [i for i in z.infolist() if not i.is_dir()]
    return {"files": len(infos), "bytes": sum(i.file_size for i in infos), "version": VERSION}


def build(exe: Path, libs: Path, resources: Path) -> Path:
    missing = [str(p) for p in [exe, *(libs / n for n in LLAMA), resources / "nexus" / "web" / "index.html"] if not p.is_file()]
    if missing:
        sys.exit(f"faltan: {', '.join(missing)}")
    aapt2, zipalign = base.tool("aapt2"), base.tool("zipalign")
    apksigner = base.tool("apksigner", bat=True)
    d8 = base.tool("d8", bat=True)
    javac = (base.JDK / "bin" / f"javac{base.EXE}") if base.JDK.exists() else Path(shutil.which("javac") or "javac")
    if not all((aapt2, zipalign, apksigner, d8)):
        sys.exit("Faltan herramientas de Android (aapt2, zipalign, apksigner, d8)")
    api_jar = base.android_jar()
    shutil.rmtree(OUT, ignore_errors=True)
    for d in ("gen", "classes", "dex"):
        (OUT / d).mkdir(parents=True)
    app = OUT / "app"
    sources(app)
    meta = runtime_zip(resources, OUT / "runtime.zip")
    print(f"[apk] runtime: {meta['files']} ficheros, {meta['bytes'] / 1e6:.0f} MB -> "
          f"{(OUT / 'runtime.zip').stat().st_size / 1e6:.0f} MB")
    base.icons(app / "res", shield=False)
    base.run([aapt2, "compile", "--dir", app / "res", "-o", OUT / "res.zip"])
    base.run([aapt2, "link", "-o", OUT / "base.apk", "-I", api_jar, "--manifest", app / "AndroidManifest.xml",
              "--rename-manifest-package", APP_ID, "-A", app / "assets", "--java", OUT / "gen",
              "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", "34",
              "--version-code", str(VERSION_CODE), "--version-name", VERSION, OUT / "res.zip"])
    srcs = [str(p) for p in (app / "src").rglob("*.java")] + [str(p) for p in (OUT / "gen").rglob("*.java")]
    base.run([javac, "-encoding", "UTF-8", "--release", "8", "-Xlint:-options", "-nowarn", "-cp", api_jar,
              "-d", OUT / "classes", *srcs])
    base.run([d8, "--release", "--min-api", str(MIN_SDK), "--lib", api_jar, "--output", OUT / "dex",
              *[str(p) for p in (OUT / "classes").rglob("*.class")]])
    with zipfile.ZipFile(OUT / "base.apk", "a", zipfile.ZIP_DEFLATED) as z:
        z.write(OUT / "dex" / "classes.dex", "classes.dex")
        z.write(exe, "lib/arm64-v8a/libnexusoffice.so")
        for n in LLAMA:
            z.write(libs / n, f"lib/arm64-v8a/{n}")
        z.write(OUT / "runtime.zip", "assets/runtime.zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("assets/runtime.json", json.dumps(meta))
        z.writestr("assets/LICENSES/LEEME.txt",
                   "Nexus-Office C++; llama.cpp y ggml (MIT), cpp-httplib (MIT), nlohmann/json (MIT), SQLite (dominio "
                   "publico), miniz (MIT), PCRE2 (BSD), rapidyaml (MIT), stb (dominio publico/MIT), mbedTLS (Apache 2.0), "
                   "LLVM libc++ (Apache 2.0 con excepcion LLVM), musl (MIT).\n")
    base.run([zipalign, "-f", "-p", "4", OUT / "base.apk", OUT / "aligned.apk"])
    ks, pw = base.keystore()
    dest = ROOT / "dist-apk" / f"Nexus-Office-{VERSION}-android-autonomo.apk"
    dest.parent.mkdir(exist_ok=True)
    base.run([apksigner, "sign", "--ks", ks, "--ks-key-alias", "nexus", "--ks-pass", f"pass:{pw}", "--key-pass", f"pass:{pw}",
              "--min-sdk-version", str(MIN_SDK), "--out", dest, OUT / "aligned.apk"])
    base.run([apksigner, "verify", "--min-sdk-version", str(MIN_SDK), dest])
    print(f"[apk] -> {dest} ({dest.stat().st_size / 1e6:.1f} MB)")
    return dest


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--libs", type=Path, required=True)
    ap.add_argument("--recursos", type=Path, required=True)
    a = ap.parse_args()
    build(a.exe, a.libs, a.recursos)
