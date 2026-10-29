#include "comun.h"

#include <cstdio>
#include <iostream>

#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nc {

using namespace nx;

fs::path raiz() {
  static fs::path r = [] {
    std::string e = env("NEXUS_SOURCE_DIR");
    if (!e.empty()) return P(e);
    for (fs::path c = fs::current_path(); !c.empty(); c = c.parent_path()) {
      if (is_file(c / "CMakeLists.txt") && is_dir(c / "recursos")) return c;
      if (is_file(c / "nexus-office-cpp" / "CMakeLists.txt")) return c / "nexus-office-cpp";
      if (c == c.parent_path()) break;
    }
#ifdef NEXUS_SOURCE_DIR
    return P(NEXUS_SOURCE_DIR);
#else
    return fs::current_path();
#endif
  }();
  return r;
}

fs::path repo() { return raiz().parent_path(); }

std::string version() {
  static std::string v = [] {
    std::string cm = read_text(raiz() / "CMakeLists.txt");
    static const Rx re(R"re(set\(NEXUS_VERSION "([^"]+)"\))re");
    auto m = re.search(cm);
    if (!m) throw Fallo("no encuentro NEXUS_VERSION en CMakeLists.txt");
    return m->str(1);
  }();
  return v;
}

void log(const std::string& tag, const std::string& msg) {
  std::cout << "[" << tag << "] " << msg << std::endl;
}

std::string mb(const fs::path& p) { return fnum((double)fsize(p) / 1e6, 1) + " MB"; }

#ifdef _WIN32
static std::wstring wquote(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t bs = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { ++bs; continue; }
    if (c == L'"') out.append(bs * 2 + 1, L'\\');
    else out.append(bs, L'\\');
    bs = 0;
    out += c;
  }
  out.append(bs * 2, L'\\');
  return out + L"\"";
}
#endif

int ejecutar_codigo(const std::vector<std::string>& args, const fs::path& cwd,
                    const std::map<std::string, std::string>& env_extra) {
  // las contrasenas (pass:..., -storepass x) no salen en el registro
  std::string shown;
  for (size_t i = 0; i < args.size(); ++i) {
    bool secret = starts_with(args[i], "pass:") || (i > 0 && (args[i - 1] == "-storepass" || args[i - 1] == "-keypass"));
    shown += (i ? " " : "") + (secret ? std::string("***") : args[i]);
  }
  std::cout << "$ " << shown << std::endl;
#ifdef _WIN32
  std::wstring cmd;
  for (auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + wquote(P(a).wstring());
  for (auto& [k, v] : env_extra) set_env(k, v);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring wcwd = cwd.empty() ? L"" : cwd.wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                      &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
#else
  std::cout.flush();
  std::fflush(nullptr);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    for (auto& [k, v] : env_extra) setenv(k.c_str(), v.c_str(), 1);
    std::vector<char*> av;
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execvp(av[0], av.data());
    std::perror(av[0]);
    _exit(127);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}

void ejecutar(const std::vector<std::string>& args, const fs::path& cwd, const std::map<std::string, std::string>& env) {
  int code = ejecutar_codigo(args, cwd, env);
  if (code != 0) throw Fallo(cat(P(args.at(0)).filename().string(), " termino con codigo ", code));
}

void modo(const fs::path& p, int mode) {
#ifndef _WIN32
  ::chmod(p.c_str(), (mode_t)mode);
#else
  (void)p;
  (void)mode;
#endif
}

void copiar(const fs::path& src, const fs::path& dst) {
  fs::create_directories(dst.parent_path());
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

void escribir(const fs::path& p, const std::string& data, int mode) {
  fs::create_directories(p.parent_path());
  std::FILE* f =
#ifdef _WIN32
      _wfopen(p.c_str(), L"wb");
#else
      std::fopen(p.c_str(), "wb");
#endif
  if (!f) throw Fallo("no puedo escribir " + S(p));
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !ok) throw Fallo("no puedo escribir " + S(p));
  modo(p, mode);
}

void copiar_arbol(const fs::path& src, const fs::path& dst, const std::function<bool(const std::string&)>& ignorar) {
  if (!is_dir(src)) throw Fallo("no existe la carpeta " + S(src));
  fs::create_directories(dst);
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) throw Fallo("no puedo leer " + S(src) + ": " + ec.message());
    // lexically_relative: fs::relative sigue los enlaces y libllama.so acabaria con el nombre de su destino
    fs::path relp = it->path().lexically_relative(src);
    std::string rel = relp.generic_string();
    if (ignorar && ignorar(rel)) {
      if (it->is_directory(ec) && !it->is_symlink(ec)) it.disable_recursion_pending();
      continue;
    }
    fs::path out = dst / relp;
    if (it->is_symlink(ec)) {
      fs::create_directories(out.parent_path());
      fs::remove(out, ec);
      fs::copy_symlink(it->path(), out, ec);
      if (ec) throw Fallo("no puedo copiar el enlace " + S(it->path()) + ": " + ec.message());
    } else if (it->is_directory(ec)) {
      fs::create_directories(out);
    } else {
      fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
      if (ec) throw Fallo("no puedo copiar " + S(it->path()) + ": " + ec.message());
#ifndef _WIN32
      struct stat st;
      if (::stat(it->path().c_str(), &st) == 0) ::chmod(out.c_str(), st.st_mode & 07777);
#endif
    }
  }
}

std::string sha256_fichero(const fs::path& p) {
  std::FILE* f =
#ifdef _WIN32
      _wfopen(p.c_str(), L"rb");
#else
      std::fopen(p.c_str(), "rb");
#endif
  if (!f) throw Fallo("no puedo leer " + S(p));
  Sha256 h;
  std::vector<char> buf(1 << 20);
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) h.update(buf.data(), n);
  std::fclose(f);
  return to_hex(h.digest());
}

std::vector<fs::path> ficheros(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!is_dir(dir)) return out;
  for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && !it->is_symlink(ec)) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    return a.generic_string() < b.generic_string();
  });
  return out;
}

uint64_t tamano_arbol(const fs::path& dir) {
  uint64_t n = 0;
  for (auto& f : ficheros(dir)) n += fsize(f);
  return n;
}

std::string Args::val(const std::string& k, const std::string& def) const {
  auto it = op.find(k);
  return it == op.end() || it->second.empty() ? def : it->second.front();
}

std::vector<std::string> Args::lista(const std::string& k) const {
  auto it = op.find(k);
  return it == op.end() ? std::vector<std::string>{} : it->second;
}

Args leer_args(const std::vector<std::string>& argv, const std::set<std::string>& multi,
               const std::set<std::string>& banderas) {
  Args a;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& s = argv[i];
    if (starts_with(s, "--")) {
      std::string k = s.substr(2), v;
      size_t eq = k.find('=');
      if (eq != std::string::npos) {
        a.op[k.substr(0, eq)].push_back(k.substr(eq + 1));
        continue;
      }
      auto& vals = a.op[k];
      if (banderas.count(k)) continue;
      if (multi.count(k)) {
        while (i + 1 < argv.size() && !starts_with(argv[i + 1], "--")) vals.push_back(argv[++i]);
      } else if (i + 1 < argv.size()) {
        vals.push_back(argv[++i]);
      } else {
        throw Fallo("falta el valor de --" + k);
      }
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

}  // namespace nc
