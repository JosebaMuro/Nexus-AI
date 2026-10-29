// Instaladores de Nexus-Office (edicion C++). Todos son 100 % portables y se instalan sin internet: llevan el
// ejecutable nativo (nexus-office), la interfaz web y los motores de IA (llama-server y sd-cli en sus variantes, piper
// y whisper-cli); solo los modelos de IA se descargan, si hay red, elegidos segun el hardware.
//
//   nexus-construir paquetes                                   todas las plataformas
//   nexus-construir paquetes --targets linux-x86_64 windows-arm64
//   nexus-construir paquetes --targets --apk dist-apk/*.apk --docs *.pdf --keep   solo codigo fuente, LEEME y sumas
//
// Entradas (en .cache/, las deja .github/workflows/cpp-entrega.yml):
//   .cache/cpp/<target>/Nexus-Office/                carpeta instalada por «cmake --install» en cada sistema (cpp.yml)
//   .cache/desktop-runtimes/<target>/llama|sd/       llama-server y sd-cli
//   .cache/desktop-runtimes/<target>/piper|whisper/  voz: piper (rhasspy/piper) y whisper-cli (whisper.cpp)
//   .cache/desktop-runtimes/<target>/litert|onnx/    motores LiteRT-LM (Google) y ONNX Runtime GenAI (Microsoft)
//   .cache/drivers/<target>/                         drivers USB-serie (Windows y macOS)
// Salida en dist/ (NEXUS_DIST):
//   windows/  Nexus-Office-Setup-<ver>-windows-x64.exe / -arm64.exe     instalador grafico (NSIS; makensis)
//   linux/    Nexus-Office-<ver>-linux-<x86_64|aarch64>.run / .AppImage / .flatpak, nexus-office-c_<ver>_<amd64|arm64>.deb
//   macos/    Nexus-Office-<ver>-macos-<arm64|x86_64>.tar.gz              abre «Nexus-Office.command»
//   android/  las APK de --apk
//   Nexus-Office-<ver>-codigo-fuente.zip, LEEME.txt, SHA256SUMS.txt, grafo de la Wikipedia y resumen del proyecto
#include <ctime>

#include "comun.h"
#include "util/archive.h"
#include "util/http.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {

struct Target {
  std::string os, deb, note;
};
const std::vector<std::pair<std::string, Target>> TARGETS = {
    {"windows-x64", {"windows", "", ""}},
    {"windows-arm64", {"windows", "", ""}},
    {"linux-x86_64", {"linux", "amd64", "PC Linux, Steam Deck (SteamOS), servidores"}},
    {"linux-aarch64", {"linux", "arm64", "Raspberry Pi 4/5 (64 bits), NVIDIA Jetson, portatiles y consolas portatiles ARM"}},
    {"macos-arm64", {"macos", "", ""}},
    {"macos-x86_64", {"macos", "", ""}},
};
const char* RUNTIMES[] = {"llama", "sd", "piper", "whisper", "litert", "onnx"};
const std::set<std::string> EXECUTABLES = {"nexus-office", "llama-server", "sd-cli", "piper", "whisper-cli", "espeak-ng"};
const std::string APP_ID = "ai.nexus.NexusOfficeCpp";
const std::string FLATHUB = "https://dl.flathub.org/repo/flathub.flatpakrepo";
const std::string FLATPAK_RUNTIME = "24.08";

const Target& target(const std::string& t) {
  for (auto& [k, v] : TARGETS)
    if (k == t) return v;
  throw Fallo("plataforma desconocida: " + t);
}

std::string arch_of(const std::string& t) { return t.substr(t.find('-') + 1); }

fs::path dist() {
  std::string e = env("NEXUS_DIST");
  return e.empty() ? raiz() / "dist" : P(e);
}
fs::path build_dir() { return raiz() / "build-paquetes"; }
fs::path cache() { return raiz() / ".cache"; }

std::string numver() {   // 2.2.0 (NSIS y metadatos que solo admiten numeros)
  static const Rx re(R"(^\d+\.\d+\.\d+)");
  std::string v = version();
  auto m = re.search(v);
  return m ? m->str() : v;
}

std::string deb_version() {   // 2.2.0~cpp: dpkg la ordena antes que la 2.2.0
  std::string v = version();
  size_t i = v.find('-');
  if (i != std::string::npos) v[i] = '~';
  return replace_all(v, "-", ".");
}

fs::path out_file(const std::string& t, const std::string& name) {
  fs::path d = dist() / target(t).os;
  fs::create_directories(d);
  return d / name;
}

void hecho(const fs::path& p) { log("build", "-> " + S(p.filename()) + " (" + mb(p) + ")"); }

// ------------------------------------------------------------------ carpeta del paquete
void mac_launchers(const fs::path& dest) {
  escribir(dest / "Nexus-Office.command",
           "#!/bin/bash\ncd \"$(dirname \"$0\")\"\n[ -f data/.preparado ] || { ./install.sh && mkdir -p data && "
           "touch data/.preparado; }\nexec ./start.sh\n",
           0755);
  fs::path app = dest / "Nexus-Office.app" / "Contents";
  escribir(app / "Info.plist", cat(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Nexus-Office</string><key>CFBundleIdentifier</key><string>ai.nexus.office.cpp</string>
<key>CFBundleVersion</key><string>)", numver(), "</string><key>CFBundleShortVersionString</key><string>", version(),
                                   R"(</string>
<key>CFBundleExecutable</key><string>nexus</string>
<key>CFBundlePackageType</key><string>APPL</string><key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
)"));
  escribir(app / "MacOS" / "nexus",
           "#!/bin/bash\nDIR=\"$(cd \"$(dirname \"$0\")/../../..\" && pwd)\"\nopen -a Terminal \"$DIR/Nexus-Office.command\"\n",
           0755);
}

fs::path stage(const std::string& t) {
  fs::path src = cache() / "cpp" / t / "Nexus-Office";
  fs::path exe = src / (starts_with(t, "windows") ? "nexus-office.exe" : "nexus-office");
  if (!is_file(exe)) throw Fallo("falta " + S(exe) + ": descarga el artefacto cpp-" + t + " de cpp.yml en .cache/cpp/" + t + "/");
  fs::path dest = build_dir() / t / "Nexus-Office";
  rm_rf(dest.parent_path());
  copiar_arbol(src, dest);
  fs::path scripts = raiz() / "packaging" / "scripts";
  for (const char* name : starts_with(t, "windows") ? std::vector<const char*>{"install.bat", "start.bat"}
                                                    : std::vector<const char*>{"install.sh", "start.sh"})
    copiar(scripts / name, dest / name);
  for (auto& f : glob(raiz() / "docs", "*.md")) copiar(f, dest / "docs" / f.filename());
  for (const char* name : {"README.md", "CHANGELOG.md", "LICENSE"}) copiar(raiz() / name, dest / name);
  // ejemplos para importar: el grafo multicapa de la Wikipedia en espanol y el resumen del proyecto para la biblioteca
  copiar_arbol(raiz() / "ejemplos", dest / "ejemplos");
  // firmware de las luces NexusLED, el reloj NexusWatch y el Pico W (binarios ya compilados; se graban con
  // nexus-flash, dentro de Nexus-Office) y drivers USB-serie de los puentes de las placas
  fs::path fw = repo() / "firmware";
  if (is_dir(fw / "bin")) {
    copiar_arbol(fw / "bin", dest / "firmware" / "bin");
    if (is_file(fw / "README.md")) copiar(fw / "README.md", dest / "firmware" / "README.md");
    fs::path drv = cache() / "drivers" / t;
    std::string so = starts_with(t, "windows") ? "windows" : starts_with(t, "macos") ? "macos" : "";
    if (!so.empty() && is_dir(drv) && !fs::is_empty(drv)) {
      copiar_arbol(drv, dest / "firmware" / "drivers" / so);
      std::vector<std::string> n;
      for (auto& e : fs::directory_iterator(drv)) n.push_back(S(e.path().filename()));
      std::sort(n.begin(), n.end());
      log("build", t + ": drivers " + join(n, ", "));
    }
    std::vector<std::string> n;
    for (auto& e : fs::directory_iterator(fw / "bin")) n.push_back(S(e.path().filename()));
    std::sort(n.begin(), n.end());
    log("build", t + ": firmware " + join(n, ", "));
  }
  for (const char* rt : RUNTIMES) {
    fs::path r = cache() / "desktop-runtimes" / t / rt;
    if (is_dir(r) && !fs::is_empty(r)) {
      copiar_arbol(r, dest / "bundled" / "runtimes" / rt);
      std::vector<std::string> n;
      for (auto& e : fs::directory_iterator(r)) n.push_back(S(e.path().filename()));
      std::sort(n.begin(), n.end());
      log("build", cat(t, ": ", rt, " ", join(n, ", ")));
    } else {
      log("build", cat(t, ": sin ", rt, " incluido"));
    }
  }
  if (target(t).os == "macos") mac_launchers(dest);
  escribir(dest / "PLATAFORMA.txt", cat("Nexus-Office ", version(), " (edicion C++) - ", t, "\n", target(t).note, "\n"));
  return dest;
}

int file_mode(const std::string& name, bool is_dir) {
  std::string base = name.substr(name.rfind('/') + 1);
  if (is_dir || EXECUTABLES.count(base) || ends_with(name, ".sh") || ends_with(name, ".command") ||
      contains(name, "/MacOS/") || (contains(name, "/bundled/runtimes/") && base.find('.') == std::string::npos))
    return 0755;
  return 0644;
}

// La carpeta entera dentro de un tar.gz con el nombre arc (modos normalizados y propietario root)
void tar_tree(archive::TarGzWriter& tar, const fs::path& dir, const std::string& arc) {
  int64_t mtime = (int64_t)std::time(nullptr);
  tar.add_dir(arc, 0755, mtime);
  std::vector<fs::directory_entry> all;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    all.push_back(*it);
  std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.path().generic_string() < b.path().generic_string(); });
  for (auto& e : all) {
    std::string name = arc + "/" + fs::relative(e.path(), dir).generic_string();
    if (e.is_symlink(ec))
      tar.add_symlink(name, fs::read_symlink(e.path(), ec).generic_string(), mtime);
    else if (e.is_directory(ec))
      tar.add_dir(name, 0755, mtime);
    else
      tar.add_file(name, e.path(), file_mode(name, false), mtime);
  }
}

fs::path tarball(const std::string& t, const fs::path& dest) {
  fs::path out = out_file(t, cat("Nexus-Office-", version(), "-", t, ".tar.gz"));
  archive::TarGzWriter tar(out);
  tar_tree(tar, dest, "Nexus-Office");
  if (!tar.close()) throw Fallo("no puedo escribir " + S(out));
  hecho(out);
  return out;
}

// ------------------------------------------------------------------ Linux: .run, .deb, AppImage, Flatpak
const char* RUN_HEADER = R"SH(#!/bin/sh
# Nexus-Office {version} (edicion C++) - instalador autoextraible para Linux ({arch}).
# Uso: sh {name} [carpeta_destino] [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto|todos|no]
# Todo va incluido (programa nativo y motores de IA): no necesita internet salvo para descargar los modelos.
set -e
DEFAULT="$HOME/.local/share/nexus-office-c"
[ "$(id -u)" = 0 ] && DEFAULT=/opt/nexus-office-c       # con sudo: para todo el equipo
DEST="${1:-$DEFAULT}"
case "$DEST" in --*) DEST="$DEFAULT";; *) [ $# -gt 0 ] && shift;; esac
echo "[Nexus-Office] Instalando en $DEST"
mkdir -p "$DEST"
LINE=$(awk '/^__NEXUS_ARCHIVE__$/{print NR + 1; exit 0}' "$0")
tail -n +"$LINE" "$0" | tar -xz -C "$DEST" --strip-components=1
cd "$DEST"
./install.sh "$@"
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ]; then chown -R "$SUDO_USER" "$DEST"; fi
exit 0
__NEXUS_ARCHIVE__
)SH";

void append_file(std::FILE* out, const fs::path& src) {
  std::FILE* in =
#ifdef _WIN32
      _wfopen(src.c_str(), L"rb");
#else
      std::fopen(src.c_str(), "rb");
#endif
  if (!in) throw Fallo("no puedo leer " + S(src));
  std::vector<char> buf(1 << 20);
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
    if (std::fwrite(buf.data(), 1, n, out) != n) {
      std::fclose(in);
      throw Fallo("no puedo escribir");
    }
  std::fclose(in);
}

std::FILE* open_out(const fs::path& p) {
#ifdef _WIN32
  std::FILE* f = _wfopen(p.c_str(), L"wb");
#else
  std::FILE* f = std::fopen(p.c_str(), "wb");
#endif
  if (!f) throw Fallo("no puedo escribir " + S(p));
  return f;
}

fs::path run_installer(const std::string& t, const fs::path& tgz) {
  std::string arch = arch_of(t);
  std::string name = cat("Nexus-Office-", version(), "-linux-", arch, ".run");
  fs::path out = out_file(t, name);
  std::string head = replace_all(replace_all(replace_all(RUN_HEADER, "{version}", version()), "{arch}", arch), "{name}", name);
  std::FILE* f = open_out(out);
  std::fwrite(head.data(), 1, head.size(), f);
  append_file(f, tgz);
  if (std::fclose(f) != 0) throw Fallo("no puedo escribir " + S(out));
  modo(out, 0755);
  hecho(out);
  return out;
}

const char* DEB_LAUNCHER = R"SH(#!/bin/sh
# Nexus-Office (edicion C++): programa en /opt/nexus-office-c (portable, sin internet), datos por usuario.
#   nexus-office-c                       arranca Nexus-Office y abre el navegador
#   nexus-office-c servicio instalar     arranque automatico (con sudo: con el equipo; --red: accesible desde moviles)
#   nexus-office-c modelos [--descargar] modelos de IA para este equipo | nexus-office-c oficina <perfil>
if [ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ]; then
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$(getent passwd "$SUDO_USER" | cut -d: -f6)/.local/share/nexus-office-c}"
else
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$HOME/.local/share/nexus-office-c}"
fi
mkdir -p "$NEXUS_DATA_DIR"
EXE=/opt/nexus-office-c/nexus-office
case "$1" in
  run|servicio|modelos|oficina|detect|pull|chat|comprobar|firmware|grafo|--version|help|--help) exec "$EXE" "$@";;
esac
if [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$EXE" modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$EXE" run "$@"
)SH";

// ar con los miembros en orden (ficheros o bytes)
void write_ar(const fs::path& out, const std::vector<std::pair<std::string, std::pair<std::string, fs::path>>>& members) {
  std::FILE* f = open_out(out);
  std::fwrite("!<arch>\n", 1, 8, f);
  long long now_s = (long long)std::time(nullptr);
  for (auto& [name, m] : members) {
    uint64_t size = m.second.empty() ? m.first.size() : fsize(m.second);
    char h[61];
    std::snprintf(h, sizeof h, "%-16s%-12lld%-6d%-6d%-8s%-10llu`\n", name.c_str(), now_s, 0, 0, "100644",
                  (unsigned long long)size);
    std::fwrite(h, 1, 60, f);
    if (m.second.empty())
      std::fwrite(m.first.data(), 1, m.first.size(), f);
    else
      append_file(f, m.second);
    if (size % 2) std::fputc('\n', f);
  }
  if (std::fclose(f) != 0) throw Fallo("no puedo escribir " + S(out));
}

fs::path deb(const std::string& t, const fs::path& dest) {
  std::string arch = target(t).deb;
  fs::path work = build_dir() / ("deb-" + t);
  fs::create_directories(work);
  fs::path data = work / "data.tar.gz";
  int64_t mtime = (int64_t)std::time(nullptr);
  {
    archive::TarGzWriter tar(data);
    for (const char* d : {"./", "./opt/", "./usr/", "./usr/bin/", "./usr/share/", "./usr/share/applications/"})
      tar.add_dir(d, 0755, mtime);
    tar_tree(tar, dest, "./opt/nexus-office-c");
    tar.add_bytes("./usr/bin/nexus-office-c", DEB_LAUNCHER, 0755, mtime);
    tar.add_bytes("./usr/share/applications/nexus-office-c.desktop", R"([Desktop Entry]
Type=Application
Name=Nexus-Office (C++)
Comment=Oficina de IA local para pymes (edicion C++)
Exec=nexus-office-c
Icon=/opt/nexus-office-c/nexus/web/icon.svg
Terminal=true
Categories=Office;Utility;
)",
                  0644, mtime);
    if (!tar.close()) throw Fallo("no puedo escribir " + S(data));
  }
  uint64_t size_kb = tamano_arbol(dest) / 1024;
  std::string control = cat("Package: nexus-office-c\nVersion: ", deb_version(), R"(
Section: utils
Priority: optional
Architecture: )", arch, R"(
Depends: libc6 (>= 2.35)
Recommends: libvulkan1, libgomp1, ca-certificates
Installed-Size: )", size_kb, R"(
Maintainer: Nexus-Office <nexus@localhost>
Description: Oficina de IA local para pymes (edicion C++)
 Un unico programa nativo con la misma interfaz que Nexus-Office: agentes por gremio, memoria,
 documentos y voz. Incluye los motores de IA: se instala sin internet. Detecta el hardware
 (CPU, GPU, NPU) y elige el modelo de IA adecuado. Puede funcionar como servicio.
)");
  std::string prerm = "#!/bin/sh\n/opt/nexus-office-c/nexus-office servicio quitar >/dev/null 2>&1 || true\nexit 0\n";
  fs::path ctrl = work / "control.tar.gz";
  {
    archive::TarGzWriter tar(ctrl, 9);
    tar.add_dir("./", 0755, mtime);
    tar.add_bytes("./control", control, 0644, mtime);
    tar.add_bytes("./prerm", prerm, 0755, mtime);
    if (!tar.close()) throw Fallo("no puedo escribir " + S(ctrl));
  }
  fs::path out = out_file(t, cat("nexus-office-c_", version(), "_", arch, ".deb"));
  write_ar(out, {{"debian-binary", {"2.0\n", {}}}, {"control.tar.gz", {"", ctrl}}, {"data.tar.gz", {"", data}}});
  rm_rf(work);
  hecho(out);
  return out;
}

const char* BUNDLE_LAUNCHER = R"SH(#!/bin/sh
# Nexus-Office ({kind}, edicion C++): el programa va dentro (solo lectura); los datos del usuario, fuera.
#   sin argumentos: arranca Nexus-Office y abre el navegador
#   servicio instalar [--red] | modelos [--descargar] | oficina <perfil> | chat ...: ordenes de nexus-office
HERE="{here}"
export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-{data}}"
mkdir -p "$NEXUS_DATA_DIR"
[ $# -eq 0 ] && set -- run
if [ "$1" = run ] && [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$HERE/nexus-office" modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$HERE/nexus-office" "$@"
)SH";

std::string bundle_launcher(const std::string& kind, const std::string& here) {
  std::string s = replace_all(BUNDLE_LAUNCHER, "{kind}", kind);
  s = replace_all(s, "{here}", here);
  return replace_all(s, "{data}", "${XDG_DATA_HOME:-$HOME/.local/share}/nexus-office-c");
}

const char* DESKTOP = R"([Desktop Entry]
Type=Application
Name=Nexus-Office (C++)
GenericName=Oficina de IA local
Comment=Agentes de IA para tu pyme, funcionando en tu equipo
Exec={exec}
Icon={icon}
Terminal=false
Categories=Office;Utility;
Keywords=IA;asistente;oficina;agentes;
StartupNotify=false
)";

std::string desktop(const std::string& exec, const std::string& icon) {
  return replace_all(replace_all(DESKTOP, "{exec}", exec), "{icon}", icon);
}

const char* METAINFO = R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>{app_id}</id>
  <name>Nexus-Office</name>
  <summary>Oficina de IA local para pymes</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>LicenseRef-proprietary</project_license>
  <description><p>Agentes de IA por gremio, memoria, documentos y voz, funcionando entero en tu equipo.
  Elige el modelo de IA segun tu hardware y puede arrancar como servicio.</p></description>
  <launchable type="desktop-id">{app_id}.desktop</launchable>
  <releases><release version="{version}"/></releases>
  <content_rating type="oars-1.1"/>
</component>
)";

// Copia del paquete para AppImage y Flatpak: sin install.sh ni start.sh y con los modos normalizados
fs::path bundle_tree(const fs::path& dest, const fs::path& to) {
  rm_rf(to);
  copiar_arbol(dest, to, [](const std::string& rel) { return rel == "install.sh" || rel == "start.sh"; });
  for (auto& f : ficheros(to)) modo(f, file_mode("nexus/" + fs::relative(f, to).generic_string(), false));
  return to;
}

fs::path icono() { return raiz() / "packaging" / "iconos" / "nexus-256.png"; }

// appimagetool y el runtime de AppImage (se descargan una vez en .cache/appimage)
fs::path herramienta(const std::string& name) {
  static const std::map<std::string, std::string> URLS = {
      {"appimagetool-x86_64.AppImage",
       "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"},
      {"runtime-x86_64", "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"},
      {"runtime-aarch64", "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-aarch64"},
  };
  fs::path p = cache() / "appimage" / name;
  if (!is_file(p)) {
    fs::create_directories(p.parent_path());
    log("build", "descargando " + name);
    http::download(URLS.at(name), p);
    modo(p, 0755);
  }
  return p;
}

fs::path appimagetool() {
  fs::path ext = cache() / "appimage" / "squashfs-root" / "AppRun";
  if (!is_file(ext)) {
    fs::path img = herramienta("appimagetool-x86_64.AppImage");
    ejecutar({S(img), "--appimage-extract"}, cache() / "appimage");
  }
  return ext;
}

fs::path appimage(const std::string& t, const fs::path& dest) {
  std::string arch = arch_of(t);
  fs::path work = build_dir() / ("appimage-" + t);
  rm_rf(work);
  fs::path appdir = work / "AppDir";
  bundle_tree(dest, appdir / "nexus");
  escribir(appdir / "AppRun", bundle_launcher("AppImage", "$(dirname \"$(readlink -f \"$0\")\")/nexus"), 0755);
  escribir(appdir / "nexus-office-c.desktop", desktop("nexus-office-c", "nexus-office-c") + "X-AppImage-Name=Nexus-Office-C\n");
  copiar(icono(), appdir / "nexus-office-c.png");
  fs::create_symlink("nexus-office-c.png", appdir / ".DirIcon");
  fs::path out = out_file(t, cat("Nexus-Office-", version(), "-linux-", arch, ".AppImage"));
  ejecutar({S(appimagetool()), "--no-appstream", "--comp", "zstd", "--runtime-file", S(herramienta("runtime-" + arch)),
            S(appdir), S(out)},
           {}, {{"ARCH", arch}, {"HOME", S(work)}});
  rm_rf(work);
  hecho(out);
  return out;
}

std::optional<fs::path> flatpak(const std::string& t, const fs::path& dest) {
  if (proc::which("flatpak").empty()) {
    log("build", "Flatpak: falta el comando flatpak (apt install flatpak)");
    return std::nullopt;
  }
  std::string arch = arch_of(t);
  fs::path work = build_dir() / ("flatpak-" + t);
  rm_rf(work);
  fs::path app = work / "app", files = app / "files";
  bundle_tree(dest, files / "nexus");
  escribir(files / "bin" / "nexus-office-c", bundle_launcher("Flatpak", "/app/nexus"), 0755);
  fs::path share = files / "share";
  escribir(share / "applications" / (APP_ID + ".desktop"), desktop("nexus-office-c", APP_ID));
  escribir(share / "metainfo" / (APP_ID + ".metainfo.xml"),
           replace_all(replace_all(METAINFO, "{app_id}", APP_ID), "{version}", numver()));
  copiar(icono(), share / "icons/hicolor/256x256/apps" / (APP_ID + ".png"));
  escribir(app / "metadata", cat("[Application]\nname=", APP_ID, "\nruntime=org.freedesktop.Platform/", arch, "/",
                                 FLATPAK_RUNTIME, "\nsdk=org.freedesktop.Sdk/", arch, "/", FLATPAK_RUNTIME, R"(
command=nexus-office-c

[Context]
shared=network;ipc;
sockets=x11;wayland;pulseaudio;
devices=all;
filesystems=xdg-config/systemd:create;xdg-documents;xdg-download;
)"));
  for (std::string rel : {"share/applications/" + APP_ID + ".desktop", "share/icons/hicolor/256x256/apps/" + APP_ID + ".png"})
    copiar(files / rel, app / "export" / rel);
  fs::path repo_dir = work / "repo";
  fs::path out = out_file(t, cat("Nexus-Office-", version(), "-linux-", arch, ".flatpak"));
  ejecutar({"flatpak", "build-export", "--arch=" + arch, "--disable-sandbox", S(repo_dir), S(app), "stable"});
  ejecutar({"flatpak", "build-bundle", "--arch=" + arch, "--runtime-repo=" + FLATHUB, S(repo_dir), S(out), APP_ID, "stable"});
  rm_rf(work);
  hecho(out);
  return out;
}

// ------------------------------------------------------------------ Windows: instalador grafico
void profiles_nsh(const fs::path& path) {
  // perfiles de oficina para el desplegable del instalador ("id: Nombre")
  static const Rx re(R"(^name:\s*(.+)$)", Rx::M);
  std::string out;
  for (auto& f : glob(raiz() / "recursos" / "office" / "perfiles", "*.yaml")) {
    std::string text = read_text(f);
    auto m = re.search(text);
    std::string name = m ? trim(trim(m->str(1)), "'\"") : stem(f);
    out += cat("  ${NSD_CB_AddString} $hCombo \"", stem(f), ": ", replace_all(name, "\"", "'"), "\"\n");
  }
  escribir(path, out);
}

std::optional<fs::path> windows_exe(const std::string& t, const fs::path& dest) {
  std::string makensis = proc::which("makensis");
  if (makensis.empty()) {
    log("build", "instalador .exe: falta makensis (apt install nsis / choco install nsis / brew install makensis)");
    return std::nullopt;
  }
  std::string arch = arch_of(t);
  fs::path work = build_dir() / ("nsis-" + arch);
  rm_rf(work);
  fs::create_directories(work);
  profiles_nsh(work / "profiles.nsh");
  fs::path out = out_file(t, cat("Nexus-Office-Setup-", version(), "-windows-", arch, ".exe"));
  fs::path win = raiz() / "packaging" / "windows";
  std::vector<std::pair<std::string, std::string>> defs = {
      {"VERSION", version()}, {"NUMVER", numver()},       {"ARCH", arch},
      {"PAYLOAD", S(dest)},   {"OUTFILE", S(out)},        {"ICON", S(win / "nexus.ico")},
      {"LICENSE", S(raiz() / "LICENSE")}, {"PROFILES", S(work / "profiles.nsh")}};
  uint64_t size = tamano_arbol(dest);
  if (size > 1800000000ull) defs.push_back({"NOSOLID", "1"});   // /SOLID no admite mas de ~2 GB sin comprimir
  log("build", cat("makensis ", S(out.filename()), " (", fnum((double)size / 1e9, 2), " GB sin comprimir)..."));
  std::vector<std::string> cmd = {makensis, "-V2", "-INPUTCHARSET", "UTF8"};
  for (auto& [k, v] : defs) cmd.push_back("-D" + k + "=" + v);
  cmd.push_back(S(win / "nexus.nsi"));
  ejecutar(cmd);
  rm_rf(work);
  hecho(out);
  return out;
}

// ------------------------------------------------------------------ codigo fuente, LEEME y sumas
// del repositorio: la edicion C++ entera (con sus recursos), sus flujos de GitHub y el firmware
const std::vector<std::string> SOURCE_PARTS = {
    "nexus-office-cpp", ".github/workflows/cpp.yml", ".github/workflows/cpp-entrega.yml",
    ".github/workflows/cpp-publicar.yml", ".github/workflows/wikipedia-grafo.yml", ".github/workflows/firmware.yml",
    "firmware"};
const std::set<std::string> SECRET_OK = {"nexus-office-cpp/third_party/certs/cacert.pem"};   // raices publicas (Mozilla)

fs::path source_zip() {
  static const Rx skip(R"((^|/)(build[^/]*|dist[^/]*|\.cache|__pycache__|node_modules|\.git)/|\.pyc$)");
  static const Rx secret(R"(\.(jks|keystore|pass|pem|key|p12|log)$|(^|/)(config\.ya?ml|\.env)$)", Rx::I);
  std::vector<std::pair<fs::path, std::string>> files;
  for (auto& part : SOURCE_PARTS) {
    fs::path p = repo() / part;
    std::vector<fs::path> list = is_file(p) ? std::vector<fs::path>{p} : ficheros(p);
    for (auto& f : list) {
      std::string rel = fs::relative(f, repo()).generic_string();
      if (!skip.test(rel)) files.push_back({f, rel});
    }
  }
  std::vector<std::string> leaks;
  for (auto& [f, rel] : files)
    if (secret.test(rel) && !SECRET_OK.count(rel)) leaks.push_back(rel);
  if (!leaks.empty()) throw Fallo("ficheros sensibles en el codigo fuente: " + join(leaks, ", "));
  fs::create_directories(dist());
  fs::path out = dist() / cat("Nexus-Office-", version(), "-codigo-fuente.zip");
  archive::ZipWriter z(out);
  for (auto& [f, rel] : files) z.add_file("Nexus-Office-C/" + rel, f, 9);
  if (!z.close()) throw Fallo("no puedo escribir " + S(out));
  log("build", cat("-> ", S(out.filename()), " (", files.size(), " ficheros, ", mb(out), ")"));
  return out;
}

const char* README = R"(Nexus-Office {version} - edicion C++ - instaladores 100 % portables (se instalan sin internet)
=====================================================================================================

La oficina de IA de Nexus-Office (la misma interfaz, los mismos agentes, perfiles y datos, y todas sus funciones),
hecha entera en C++: un unico programa nativo, sin Python, que arranca en menos de un segundo y ocupa mucho menos. Lleva
dentro los motores de IA (llama-server y sd-cli: CPU, Vulkan y, segun la plataforma, CUDA o Metal; voz con piper y
whisper). Al instalar evalua el hardware (memoria, grafica, procesador) y elige el modelo de IA adecuado; lo descarga si
hay internet. Todos pueden dejar Nexus-Office como servicio que arranca solo con el equipo. Al arrancar se comprueba y
se repara solo (configuracion, base de datos, modelos y puerto). Se instala junto a la edicion Python sin pisarla.
Detalles de la edicion en docs/CPP.md.

windows/  Nexus-Office-Setup-{version}-windows-x64.exe    PC con Windows 10/11 de 64 bits (Intel/AMD; NVIDIA con CUDA)
          Nexus-Office-Setup-{version}-windows-arm64.exe  Windows 11 en ARM (Snapdragon X)
          Doble clic; si Windows avisa "protegio su PC": Mas informacion > Ejecutar de todas formas.
          Silencioso: Nexus-Office-Setup-...exe /S /OFICINA=herreria /SERVICIO /RED
linux/    sh Nexus-Office-{version}-linux-x86_64.run [carpeta] [--oficina=herreria] [--servicio] [--red]
          sh Nexus-Office-{version}-linux-aarch64.run      ARM de 64 bits: Raspberry Pi 4/5 (64 bits), Jetson, portatiles ARM
          sudo apt install ./nexus-office-c_{version}_amd64.deb   (o _arm64.deb en Raspberry Pi OS 64 bits / ARM)
          luego: nexus-office-c   |   servicio: sudo nexus-office-c servicio instalar [--red]
          Nexus-Office-{version}-linux-x86_64.AppImage / -aarch64.AppImage   sin instalar: chmod +x y abrir
          flatpak install --user Nexus-Office-{version}-linux-x86_64.flatpak  (o -aarch64; pide el runtime de Flathub)
macos/    Nexus-Office-{version}-macos-arm64.tar.gz (Apple Silicon) / macos-x86_64 (Intel): descomprimir y abrir
          "Nexus-Office.command" (o ./install.sh --oficina=herreria --servicio && ./start.sh)
android/  Nexus-Office-{version}-android-autonomo.apk  Nexus-Office C++ completo dentro del movil (ARM64, Android 9+)
          Nexus-Office-{version}-android-vpn.apk       app ligera que conecta con el Nexus-Office de la oficina
Nexus-Office-{version}-codigo-fuente.zip               codigo C++ limpio, pruebas, empaquetado, recursos y firmware
Nexus-Office-{version}-grafo-wikipedia-es.zip          ejemplo para importar: grafo multicapa de la Wikipedia en espanol
Nexus-Office-resumen.md                                resumen del proyecto para la biblioteca (RAG) de Nexus-Office
Manual-de-usuario.pdf / Documentacion-tecnica.pdf

Raspberry Pi: hace falta el sistema de 64 bits (Raspberry Pi OS 64-bit, Ubuntu); Pi 4 o Pi 5 con 4 GB o mas.
Integridad: SHA256SUMS.txt
)";

void examples() {
  fs::path grafo = raiz() / "ejemplos" / "grafo-wikipedia-es";
  fs::path out = dist() / cat("Nexus-Office-", version(), "-grafo-wikipedia-es.zip");
  archive::ZipWriter z(out);
  for (auto& f : ficheros(grafo)) z.add_file("grafo-wikipedia-es/" + S(f.filename()), f, 9);
  if (!z.close()) throw Fallo("no puedo escribir " + S(out));
  copiar(raiz() / "ejemplos" / "resumen-proyecto" / "Nexus-Office-resumen.md", dist() / "Nexus-Office-resumen.md");
}

void checksums() {
  std::string out;
  for (auto& f : ficheros(dist()))
    if (f.filename() != "SHA256SUMS.txt")
      out += sha256_fichero(f) + "  " + fs::relative(f, dist()).generic_string() + "\n";
  escribir(dist() / "SHA256SUMS.txt", out);
}

void build_target(const std::string& t) {
  log("build", "=== " + t + " ===");
  fs::path dest = stage(t);
  const std::string& kind = target(t).os;
  if (kind == "windows") {
    windows_exe(t, dest);
  } else if (kind == "linux") {
    deb(t, dest);
    fs::path tgz = tarball(t, dest);
    run_installer(t, tgz);
    remove_file(tgz);
    appimage(t, dest);
    flatpak(t, dest);
  } else {
    tarball(t, dest);
  }
  rm_rf(dest.parent_path());
}

}  // namespace

int paquetes(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {"targets", "apk", "docs"}, {"keep"});
  std::vector<std::string> targets;
  if (a.tiene("targets")) targets = a.lista("targets");
  else for (auto& [k, v] : TARGETS) targets.push_back(k);
  for (auto& t : targets) target(t);   // valida antes de empezar
  if (!a.tiene("keep")) rm_rf(dist());
  fs::create_directories(dist());
  for (auto& t : targets) build_target(t);
  for (auto& extra : a.lista("apk")) copiar(P(extra), dist() / "android" / P(extra).filename());
  for (auto& extra : a.lista("docs")) copiar(P(extra), dist() / P(extra).filename());
  source_zip();
  examples();
  escribir(dist() / "LEEME.txt", replace_all(README, "{version}", version()));
  checksums();
  log("build", "Listo: " + S(dist()));
  return 0;
}

}  // namespace nc
