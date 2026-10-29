// GIMP como motor de edicion de imagen para los agentes (nexus/media/gimp.py).
//
// Orden de busqueda del programa: media.gimp_path de la configuracion, el GIMP incluido en el paquete offline
// (bundled/runtimes/gimp) y el del sistema (PATH, Archivos de programa, /Applications, Flatpak, Snap). GIMP se
// ejecuta sin ventana con su propio perfil en data/gimp. Sin GIMP el trabajo lo hace media/raster con las mismas
// operaciones y parametros.
#include "media/gimp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <set>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "core/config.h"
#include "media/raster.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::gimp {

namespace {
auto L = log::get("gimp");

const char* SCM_PRELUDE = R"NX(
(define img (car (gimp-file-load RUN-NONINTERACTIVE nx-in nx-in)))
(define drw 0)
(define (nx-merge) (set! drw (car (gimp-image-merge-visible-layers img CLIP-TO-IMAGE))))
(nx-merge)
(gimp-context-push)
(gimp-context-set-interpolation INTERPOLATION-LOHALO)
(gimp-context-set-background '(255 255 255))
(define (nx-lienzo w h x y c)
  (gimp-image-resize img w h x y)
  (let ((bg (car (gimp-layer-new img w h RGBA-IMAGE "fondo" 100 LAYER-MODE-NORMAL))))
    (gimp-image-insert-layer img bg 0 -1)
    (gimp-image-lower-item-to-bottom img bg)
    (gimp-context-set-foreground c)
    (gimp-drawable-fill bg FILL-FOREGROUND)
    (nx-merge)))
(define (nx-vineta f)
  (let* ((W (car (gimp-image-width img))) (H (car (gimp-image-height img)))
         (l (car (gimp-layer-new img W H RGBA-IMAGE "vineta" f LAYER-MODE-NORMAL))))
    (gimp-image-insert-layer img l 0 -1)
    (gimp-image-select-ellipse img CHANNEL-OP-REPLACE (* W 0.04) (* H 0.04) (* W 0.92) (* H 0.92))
    (gimp-selection-feather img (* 0.4 (min W H)))
    (gimp-selection-invert img)
    (gimp-context-set-foreground '(0 0 0))
    (gimp-drawable-edit-fill l FILL-FOREGROUND)
    (gimp-selection-none img)
    (nx-merge)))
(define (nx-texto t s c pos op font)
  (let* ((W (car (gimp-image-width img))) (H (car (gimp-image-height img)))
         (e (gimp-text-get-extents-fontname t s PIXELS font)) (tw (car e)) (th (cadr e))
         (m (* 0.03 (min W H)))
         (x (cond ((member pos '("arriba-izquierda" "izquierda" "abajo-izquierda")) m)
                  ((member pos '("arriba-derecha" "derecha" "abajo-derecha")) (- W tw m))
                  (else (/ (- W tw) 2))))
         (y (cond ((member pos '("arriba-izquierda" "arriba" "arriba-derecha")) m)
                  ((member pos '("abajo-izquierda" "abajo" "abajo-derecha")) (- H th m))
                  (else (/ (- H th) 2)))))
    (gimp-context-set-foreground c)
    (let ((l (car (gimp-text-fontname img -1 x y t 0 TRUE s PIXELS font))))
      (gimp-layer-set-opacity l op))
    (nx-merge)))
)NX";
const char* PY3 = R"NX(
import json, math, sys, traceback
import gi
gi.require_version("Gimp", "3.0")
gi.require_version("Gegl", "0.4")
from gi.repository import Gimp, Gio, Gegl
J = json.loads(JOB)
NONI = Gimp.RunMode.NONINTERACTIVE


def col(c):
    return Gegl.Color.new("#%02x%02x%02x" % tuple(c))


def merge():
    return img.merge_visible_layers(Gimp.MergeType.CLIP_TO_IMAGE)


def filt(d, op, props):
    f = Gimp.DrawableFilter.new(d, op, "")
    cfg = f.get_config()
    for k, v in props.items():
        cfg.set_property(k, v)
    f.update()
    d.merge_filter(f)


def place(pos, W, H, w, h):
    m = round(0.03 * min(W, H))
    x = m if "izquierda" in pos else W - w - m if "derecha" in pos else (W - w) // 2
    y = m if pos.startswith("arriba") else H - h - m if pos.startswith("abajo") else (H - h) // 2
    return int(x), int(y)


def lienzo(w, h, x, y, c):
    img.resize(w, h, x, y)
    bg = Gimp.Layer.new(img, "fondo", w, h, Gimp.ImageType.RGBA_IMAGE, 100, Gimp.LayerMode.NORMAL)
    img.insert_layer(bg, None, len(img.get_layers()))
    Gimp.context_set_foreground(col(c))
    bg.fill(Gimp.FillType.FOREGROUND)
    return merge()


def vineta(f):
    W, H = img.get_width(), img.get_height()
    lay = Gimp.Layer.new(img, "vineta", W, H, Gimp.ImageType.RGBA_IMAGE, f, Gimp.LayerMode.NORMAL)
    img.insert_layer(lay, None, 0)
    img.select_ellipse(Gimp.ChannelOps.REPLACE, W * 0.04, H * 0.04, W * 0.92, H * 0.92)
    Gimp.Selection.feather(img, 0.4 * min(W, H))
    Gimp.Selection.invert(img)
    Gimp.context_set_foreground(col((0, 0, 0)))
    lay.edit_fill(Gimp.FillType.FOREGROUND)
    Gimp.Selection.none(img)
    return merge()


def texto(p):
    W, H = img.get_width(), img.get_height()
    font = Gimp.Font.get_by_name(p["fuente"]) or Gimp.Font.get_by_name("Sans-serif") or Gimp.context_get_font()
    ext = Gimp.text_get_extents_font(p["texto"], p["tamano"], font)
    vals = [v for v in (ext if isinstance(ext, tuple) else (ext,)) if isinstance(v, int) and not isinstance(v, bool)]
    tw, th = (vals[0], vals[1]) if len(vals) >= 2 else (p["tamano"] * len(p["texto"]) // 2, p["tamano"])
    x, y = place(p["posicion"], W, H, tw, th)
    Gimp.context_set_foreground(col(p["color"]))
    lay = Gimp.text_font(img, None, x, y, p["texto"], 0, True, p["tamano"], font)
    lay.set_opacity(p["opacidad"])
    return merge()


def export(d):
    out = Gio.File.new_for_path(J["out"])
    if J["fmt"] == "jpg":
        try:
            proc = Gimp.get_pdb().lookup_procedure("file-jpeg-export")
            cfg = proc.create_config()
            for k, v in (("run-mode", NONI), ("image", img), ("file", out), ("quality", J["quality"] / 100.0)):
                cfg.set_property(k, v)
            res = proc.run(cfg)
            if res.index(0) == Gimp.PDBStatusType.SUCCESS:
                return
        except Exception:
            traceback.print_exc()
    if not Gimp.file_save(NONI, img, out, None):
        raise RuntimeError("no se pudo exportar")


img = Gimp.file_load(NONI, Gio.File.new_for_path(J["in"]))
try:
    Gimp.context_push()
    Gimp.context_set_interpolation(Gimp.InterpolationType.LOHALO)
    Gimp.context_set_background(col((255, 255, 255)))
    d = merge()
    for p in J["ops"]:
        n = p["op"]
        if n == "escalar":
            img.scale(p["w"], p["h"])
        elif n == "recortar":
            img.crop(p["w"], p["h"], p["x"], p["y"])
        elif n == "lienzo":
            d = lienzo(p["w"], p["h"], p["x"], p["y"], p["color"])
        elif n == "rotar":
            g = p["grados"]
            if g in (90, 180, 270):
                img.rotate(getattr(Gimp.RotationType, "DEGREES%d" % g))
            else:
                d.transform_rotate(math.radians(g), True, 0, 0)
                img.resize_to_layers()
        elif n == "voltear":
            img.flip(Gimp.OrientationType.HORIZONTAL if p["direccion"] == "horizontal" else Gimp.OrientationType.VERTICAL)
        elif n == "brillo_contraste":
            d.brightness_contrast(p["brillo"] / 100.0, p["contraste"] / 100.0)
        elif n == "niveles_auto":
            filt(d, "gegl:stretch-contrast", {"keep-colors": True})
        elif n == "balance_blancos":
            d.levels_stretch()
        elif n == "saturacion":
            d.hue_saturation(Gimp.HueRange.ALL, 0, 0, p["cantidad"], 0)
        elif n == "tono":
            d.hue_saturation(Gimp.HueRange.ALL, p["grados"], 0, 0, 0)
        elif n == "gamma":
            d.levels(Gimp.HistogramChannel.VALUE, 0.0, 1.0, True, p["valor"], 0.0, 1.0, True)
        elif n == "blanco_negro":
            d.desaturate(Gimp.DesaturateMode.LUMINANCE)
        elif n == "sepia":
            d.colorize_hsl(32, 28, 0)
        elif n == "enfocar":
            filt(d, "gegl:unsharp-mask", {"std-dev": p["radio"], "scale": p["cantidad"], "threshold": p["umbral"] / 255.0})
        elif n == "desenfocar":
            s = p["radio"] / 3.33
            filt(d, "gegl:gaussian-blur", {"std-dev-x": s, "std-dev-y": s})
        elif n == "reducir_ruido":
            filt(d, "gegl:noise-reduction", {"iterations": min(8, p["fuerza"])})
        elif n == "invertir":
            d.invert(False)
        elif n == "posterizar":
            d.posterize(p["niveles"])
        elif n == "vineta":
            d = vineta(p["fuerza"])
        elif n == "texto":
            d = texto(p)
        elif n == "superponer":
            lay = Gimp.file_load_layer(NONI, img, Gio.File.new_for_path(p["ruta"]))
            img.insert_layer(lay, None, 0)
            lay.scale(p["w"], p["h"], False)
            lay.set_offsets(p["x"], p["y"])
            lay.set_opacity(p["opacidad"])
            d = merge()
        elif n == "ppp":
            img.set_resolution(p["valor"], p["valor"])
        elif n == "aplanar":
            Gimp.context_set_background(col(p["color"]))
            d = img.flatten()
            Gimp.context_set_background(col((255, 255, 255)))
    d = img.flatten() if J["fmt"] in ("jpg", "bmp") or not J["alpha"] else merge()
    export(d)
    print("NEXUS-OK")
except Exception:
    print("NEXUS-ERROR")
    traceback.print_exc()
finally:
    img.delete()
    Gimp.context_pop()
)NX";

long iround(double v) { return (long)std::nearbyint(v); }   // round() de Python (a par en los empates)
long floordiv(long a, long b) { return (long)std::floor((double)a / (double)b); }

std::string fix(double v, int d) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}

// repr() de Python, para los mensajes de error
std::string pyrepr(const json& v) {
  if (v.is_null()) return "None";
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_number_integer() || v.is_number_unsigned()) return v.dump();
  if (v.is_number_float()) return pyfloat(v.get<double>());
  if (v.is_string()) {
    std::string s = v.get<std::string>();
    char q = contains(s, "'") && !contains(s, "\"") ? '"' : '\'';
    std::string o(1, q);
    for (unsigned char c : s) {
      if (c == '\\') o += "\\\\";
      else if (c == (unsigned char)q) o += std::string("\\") + q;
      else if (c == '\n') o += "\\n";
      else if (c == '\r') o += "\\r";
      else if (c == '\t') o += "\\t";
      else if (c < 0x20 || c == 0x7f) {
        char b[8];
        std::snprintf(b, sizeof b, "\\x%02x", c);
        o += b;
      } else {
        o += (char)c;
      }
    }
    return o + q;
  }
  std::vector<std::string> parts;
  if (v.is_array()) {
    for (auto& x : v) parts.push_back(pyrepr(x));
    return "[" + join(parts, ", ") + "]";
  }
  for (auto& [k, x] : v.items()) parts.push_back(pyrepr(json(k)) + ": " + pyrepr(x));
  return "{" + join(parts, ", ") + "}";
}

std::string pystr(const json& v) { return v.is_string() ? v.get<std::string>() : pyrepr(v); }

bool pytruthy(const json& v) {
  if (v.is_null()) return false;
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number()) return v.get<double>() != 0;
  if (v.is_string() || v.is_array() || v.is_object()) return !v.empty();
  return true;
}

// float() de Python sobre un valor JSON (lanza si no se puede)
double pyfloat_of(const json& v) {
  if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    std::string s = trim(v.get<std::string>());
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    if (s.empty()) throw std::invalid_argument("float");
    size_t pos = 0;
    double d = std::stod(s, &pos);
    if (pos != s.size()) throw std::invalid_argument("float");
    return d;
  }
  throw std::invalid_argument("float");
}

double ratio(const std::string& s) {
  static const Rx R(R"(\s*(\d+(?:[.,]\d+)?)\s*[:x/]\s*(\d+(?:[.,]\d+)?)\s*)");
  auto m = R.fullmatch(s);
  if (!m) throw EditError("proporcion no valida: " + pyrepr(json(s)) + " (p. ej. 4:5)");
  double a = std::stod(replace_all(m->str(1), ",", ".")), b = std::stod(replace_all(m->str(2), ",", "."));
  if (a <= 0 || b <= 0) throw EditError("proporcion no valida");
  return a / b;
}

std::pair<long, long> place(const std::string& pos, long W, long H, long w, long h) {
  long m = iround(0.03 * std::min(W, H));
  long x = contains(pos, "izquierda") ? m : contains(pos, "derecha") ? W - w - m : floordiv(W - w, 2);
  long y = starts_with(pos, "arriba") ? m : starts_with(pos, "abajo") ? H - h - m : floordiv(H - h, 2);
  return {x, y};
}

std::string stamp() {
  static std::atomic<int> n{0};
  return std::to_string((long long)(now() * 1000)) + "_" + std::to_string(++n);
}

std::map<std::string, std::string> gimp_env(int major) {
  std::map<std::string, std::string> e;
  e[major >= 3 ? "GIMP3_DIRECTORY" : "GIMP2_DIRECTORY"] = S(paths::sub({"gimp", "perfil-" + std::to_string(major)}));
  if (!sys::is_windows() && env("LC_ALL").empty()) e["LC_ALL"] = "C.UTF-8";
  e["PYTHONHOME"] = "";   // el Python del sistema no debe colarse en el Python-Fu de GIMP
  e["PYTHONPATH"] = "";
  return e;
}

std::pair<int, std::string> run(const std::vector<std::string>& cmd, int major, double timeout) {
  proc::Options o;
  o.env = gimp_env(major);
  o.cwd = paths::sub({"gimp"});
  o.capture = true;
  auto r = proc::run(cmd, timeout, o);
  if (r.timed_out) throw EditError("GIMP no termino en " + std::to_string((long)timeout) + " s");
  if (!r.started) throw EditError("GIMP no arranca: " + (cmd.empty() ? std::string() : cmd[0]));
  return {r.code, r.out + r.err};
}

std::string clean_log(const std::string& text) {
  static const std::vector<std::string> noise = {
      "Gtk-WARNING", "cannot open display", "gimp_wire_read", "resource name is invalid", "Welcome to GIMP",
      "using gegl copy", "GLib-GObject-CRITICAL", "batch command executed successfully", "Fontconfig",
      "writable data folder", "Failed to save data", "gimp_flush(): error: Broken pipe"};
  std::vector<std::string> keep;
  for (auto& ln : splitlines(text)) {
    if (trim(ln).empty()) continue;
    bool bad = false;
    for (auto& n : noise) bad = bad || contains(ln, n);
    if (!bad) keep.push_back(ln);
  }
  return uright(join(keep, "\n"), 1500);
}

std::string scm_str(const std::string& s) {
  return "\"" + replace_all(replace_all(s, "\\", "\\\\"), "\"", "\\\"") + "\"";
}
std::string scm_col(const json& c) {
  return "'(" + c[0].dump() + " " + c[1].dump() + " " + c[2].dump() + ")";
}
std::string posix(const fs::path& p) { return p.generic_string(); }
// repr() de una ruta para el codigo Python de GIMP
std::string py_path(const fs::path& p) { return pyrepr(json(S(p))); }

double D(const json& p, const char* k) { return jnum(p[k]); }
long I(const json& p, const char* k) { return (long)jnum(p[k]); }

// rutas del GIMP incluido que acaban en estos componentes (Path.glob("**/a/b*"))
std::vector<fs::path> bundled_matches(const fs::path& base, const std::string& pattern) {
  std::vector<fs::path> out;
  bool deep = starts_with(pattern, "**/");
  auto parts = split(deep ? pattern.substr(3) : pattern, "/");
  std::error_code ec;
  if (!deep) {
    fs::path p = base;
    for (auto& x : parts) p /= P(x);
    for (auto& f : glob(p.parent_path(), parts.back()))
      if (is_file(f)) out.push_back(f);
    return out;
  }
  for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    fs::path rel = it->path().lexically_relative(base);
    std::vector<std::string> comps;
    for (const auto& c : rel) comps.push_back(S(c));
    if (comps.size() < parts.size()) continue;
    bool ok = true;
    for (size_t i = 0; i < parts.size() && ok; ++i)
      ok = fnmatch(parts[parts.size() - 1 - i], comps[comps.size() - 1 - i]);
    if (ok) out.push_back(it->path());
  }
  return out;
}

std::vector<fs::path> console_first(std::vector<fs::path> v) {
  std::stable_sort(v.begin(), v.end(), [](const fs::path& a, const fs::path& b) {
    std::string na = S(a.filename()), nb = S(b.filename());
    int ka = contains(na, "console") ? 0 : 1, kb = contains(nb, "console") ? 0 : 1;
    if (ka != kb) return ka < kb;
    return na.size() > nb.size();
  });
  return v;
}

std::vector<fs::path> sorted_rev(std::vector<fs::path> v) {
  std::sort(v.begin(), v.end());
  std::reverse(v.begin(), v.end());
  return v;
}

std::mutex probed_mu;
std::map<std::vector<std::string>, std::optional<Found>> probed;

}  // namespace

const std::vector<std::string> POSITIONS = {"arriba-izquierda", "arriba",         "arriba-derecha",
                                            "izquierda",        "centro",         "derecha",
                                            "abajo-izquierda",  "abajo",          "abajo-derecha"};
const std::map<std::string, std::string> FORMATS = {{"png", "png"},  {"jpg", "jpg"}, {"jpeg", "jpg"},
                                                    {"webp", "webp"}, {"tif", "tif"}, {"tiff", "tif"},
                                                    {"xcf", "xcf"},  {"bmp", "bmp"}};

const json& ops_spec() {
  static const json pos = POSITIONS;
  static const json OPS = [] {
    json c = json::array({"color", "#ffffff", nullptr, nullptr});
    return json{
        {"redimensionar", {{"ancho", {"int", 0, 0, 20000}}, {"alto", {"int", 0, 0, 20000}},
                           {"porcentaje", {"float", 0, 0, 1000}},
                           {"modo", {"str", "ajustar", nullptr, {"ajustar", "exacto", "rellenar"}}}}},
        {"recortar", {{"x", {"int", 0, 0, 20000}}, {"y", {"int", 0, 0, 20000}}, {"ancho", {"int", 0, 0, 20000}},
                      {"alto", {"int", 0, 0, 20000}}}},
        {"encuadrar", {{"proporcion", {"str", "1:1", nullptr, nullptr}}, {"ancho", {"int", 0, 0, 20000}},
                       {"alto", {"int", 0, 0, 20000}}, {"posicion", {"str", "centro", nullptr, pos}}}},
        {"lienzo", {{"ancho", {"int", 0, 0, 20000}}, {"alto", {"int", 0, 0, 20000}}, {"color", c}}},
        {"borde", {{"grosor", {"int", 20, 1, 2000}}, {"color", c}}},
        {"rotar", {{"grados", {"float", 90, -360, 360}}}},
        {"voltear", {{"direccion", {"str", "horizontal", nullptr, {"horizontal", "vertical"}}}}},
        {"brillo_contraste", {{"brillo", {"float", 0, -100, 100}}, {"contraste", {"float", 0, -100, 100}}}},
        {"niveles_auto", json::object()},
        {"balance_blancos", json::object()},
        {"saturacion", {{"cantidad", {"float", 15, -100, 100}}}},
        {"tono", {{"grados", {"float", 0, -180, 180}}}},
        {"gamma", {{"valor", {"float", 1.0, 0.1, 10}}}},
        {"blanco_negro", json::object()},
        {"sepia", json::object()},
        {"enfocar", {{"radio", {"float", 2.0, 0.1, 50}}, {"cantidad", {"float", 0.6, 0, 5}},
                     {"umbral", {"int", 0, 0, 255}}}},
        {"desenfocar", {{"radio", {"float", 5, 0.1, 200}}}},
        {"reducir_ruido", {{"fuerza", {"int", 3, 1, 10}}}},
        {"invertir", json::object()},
        {"posterizar", {{"niveles", {"int", 6, 2, 64}}}},
        {"vineta", {{"fuerza", {"float", 45, 0, 100}}}},
        {"texto", {{"texto", {"str", "", nullptr, nullptr}}, {"tamano", {"int", 0, 0, 2000}}, {"color", c},
                   {"posicion", {"str", "abajo-derecha", nullptr, pos}}, {"opacidad", {"float", 100, 0, 100}},
                   {"fuente", {"str", "Sans-serif", nullptr, nullptr}}}},
        {"superponer", {{"imagen", {"str", "", nullptr, nullptr}}, {"escala", {"float", 20, 1, 100}},
                        {"posicion", {"str", "abajo-derecha", nullptr, pos}}, {"opacidad", {"float", 85, 0, 100}}}},
        {"ppp", {{"valor", {"float", 300, 10, 2400}}}},
        {"aplanar", {{"color", c}}},
    };
  }();
  return OPS;
}

const json& op_help() {
  static const json H = {
      {"redimensionar", "ancho y/o alto en px o porcentaje; modo ajustar (cabe dentro, sin deformar), exacto o rellenar"},
      {"recortar", "x, y, ancho, alto en px"},
      {"encuadrar", "recorta a una proporción (1:1, 4:5, 16:9, 26:32...) y, si se indica, deja ancho x alto exactos"},
      {"lienzo", "amplía el lienzo centrado con un color"},
      {"borde", "marco de grosor px"},
      {"rotar", "grados en sentido horario"},
      {"voltear", "horizontal o vertical"},
      {"brillo_contraste", "-100 a 100"},
      {"niveles_auto", "estira el contraste sin cambiar los colores"},
      {"balance_blancos", "corrige dominantes de color"},
      {"saturacion", "-100 a 100"},
      {"tono", "-180 a 180 grados"},
      {"gamma", "0,1 a 10 (más de 1 aclara las sombras)"},
      {"blanco_negro", ""},
      {"sepia", ""},
      {"enfocar", "máscara de enfoque: radio, cantidad, umbral"},
      {"desenfocar", "desenfoque gaussiano, radio px"},
      {"reducir_ruido", "fuerza 1-10"},
      {"invertir", "negativo"},
      {"posterizar", "niveles 2-64"},
      {"vineta", "oscurece los bordes, fuerza 0-100"},
      {"texto", "texto, tamano px, color, posicion, opacidad, fuente"},
      {"superponer", "pega otra imagen (logo) de la carpeta de trabajo: imagen, escala % del ancho, posicion, opacidad"},
      {"ppp", "resolución de impresión"},
      {"aplanar", "quita la transparencia sobre un color"},
  };
  return H;
}

const json& recipes() {
  static const json R = {
      {"foto_carnet",
       {{"descripcion", "DNI y pasaporte: 26 x 32 mm a 300 ppp (307 x 378 px), fondo claro"},
        {"ops", {{{"op", "aplanar"}, {"color", "#ffffff"}},
                 {{"op", "encuadrar"}, {"proporcion", "26:32"}, {"ancho", 307}, {"alto", 378}},
                 {{"op", "niveles_auto"}},
                 {{"op", "enfocar"}, {"radio", 1.0}, {"cantidad", 0.4}},
                 {{"op", "ppp"}, {"valor", 300}}}},
        {"formato", "jpg"}}},
      {"redes_cuadrado",
       {{"descripcion", "Publicación cuadrada 1080 x 1080 (Instagram, Facebook, LinkedIn)"},
        {"ops", {{{"op", "encuadrar"}, {"proporcion", "1:1"}, {"ancho", 1080}, {"alto", 1080}},
                 {{"op", "enfocar"}, {"radio", 1.0}, {"cantidad", 0.35}}}},
        {"formato", "jpg"}}},
      {"redes_vertical",
       {{"descripcion", "Publicación vertical 1080 x 1350 (4:5)"},
        {"ops", {{{"op", "encuadrar"}, {"proporcion", "4:5"}, {"ancho", 1080}, {"alto", 1350}},
                 {{"op", "enfocar"}, {"radio", 1.0}, {"cantidad", 0.35}}}},
        {"formato", "jpg"}}},
      {"historia",
       {{"descripcion", "Historia o reel 1080 x 1920 (9:16)"},
        {"ops", {{{"op", "encuadrar"}, {"proporcion", "9:16"}, {"ancho", 1080}, {"alto", 1920}}}},
        {"formato", "jpg"}}},
      {"banner_web",
       {{"descripcion", "Cabecera web 1920 x 600"},
        {"ops", {{{"op", "encuadrar"}, {"proporcion", "16:5"}, {"ancho", 1920}, {"alto", 600}}}},
        {"formato", "jpg"}}},
      {"producto_fondo_blanco",
       {{"descripcion", "Producto para tienda online: cuadrado 2000 px sobre blanco, colores corregidos y enfocado"},
        {"ops", {{{"op", "aplanar"}, {"color", "#ffffff"}},
                 {{"op", "balance_blancos"}},
                 {{"op", "niveles_auto"}},
                 {{"op", "redimensionar"}, {"ancho", 1800}, {"alto", 1800}},
                 {{"op", "lienzo"}, {"ancho", 2000}, {"alto", 2000}, {"color", "#ffffff"}},
                 {{"op", "enfocar"}, {"radio", 1.2}, {"cantidad", 0.5}}}},
        {"formato", "jpg"}}},
      {"mejora_rapida",
       {{"descripcion", "Retoque general: niveles, color, algo de saturación y enfoque"},
        {"ops", {{{"op", "balance_blancos"}},
                 {{"op", "niveles_auto"}},
                 {{"op", "saturacion"}, {"cantidad", 10}},
                 {{"op", "enfocar"}, {"radio", 1.5}, {"cantidad", 0.5}}}}}},
      {"web_ligera",
       {{"descripcion", "Imagen para web: 1600 px de lado mayor en WebP"},
        {"ops", {{{"op", "redimensionar"}, {"ancho", 1600}, {"alto", 1600}}}},
        {"formato", "webp"},
        {"calidad", 82}}},
      {"impresion_a4",
       {{"descripcion", "Para imprimir en A4 a 300 ppp (2480 x 3508 px), en TIFF"},
        {"ops", {{{"op", "redimensionar"}, {"ancho", 2480}, {"alto", 3508}}, {{"op", "ppp"}, {"valor", 300}}}},
        {"formato", "tif"}}},
  };
  return R;
}

std::array<int, 3> parse_color(const json& v) {
  static const std::map<std::string, std::array<int, 3>> NAMED = {
      {"blanco", {255, 255, 255}}, {"white", {255, 255, 255}}, {"negro", {0, 0, 0}},       {"black", {0, 0, 0}},
      {"rojo", {220, 30, 40}},     {"red", {220, 30, 40}},     {"verde", {30, 160, 70}},   {"green", {30, 160, 70}},
      {"azul", {30, 90, 200}},     {"blue", {30, 90, 200}},    {"amarillo", {250, 210, 30}},
      {"yellow", {250, 210, 30}},  {"gris", {128, 128, 128}},  {"gray", {128, 128, 128}}, {"grey", {128, 128, 128}},
      {"naranja", {245, 130, 30}}, {"orange", {245, 130, 30}}, {"rosa", {240, 110, 170}}, {"morado", {120, 60, 160}},
      {"beige", {235, 222, 200}},  {"transparente", {255, 255, 255}}};
  if (v.is_array() && v.size() >= 3) {
    std::array<int, 3> c{};
    for (int i = 0; i < 3; ++i) {
      double d = v[i].is_string() ? (double)std::stol(trim(v[i].get<std::string>())) : pyfloat_of(v[i]);
      c[i] = std::max(0, std::min(255, (int)std::trunc(d)));
    }
    return c;
  }
  std::string s = lower(trim(pytruthy(v) ? pystr(v) : std::string()));
  auto it = NAMED.find(s);
  if (it != NAMED.end()) return it->second;
  static const Rx HEX("#?([0-9a-f]{6}|[0-9a-f]{3})");
  auto m = HEX.fullmatch(s);
  if (!m) throw EditError("color no valido: " + pyrepr(v) + " (usa #rrggbb o un nombre como blanco, negro, rojo)");
  std::string h = m->str(1);
  if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
  return {std::stoi(h.substr(0, 2), nullptr, 16), std::stoi(h.substr(2, 2), nullptr, 16),
          std::stoi(h.substr(4, 2), nullptr, 16)};
}

json normalize(json ops) {
  static const std::map<std::string, std::string> ALIAS = {
      {"resize", "redimensionar"}, {"crop", "recortar"},     {"escalar", "redimensionar"}, {"girar", "rotar"},
      {"brillo", "brillo_contraste"}, {"contraste", "brillo_contraste"}, {"byn", "blanco_negro"},
      {"nitidez", "enfocar"},      {"marca_agua", "texto"},  {"logo", "superponer"},        {"dpi", "ppp"},
      {"viñeta", "vineta"},        {"ruido", "reducir_ruido"}};
  if (ops.is_string() || ops.is_object()) ops = json::array({ops});
  if (!ops.is_array()) ops = json::array();
  const json& OPS = ops_spec();
  json out = json::array();
  for (auto& raw : ops) {
    json item = raw.is_string() ? json{{"op", raw}} : raw.is_object() ? raw : json::object();
    json nm = item.value("op", json(""));
    item.erase("op");
    if (!pytruthy(nm)) nm = item.value("operacion", json(""));
    item.erase("operacion");
    std::string name = replace_all(lower(trim(pystr(nm))), " ", "_");
    if (auto a = ALIAS.find(name); a != ALIAS.end()) name = a->second;
    if (!OPS.contains(name)) {
      std::vector<std::string> names;
      for (auto& [k, _] : OPS.items()) names.push_back(k);
      throw EditError("operacion desconocida: " + pyrepr(json(name)) + ". Validas: " + join(names, ", "));
    }
    json clean = {{"op", name}};
    for (auto& [key, spec] : OPS[name].items()) {
      std::string typ = spec[0];
      json val = item.contains(key) ? item[key] : spec[1];
      try {
        if (typ == "color") {
          auto c = parse_color(val);
          clean[key] = {c[0], c[1], c[2]};
        } else if (typ == "str") {
          std::string s = pystr(val);
          if (spec[3].is_array()) {
            bool ok = false;
            for (auto& x : spec[3]) ok = ok || x == s;
            if (!ok) throw EditError("enum");
          }
          clean[key] = s;
        } else {
          double d = pyfloat_of(val);
          if (!std::isfinite(d)) throw std::invalid_argument("inf");
          double lo = spec[2].get<double>(), hi = spec[3].get<double>();
          if (typ == "int") clean[key] = (long long)std::max(lo, std::min(hi, std::trunc(d)));
          else clean[key] = std::max(lo, std::min(hi, d));
        }
      } catch (const std::exception&) {
        throw EditError(name + "." + key + ": valor no valido (" + pyrepr(item.value(key, json())) + ")");
      }
    }
    if (name == "texto" && trim(jstr(clean, "texto")).empty()) throw EditError("texto: falta el texto");
    if (name == "superponer" && jstr(clean, "imagen").empty())
      throw EditError("superponer: falta la imagen (ruta en la carpeta de trabajo)");
    out.push_back(clean);
  }
  if (out.empty()) throw EditError("no hay operaciones");
  if (out.size() > 40) throw EditError("como maximo 40 operaciones por edicion");
  return out;
}

std::pair<json, std::pair<int, int>> resolve(const json& ops, int W0, int H0,
                                             const std::function<std::pair<int, int>(const std::string&)>& overlay_size) {
  json prim = json::array();
  long W = W0, H = H0;
  for (auto& op : ops) {
    std::string n = op["op"];
    if (n == "redimensionar") {
      double w, h, pc = D(op, "porcentaje");
      long aw = I(op, "ancho"), ah = I(op, "alto");
      if (pc) {
        w = W * pc / 100, h = H * pc / 100;
      } else if (aw && ah) {
        w = aw, h = ah;
        std::string modo = op["modo"];
        if (modo == "ajustar" || modo == "rellenar") {
          double kw = w / W, kh = h / H, k = modo == "ajustar" ? std::min(kw, kh) : std::max(kw, kh);
          w = W * k, h = H * k;
        }
      } else if (aw) {
        w = aw, h = (double)H * aw / W;
      } else if (ah) {
        w = (double)W * ah / H, h = ah;
      } else {
        throw EditError("redimensionar: indica ancho, alto o porcentaje");
      }
      long rw = std::max(1L, iround(w)), rh = std::max(1L, iround(h));
      if (rw != W || rh != H) prim.push_back({{"op", "escalar"}, {"w", rw}, {"h", rh}});
      W = rw, H = rh;
    } else if (n == "recortar") {
      long x = std::min(I(op, "x"), W - 1), y = std::min(I(op, "y"), H - 1);
      long w = std::min(I(op, "ancho") ? I(op, "ancho") : W - x, W - x);
      long h = std::min(I(op, "alto") ? I(op, "alto") : H - y, H - y);
      prim.push_back({{"op", "recortar"}, {"x", x}, {"y", y}, {"w", w}, {"h", h}});
      W = w, H = h;
    } else if (n == "encuadrar") {
      double r = ratio(op["proporcion"]);
      long w, h;
      if ((double)W / H > r) w = std::max(1L, iround(H * r)), h = H;
      else w = W, h = std::max(1L, iround(W / r));
      std::string pos = op["posicion"];
      long x = contains(pos, "izquierda") ? 0 : contains(pos, "derecha") ? W - w : floordiv(W - w, 2);
      long y = starts_with(pos, "arriba") ? 0 : starts_with(pos, "abajo") ? H - h : floordiv(H - h, 2);
      if (w != W || h != H) prim.push_back({{"op", "recortar"}, {"x", x}, {"y", y}, {"w", w}, {"h", h}});
      W = w, H = h;
      long aw = I(op, "ancho"), ah = I(op, "alto");
      if (aw || ah) {
        long tw = aw ? aw : iround(ah * r), th = ah ? ah : iround(aw / r);
        if (tw != W || th != H) prim.push_back({{"op", "escalar"}, {"w", tw}, {"h", th}});
        W = tw, H = th;
      }
    } else if (n == "lienzo") {
      long w = std::max(W, I(op, "ancho") ? I(op, "ancho") : W), h = std::max(H, I(op, "alto") ? I(op, "alto") : H);
      prim.push_back({{"op", "lienzo"}, {"w", w}, {"h", h}, {"x", (w - W) / 2}, {"y", (h - H) / 2}, {"color", op["color"]}});
      W = w, H = h;
    } else if (n == "borde") {
      long g = I(op, "grosor");
      prim.push_back({{"op", "lienzo"}, {"w", W + 2 * g}, {"h", H + 2 * g}, {"x", g}, {"y", g}, {"color", op["color"]}});
      W += 2 * g, H += 2 * g;
    } else if (n == "rotar") {
      double deg = std::fmod(D(op, "grados"), 360.0);
      if (deg < 0) deg += 360;
      if (deg == 0) continue;
      prim.push_back({{"op", "rotar"}, {"grados", deg}});
      if (deg == 90 || deg == 270) {
        std::swap(W, H);
      } else if (deg != 180) {
        double a = deg * M_PI / 180;
        long nw = iround(std::fabs(W * std::cos(a)) + std::fabs(H * std::sin(a)));
        long nh = iround(std::fabs(W * std::sin(a)) + std::fabs(H * std::cos(a)));
        W = nw, H = nh;
      }
    } else if (n == "texto") {
      json p = op;
      if (!I(op, "tamano")) p["tamano"] = std::max(12L, iround(std::min(W, H) * 0.06));
      prim.push_back(p);
    } else if (n == "superponer") {
      auto [ow, oh] = overlay_size ? overlay_size(op["imagen"]) : std::pair<int, int>((int)W, (int)H);
      long w = std::max(1L, iround(W * D(op, "escala") / 100));
      long h = std::max(1L, iround((double)oh * w / ow));
      auto [x, y] = place(op["posicion"], W, H, w, h);
      json p = op;
      p["w"] = w, p["h"] = h, p["x"] = x, p["y"] = y;
      prim.push_back(p);
    } else {
      prim.push_back(op);
    }
    if ((double)W * H > 400000000.0) throw EditError("la imagen resultante seria demasiado grande");
  }
  return {prim, {(int)W, (int)H}};
}

// ------------------------------------------------------------------ deteccion de GIMP
std::vector<std::pair<std::vector<std::string>, std::string>> candidates(Config* config) {
  std::vector<std::pair<std::vector<std::string>, std::string>> out;
  std::string custom = config ? config->str("media.gimp_path") : "";
  if (custom.empty()) custom = env("NEXUS_GIMP");
  if (!custom.empty()) out.push_back({contains(custom, "|") ? split(custom, "|") : std::vector<std::string>{custom},
                                      "configuracion"});
  std::string os = sys::os_name();
  fs::path b = paths::root() / "bundled" / "runtimes" / "gimp";
  if (is_dir(b)) {
    std::vector<std::string> pats =
        os == "Windows" ? std::vector<std::string>{"**/bin/gimp-console*.exe", "**/bin/gimp-[0-9]*.exe"}
        : os == "Darwin" ? std::vector<std::string>{"**/Contents/MacOS/gimp-console*", "**/Contents/MacOS/gimp"}
                         : std::vector<std::string>{"nexus-gimp-console", "**/usr/bin/gimp-console*",
                                                    "**/bin/gimp-console*"};
    std::vector<fs::path> found;
    for (auto& pat : pats)
      for (auto& p : bundled_matches(b, pat)) found.push_back(p);
    for (auto& p : console_first(found)) out.push_back({{S(p)}, "incluido"});
  }
  std::vector<std::string> seen_which;
  for (const char* n : {"gimp-console-3.2", "gimp-console-3.0", "gimp-console-3", "gimp-console", "gimp-console-2.10",
                        "gimp-3.2", "gimp-3.0", "gimp-3", "gimp", "gimp-2.10"}) {
    std::string p = proc::which(n);
    if (!p.empty() && std::find(seen_which.begin(), seen_which.end(), p) == seen_which.end()) {
      seen_which.push_back(p);
      out.push_back({{p}, "sistema"});
    }
  }
  if (os == "Windows") {
    for (const char* v : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
      std::string r = env(v);
      if (r.empty()) continue;
      std::vector<fs::path> dirs;
      for (auto& d : sorted_rev(glob(P(r), "GIMP*")))
        if (is_dir(d / "bin")) dirs.push_back(d / "bin");
      for (auto& d : sorted_rev(glob(P(r) / "Programs", "GIMP*")))
        if (is_dir(d / "bin")) dirs.push_back(d / "bin");
      for (auto& d : dirs)
        for (auto& p : console_first(glob(d, "gimp-console*.exe"))) out.push_back({{S(p)}, "sistema"});
    }
  } else if (os == "Darwin") {
    std::vector<fs::path> apps = sorted_rev(glob("/Applications", "GIMP*.app"));
    for (auto& a : sorted_rev(glob(home_dir() / "Applications", "GIMP*.app"))) apps.push_back(a);
    for (auto& app : apps)
      for (auto& p : console_first(glob(app / "Contents" / "MacOS", "gimp*"))) out.push_back({{S(p)}, "sistema"});
  } else {
    for (auto fp : {fs::path("/var/lib/flatpak/exports/bin/org.gimp.GIMP"),
                    home_dir() / ".local/share/flatpak/exports/bin/org.gimp.GIMP"}) {
      if (path_exists(fp) && !proc::which("flatpak").empty()) {
        for (const char* c : {"gimp-console-3.2", "gimp-console-3.0", "gimp-console-2.10"})
          out.push_back({{"flatpak", "run", std::string("--command=") + c, "org.gimp.GIMP"}, "flatpak"});
        break;
      }
    }
    if (path_exists("/snap/bin/gimp")) out.push_back({{"/snap/bin/gimp"}, "snap"});
  }
  std::set<std::vector<std::string>> seen;
  std::vector<std::pair<std::vector<std::string>, std::string>> uniq;
  for (auto& c : out)
    if (seen.insert(c.first).second) uniq.push_back(c);
  return uniq;
}

std::optional<Found> probe(const std::vector<std::string>& cmd, const std::string& origin) {
  {
    std::lock_guard<std::mutex> lk(probed_mu);
    if (auto it = probed.find(cmd); it != probed.end()) return it->second;
  }
  std::optional<Found> info;
  static const Rx VERSION(R"(version\s+(\d+)\.(\d+)(?:\.(\d+))?)", Rx::I);
  std::vector<std::string> args = cmd;
  args.push_back("--version");
  proc::Options o;
  o.env = gimp_env(3);
  o.capture = true;
  auto r = proc::run(args, 60, o);
  if (r.started) {
    std::string text = r.out + r.err;
    if (auto m = VERSION.search(text)) {
      int major = std::stoi(m->str(1)), minor = std::stoi(m->str(2));
      std::string fname = S(P(cmd[0]).filename());
      static const Rx GUI(R"(gimp(-[\d.]+)?)");
      bool gui_only = cmd.size() == 1 && GUI.fullmatch(fname) && major >= 3 &&
                      sys::os_name() == "Linux" && env("DISPLAY").empty() && env("WAYLAND_DISPLAY").empty();
      if (major >= 2 && std::make_pair(major, minor) >= std::make_pair(2, 10) && !gui_only) {
        Found f;
        f.cmd = cmd;
        std::vector<std::string> v = {m->str(1), m->str(2)};
        if (m->has(3)) v.push_back(m->str(3));
        f.version = join(v, ".");
        f.major = major;
        f.origin = origin;
        f.interpreter = major >= 3 ? "python-fu" : "script-fu";
        info = f;
      }
    }
  } else {
    L.debug("GIMP no arranca con ", join(cmd, " "));
  }
  std::lock_guard<std::mutex> lk(probed_mu);
  probed[cmd] = info;
  return info;
}

std::optional<Found> find(Config* config, bool refresh) {
  if (refresh) {
    std::lock_guard<std::mutex> lk(probed_mu);
    probed.clear();
  }
  for (auto& [cmd, origin] : candidates(config))
    if (auto f = probe(cmd, origin)) return f;
  return std::nullopt;
}

// ------------------------------------------------------------------ Script-Fu (GIMP 2.10)
std::string script_fu(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                      bool alpha) {
  std::vector<std::string> lines = {"(define nx-in " + scm_str(posix(src)) + ")",
                                    "(define nx-out " + scm_str(posix(out)) + ")", SCM_PRELUDE};
  const std::string R = "RUN-NONINTERACTIVE";
  for (auto& p : prim) {
    std::string n = p["op"];
    auto i = [&](const char* k) { return std::to_string(I(p, k)); };
    if (n == "escalar") {
      lines.push_back("(gimp-image-scale img " + i("w") + " " + i("h") + ")");
    } else if (n == "recortar") {
      lines.push_back("(gimp-image-crop img " + i("w") + " " + i("h") + " " + i("x") + " " + i("y") + ")");
    } else if (n == "lienzo") {
      lines.push_back("(nx-lienzo " + i("w") + " " + i("h") + " " + i("x") + " " + i("y") + " " + scm_col(p["color"]) + ")");
    } else if (n == "rotar") {
      double d = D(p, "grados");
      if (d == 90 || d == 180 || d == 270)
        lines.push_back("(gimp-image-rotate img ROTATE-" + std::to_string((int)d) + ")");
      else
        lines.push_back("(gimp-item-transform-rotate drw " + fix(d * M_PI / 180, 6) +
                        " TRUE 0 0)(gimp-image-resize-to-layers img)");
    } else if (n == "voltear") {
      lines.push_back("(gimp-image-flip img ORIENTATION-" + upper(jstr(p, "direccion")) + ")");
    } else if (n == "brillo_contraste") {
      lines.push_back("(gimp-drawable-brightness-contrast drw " + fix(D(p, "brillo") / 100, 4) + " " +
                      fix(D(p, "contraste") / 100, 4) + ")");
    } else if (n == "niveles_auto") {
      lines.push_back("(plug-in-autostretch-hsv " + R + " img drw)");
    } else if (n == "balance_blancos") {
      lines.push_back("(gimp-drawable-levels-stretch drw)");
    } else if (n == "saturacion") {
      lines.push_back("(gimp-drawable-hue-saturation drw HUE-RANGE-ALL 0 0 " + fix(D(p, "cantidad"), 2) + " 0)");
    } else if (n == "tono") {
      lines.push_back("(gimp-drawable-hue-saturation drw HUE-RANGE-ALL " + fix(D(p, "grados"), 2) + " 0 0 0)");
    } else if (n == "gamma") {
      lines.push_back("(gimp-drawable-levels drw HISTOGRAM-VALUE 0 1 TRUE " + fix(D(p, "valor"), 3) + " 0 1 TRUE)");
    } else if (n == "blanco_negro") {
      lines.push_back("(gimp-drawable-desaturate drw DESATURATE-LUMINANCE)");
    } else if (n == "sepia") {
      lines.push_back("(gimp-drawable-colorize-hsl drw 32 28 0)");
    } else if (n == "enfocar") {
      lines.push_back("(plug-in-unsharp-mask " + R + " img drw " + fix(D(p, "radio"), 2) + " " +
                      fix(D(p, "cantidad"), 3) + " " + i("umbral") + ")");
    } else if (n == "desenfocar") {
      lines.push_back("(plug-in-gauss " + R + " img drw " + fix(D(p, "radio"), 2) + " " + fix(D(p, "radio"), 2) + " 0)");
    } else if (n == "reducir_ruido") {
      lines.push_back("(plug-in-despeckle " + R + " img drw " +
                      std::to_string(std::max(1L, iround(D(p, "fuerza") / 2))) + " 1 7 248)");
    } else if (n == "invertir") {
      lines.push_back("(gimp-drawable-invert drw FALSE)");
    } else if (n == "posterizar") {
      lines.push_back("(gimp-drawable-posterize drw " + i("niveles") + ")");
    } else if (n == "vineta") {
      lines.push_back("(nx-vineta " + fix(D(p, "fuerza"), 1) + ")");
    } else if (n == "texto") {
      lines.push_back("(nx-texto " + scm_str(jstr(p, "texto")) + " " + i("tamano") + " " + scm_col(p["color"]) + " " +
                      scm_str(jstr(p, "posicion")) + " " + fix(D(p, "opacidad"), 1) + " " + scm_str(jstr(p, "fuente")) + ")");
    } else if (n == "superponer") {
      lines.push_back("(let ((l (car (gimp-file-load-layer " + R + " img " + scm_str(posix(P(jstr(p, "ruta")))) +
                      "))))(gimp-image-insert-layer img l 0 -1)(gimp-layer-scale l " + i("w") + " " + i("h") +
                      " FALSE)(gimp-layer-set-offsets l " + i("x") + " " + i("y") + ")(gimp-layer-set-opacity l " +
                      fix(D(p, "opacidad"), 1) + ")(nx-merge))");
    } else if (n == "ppp") {
      std::string v = pyfloat(D(p, "valor"));
      lines.push_back("(gimp-image-set-resolution img " + v + " " + v + ")");
    } else if (n == "aplanar") {
      lines.push_back("(gimp-context-set-background " + scm_col(p["color"]) +
                      ")(set! drw (car (gimp-image-flatten img)))(gimp-context-set-background '(255 255 255))");
    }
  }
  if (fmt == "jpg" || fmt == "bmp" || !alpha) lines.push_back("(set! drw (car (gimp-image-flatten img)))");
  else lines.push_back("(nx-merge)");
  if (fmt == "jpg")
    lines.push_back("(file-jpeg-save " + R + " img drw nx-out nx-out " + fix(quality / 100.0, 2) + " 0 1 1 \"\" 2 1 0 2)");
  else if (fmt == "png")
    lines.push_back("(file-png-save-defaults " + R + " img drw nx-out nx-out)");
  else
    lines.push_back("(gimp-file-save " + R + " img drw nx-out nx-out)");
  lines.push_back("(gimp-image-delete img)(gimp-context-pop)");
  return join(lines, "\n") + "\n";
}

// ------------------------------------------------------------------ Python-Fu (GIMP 3)
std::string python_fu(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                      bool alpha) {
  json job = {{"in", S(src)}, {"out", S(out)}, {"fmt", fmt}, {"quality", quality}, {"alpha", alpha}, {"ops", prim}};
  return "JOB = " + json(job.dump()).dump() + "\n" + PY3;
}

// ------------------------------------------------------------------ sin GIMP
void raster_edit(const json& prim, const fs::path& src, const fs::path& out, const std::string& fmt, int quality,
                 bool alpha) {
  try {
    raster::Info info;
    raster::Image img = raster::load(src, &info);
    static const std::set<std::string> A = {"RGBA", "LA", "P", "PA"};
    img = A.count(info.mode) || info.transparency ? raster::to_rgba(img) : raster::to_rgb(img);
    auto dpi = info.dpi;
    img = raster::apply(std::move(img), prim, dpi);
    if ((fmt == "jpg" || fmt == "bmp" || !alpha) && img.c == 4)
      img = raster::apply(std::move(img), json::array({{{"op", "aplanar"}, {"color", {255, 255, 255}}}}), dpi);
    if (fmt == "xcf") throw EditError("el formato XCF (proyecto de GIMP) necesita GIMP");
    raster::save(img, out, fmt, quality, dpi);
  } catch (const raster::Error& e) {
    throw EditError(e.what());
  }
}

// ------------------------------------------------------------------ motor
std::optional<Found> GimpEngine::info(bool refresh) {
  std::lock_guard<std::mutex> lk(mu_);
  if (refresh || !checked_ || now() - checked_ > 600) {
    info_ = find(config_, refresh);
    checked_ = now();
  }
  return info_;
}

json GimpEngine::status(bool refresh) {
  auto g = info(refresh);
  std::string mode = config_ ? config_->str("media.gimp", "auto") : "auto";
  json ops = json::object();
  for (auto& [k, _] : ops_spec().items()) ops[k] = op_help().value(k, "");
  json rec = json::object();
  for (auto& [k, v] : recipes().items()) rec[k] = v["descripcion"];
  std::set<std::string> fm;
  for (auto& [_, v] : FORMATS) fm.insert(v);
  json out = {{"modo", mode}, {"gimp", (bool)g}, {"motor", g && mode != "pil" ? "gimp" : "pillow"},
              {"operaciones", ops}, {"recetas", rec}, {"formatos", json(std::vector<std::string>(fm.begin(), fm.end()))}};
  if (g) {
    out["version"] = g->version;
    out["origen"] = g->origin;
    out["interprete"] = g->interpreter;
  } else {
    out["nota"] =
        "GIMP no esta disponible en este equipo: las ediciones se hacen con Pillow (mismas operaciones; el formato "
        "XCF y los scripts de GIMP no estan disponibles). Los instaladores de escritorio de Nexus-Office lo traen "
        "incluido; en Android no existe GIMP.";
  }
  return out;
}

std::tuple<int, int, bool> GimpEngine::size_of(const fs::path& src) {
  auto from = [](const raster::Info& in) {
    int w = in.w, h = in.h;
    if (in.orientation >= 5 && in.orientation <= 8) std::swap(w, h);
    bool a = in.mode == "RGBA" || in.mode == "LA" || in.mode == "PA" || in.transparency;
    return std::make_tuple(w, h, a);
  };
  try {
    return from(raster::probe(src));
  } catch (const std::exception&) {
    auto g = info();
    if (!g) throw EditError("no se puede leer la imagen " + S(src.filename()));
    fs::path tmp = paths::sub({"gimp", "trabajo"}) / ("size_" + stamp() + ".png");
    try {
      run_gimp(*g, json::array(), src, tmp, "png", 90, 120);
      auto in = raster::probe(tmp);
      remove_file(tmp);
      return {in.w, in.h, in.mode == "RGBA"};
    } catch (...) {
      remove_file(tmp);
      throw;
    }
  }
}

json GimpEngine::edit(const fs::path& src, json ops_in, const fs::path& out, int quality, const std::string& engine,
                      const std::function<fs::path(const std::string&)>& overlay_resolver, double timeout) {
  json ops = normalize(std::move(ops_in));
  std::string sfx = suffix_lower(out);
  auto fit = FORMATS.find(sfx.empty() ? "" : sfx.substr(1));
  if (fit == FORMATS.end()) throw EditError("formato de salida no admitido: " + S(out.extension()));
  std::string fmt = fit->second;
  quality = std::max(10, std::min(100, quality));
  std::map<std::string, std::pair<fs::path, std::tuple<int, int, bool>>> overlays;
  for (auto& o : ops)
    if (o["op"] == "superponer") {
      std::string ref = o["imagen"];
      fs::path p = overlay_resolver ? overlay_resolver(ref) : P(ref);
      overlays[ref] = {p, size_of(p)};
    }
  auto [W, H, alpha] = size_of(src);
  auto [prim, final] = resolve(ops, W, H, [&](const std::string& k) {
    auto& t = overlays.at(k).second;
    return std::make_pair(std::get<0>(t), std::get<1>(t));
  });
  for (auto& p : prim) {
    if (p["op"] == "rotar") {
      double d = D(p, "grados");
      alpha = alpha || (d != 90 && d != 180 && d != 270);   // un giro libre deja esquinas transparentes
    }
    if (p["op"] == "superponer") p["ruta"] = S(overlays.at(jstr(p, "imagen")).first);
  }
  std::string mode = !engine.empty() ? engine : config_ ? config_->str("media.gimp", "auto") : "auto";
  std::optional<Found> g = mode != "pil" ? info() : std::nullopt;
  if (mode == "gimp" && !g) throw EditError("GIMP no esta disponible en este equipo");
  mkdirs(out.parent_path());
  json opnames = json::array();
  for (auto& o : ops) opnames.push_back(o["op"]);
  std::string note;
  if (g) {
    try {
      double t0 = now();
      run_gimp(*g, prim, src, out, fmt, quality, timeout, alpha);
      return {{"motor", "GIMP " + g->version}, {"tamano", {final.first, final.second}},
              {"segundos", round_to(now() - t0, 1)}, {"operaciones", opnames}};
    } catch (const EditError& e) {
      if (mode == "gimp" || fmt == "xcf") throw;
      note = "GIMP fallo (" + uleft(e.what(), 300) + "); se ha hecho con Pillow";
      L.warning(note);
    }
  }
  raster_edit(prim, src, out, fmt, quality, alpha);
  json res = {{"motor", "Pillow"}, {"tamano", {final.first, final.second}}, {"operaciones", opnames}};
  if (!note.empty()) res["nota"] = note;
  return res;
}

void GimpEngine::run_gimp(const Found& g, const json& prim, const fs::path& src0, const fs::path& out0,
                          const std::string& fmt, int quality, double timeout, bool alpha) {
  // rutas absolutas: el GIMP incluido en Linux se ejecuta desde su propia carpeta
  fs::path src = nx::resolve(src0), out = nx::resolve(out0), work = nx::resolve(paths::sub({"gimp", "trabajo"}));
  std::string st = stamp();
  fs::path tmp_out = out.parent_path() / ("." + st + S(out.extension()));
  bool needs_fonts = false;
  for (auto& p : prim) needs_fonts = needs_fonts || p["op"] == "texto";
  fs::path script;
  std::vector<std::string> cmd = g.cmd;
  if (g.major >= 3) {
    script = work / ("edit_" + st + ".py");
    write_file(script, python_fu(prim, src, tmp_out, fmt, quality, alpha));
    for (std::string a : {"-i", "-d"}) cmd.push_back(a);
    if (!needs_fonts) cmd.push_back("-f");
    for (std::string a : {"--quit", "--batch-interpreter", "python-fu-eval", "-b"}) cmd.push_back(a);
    cmd.push_back("exec(open(" + py_path(script) + ", encoding='utf-8').read())");
  } else {
    script = work / ("edit_" + st + ".scm");
    write_file(script, script_fu(prim, src, tmp_out, fmt, quality, alpha));
    for (std::string a : {"-i", "-d"}) cmd.push_back(a);
    if (!needs_fonts) cmd.push_back("-f");
    for (std::string a : {"--batch-interpreter", "plug-in-script-fu-eval", "-b"}) cmd.push_back(a);
    cmd.push_back("(load " + scm_str(posix(script)) + ")");
    cmd.push_back("-b");
    cmd.push_back("(gimp-quit 0)");
  }
  auto cleanup = [&] {
    remove_file(script);
    remove_file(tmp_out);
  };
  try {
    auto [code, text] = run(cmd, g.major, timeout);
    bool failed = contains(text, "NEXUS-ERROR") || contains(text, "execution error");
    if (failed || !is_file(tmp_out) || fsize(tmp_out) == 0) {
      std::string log = clean_log(text);
      throw EditError(!log.empty() ? log : "GIMP termino con codigo " + std::to_string(code) + " sin generar la imagen");
    }
    std::error_code ec;
    fs::rename(tmp_out, out, ec);
    if (ec) {
      remove_file(out);
      fs::rename(tmp_out, out, ec);
    }
    if (ec) throw EditError("no se pudo guardar " + S(out.filename()));
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
}

json GimpEngine::run_script(const std::string& code, const std::optional<fs::path>& src0,
                            const std::optional<fs::path>& out0, double timeout) {
  auto g = info();
  if (!g) throw EditError("GIMP no esta disponible en este equipo: usa imagen_editar (funciona sin GIMP)");
  fs::path work = nx::resolve(paths::sub({"gimp", "trabajo"}));
  std::optional<fs::path> src, out;
  if (src0) src = nx::resolve(*src0);
  if (out0) out = nx::resolve(*out0);
  std::string st = stamp();
  fs::path script;
  std::vector<std::string> cmd = g->cmd;
  if (g->major >= 3) {
    script = work / ("script_" + st + ".py");
    std::string head =
        "import gi\ngi.require_version('Gimp', '3.0')\ngi.require_version('Gegl', '0.4')\n"
        "from gi.repository import Gimp, Gio, Gegl\n"
        "ENTRADA = " + pyrepr(json(src ? S(*src) : "")) + "\nSALIDA = " + pyrepr(json(out ? S(*out) : "")) + "\n";
    write_file(script, head + code + "\nprint('NEXUS-OK')\n");
    for (std::string a : {"-i", "-d", "--quit", "--batch-interpreter", "python-fu-eval", "-b"}) cmd.push_back(a);
    cmd.push_back("exec(open(" + py_path(script) + ", encoding='utf-8').read())");
  } else {
    script = work / ("script_" + st + ".scm");
    std::string head = "(define ENTRADA " + scm_str(src ? posix(*src) : "") + ")\n(define SALIDA " +
                       scm_str(out ? posix(*out) : "") + ")\n";
    write_file(script, head + code + "\n");
    for (std::string a : {"-i", "-d", "--batch-interpreter", "plug-in-script-fu-eval", "-b"}) cmd.push_back(a);
    cmd.push_back("(load " + scm_str(posix(script)) + ")");
    cmd.push_back("-b");
    cmd.push_back("(gimp-quit 0)");
  }
  std::pair<int, std::string> r;
  try {
    r = run(cmd, g->major, timeout);
  } catch (...) {
    remove_file(script);
    throw;
  }
  remove_file(script);
  bool ok = !contains(r.second, "execution error") && !contains(r.second, "Traceback") && r.first == 0;
  return {{"ok", ok}, {"motor", "GIMP " + g->version}, {"interprete", g->interpreter},
          {"salida", clean_log(r.second)}, {"fichero", out.has_value() && is_file(*out)}};
}

}  // namespace nx::gimp
