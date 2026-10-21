// Ficheros y ejecucion de codigo confinados a data/sandbox (tools/builtin/sandbox_tools.py).
#include <algorithm>

#include "core/services.h"
#include "tools/tools.h"
#include "util/proc.h"
#include "util/text.h"

namespace nx::tools {

fs::path sandbox_safe(const std::string& rel) {
  fs::path root = resolve(paths::sandbox_dir());
  fs::path p = resolve(root / P(rel));
  if (!is_relative_to(p, root)) throw ToolError("ValueError", "ruta fuera del sandbox");
  return p;
}

namespace {

constexpr long READ_CHUNK = 20000;

std::string posix_rel(const fs::path& p, const fs::path& root) {
  std::string s = S(p.lexically_relative(root));
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

json file_list(const json& args, ToolContext&) {
  fs::path root = paths::sandbox_dir();
  fs::path base = truthy(args.value("path", json())) ? sandbox_safe(jstr(args, "path")) : root;
  std::vector<fs::path> all;
  std::error_code ec;
  if (is_dir(base))
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec)) all.push_back(it->path());
    }
  std::sort(all.begin(), all.end());
  fs::path rroot = resolve(root);
  std::vector<std::string> files;
  for (auto& p : all) files.push_back(posix_rel(resolve(p), rroot));
  long offset = std::max(0L, (long)jnum(args.value("offset", json(0))));
  json page = json::array();
  for (long i = offset; i < (long)files.size() && i < offset + 500; ++i) page.push_back(files[i]);
  json out = {{"files", page}, {"total", files.size()}, {"offset", offset}};
  if (offset + 500 < (long)files.size()) out["next_offset"] = offset + 500;
  return out;
}

json file_read(const json& args, ToolContext&) {
  fs::path p = sandbox_safe(jstr(args, "path"));
  if (!is_file(p)) throw ToolError("FileNotFoundError", "[Errno 2] No such file or directory: '" + jstr(args, "path") + "'");
  long offset = std::max(0L, (long)jnum(args.value("offset", json(0))));
  long size = std::max(1000L, std::min((long)jnum(args.value("max_chars", json(READ_CHUNK)), READ_CHUNK), 200000L));
  // como mucho 4 bytes por caracter: no hace falta leer mas del fichero
  auto raw = read_file(p, (size_t)(offset + size + 1) * 4 + 4);
  std::string text = valid_utf8(raw.value_or(""));
  std::u32string u = u32(text);
  std::string content = u8(u.substr(std::min<size_t>(offset, u.size()), size));
  bool more = (size_t)(offset + size) < u.size();
  json out = {{"path", jstr(args, "path")}, {"content", content}, {"offset", offset}};
  if (more) out["next_offset"] = offset + (long)ulen(content);
  return out;
}

json file_write(const json& args, ToolContext&) {
  fs::path p = sandbox_safe(jstr(args, "path"));
  mkdirs(p.parent_path());
  if (!write_file(p, jstr(args, "content"))) throw ToolError("OSError", "no se pudo escribir " + jstr(args, "path"));
  return {{"path", jstr(args, "path")}, {"bytes", fsize(p)}};
}

json python_exec(const json& args, ToolContext&) {
  auto py = python_cmd();
  if (py.empty()) return {{"error", "no hay Python en este equipo: instala Python 3 para ejecutar codigo"}};
  fs::path code_file = paths::sandbox_dir() / "_exec.py";
  write_file(code_file, jstr(args, "code"));
  std::vector<std::string> cmd = py;
  cmd.insert(cmd.end(), {"-I", "-X", "utf8", S(code_file)});
  auto r = proc::run(cmd, jnum(args.value("timeout", json(20)), 20),
                     {.cwd = paths::sandbox_dir(), .env = {{"PYTHONIOENCODING", "utf-8"}}, .capture = true});
  remove_file(code_file);
  if (r.timed_out) return {{"error", "tiempo agotado"}};
  if (!r.started) return {{"error", "no se pudo iniciar Python"}};
  return {{"returncode", r.code}, {"stdout", uright(valid_utf8(r.out), 6000)}, {"stderr", uright(valid_utf8(r.err), 3000)}};
}

Register r1("file_list", file_list), r2("file_read", file_read), r3("file_write", file_write),
    r4("python_exec", python_exec);

}  // namespace

void link_builtin_files() {}

}  // namespace nx::tools
