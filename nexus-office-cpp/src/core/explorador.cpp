// Explorador de archivos para importar modelos: selector del sistema y listado de carpetas para el explorador propio.
#include "core/explorador.h"

#include <algorithm>
#include <map>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

#include "util/log.h"
#include "util/proc.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::explorador {

static auto L = log::get("explorador");

std::string formato_modelo(const fs::path& p) {
  if (is_dir(p)) {
    if (is_file(p / "genai_config.json")) return "onnx-genai";
    if (is_file(p / "genie_config.json")) return "genie";
    if (is_file(p / "config.json") && !glob(p, "*.safetensors").empty()) return "safetensors";
    return "";
  }
  static const std::map<std::string, std::string> EXT = {
      {".gguf", "gguf"},       {".onnx", "onnx"},     {".safetensors", "safetensors"}, {".litertlm", "litertlm"},
      {".task", "litertlm"},   {".tflite", "tflite"}, {".bin", "bin"},                 {".ckpt", "ckpt"},
      {".pth", "pytorch"},     {".pt", "pytorch"}};
  auto it = EXT.find(suffix_lower(p));
  return it == EXT.end() ? "" : it->second;
}

json raices() {
  json out = json::array();
  auto add = [&](const std::string& name, const fs::path& p) {
    if (!p.empty() && is_dir(p)) out.push_back({{"name", name}, {"path", S(p)}});
  };
  fs::path home = home_dir();
  if (sys::is_android()) {
    add("Almacenamiento", "/storage/emulated/0");
    add("Descargas", "/storage/emulated/0/Download");
    std::error_code ec;   // tarjetas SD y memorias USB
    for (auto& e : fs::directory_iterator("/storage", fs::directory_options::skip_permission_denied, ec)) {
      std::string n = S(e.path().filename());
      if (n != "emulated" && n != "self" && e.is_directory(ec)) add("Tarjeta " + n, e.path());
    }
  } else {
    add("Carpeta personal", home);
    for (auto* d : {"Downloads", "Descargas"}) add("Descargas", home / d);
    for (auto* d : {"Desktop", "Escritorio"}) add("Escritorio", home / d);
    add("Documentos", home / "Documents");
  }
  add("Modelos de Nexus-Office", paths::models_dir());
#ifdef _WIN32
  DWORD m = GetLogicalDrives();
  for (int i = 0; i < 26; ++i)
    if (m & (1u << i)) {
      std::string d = std::string(1, char('A' + i)) + ":\\";
      out.push_back({{"name", "Unidad " + d.substr(0, 2)}, {"path", d}});
    }
#else
  if (!sys::is_android()) {
    add("Raíz del sistema", "/");
    for (auto* d : {"/media", "/mnt", "/Volumes", "/run/media"}) add(d, d);
  }
#endif
  // sin repetir la misma carpeta con dos nombres (Downloads y Descargas pueden ser la misma)
  json uniq = json::array();
  std::set<std::string> seen;
  for (auto& r : out)
    if (seen.insert(jstr(r, "path")).second) uniq.push_back(r);
  return uniq;
}

json listar(const std::string& ruta, bool todos) {
  std::string r = trim(ruta);
  if (starts_with(r, "~")) r = S(home_dir()) + r.substr(1);
  fs::path dir = r.empty() ? home_dir() : P(r);
  if (is_file(dir)) dir = dir.parent_path();
  if (!is_dir(dir)) throw std::runtime_error("No existe la carpeta: " + ruta);
  dir = resolve(dir);
  json dirs = json::array(), files = json::array();
  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) throw std::runtime_error("No se puede abrir " + S(dir) + ": " + ec.message());
  for (auto& e : it) {
    std::string name = S(e.path().filename());
    if (name.empty() || name[0] == '.' || name[0] == '$') continue;   // ocultas y de sistema
    std::error_code e2;
    if (e.is_directory(e2)) {
      dirs.push_back({{"name", name}, {"path", S(e.path())}, {"modelo", formato_modelo(e.path())}});
    } else if (e.is_regular_file(e2)) {
      std::string f = formato_modelo(e.path());
      if (f.empty() && !todos) continue;
      files.push_back({{"name", name}, {"path", S(e.path())}, {"size", (long long)e.file_size(e2)}, {"formato", f}});
    }
    if (dirs.size() + files.size() >= 5000) break;
  }
  auto by_name = [](const json& a, const json& b) { return lower(jstr(a, "name")) < lower(jstr(b, "name")); };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  fs::path parent = dir.parent_path();
  return {{"path", S(dir)},
          {"parent", parent == dir ? "" : S(parent)},
          {"modelo", formato_modelo(dir)},
          {"dirs", dirs},
          {"files", files},
          {"roots", raices()}};
}

// ---------------------------------------------------------------- selector del sistema
#ifdef _WIN32
static bool sesion_de_escritorio() {
  DWORD sid = 0;   // como servicio va en la sesion 0, sin escritorio: el dialogo no lo veria nadie
  return ProcessIdToSessionId(GetCurrentProcessId(), &sid) && sid != 0;
}

static json dialogo_windows(bool carpeta) {
  json out = {{"unavailable", true}, {"detail", "no se pudo abrir el selector de Windows"}};
  // el dialogo necesita un hilo STA propio: el del servidor HTTP no lo es
  std::thread t([&] {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
      FILEOPENDIALOGOPTIONS o = 0;
      dlg->GetOptions(&o);
      o |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (carpeta ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
      dlg->SetOptions(o);
      dlg->SetTitle(carpeta ? L"Nexus-Office: carpeta del modelo (ONNX, Genie, safetensors)" : L"Nexus-Office: importar modelo");
      if (!carpeta) {
        COMDLG_FILTERSPEC tipos[] = {
            {L"Modelos (GGUF, ONNX, LiteRT, safetensors...)", L"*.gguf;*.onnx;*.safetensors;*.litertlm;*.task;*.tflite;*.bin;*.ckpt;*.pth;*.pt"},
            {L"Todos los archivos", L"*.*"}};
        dlg->SetFileTypes(2, tipos);
      }
      HWND owner = GetForegroundWindow();   // el navegador con la interfaz: el dialogo sale delante
      HRESULT hr = dlg->Show(owner);
      if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        out = {{"cancelled", true}};
      } else if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        PWSTR w = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
          out = {{"path", S(fs::path(w))}};
          CoTaskMemFree(w);
        }
        if (item) item->Release();
      }
      dlg->Release();
    }
    CoUninitialize();
  });
  t.join();
  return out;
}
#endif

// orden del selector en Linux: zenity (GNOME, XFCE...), kdialog (KDE), yad
static std::vector<std::string> orden_linux(bool carpeta) {
  const std::string tit = carpeta ? "Nexus-Office: carpeta del modelo" : "Nexus-Office: importar modelo";
  const std::string pat = "*.gguf *.onnx *.safetensors *.litertlm *.task *.tflite *.bin *.ckpt *.pth *.pt";
  std::string start = S(home_dir()) + "/";
  if (!proc::which("zenity").empty()) {
    std::vector<std::string> a = {"zenity", "--file-selection", "--title=" + tit, "--filename=" + start};
    if (carpeta) a.push_back("--directory");
    else a.insert(a.end(), {"--file-filter=Modelos | " + pat, "--file-filter=Todos | *"});
    return a;
  }
  if (!proc::which("kdialog").empty()) {
    if (carpeta) return {"kdialog", "--title", tit, "--getexistingdirectory", start};
    return {"kdialog", "--title", tit, "--getopenfilename", start, "Modelos (" + pat + ")"};
  }
  if (!proc::which("yad").empty()) {
    std::vector<std::string> a = {"yad", "--file", "--title=" + tit, "--filename=" + start};
    if (carpeta) a.push_back("--directory");
    return a;
  }
  return {};
}

bool dialogo_disponible() {
  if (sys::is_android()) return false;
#ifdef _WIN32
  return sesion_de_escritorio();
#else
  if (sys::is_macos()) return !proc::which("osascript").empty();
  if (env("DISPLAY").empty() && env("WAYLAND_DISPLAY").empty()) return false;   // servicio o servidor sin escritorio
  return !orden_linux(false).empty();
#endif
}

json dialogo(bool carpeta) {
  if (!dialogo_disponible())
    return {{"unavailable", true}, {"detail", "este equipo no tiene un escritorio donde abrir el selector de archivos"}};
#ifdef _WIN32
  return dialogo_windows(carpeta);
#else
  std::vector<std::string> args;
  if (sys::is_macos()) {
    std::string elegir = carpeta ? "choose folder with prompt \"Carpeta del modelo (ONNX, Genie, safetensors)\""
                                 : "choose file with prompt \"Modelo a importar en Nexus-Office\"";
    // System Events lo trae delante del navegador
    args = {"osascript", "-e", "tell application \"System Events\"", "-e", "activate", "-e",
            "set f to " + elegir, "-e", "end tell", "-e", "POSIX path of f"};
  } else {
    args = orden_linux(carpeta);
  }
  proc::Result r = proc::run(args, 1800, {.capture = true, .hide_window = false});
  if (!r.started) return {{"unavailable", true}, {"detail", "no se pudo abrir " + args[0]}};
  std::string path = trim(r.out);
  if (r.timed_out || r.code != 0 || path.empty()) return {{"cancelled", true}};
  // osascript devuelve la carpeta con «/» al final
  if (path.size() > 1 && ends_with(path, "/")) path.pop_back();
  L.info("elegido en el selector del sistema: ", path);
  return {{"path", path}};
#endif
}

}  // namespace nx::explorador
