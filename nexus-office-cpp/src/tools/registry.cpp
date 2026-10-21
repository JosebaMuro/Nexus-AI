// Registro de herramientas (tools/base.py + tools/registry.py). Los manifiestos de las integradas salen de la
// version Python (gen/data: mismos nombres, descripciones y parametros); las funciones estan en builtin_*.cpp.
// Las de la comunidad son ficheros Python (data/tools/community) que se ejecutan con el Python del sistema.
#include <algorithm>

#include "gen/data.h"
#include "tools/tools.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::tools {

static auto L = log::get("tools");
const std::vector<std::string> RISKS = {"low", "medium", "high"};
const std::vector<std::string> PERMISSIONS = {"network", "filesystem", "code_exec", "system", "memory", "media", "agents"};

std::map<std::string, ToolFn>& builtin_functions() {
  static std::map<std::string, ToolFn> m;
  return m;
}

json ToolSpec::public_json() const {
  return {{"name", name},       {"description", description}, {"parameters", parameters}, {"risk", risk},
          {"permissions", permissions}, {"version", version},  {"author", author},         {"source", source},
          {"enabled", enabled}};
}

mem::ToolView ToolSpec::view() const { return {name, description, risk, parameters}; }

// ---------------------------------------------------------------- validacion (tools/base.py: validate_args)
static bool py_isinstance(const json& v, const std::string& typ) {
  if (typ == "string") return v.is_string();
  if (typ == "integer") return v.is_number_integer() || v.is_boolean();   // bool es int en Python
  if (typ == "number") return v.is_number() || v.is_boolean();
  if (typ == "boolean") return v.is_boolean();
  if (typ == "object") return v.is_object();
  if (typ == "array") return v.is_array();
  return true;
}

static std::string py_str(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_null()) return "None";
  if (v.is_number_float()) return pyfloat(v.get<double>());
  return pydump(v);
}

static std::string py_repr_list(const json& a) {
  std::vector<std::string> out;
  for (auto& x : a) out.push_back(x.is_string() ? "'" + x.get<std::string>() + "'" : py_str(x));
  return "[" + join(out, ", ") + "]";
}

std::pair<json, std::vector<std::string>> validate_args(const json& schema, const json& args_in) {
  std::vector<std::string> errors;
  json args = args_in.is_object() ? args_in : json::object();
  json props = schema.is_object() ? schema.value("properties", json::object()) : json::object();
  if (!props.is_object()) props = json::object();
  json clean = json::object();
  for (auto& key : schema.value("required", json::array())) {
    std::string k = key.is_string() ? key.get<std::string>() : key.dump();
    if (!args.contains(k) || args[k].is_null() || (args[k].is_string() && args[k].get<std::string>().empty()))
      errors.push_back("falta el parametro '" + k + "'");
  }
  for (auto& [key, value0] : args.items()) {
    if (!props.contains(key)) continue;   // se ignoran parametros desconocidos
    const json& spec = props[key];
    std::string typ = spec.is_object() ? spec.value("type", "string") : "string";
    json value = value0;
    bool bad = false;
    try {
      if (typ == "integer" && !(value.is_number_integer() || value.is_boolean())) {
        double d;
        if (value.is_number()) {
          d = value.get<double>();
        } else if (value.is_string()) {
          std::string s = trim(value.get<std::string>());
          size_t used = 0;
          d = std::stod(s, &used);
          if (used != s.size()) throw std::invalid_argument("x");
        } else {
          throw std::invalid_argument("x");
        }
        if (!std::isfinite(d)) throw std::invalid_argument("x");
        value = (long long)d;
      } else if (typ == "number" && !(value.is_number() || value.is_boolean())) {
        if (!value.is_string()) throw std::invalid_argument("x");
        std::string s = trim(value.get<std::string>());
        size_t used = 0;
        double d = std::stod(s, &used);
        if (used != s.size()) throw std::invalid_argument("x");
        value = d;
      } else if (typ == "boolean" && !value.is_boolean()) {
        std::string s = lower(py_str(value));
        value = s == "1" || s == "true" || s == "si" || s == "sí" || s == "yes";
      } else if (typ == "string" && !value.is_string()) {
        value = py_str(value);
      }
    } catch (...) {
      errors.push_back("'" + key + "' debe ser " + typ);
      bad = true;
    }
    if (bad) continue;
    if (spec.is_object() && spec.contains("enum")) {
      bool in = false;
      for (auto& e : spec["enum"])
        if (e == value || (e.is_number() && value.is_number() && e.get<double>() == value.get<double>())) in = true;
      if (!in) errors.push_back("'" + key + "' debe ser uno de " + py_repr_list(spec["enum"]));
    }
    if (!py_isinstance(value, typ)) errors.push_back("'" + key + "' debe ser " + typ);
    clean[key] = value;
  }
  for (auto& [key, spec] : props.items())
    if (!clean.contains(key) && spec.is_object() && spec.contains("default")) clean[key] = spec["default"];
  return {clean, errors};
}

// ---------------------------------------------------------------- Python del sistema
std::vector<std::string> python_cmd() {
  static std::vector<std::string> cmd = [] {
    std::string forced = env("NEXUS_PYTHON");
    if (!forced.empty()) return std::vector<std::string>{forced};
    for (auto n : {"python3", "python"}) {
      std::string p = proc::which(n);
      if (p.empty()) continue;
      auto r = proc::run({p, "-c", "import sys; print(sys.version_info[0])"}, 10, {.capture = true});
      if (r.ok() && trim(r.out) == "3") return std::vector<std::string>{p};
    }
    if (sys::is_windows()) {
      std::string py = proc::which("py");
      if (!py.empty()) return std::vector<std::string>{py, "-3"};
    }
    return std::vector<std::string>{};
  }();
  return cmd;
}

// ayudante Python: manifiesto por AST, analisis de propuestas y ejecucion aislada (tools/runner.py)
static const char* HELPER = R"PY(import ast, importlib.util, json, sys

def manifest(path):
    try:
        tree = ast.parse(open(path, encoding="utf-8").read())
    except (SyntaxError, OSError):
        return None
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "TOOL" for t in node.targets):
            try:
                val = ast.literal_eval(node.value)
                return val if isinstance(val, dict) else None
            except ValueError:
                return None
    return None

DANGEROUS_IMPORTS = {"subprocess": "high", "os": "medium", "shutil": "high", "socket": "medium", "ctypes": "high",
                     "multiprocessing": "high", "winreg": "high", "importlib": "high", "pickle": "medium",
                     "requests": "medium", "httpx": "medium", "urllib": "medium", "sys": "medium", "pathlib": "medium",
                     "threading": "medium", "signal": "high", "marshal": "high", "builtins": "high"}
DANGEROUS_CALLS = {"eval": "high", "exec": "high", "compile": "high", "__import__": "high", "open": "medium",
                   "globals": "high", "locals": "medium", "getattr": "medium", "setattr": "high", "delattr": "high",
                   "system": "high", "popen": "high", "remove": "high", "rmtree": "high", "unlink": "high",
                   "kill": "high"}
RISKS = ("low", "medium", "high")

def analyze(code, name=""):
    errors, warnings, imports, calls = [], [], [], []
    try:
        tree = ast.parse(code)
    except SyntaxError as e:
        return {"ok": False, "errors": [f"Error de sintaxis linea {e.lineno}: {e.msg}"], "warnings": [],
                "imports": [], "calls": [], "suggested_risk": "high", "manifest": None}
    manifest, has_run = None, False
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "TOOL" for t in node.targets):
            try:
                manifest = ast.literal_eval(node.value)
            except ValueError:
                errors.append("TOOL debe ser un diccionario literal")
        if isinstance(node, ast.FunctionDef) and node.name == "run":
            has_run = len(node.args.args) >= 1
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imports += [a.name.split(".")[0] for a in node.names]
        elif isinstance(node, ast.ImportFrom) and node.module:
            imports.append(node.module.split(".")[0])
        elif isinstance(node, ast.Call):
            fn = node.func
            nm = fn.id if isinstance(fn, ast.Name) else fn.attr if isinstance(fn, ast.Attribute) else ""
            if nm in DANGEROUS_CALLS:
                calls.append(nm)
        elif isinstance(node, ast.Attribute) and node.attr.startswith("__") and node.attr not in ("__name__", "__doc__"):
            warnings.append(f"acceso a atributo interno {node.attr}")
    if manifest is None:
        errors.append("Falta TOOL = {...} con name/description/parameters")
    elif not isinstance(manifest, dict) or not manifest.get("name"):
        errors.append("TOOL necesita al menos 'name' y 'description'")
    if not has_run:
        errors.append("Falta la funcion run(args, ctx)")
    if name and manifest and isinstance(manifest, dict) and manifest.get("name") != name:
        warnings.append(f"el nombre del manifiesto ({manifest.get('name')}) no coincide con '{name}'")
    risks = [DANGEROUS_IMPORTS[i] for i in imports if i in DANGEROUS_IMPORTS] + [DANGEROUS_CALLS[c] for c in calls]
    for i in sorted(set(imports)):
        if i in DANGEROUS_IMPORTS:
            warnings.append(f"importa '{i}' (riesgo {DANGEROUS_IMPORTS[i]})")
    for c in sorted(set(calls)):
        warnings.append(f"llama a '{c}' (riesgo {DANGEROUS_CALLS[c]})")
    return {"ok": not errors, "errors": errors, "warnings": warnings, "imports": sorted(set(imports)),
            "calls": sorted(set(calls)), "suggested_risk": max(["low", *risks], key=RISKS.index), "manifest": manifest}

def run(path, args):
    spec = importlib.util.spec_from_file_location("community_tool", path)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
        return {"ok": True, "result": mod.run(args, None)}
    except Exception as e:
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}

req = json.loads(sys.stdin.read())
if req["cmd"] == "manifest":
    out = manifest(req["path"])
elif req["cmd"] == "analyze":
    out = analyze(req["code"], req.get("name", ""))
else:
    out = run(req["path"], req.get("args", {}))
sys.stdout.write("\n@@NEXUS_RESULT@@" + json.dumps(out, ensure_ascii=False, default=str))
)PY";

json python_helper(const json& req, double timeout, const fs::path& cwd) {
  auto py = python_cmd();
  if (py.empty()) throw ToolError("RuntimeError", "no hay Python en este equipo (necesario para las herramientas de la comunidad)");
  fs::path helper = paths::sub({"tools"}) / "_nexus_helper.py";
  if (read_text(helper) != HELPER) write_file(helper, HELPER);
  std::vector<std::string> cmd = py;
  cmd.insert(cmd.end(), {"-I", "-X", "utf8", S(helper)});
  proc::Options o;
  o.capture = true;
  o.input = jdump(req);
  o.cwd = cwd;
  o.env["PYTHONIOENCODING"] = "utf-8";
  auto r = proc::run(cmd, timeout, o);
  if (r.timed_out) throw ToolError("TimeoutError", "tiempo agotado");
  size_t m = r.out.rfind("@@NEXUS_RESULT@@");
  if (m == std::string::npos)
    throw ToolError("RuntimeError", "la herramienta no devolvio resultado. stderr: " + uright(r.err, 500));
  return jparse(r.out.substr(m + 16));
}

json read_manifest(const fs::path& path) {
  try {
    json m = python_helper({{"cmd", "manifest"}, {"path", S(path)}}, 30, paths::sandbox_dir());
    return m.is_object() ? m : json(nullptr);
  } catch (const std::exception&) {
    return nullptr;
  }
}

// ---------------------------------------------------------------- registro
ToolRegistry::ToolRegistry(bool sandbox_community) : sandbox_community_(sandbox_community) {
  json d = jparse(read_text(disabled_file()));
  if (d.is_array())
    for (auto& x : d)
      if (x.is_string()) disabled_.insert(x.get<std::string>());
  reload();
}

fs::path ToolRegistry::disabled_file() const { return paths::sub({"tools"}) / "disabled.json"; }

void ToolRegistry::set_enabled(const std::string& name, bool enabled) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (enabled)
    disabled_.erase(name);
  else
    disabled_.insert(name);
  json arr = json::array();
  for (auto& n : disabled_) arr.push_back(n);
  write_file(disabled_file(), jdump(arr));
  auto it = tools_.find(name);
  if (it != tools_.end()) it->second->enabled = enabled;
}

std::shared_ptr<ToolSpec> ToolRegistry::spec(const json& m, const std::string& source, const std::string& path, ToolFn func) {
  if (!m.is_object() || jstr(m, "name").empty() || jstr(m, "description").empty()) return nullptr;
  auto s = std::make_shared<ToolSpec>();
  s->name = jstr(m, "name");
  s->description = jstr(m, "description");
  if (m.contains("parameters") && truthy(m["parameters"])) s->parameters = m["parameters"];
  std::string risk = m.contains("risk") ? jstr(m, "risk") : "medium";
  s->risk = std::find(RISKS.begin(), RISKS.end(), risk) != RISKS.end() ? risk : "high";
  for (auto& p : m.value("permissions", json::array()))
    if (p.is_string()) s->permissions.push_back(p);
  s->version = m.contains("version") ? jstr(m, "version") : "1.0";
  if (m.contains("version") && m["version"].is_number_float()) s->version = pyfloat(m["version"].get<double>());
  s->author = m.contains("author") ? jstr(m, "author") : (source == "builtin" ? "nexus" : "comunidad");
  s->source = source;
  s->path = path;
  s->func = std::move(func);
  s->enabled = !disabled_.count(s->name);
  if (m.contains("timeout") && truthy(m["timeout"])) s->timeout = jnum(m["timeout"]);
  return s;
}

void ToolRegistry::reload() {
  std::vector<std::shared_ptr<ToolSpec>> order;
  std::map<std::string, std::shared_ptr<ToolSpec>> found;
  std::vector<std::string> missing;
  auto& fns = builtin_functions();
  for (auto& m : data::get("tools")) {
    std::string name = jstr(m, "name");
    auto it = fns.find(name);
    if (it == fns.end()) {
      missing.push_back(name);
      continue;
    }
    auto s = spec(m, "builtin", "builtin", it->second);
    if (!s) continue;
    if (!found.count(s->name)) order.push_back(s);
    found[s->name] = s;
  }
  fs::path cdir = paths::sub({"tools", "community"});
  auto py = python_cmd();
  for (auto& path : glob(cdir, "*.py")) {
    if (py.empty()) {
      L.warning("Herramienta de comunidad ", S(path.filename()), " sin Python en el equipo: no se carga");
      continue;
    }
    json manifest = read_manifest(path);
    if (manifest.is_null()) {
      L.warning("Herramienta de comunidad sin manifiesto valido: ", S(path.filename()));
      continue;
    }
    auto s = spec(manifest, "community", S(path), nullptr);   // siempre aislada en un subproceso
    if (s && !found.count(s->name)) {   // una de comunidad nunca sustituye a una integrada
      found[s->name] = s;
      order.push_back(s);
    }
  }
  size_t community = 0;
  for (auto& s : order) community += s->source == "community";
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    order_ = order;
    tools_ = found;
    this->missing = missing;
  }
  L.info(order.size(), " herramientas cargadas (", community, " de comunidad)");
  if (!missing.empty()) L.info("Herramientas aun no portadas a la edicion C++: ", join(missing, ", "));
}

std::shared_ptr<ToolSpec> ToolRegistry::get(const std::string& name) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<ToolSpec>> ToolRegistry::all() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return order_;
}

std::vector<std::shared_ptr<ToolSpec>> ToolRegistry::for_agent(const std::vector<std::string>& allowed) {
  if (allowed.empty()) return {};
  bool every = std::find(allowed.begin(), allowed.end(), "*") != allowed.end();
  std::vector<std::shared_ptr<ToolSpec>> out;
  for (auto& t : all())
    if (t->enabled && (every || std::find(allowed.begin(), allowed.end(), t->name) != allowed.end())) out.push_back(t);
  return out;
}

json ToolRegistry::list() {
  auto v = all();
  std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) {
    return std::tie(a->source, a->name) < std::tie(b->source, b->name);
  });
  json out = json::array();
  for (auto& t : v) out.push_back(t->public_json());
  return out;
}

}  // namespace nx::tools
