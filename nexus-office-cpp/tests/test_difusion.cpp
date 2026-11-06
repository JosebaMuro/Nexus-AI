// Motor de difusion sobre ONNX Runtime: tokenizador, planificador, fp16 y una imagen entera con redes diminutas.
#include <atomic>
#include <cmath>

#include "check.h"
#include "hal/cpu.h"
#include "media/difusion_onnx.h"
#include "onnx_min.h"
#include "util/fs.h"
#include "util/json.h"

using namespace nx;
namespace om = onnxmin;

namespace {

fs::path carpeta_prueba(const std::string& n) {
  fs::path d = fs::temp_directory_path() / ("nexus-difusion-" + n);
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

void escribir(const fs::path& f, const std::string& s) {
  fs::create_directories(f.parent_path());
  write_file(f, s);
}

void tokenizador(const fs::path& d) {
  escribir(d / "tokenizer" / "vocab.json",
           R"({"!":0,"c":1,"a":2,"t":3,"t</w>":4,"ca":5,"cat</w>":6,"<|startoftext|>":7,"<|endoftext|>":8,"d":9,"o":10,"g</w>":11})");
  escribir(d / "tokenizer" / "merges.txt", "#version: 0.2\nc a\nca t</w>\n");
  escribir(d / "tokenizer" / "special_tokens_map.json", R"({"pad_token":"!"})");
}

// tres redes con las entradas y salidas de Stable Diffusion: D=8 en el texto, UNet = 0,1*sample, VAE = Conv 1x1 + x8
void modelo_minimo(const fs::path& d) {
  tokenizador(d);
  escribir(d / "scheduler" / "scheduler_config.json",
           R"({"_class_name":"EulerDiscreteScheduler","timestep_spacing":"trailing","prediction_type":"epsilon"})");
  {
    om::Grafo g;
    g.entradas.push_back(om::valor_info("input_ids", om::INT32, {{1}, {77}}));
    g.salidas.push_back(om::valor_info("last_hidden_state", om::FLOAT, {{1}, {77}, {8}}));
    g.iniciales.push_back(om::inicial_i64("ejes", {1}, {2}));
    g.iniciales.push_back(om::inicial_f("peso", {1, 1, 8}, std::vector<float>(8, 0.01f)));
    g.nodos.push_back(om::nodo("Cast", {"input_ids"}, {"f"}, {{"to", om::FLOAT}}));
    g.nodos.push_back(om::nodo("Unsqueeze", {"f", "ejes"}, {"u"}));
    g.nodos.push_back(om::nodo("Mul", {"u", "peso"}, {"last_hidden_state"}));
    escribir(d / "text_encoder" / "model.onnx", g.modelo());
  }
  {
    om::Grafo g;
    g.entradas.push_back(om::valor_info("sample", om::FLOAT, {{-1, "b"}, {4}, {-1, "h"}, {-1, "w"}}));
    g.entradas.push_back(om::valor_info("timestep", om::FLOAT, {{1}}));
    g.entradas.push_back(om::valor_info("encoder_hidden_states", om::FLOAT, {{-1, "b"}, {77}, {8}}));
    g.salidas.push_back(om::valor_info("out_sample", om::FLOAT, {{-1, "b"}, {4}, {-1, "h"}, {-1, "w"}}));
    g.iniciales.push_back(om::inicial_f("k", {}, {0.1f}));
    g.nodos.push_back(om::nodo("Mul", {"sample", "k"}, {"out_sample"}));
    escribir(d / "unet" / "model.onnx", g.modelo());
  }
  {
    om::Grafo g;
    g.entradas.push_back(om::valor_info("latent_sample", om::FLOAT, {{1}, {4}, {-1, "h"}, {-1, "w"}}));
    g.salidas.push_back(om::valor_info("sample", om::FLOAT, {{1}, {3}, {-1, "H"}, {-1, "W"}}));
    g.iniciales.push_back(om::inicial_f("W", {3, 4, 1, 1}, {0.2f, 0, 0, 0, 0, 0.2f, 0, 0, 0, 0, 0.2f, 0}));
    g.iniciales.push_back(om::inicial_f("escalas", {4}, {1, 1, 8, 8}));
    g.nodos.push_back(om::nodo("Conv", {"latent_sample", "W"}, {"c"}));
    g.nodos.push_back(om::nodo("Resize", {"c", "", "escalas"}, {"sample"}, {{"mode", 0, "nearest", true}}));
    escribir(d / "vae_decoder" / "model.onnx", g.modelo());
  }
}

}  // namespace

TEST("difusion: fp16 de ida y vuelta") {
  for (float f : {0.f, 1.f, -2.5f, 0.333f, 65504.f, 1e-5f}) {
    float r = difusion::de_fp16(difusion::a_fp16(f));
    CHECK(std::fabs(r - f) <= std::fabs(f) * 1e-3f + 1e-6f);
  }
  CHECK_EQ(difusion::a_fp16(1.f), (uint16_t)0x3c00);
  CHECK(std::isinf(difusion::de_fp16(difusion::a_fp16(1e6f))));
}

TEST("difusion: tokenizador CLIP con BPE y relleno") {
  fs::path d = carpeta_prueba("tok");
  tokenizador(d);
  difusion::TokenizadorClip t;
  CHECK(t.cargar(d / "tokenizer"));
  CHECK_EQ(t.inicio, 7);
  CHECK_EQ(t.fin, 8);
  CHECK_EQ(t.relleno, 0);   // «!» como en SD 2.x
  auto ids = t.codificar("  Cat  ");
  CHECK_EQ(ids.size(), (size_t)77);
  CHECK_EQ(ids[0], 7);
  CHECK_EQ(ids[1], 6);   // c a t</w> -> ca t</w> -> cat</w>
  CHECK_EQ(ids[2], 8);
  CHECK_EQ(ids[3], 0);
  auto largo = t.codificar(std::string(200, 'a'), 77);   // se corta a 75 + inicio y fin
  CHECK_EQ(largo.size(), (size_t)77);
  CHECK_EQ(largo[76], 8);
}

TEST("difusion: planificador Euler (leading, trailing y v-prediction)") {
  difusion::Planificador p;
  p.espaciado = "trailing";
  p.preparar(1);
  CHECK_EQ(p.timesteps.size(), (size_t)1);
  CHECK_EQ(p.timesteps[0], 999.0);
  CHECK(std::fabs(p.sigmas[0] - 14.6146) < 0.01);   // sigma maxima de SD
  CHECK_EQ(p.sigmas[1], 0.0);
  CHECK(std::fabs(p.sigma_inicial - p.sigmas[0]) < 1e-9);
  difusion::Planificador q;   // leading (SD 1.5): 20 pasos, de 951 a 1
  q.preparar(20);
  CHECK_EQ(q.timesteps.front(), 951.0);
  CHECK_EQ(q.timesteps.back(), 1.0);
  CHECK(q.sigma_inicial > q.sigmas[0]);
  for (size_t i = 0; i + 1 < q.sigmas.size(); ++i) CHECK(q.sigmas[i] > q.sigmas[i + 1]);
}

TEST("difusion: proveedores por chip segun la matriz del dossier") {
  auto v = difusion::proveedores_para({{"accelerator", "cuda"}, {"id", "cuda-nvidia-dgpu"}});
  CHECK(v.front() == "tensorrt" || v.front() == "cuda");
  CHECK_EQ(v.back(), std::string("cpu"));
  auto q = difusion::proveedores_para({{"accelerator", "cpu"}, {"npu", {{"vendor", "Qualcomm"}, {"soc", ""}}}});
  CHECK(std::find(q.begin(), q.end(), "qnn") != q.end());
  CHECK_EQ(difusion::variante_de("tensorrt"), std::string("cuda"));
  CHECK_EQ(difusion::variante_de("openvino-npu"), std::string("openvino"));
}

TEST("difusion: imagen entera con ONNX Runtime (redes diminutas, vista previa por paso)") {
  auto lib = difusion::biblioteca_ort();
  if (!lib) {
    std::cerr << "    (sin ONNX Runtime: define NEXUS_ORT_LIB para probar el motor)\n";
    return;
  }
  fs::path d = carpeta_prueba("modelo");
  modelo_minimo(d);
  CHECK(difusion::es_modelo(d));
  json info = difusion::info_ort(*lib);
  CHECK(!jstr(info, "version").empty());
  difusion::Opciones o;
  o.lib = *lib;
  o.modelo = d;
  o.cache = d / "cache";
  o.proveedor = "cpu";
  o.memoria = hal::Memoria::Compartida;
  auto m = difusion::Motor::abrir(o);
  CHECK_EQ(m->proveedor(), std::string("cpu"));
  int vistas = 0;
  difusion::Peticion p;
  p.prompt = "cat";
  p.ancho = p.alto = 64;
  p.pasos = 3;
  p.semilla = 42;
  p.vista = [&](const hal::Buffer& png, int paso, int total) {
    ++vistas;
    CHECK(png.size() > 8);
    CHECK_EQ(total, 3);
    CHECK(paso >= 1 && paso < 3);
  };
  auto r = m->generar(p);
  CHECK_EQ(r.ancho, 64);
  CHECK_EQ(r.alto, 64);
  CHECK_EQ(r.pasos, 3);
  CHECK_EQ(vistas, 2);
  CHECK(r.png.size() > 100);
  CHECK_EQ(std::string((const char*)r.png.data() + 1, 3), std::string("PNG"));
  // misma semilla -> misma imagen; con guia (lote de 2) tambien funciona
  auto r2 = m->generar(p);
  CHECK(r.rgb == r2.rgb);
  p.guia = 4.0;
  p.vista = nullptr;
  auto r3 = m->generar(p);
  CHECK_EQ(r3.ancho, 64);
  CHECK(r3.tiempos.contains("por_paso"));
}

TEST("vision: codificador CLIP en ONNX Runtime (imagen y texto al mismo espacio)") {
  auto lib = difusion::biblioteca_ort();
  if (!lib) return;
  fs::path d = carpeta_prueba("vision");
  // tokenizador CLIP en la raiz (como los repos ONNX de CLIP) y redes diminutas: la imagen -> media de cada canal,
  // el texto -> los 3 primeros ids
  escribir(d / "vocab.json", R"({"!":0,"c":1,"a":2,"t":3,"t</w>":4,"ca":5,"cat</w>":6,"<|startoftext|>":7,"<|endoftext|>":8})");
  escribir(d / "merges.txt", "#version: 0.2\nc a\nca t</w>\n");
  escribir(d / "preprocessor_config.json", R"({"crop_size":8,"image_mean":[0,0,0],"image_std":[1,1,1]})");
  {
    om::Grafo g;
    g.entradas.push_back(om::valor_info("pixel_values", om::FLOAT, {{1}, {3}, {8}, {8}}));
    g.salidas.push_back(om::valor_info("image_embeds", om::FLOAT, {{1}, {3}}));
    g.iniciales.push_back(om::inicial_i64("ejes", {2}, {2, 3}));
    g.nodos.push_back(om::nodo("ReduceMean", {"pixel_values", "ejes"}, {"image_embeds"}, {{"keepdims", 0}}));
    escribir(d / "onnx" / "vision_model.onnx", g.modelo(18));
  }
  {
    om::Grafo g;
    g.entradas.push_back(om::valor_info("input_ids", om::INT64, {{1}, {77}}));
    g.salidas.push_back(om::valor_info("text_embeds", om::FLOAT, {{1}, {3}}));
    g.iniciales.push_back(om::inicial_i64("ini", {1}, {0}));
    g.iniciales.push_back(om::inicial_i64("fin", {1}, {3}));
    g.iniciales.push_back(om::inicial_i64("eje", {1}, {1}));
    g.nodos.push_back(om::nodo("Slice", {"input_ids", "ini", "fin", "eje"}, {"s"}));
    g.nodos.push_back(om::nodo("Cast", {"s"}, {"text_embeds"}, {{"to", om::FLOAT}}));
    escribir(d / "onnx" / "text_model.onnx", g.modelo(18));
  }
  CHECK(difusion::es_modelo_vision(d));
  auto c = difusion::CodificadorVision::abrir(*lib, d, "cpu");
  CHECK(c->tiene_texto());
  std::vector<uint8_t> rojo(16 * 16 * 3, 0);
  for (size_t i = 0; i < rojo.size(); i += 3) rojo[i] = 255;
  auto v = c->imagen(rojo.data(), 16, 16);
  CHECK_EQ(v.size(), (size_t)3);
  CHECK(std::fabs(v[0] - 1.f) < 1e-4f && std::fabs(v[1]) < 1e-4f);   // normalizado
  auto t = c->texto("Cat");
  CHECK_EQ(t.size(), (size_t)3);
  float n = std::sqrt(7.f * 7 + 6 * 6 + 8 * 8);
  CHECK(std::fabs(t[0] - 7 / n) < 1e-4f && std::fabs(t[1] - 6 / n) < 1e-4f);
  CHECK(!c->id().empty());
}
