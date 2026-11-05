// Herramienta generar_video (secuencia de imagenes -> video): prepara la carpeta outputs/videos/<id>/ con las imagenes
// copiadas y el manifiesto que lee la pagina generadora (recursos/web/video.html). El video lo codifica el navegador;
// esa parte la prueba tests/video_smoke.js en un Chrome real.
#include "check.h"
#include "tools/tools.h"
#include "util/fs.h"
#include "util/text.h"

namespace nx::tools {
void link_builtin_gimp();
}

namespace {

nx::json call(const nx::json& args) {
  nx::tools::link_builtin_gimp();
  nx::tools::ToolContext ctx;
  return nx::tools::builtin_functions().at("generar_video")(args, ctx);
}

std::string error_of(const nx::json& args) {
  try {
    call(args);
  } catch (const nx::tools::ToolError& e) {
    return e.what();
  }
  return "";
}

void put(const std::string& rel, const std::string& bytes) {
  auto p = nx::paths::sandbox_dir() / nx::P(rel);
  nx::mkdirs(p.parent_path());
  nx::write_file(p, bytes);
}

}  // namespace

TEST("generar_video: copia las imagenes y escribe el manifiesto") {
  put("imagenes/a.png", "png-a");
  put("imagenes/b.JPG", "jpg-b");
  nx::json r = call({{"imagenes", {"imagenes/a.png", "imagenes/b.JPG"}},
                     {"segundos", 2},
                     {"transicion", "deslizar"},
                     {"proporcion", "9:16"},
                     {"titulo", "Mi video"},
                     {"textos", {"uno", ""}},
                     {"movimiento", true}});
  CHECK(r["ok"].get<bool>());
  CHECK_EQ(r["imagenes"].get<int>(), 2);
  CHECK_EQ(r["duracion_segundos"].get<double>(), 3.4);   // 2 imagenes de 2 s con 0,6 s de solape
  std::string id = r["id"].get<std::string>();
  CHECK(nx::contains(r["generador"].get<std::string>(), "/static/video.html?m=/outputs/videos/" + id + "/manifest.json"));
  CHECK(nx::contains(r["generador"].get<std::string>(), "&auto=1"));
  auto dir = nx::paths::outputs_dir() / "videos" / nx::P(id);
  CHECK(nx::is_file(dir / "001.png"));
  CHECK(nx::is_file(dir / "002.jpg"));   // la extension se guarda en minusculas
  CHECK_EQ(nx::read_text(dir / "001.png"), std::string("png-a"));   // copia: el original no se toca
  nx::json m = nx::jparse(nx::read_text(dir / "manifest.json"));
  CHECK_EQ(m["imagenes"].size(), (size_t)2);
  CHECK_EQ(m["imagenes"][0]["texto"].get<std::string>(), std::string("uno"));
  CHECK(!m["imagenes"][1].contains("texto"));   // texto vacio = sin pie
  CHECK_EQ(m["imagenes"][0]["url"].get<std::string>(), "/outputs/videos/" + id + "/001.png");
  CHECK_EQ(m["proporcion"].get<std::string>(), std::string("9:16"));
  CHECK_EQ(m["transicion"].get<std::string>(), std::string("deslizar"));
  CHECK(m["kenburns"].get<bool>());
  CHECK_EQ(m["titulo"].get<std::string>(), std::string("Mi video"));
}

TEST("generar_video: valores por defecto y limites") {
  put("imagenes/c.png", "c");
  nx::json r = call({{"imagenes", "imagenes/c.png"}, {"segundos", 999}, {"fps", 1}});   // una sola imagen, fuera de rango
  std::string id = r["id"].get<std::string>();
  nx::json m = nx::jparse(nx::read_text(nx::paths::outputs_dir() / "videos" / nx::P(id) / "manifest.json"));
  CHECK_EQ(m["segundos"].get<double>(), 30.0);   // maximo 30 s por imagen
  CHECK_EQ(m["fps"].get<int>(), 10);             // minimo 10 fotogramas/s
  CHECK_EQ(m["transicion"].get<std::string>(), std::string("fundido"));
  CHECK_EQ(m["proporcion"].get<std::string>(), std::string("16:9"));
  CHECK_EQ(m["ajuste"].get<std::string>(), std::string("cubrir"));
  CHECK(!m["kenburns"].get<bool>());
  CHECK_EQ(r["duracion_segundos"].get<double>(), 30.0);   // una imagen: sin solape
}

TEST("generar_video: con audio") {
  put("imagenes/d.png", "d");
  put("music/pista.wav", "RIFF");   // en la carpeta de trabajo
  nx::json r = call({{"imagenes", {"imagenes/d.png"}}, {"audio", "music/pista.wav"}});
  std::string id = r["id"].get<std::string>();
  nx::json m = nx::jparse(nx::read_text(nx::paths::outputs_dir() / "videos" / nx::P(id) / "manifest.json"));
  CHECK_EQ(m["audio"].get<std::string>(), "/outputs/videos/" + id + "/audio.wav");
  CHECK(nx::is_file(nx::paths::outputs_dir() / "videos" / nx::P(id) / "audio.wav"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/d.png"}}, {"audio", "music/pista.midi"}}), "no existe el audio"));
  put("music/pista.mid", "MThd");
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/d.png"}}, {"audio", "music/pista.mid"}}), "wav, mp3"));
}

TEST("generar_video: errores claros") {
  put("imagenes/e.png", "e");
  put("imagenes/f.tif", "tif");
  CHECK(nx::contains(error_of(nlohmann::json::object()), "indica «imagenes»"));
  CHECK(nx::contains(error_of({{"imagenes", nlohmann::json::array()}}), "indica «imagenes»"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/e.png"}}, {"transicion", "giro"}}), "transicion desconocida"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/e.png"}}, {"proporcion", "21:9"}}), "proporcion desconocida"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/e.png"}}, {"ajuste", "estirar"}}), "ajuste desconocido"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/no-existe.png"}}}), "no existe la imagen"));
  CHECK(nx::contains(error_of({{"imagenes", {"imagenes/f.tif"}}}), "imagen_editar"));   // no se puede mostrar en un video
  CHECK(nx::contains(error_of({{"imagenes", {"../fuera.png"}}}), "carpeta de trabajo"));   // no sale de la carpeta de trabajo
  nx::json many = nx::json::array();
  for (int i = 0; i < 201; ++i) many.push_back("imagenes/e.png");
  CHECK(nx::contains(error_of({{"imagenes", many}}), "maximo 200"));
}
