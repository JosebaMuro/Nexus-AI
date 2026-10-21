// Edicion profesional de imagenes con GIMP (media/gimp) sobre la carpeta de trabajo de los agentes
// (tools/builtin/gimp_tools.py). Las adjuntas del chat estan en adjuntos/, las editadas van a imagenes/. Tambien se
// aceptan las imagenes generadas (/outputs/images/...) y «ultima» para la mas reciente. Sin GIMP lo hace media/raster.
#include <set>

#include "core/services.h"
#include "media/gimp.h"
#include "media/media.h"
#include "media/raster.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/text.h"

namespace nx::tools {

namespace {

const std::set<std::string> IMAGE_EXT = {".png", ".jpg", ".jpeg", ".webp", ".tif", ".tiff", ".bmp", ".gif", ".xcf", ".psd"};
const std::set<std::string> PREVIEW_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif"};
const std::set<std::string> LAST = {"", "ultima", "última", "last", "la ultima", "la última"};

[[noreturn]] void fail(const std::string& msg) { throw ToolError("EditError", msg); }

gimp::GimpEngine& engine(Services* s) {
  static std::mutex mu;
  static std::map<Services*, std::unique_ptr<gimp::GimpEngine>> engines;
  std::lock_guard<std::mutex> lk(mu);
  auto& e = engines[s];
  if (!e) e = std::make_unique<gimp::GimpEngine>(s ? &s->config : nullptr);
  return *e;
}

std::optional<fs::path> latest_image() {
  std::optional<fs::path> best;
  double bt = -1;
  for (auto& r : {paths::sandbox_dir() / "imagenes", paths::sandbox_dir() / "adjuntos", paths::sub({"outputs", "images"})}) {
    if (!is_dir(r)) continue;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(r, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || !IMAGE_EXT.count(suffix_lower(it->path())) ||
          starts_with(S(it->path().filename()), "."))
        continue;
      double t = mtime(it->path());
      if (t > bt) bt = t, best = it->path();
    }
  }
  return best;
}

fs::path resolve_input(const std::string& ref0) {
  std::string ref = replace_all(trim(trim(ref0), "\""), "\\", "/");
  if (LAST.count(lower(ref))) {
    auto p = latest_image();
    if (!p) fail("no hay ninguna imagen en la carpeta de trabajo");
    return *p;
  }
  fs::path root, p;
  if (starts_with(ref, "/outputs/") || starts_with(ref, "outputs/")) {
    root = paths::outputs_dir();
    p = resolve(root / P(ref.substr(ref.find("outputs/") + 8)));
  } else {
    root = paths::sandbox_dir();
    p = resolve(root / P(ltrim(ref, "/")));
  }
  if (!is_relative_to(p, root)) fail("la imagen tiene que estar en la carpeta de trabajo");
  if (!is_file(p)) fail("no existe la imagen " + ref + " (usa file_list para ver la carpeta de trabajo)");
  if (!IMAGE_EXT.count(suffix_lower(p))) fail(S(p.filename()) + " no es una imagen");
  return p;
}

std::string workspace_path(const fs::path& p) {
  fs::path r = resolve(p), sb = resolve(paths::sandbox_dir());
  if (is_relative_to(r, sb)) return r.lexically_relative(sb).generic_string();
  return "/outputs/" + r.lexically_relative(resolve(paths::outputs_dir())).generic_string();
}

std::string fmt_of(const std::string& ext) {
  auto it = gimp::FORMATS.find(ext);
  return it == gimp::FORMATS.end() ? "" : it->second;
}

fs::path output_path(const fs::path& src, const std::string& salida0, const std::string& fmt) {
  fs::path root = paths::sandbox_dir();
  std::string salida = ltrim(replace_all(trim(salida0), "\\", "/"), "/");
  std::string rel = !salida.empty() ? (contains(salida, "/") ? salida : "imagenes/" + salida)
                                    : "imagenes/" + stem(src) + "_editada";
  // re.sub(r"[^\w./ -]+", "_", rel)
  std::u32string in = u32(rel), outs;
  bool run = false;
  for (char32_t c : in) {
    bool ok = is_word_cp(c) || c == U'.' || c == U'/' || c == U' ' || c == U'-';
    if (ok) outs += c, run = false;
    else if (!run) outs += U'_', run = true;
  }
  fs::path p = resolve(root / P(u8(outs)));
  if (!is_relative_to(p, root)) fail("la salida tiene que quedar en la carpeta de trabajo");
  std::string sx = suffix_lower(p);
  std::string ext = fmt_of(sx.empty() ? "" : sx.substr(1));
  std::string want = fmt_of(fmt);
  if (!want.empty()) {
    p = !ext.empty() ? fs::path(p).replace_extension("." + want) : p.parent_path() / (S(p.filename()) + "." + want);
  } else if (ext.empty()) {   // sin formato: el del original, si no PNG
    std::string ss = suffix_lower(src);
    std::string f = fmt_of(ss.empty() ? "" : ss.substr(1));
    p = p.parent_path() / (S(p.filename()) + "." + (f.empty() ? "png" : f));
  }
  fs::path base = p;
  for (int n = 2; path_exists(p); ++n)   // nunca se pisa un fichero (tampoco el original)
    p = base.parent_path() / (stem(base) + "_" + std::to_string(n) + S(base.extension()));
  mkdirs(p.parent_path());
  return p;
}

std::optional<std::string> preview(const fs::path& p) {
  if (!PREVIEW_EXT.count(suffix_lower(p))) return std::nullopt;
  fs::path dst = paths::sub({"outputs", "images"}) / (std::to_string((long long)now()) + "_" + S(p.filename()));
  std::error_code ec;
  fs::copy_file(p, dst, fs::copy_options::overwrite_existing, ec);
  return "/outputs/images/" + S(dst.filename());
}

template <class F>
json guarded(F&& f) {
  try {
    return f();
  } catch (const gimp::EditError& e) {
    fail(e.what());
  }
}

json imagen_editar(const json& args, ToolContext& ctx) {
  return guarded([&] {
    fs::path src = resolve_input(jstr(args, "imagen"));
    json ops = args.value("operaciones", json());
    if (!truthy(ops)) ops = json::array();
    std::string fmt = lower(jstr(args, "formato"));
    json quality = truthy(args.value("calidad", json())) ? args["calidad"] : json(90);
    std::string receta = trim(jstr(args, "receta"));
    if (!receta.empty()) {
      const json& R = gimp::recipes();
      if (!R.contains(receta)) {
        std::vector<std::string> names;
        for (auto& [k, _] : R.items()) names.push_back(k);
        fail("receta desconocida: " + receta + ". Disponibles: " + join(names, ", "));
      }
      const json& r = R[receta];
      json all = r["ops"];
      if (ops.is_array())
        for (auto& o : ops) all.push_back(o);
      else
        all.push_back(ops);
      ops = all;
      if (fmt.empty()) fmt = jstr(r, "formato");
      quality = truthy(args.value("calidad", json())) ? args["calidad"] : r.value("calidad", json(90));
    }
    std::string sx = suffix_lower(src);
    if (fmt.empty() && (sx == ".xcf" || sx == ".psd" || sx == ".gif")) fmt = "png";
    fs::path out = output_path(src, jstr(args, "salida"), fmt);
    if (ctx.emit) ctx.emit({{"type", "progress"}, {"text", "Editando " + S(src.filename())}});
    json res = engine(ctx.services).edit(src, ops, out, (int)jnum(quality, 90), "",
                                         [](const std::string& ref) { return resolve_input(ref); });
    res["original"] = workspace_path(src);
    res["ruta"] = workspace_path(out);
    res["bytes"] = fsize(out);
    if (auto url = preview(out)) {
      res["url"] = *url;
      if (ctx.emit) ctx.emit({{"type", "media"}, {"kind", "image"}, {"url", *url}, {"engine", res["motor"]}});
    }
    return res;
  });
}

json imagen_info(const json& args, ToolContext&) {
  return guarded([&] {
    fs::path src = resolve_input(jstr(args, "imagen"));
    json out = {{"imagen", workspace_path(src)}, {"bytes", fsize(src)}};
    try {
      raster::Info in = raster::probe(src);
      out["formato"] = in.format;
      out["modo"] = in.mode;
      out["ancho"] = in.w;
      out["alto"] = in.h;
      out["transparencia"] = in.mode == "RGBA" || in.mode == "LA" || in.mode == "PA" || in.transparency;
      if (in.dpi && (in.dpi->first || in.dpi->second)) {
        long dx = std::lround(in.dpi->first), dy = std::lround(in.dpi->second);
        out["ppp"] = {dx, dy};
        if (dx && dy)
          out["tamano_impresion_cm"] = {round_to(in.w / (double)dx * 2.54, 1), round_to(in.h / (double)dy * 2.54, 1)};
      }
      static const std::vector<std::pair<int, const char*>> names = {
          {0x010F, "fabricante"}, {0x0110, "camara"}, {0x0132, "fecha"},
          {0x0112, "orientacion"}, {0x013B, "autor"}, {0x8298, "copyright"}};
      json meta = json::object();
      for (auto& [k, v] : names)
        if (auto it = in.exif.find(k); it != in.exif.end()) meta[v] = uleft(it->second, 80);
      if (!meta.empty()) out["exif"] = meta;
      raster::Image img = raster::to_rgb(raster::load(src, nullptr, false));
      json stats = media::ImageEngine::describe_stats(img.px, img.w, img.h);
      json an = json::object();
      for (const char* k : {"dominant_colors", "brightness", "summary"})
        if (stats.contains(k)) an[k] = stats[k];
      out["analisis"] = an;
    } catch (const std::exception& e) {
      out["nota"] = std::string("Pillow no lee este formato (") + e.what() + "); GIMP si puede editarlo";
    }
    return out;
  });
}

json gimp_estado(const json& args, ToolContext& ctx) {
  return engine(ctx.services).status(truthy(args.value("buscar_de_nuevo", json(false))));
}

json gimp_script(const json& args, ToolContext& ctx) {
  return guarded([&] {
    std::optional<fs::path> src, out;
    if (truthy(args.value("imagen", json()))) src = resolve_input(jstr(args, "imagen"));
    if (truthy(args.value("salida", json()))) out = output_path(src ? *src : fs::path("script"), jstr(args, "salida"), "");
    json res = engine(ctx.services).run_script(jstr(args, "codigo"), src, out, jnum(args.value("segundos", json(300)), 300));
    if (out && is_file(*out)) {
      res["ruta"] = workspace_path(*out);
      if (auto url = preview(*out)) {
        res["url"] = *url;
        if (ctx.emit) ctx.emit({{"type", "media"}, {"kind", "image"}, {"url", *url}, {"engine", res["motor"]}});
      }
    }
    return res;
  });
}

Register r1("imagen_editar", imagen_editar), r2("imagen_info", imagen_info), r3("gimp_estado", gimp_estado),
    r4("gimp_script", gimp_script);

}  // namespace

void link_builtin_gimp() {}

}  // namespace nx::tools
