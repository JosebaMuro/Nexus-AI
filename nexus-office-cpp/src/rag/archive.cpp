// Importacion de archivos comprimidos (rag/archive.py): ZIP, tar(.gz/.bz2/.xz), RAR y 7z con proyectos o
// documentacion. Se descomprimen con limites en data/sandbox/proyectos/<nombre>/ y se indexan como un documento.
#include <algorithm>
#include <future>
#include <set>

#include "rag/rag.h"
#include "util/archive.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::rag {

static auto L = log::get("rag");

static const size_t MAX_FILES = 20000;
static const uint64_t MAX_TOTAL = 1024ull * 1024 * 1024;
static const uint64_t MAX_FILE = 50ull * 1024 * 1024;
static const uint64_t MAX_RATIO = 200;
static const size_t MAX_INDEX_FILE = 512 * 1024;
static const size_t MAX_INDEX_FILES = 20000;
static const size_t MAX_INDEX_CHARS = 6000000;
static const size_t READ_GROUP = 200;

static const std::set<std::string> DOC_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub",
                                              ".rtf", ".html", ".htm", ".xhtml", ".md", ".markdown", ".txt", ".rst",
                                              ".adoc", ".org"};
const std::set<std::string> RICH_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx",
                                               ".xlsx", ".epub", ".rtf", ".html", ".htm", ".xhtml"};
static const std::set<std::string> CODE_EXT = {
    ".py",   ".pyi",    ".ipynb",  ".js",    ".mjs",  ".cjs",  ".ts",    ".tsx",  ".jsx",   ".vue",  ".svelte",
    ".java", ".kt",     ".kts",    ".scala", ".groovy", ".gradle", ".c",  ".h",    ".cc",    ".cpp",  ".hpp",
    ".cs",   ".go",     ".rs",     ".rb",    ".php",  ".swift", ".m",    ".mm",   ".dart",  ".lua",  ".r",
    ".jl",   ".pl",     ".sh",     ".bash",  ".zsh",  ".ps1",  ".bat",   ".cmd",  ".sql",   ".graphql", ".proto",
    ".json", ".yaml",   ".yml",    ".toml",  ".ini",  ".cfg",  ".conf",  ".properties", ".xml", ".csv", ".tsv",
    ".css",  ".scss",   ".sass",   ".less",  ".tex",  ".el",   ".clj",   ".ex",   ".exs",   ".erl",  ".hs",
    ".ml",   ".fs",     ".vb",     ".asm",   ".cmake", ".tf",  ".nix"};
static const std::set<std::string> PLAIN_NAMES = {
    "dockerfile", "makefile", "cmakelists.txt", "readme", "license", "licence", "changelog", "authors", "contributing",
    "procfile", "gemfile", "rakefile", "vagrantfile", "jenkinsfile", ".gitignore", ".dockerignore", ".editorconfig"};
static const std::set<std::string> SKIP_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", "venv", ".tox", ".mypy_cache", ".pytest_cache",
    ".ruff_cache", ".idea", ".vscode", ".gradle", ".next", ".nuxt", "__MACOSX", "dist", "build", "target", "obj",
    ".dart_tool", "Pods", "DerivedData"};
static const std::set<std::string> SKIP_FILES = {".ds_store", "thumbs.db", "desktop.ini"};

using Parts = std::vector<std::string>;

static std::string pjoin(const Parts& p, size_t from = 0) {
  std::string s;
  for (size_t i = from; i < p.size(); ++i) s += (i > from ? "/" : "") + p[i];
  return s;
}

// ruta relativa segura (sin zip slip, absolutas ni unidades de Windows); vacia si es peligrosa
static std::optional<Parts> clean_name(const std::string& raw) {
  std::string name = replace_all(raw, "\\", "/");
  static const Rx drive("^[A-Za-z]:");
  if (starts_with(name, "/") || drive.test(name)) return std::nullopt;
  Parts parts;
  for (auto& p : split(name, "/"))
    if (!p.empty() && p != ".") parts.push_back(p);
  if (parts.empty()) return std::nullopt;
  for (auto& p : parts)
    if (p == "..") return std::nullopt;
  return parts;
}

bool indexable(const std::string& rel) {
  static const Rx secret(R"((^|/)(\.env(\..*)?|.*\.(pem|key|p12|pfx|keystore|jks)|id_(rsa|ed25519|ecdsa|dsa))$)", Rx::I);
  static const Rx noise(R"(\.min\.(js|css)$|\.map$|(^|/)(package-lock\.json|yarn\.lock|pnpm-lock\.yaml|poetry\.lock|)"
                        R"(Cargo\.lock|composer\.lock|Gemfile\.lock)$|(^|/)(vendor|third_party)/)",
                        Rx::I);
  if (secret.test(rel) || noise.test(rel)) return false;
  std::string name = lower(S(P(rel).filename()));
  std::string suf = suffix_lower(P(rel));
  return DOC_EXT.count(suf) || CODE_EXT.count(suf) || PLAIN_NAMES.count(name);
}

static bool strip_root(const std::vector<std::optional<Parts>>& rels) {
  std::set<std::string> tops;
  for (auto& r : rels) {
    if (r && r->size() > 1) tops.insert((*r)[0]);
    if (r && r->size() <= 1) return false;
  }
  return tops.size() == 1;
}

using Result = ExtractResult;

static bool skip_path(const Parts& rel) {
  for (size_t i = 0; i + 1 < rel.size(); ++i)
    if (SKIP_DIRS.count(rel[i])) return true;
  return SKIP_FILES.count(lower(rel.back())) > 0;
}

static Result safe_extract_zip(const fs::path& zip_path, const fs::path& dest) {
  archive::Zip z(zip_path);
  if (!z.ok()) throw std::invalid_argument("el fichero no es un ZIP valido");
  Result res;
  std::vector<archive::Entry> infos;
  for (auto& e : z.entries())
    if (!e.dir) infos.push_back(e);
  std::vector<std::optional<Parts>> cleaned;
  for (auto& e : infos) cleaned.push_back(clean_name(e.name));
  bool strip = strip_root(cleaned);
  fs::path droot = resolve(dest);
  for (size_t k = 0; k < infos.size(); ++k) {
    auto& info = infos[k];
    if (!cleaned[k]) {
      res.skipped.emplace_back(info.name, "ruta no permitida");
      continue;
    }
    Parts rel = *cleaned[k];
    if (strip) rel.erase(rel.begin());
    if (skip_path(rel)) continue;
    std::string r = pjoin(rel);
    if (info.symlink) {
      res.skipped.emplace_back(r, "enlace simbolico");
      continue;
    }
    if (info.encrypted) {
      res.skipped.emplace_back(r, "cifrado");
      continue;
    }
    if (info.size > MAX_FILE) {
      res.skipped.emplace_back(r, "demasiado grande");
      continue;
    }
    if (info.size > 1024 * 1024 && info.size > MAX_RATIO * std::max<uint64_t>(info.comp_size, 1)) {
      res.skipped.emplace_back(r, "compresion sospechosa");
      continue;
    }
    if (res.files.size() >= MAX_FILES) {
      res.skipped.emplace_back(r, "limite de ficheros");
      continue;
    }
    if (res.bytes + info.size > MAX_TOTAL) {
      res.skipped.emplace_back(r, "limite de tamano total");
      continue;
    }
    fs::path out = dest;
    for (auto& p : rel) out /= P(p);
    if (!is_relative_to(resolve(out), droot)) {
      res.skipped.emplace_back(r, "ruta no permitida");
      continue;
    }
    // no nos fiamos del tamano declarado
    auto data = z.read(info.name, (size_t)MAX_FILE + 1);
    if (!data || data->size() > MAX_FILE || res.bytes + data->size() > MAX_TOTAL) {
      res.skipped.emplace_back(r, "tamano real excesivo");
      continue;
    }
    mkdirs(out.parent_path());
    write_file(out, *data);
    res.bytes += data->size();
    res.files.push_back(r);
  }
  std::sort(res.files.begin(), res.files.end());
  return res;
}

// ---------------------------------------------------------------- herramientas externas (RAR, 7z, tar.bz2/xz)
static std::string which_any(std::initializer_list<const char*> names) {
  for (auto n : names) {
    std::string p = proc::which(n);
    if (!p.empty()) return p;
  }
  if (sys::is_windows()) {
    for (auto base : {env("ProgramFiles", "C:\\Program Files"), env("ProgramFiles(x86)")}) {
      if (base.empty()) continue;
      for (auto exe : {"7-Zip\\7z.exe", "WinRAR\\UnRAR.exe"}) {
        fs::path p = P(base) / P(exe);
        if (path_exists(p)) return S(p);
      }
    }
  }
  return "";
}

static bool is_bsdtar(const std::string& exe) {
  auto r = proc::run({exe, "--version"}, 10, {.capture = true});
  std::string o = lower(r.out + r.err);
  return contains(o, "bsdtar") || contains(o, "libarchive");
}

static bool reads_rar(const std::string& exe) {
  static std::map<std::string, bool> cache;
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  auto it = cache.find(exe);
  if (it != cache.end()) return it->second;
  auto r = proc::run({exe, "i"}, 15, {.capture = true});
  std::string out = r.out;
  size_t c = out.find("Codecs:");
  std::string codecs = c == std::string::npos ? "" : out.substr(c);
  bool ok = out.empty() || re_search(R"(\bRar[1-5]\b)", codecs) || c == std::string::npos;
  cache[exe] = ok;
  return ok;
}

std::pair<std::string, std::string> find_extractor(const std::string& kind) {
  std::vector<std::string> order = kind == "rar" ? std::vector<std::string>{"unrar", "7z", "bsdtar", "unar"}
                                                 : std::vector<std::string>{"7z", "bsdtar", "unar"};
  for (auto& tool : order) {
    if (tool == "unrar") {
      std::string exe = which_any({"unrar", "UnRAR.exe"});
      if (!exe.empty() && contains(lower(S(P(exe).filename())), "unrar")) return {"unrar", exe};
    } else if (tool == "7z") {
      std::string exe = which_any({"7zz", "7z"});
      if (exe.empty() && kind == "7z") exe = which_any({"7za"});
      if (!exe.empty() && (kind != "rar" || reads_rar(exe))) return {"7z", exe};
    } else if (tool == "bsdtar") {
      for (auto cand : {"bsdtar", "tar"}) {
        std::string exe = proc::which(cand);
        if (!exe.empty() && is_bsdtar(exe)) return {"bsdtar", exe};
      }
    } else if (tool == "unar") {
      std::string exe = proc::which("unar");
      if (!exe.empty()) return {"unar", exe};
    }
  }
  return {"", ""};
}

static const std::map<std::string, std::string> INSTALL_HINT = {
    {"rar",
     "Instala una herramienta para RAR: Windows: 7-Zip (7-zip.org) o WinRAR; Linux/WSL: sudo apt install unrar "
     "(el paquete 7zip de Debian/Ubuntu no lee RAR); macOS: brew install sevenzip o unar; Termux: pkg install unrar. "
     "En Windows 10/11 y macOS tambien vale el 'tar' del sistema."},
    {"7z",
     "Instala 7-Zip (Windows: 7-zip.org; Linux/WSL: sudo apt install 7zip; macOS: brew install sevenzip; Termux: pkg "
     "install p7zip)."},
    {"tar", "Instala tar (con soporte bzip2/xz) para descomprimir este archivo."}};

// valida y mueve lo extraido en la cuarentena: mismos limites para todos los formatos
static Result adopt(const fs::path& tmp, const fs::path& dest, const std::set<std::string>& bad, Result res) {
  std::vector<std::pair<std::optional<Parts>, fs::path>> found;
  fs::path root = resolve(tmp);
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(tmp, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const fs::path& p = it->path();
    std::string rel = replace_all(S(fs::relative(p, tmp, ec)), "\\", "/");
    if (it->is_symlink(ec)) {
      res.skipped.emplace_back(rel, "enlace simbolico");
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory(ec)) continue;
    if (!it->is_regular_file(ec) || !is_relative_to(resolve(p), root)) {
      res.skipped.emplace_back(rel, "ruta no permitida");
      continue;
    }
    if (bad.count(rel)) {
      res.skipped.emplace_back(rel, "cifrado o dañado");
      continue;
    }
    found.emplace_back(clean_name(rel), p);
  }
  std::sort(found.begin(), found.end(),
            [](auto& a, auto& b) { return (a.first ? pjoin(*a.first) : "") < (b.first ? pjoin(*b.first) : ""); });
  std::vector<std::optional<Parts>> rels;
  for (auto& f : found) rels.push_back(f.first);
  bool strip = strip_root(rels);
  fs::path droot = resolve(dest);
  for (auto& [rel0, p] : found) {
    if (!rel0) {
      res.skipped.emplace_back(S(p), "ruta no permitida");
      continue;
    }
    Parts rel = *rel0;
    if (strip) rel.erase(rel.begin());
    if (skip_path(rel)) continue;
    std::string r = pjoin(rel);
    uint64_t size = fsize(p);
    if (size > MAX_FILE) {
      res.skipped.emplace_back(r, "demasiado grande");
      continue;
    }
    if (res.files.size() >= MAX_FILES) {
      res.skipped.emplace_back(r, "limite de ficheros");
      continue;
    }
    if (res.bytes + size > MAX_TOTAL) {
      res.skipped.emplace_back(r, "limite de tamano total");
      continue;
    }
    fs::path out = dest;
    for (auto& x : rel) out /= P(x);
    if (!is_relative_to(resolve(out), droot)) {
      res.skipped.emplace_back(r, "ruta no permitida");
      continue;
    }
    mkdirs(out.parent_path());
    fs::rename(p, out, ec);
    if (ec) {
      fs::copy_file(p, out, fs::copy_options::overwrite_existing, ec);
      if (ec) continue;
    }
    res.bytes += size;
    res.files.push_back(r);
  }
  std::sort(res.files.begin(), res.files.end());
  return res;
}

static Result safe_extract_tar(const fs::path& path, const fs::path& dest) {
  fs::path tmp = dest.parent_path() / (".cuarentena-" + token_hex(4));
  mkdirs(tmp);
  Result res;
  try {
    auto head = read_file(path, 4).value_or("");
    bool plain_or_gz = !(starts_with(head, "BZh") || starts_with(head, "\xFD" "7zXZ"));
    bool ok = plain_or_gz && archive::extract_tar(path, tmp);
    if (!ok) {   // tar.bz2 / tar.xz: el tar del sistema
      std::string exe = proc::which("tar");
      if (exe.empty()) throw std::invalid_argument("no hay ninguna herramienta para descomprimir este tar. " + INSTALL_HINT.at("tar"));
      auto r = proc::run({exe, "-xf", S(path), "-C", S(tmp)}, 900, {.capture = true});
      if (!r.ok()) throw std::invalid_argument("no se pudo descomprimir el tar: " + uleft(trim(r.err), 200));
    }
    res = adopt(tmp, dest, {}, res);
  } catch (...) {
    rm_rf(tmp);
    throw;
  }
  rm_rf(tmp);
  return res;
}

static Result safe_extract_external(const fs::path& path, const fs::path& dest, const std::string& kind) {
  auto [tool, exe] = find_extractor(kind);
  if (tool.empty())
    throw std::invalid_argument("no hay ninguna herramienta para descomprimir " + upper(kind) + ". " + INSTALL_HINT.at(kind));
  // listado previo: tamano total y ficheros cifrados
  std::set<std::string> encrypted;
  uint64_t listed = 0;
  try {
    if (tool == "7z") {
      auto r = proc::run({exe, "l", "-slt", "-pNEXUS_SIN_CLAVE", S(path)}, 120, {.capture = true});
      for (auto& b : split(r.out, "\n\n")) {
        auto name = Rx(R"(^Path = (.+)$)", Rx::M).search(b);
        auto size = Rx(R"(^Size = (\d+))", Rx::M).search(b);
        if (!name || !re_search(R"(^Folder = -)", b, Rx::M)) continue;
        if (size) listed += std::stoull(size->str(1));
        if (re_search(R"(^Encrypted = \+)", b, Rx::M)) encrypted.insert(replace_all(trim(name->str(1)), "\\", "/"));
      }
    } else if (tool == "unrar") {
      auto r = proc::run({exe, "lt", "-p-", S(path)}, 120, {.capture = true});
      for (auto& b : Rx(R"(\n\s*\n)").split(r.out)) {
        auto name = Rx(R"(^\s*Name: (.+)$)", Rx::M).search(b);
        auto size = Rx(R"(^\s*Size: (\d+))", Rx::M).search(b);
        if (!name || re_search(R"(^\s*Type: Directory)", b, Rx::M)) continue;
        if (size) listed += std::stoull(size->str(1));
        if (re_search(R"(^\s*Flags:.*encrypted)", b, Rx::M)) encrypted.insert(replace_all(trim(name->str(1)), "\\", "/"));
      }
    }
  } catch (...) {
  }
  if (listed > MAX_TOTAL * 4)
    throw std::invalid_argument(cat("el archivo descomprimido ocuparia ", (long long)(listed / 1e6), " MB (limite ",
                                    (long long)(MAX_TOTAL / 1e6), " MB)"));
  fs::path tmp = dest.parent_path() / (".cuarentena-" + token_hex(4));
  mkdirs(tmp);
  Result res;
  try {
    std::vector<std::string> cmd;
    if (tool == "7z") cmd = {exe, "x", "-y", "-bd", "-pNEXUS_SIN_CLAVE", "-o" + S(tmp), S(path)};
    if (tool == "unrar") cmd = {exe, "x", "-y", "-p-", "-idq", S(path), S(tmp) + (sys::is_windows() ? "\\" : "/")};
    if (tool == "bsdtar") cmd = {exe, "-xf", S(path), "-C", S(tmp)};
    if (tool == "unar") cmd = {exe, "-q", "-f", "-D", "-p", "NEXUS_SIN_CLAVE", "-o", S(tmp), S(path)};
    auto r = proc::run(cmd, 900, {.capture = true});
    std::vector<std::string> problems;
    if (r.code != 0) {
      auto lines = splitlines(trim(!r.err.empty() ? r.err : r.out));
      if (!lines.empty()) problems.push_back(lines.back());
    }
    std::string logtxt = r.out + "\n" + r.err;
    std::set<std::string> bad = encrypted;
    for (auto& m : Rx(R"(^ERROR: [^:\n]+ : (.+)$)", Rx::M).finditer(logtxt)) bad.insert(replace_all(trim(m.str(1)), "\\", "/"));
    for (auto& m : Rx(R"(^\s+(.+?)\s+\([^)]*\)\.\.\. Failed!)", Rx::M).finditer(logtxt))
      bad.insert(replace_all(trim(m.str(1)), "\\", "/"));
    if (tool == "bsdtar")
      for (auto& m : Rx(R"(^(?!bsdtar: )([^:\n]+): )", Rx::M).finditer(r.err)) bad.insert(trim(m.str(1)));
    res.tool = tool;
    res = adopt(tmp, dest, bad, res);
    if (!problems.empty())
      res.skipped.emplace_back("(archivo)", tool + ": " + uleft(problems[0], 150) + " (¿ficheros cifrados o dañados?)");
    if (res.files.empty() && r.code != 0)
      throw std::invalid_argument("no se pudo descomprimir el " + upper(kind) + ": " +
                                  (problems.empty() ? std::to_string(r.code) : uleft(problems[0], 200)));
  } catch (...) {
    rm_rf(tmp);
    throw;
  }
  rm_rf(tmp);
  return res;
}

std::string detect_kind(const fs::path& path, const std::string& name) {
  std::string head = read_file(path, 512).value_or("");
  if (starts_with(head, "Rar!\x1a\x07")) return "rar";
  if (starts_with(head, std::string("7z\xbc\xaf\x27\x1c", 6))) return "7z";
  if (archive::Zip(path).ok()) return "zip";
  if (starts_with(head, "\x1f\x8b") || (head.size() >= 262 && head.compare(257, 5, "ustar") == 0) ||
      starts_with(head, "BZh") || starts_with(head, "\xFD" "7zXZ"))
    return "tar";
  std::string suf = archive_suffix(name.empty() ? S(path.filename()) : name);
  if (suf == ".rar") return "rar";
  if (suf == ".7z") return "7z";
  return "";
}

Result extract_any(const fs::path& path, const fs::path& dest, const std::string& kind) {
  if (kind == "zip") return safe_extract_zip(path, dest);
  if (kind == "tar") return safe_extract_tar(path, dest);
  return safe_extract_external(path, dest, kind);
}

std::string tree_text(const std::vector<std::string>& files, size_t limit) {
  std::vector<std::string> lines;
  std::set<std::string> seen;
  for (auto& f : files) {
    auto parts = split(f, "/");
    for (size_t depth = 1; depth < parts.size(); ++depth) {
      std::string d = pjoin(Parts(parts.begin(), parts.begin() + depth));
      if (seen.insert(d).second) lines.push_back(repeat("  ", (int)depth - 1) + parts[depth - 1] + "/");
    }
    lines.push_back(repeat("  ", (int)parts.size() - 1) + parts.back());
    if (lines.size() >= limit) {
      lines.push_back(cat("… (", files.size(), " ficheros en total)"));
      break;
    }
  }
  return join(lines, "\n");
}

std::pair<std::vector<std::string>, std::string> read_for_index(const fs::path& p) {
  try {
    if (RICH_EXT.count(suffix_lower(p))) return {read_document(p).pages, ""};
    std::string data = read_file(p, MAX_INDEX_FILE).value_or("");
    if (data.substr(0, 4096).find('\0') != std::string::npos) return {{}, ""};
    return {{valid_utf8(data)}, ""};
  } catch (const std::exception& e) {
    return {{}, uleft(std::string("Error: ") + e.what(), 160)};
  }
}

static std::string slug_name(const std::string& name) {
  static const Rx bad("[^A-Za-z0-9._-]+");
  std::string s = trim(bad.sub(stem(P(name)), "_"), "._-");
  return uleft(s.empty() ? "proyecto" : s, 60);
}

static const std::map<std::string, std::string> LABEL = {{"zip", "ZIP"}, {"rar", "RAR"}, {"7z", "7z"}, {"tar", "tar"}};

json ingest_archive(RAGStore& rag, const fs::path& src, std::string title, const std::string& source, const json& meta,
                    const std::string& agent) {
  std::string sname = source.empty() ? S(src.filename()) : source;
  std::string kind = detect_kind(src, sname);
  if (kind.empty()) {
    std::string suf = archive_suffix(sname);
    if (suf == ".rar" || suf == ".7z") throw std::invalid_argument("el fichero no es un " + upper(suf.substr(1)) + " valido");
    auto it = LABEL.find(suf.empty() ? "" : suf.substr(1));
    throw std::invalid_argument("el fichero no es un " + (it == LABEL.end() ? std::string("ZIP") : it->second) + " valido");
  }
  fs::path stored = rag.files / (std::to_string((long long)now()) + "_" + safe_name(S(src.filename()), 80));
  if (resolve(stored) != resolve(src)) {
    std::error_code ec;
    fs::copy_file(src, stored, fs::copy_options::overwrite_existing, ec);
  }
  fs::path base = paths::sub({"sandbox", "proyectos"});
  static const Rx tar_end(R"(\.tar$)");
  std::string slug = slug_name(tar_end.sub(stem(P(sname)), "") + ".x");
  fs::path dest = base / slug;
  for (int n = 2; path_exists(dest); ++n) dest = base / (slug + "_" + std::to_string(n));
  mkdirs(dest);
  Result res;
  try {
    res = extract_any(stored, dest, kind);
  } catch (...) {
    rm_rf(dest);
    remove_file(stored);
    throw;
  }
  auto& files = res.files;
  std::string rel_dest = replace_all(S(fs::relative(dest, paths::sandbox_dir())), "\\", "/");
  if (title.empty()) title = tar_end.sub(stem(P(sname)), "");
  std::string tree = tree_text(files);
  std::vector<std::string> pages = {cat("Proyecto «", title, "» importado desde ", sname, ": ", files.size(),
                                        " ficheros. Esta en el sandbox en ", rel_dest,
                                        "/ (usa file_list/file_read con esa ruta).\n\nEstructura:\n", tree)};
  std::vector<std::string> labels = {"[" + title + " · estructura]"};
  std::vector<std::string> cands;
  for (auto& f : files)
    if (indexable(f) && cands.size() < MAX_INDEX_FILES) cands.push_back(f);
  std::vector<std::string> indexed, errors;
  std::vector<json> parts_out;
  size_t chars = 0, first_file = 1;

  auto flush = [&](bool final) {
    if (pages.empty() && !final) return;
    size_t n = parts_out.size() + 1;
    size_t last_file = indexed.size();
    json info = meta.is_object() ? meta : json::object();
    info["archive"] = sname;
    info["format"] = kind;
    info["sandbox"] = rel_dest;
    info["files"] = files.size();
    info["indexed"] = indexed.size();
    info["skipped"] = res.skipped.size();
    info["bytes"] = res.bytes;
    json detail = json::array();
    for (size_t i = 0; i < res.skipped.size() && i < 50; ++i)
      detail.push_back(res.skipped[i].first + ": " + res.skipped[i].second);
    for (size_t i = 0; i < errors.size() && i < 20; ++i) detail.push_back(errors[i]);
    info["skipped_detail"] = detail;
    if (!res.tool.empty()) info["tool"] = res.tool;
    std::string part_title = title;
    if (n > 1 || !final) {
      part_title = cat(title, " · parte ", n, " (ficheros ", first_file, "-", last_file, ")");
      info["part"] = n;
      info["file_from"] = first_file;
      info["file_to"] = last_file;
    }
    if (n > 1) {   // solo la primera parte es duena del sandbox (al borrarla se borran las demas)
      info.erase("sandbox");
      info["archive_root"] = parts_out[0]["id"];
    }
    if (!pages.empty())
      parts_out.push_back(rag.ingest_pages(part_title, pages, sname, kind, n == 1 ? S(stored) : "", info, labels, agent));
    pages.clear();
    labels.clear();
    chars = 0;
    first_file = last_file + 1;
  };

  try {
    for (size_t g = 0; g < cands.size(); g += READ_GROUP) {
      std::vector<std::string> group(cands.begin() + g, cands.begin() + std::min(cands.size(), g + READ_GROUP));
      std::vector<std::future<std::pair<std::vector<std::string>, std::string>>> futs;
      size_t heavy = 0;
      for (auto& r : group) heavy += RICH_EXT.count(suffix_lower(P(r)));
      std::vector<std::pair<std::vector<std::string>, std::string>> results;
      if (heavy >= 2) {
        for (auto& r : group) futs.push_back(std::async(std::launch::async, [p = dest / P(r)] { return read_for_index(p); }));
        for (auto& f : futs) results.push_back(f.get());
      } else {
        for (auto& r : group) results.push_back(read_for_index(dest / P(r)));
      }
      for (size_t k = 0; k < group.size(); ++k) {
        auto& [texts0, err] = results[k];
        const std::string& rel = group[k];
        if (!err.empty()) {
          L.info(LABEL.at(kind), ": no se pudo leer ", rel, ": ", err);
          errors.push_back(rel + ": " + err);
          continue;
        }
        std::vector<std::string> texts;
        for (auto& p : texts0)
          if (!trim(p).empty()) texts.push_back(p);
        if (texts.empty()) continue;
        size_t size = 0;
        for (auto& p : texts) size += ulen(p);
        if (chars && chars + size > MAX_INDEX_CHARS) flush(false);
        for (size_t i = 0; i < texts.size(); ++i) {
          pages.push_back(texts[i]);
          labels.push_back("[" + rel_dest + "/" + rel + (texts.size() > 1 ? cat(" p.", i + 1) : "") + "]");
          chars += ulen(texts[i]);
        }
        indexed.push_back(rel);
      }
    }
    flush(true);
  } catch (...) {
    for (auto& d : parts_out) rag.remove(d["id"].get<long long>());
    rm_rf(dest);
    remove_file(stored);
    throw;
  }
  json doc = parts_out[0];
  if (parts_out.size() > 1) {
    json ids = json::array();
    for (auto& d : parts_out) ids.push_back(d["id"]);
    json m0 = doc["meta"];
    m0["parts"] = ids.size();
    m0["part_ids"] = ids;
    rag.db.execute("UPDATE rag_docs SET meta=? WHERE id=?", {jdump(m0), doc["id"]});
    doc = rag.get(doc["id"].get<long long>());
  }
  L.info(LABEL.at(kind), " '", title, "': ", files.size(), " ficheros en ", rel_dest, ", ", indexed.size(), " indexados, ",
         res.skipped.size(), " omitidos");
  return doc;
}

}  // namespace nx::rag
