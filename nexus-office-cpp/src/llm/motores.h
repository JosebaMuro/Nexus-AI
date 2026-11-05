// Motores de inferencia aparte de llama.cpp (llm/motores): los que mejor aprovechan las NPU y GPU de cada fabricante.
//
// * LiteRT-LM (Google): modelos .litertlm. CPU, GPU (OpenCL/WebGPU/Metal) y NPU con el «dispatch» del fabricante:
//   Google Tensor (Pixel), Qualcomm Snapdragon (8 Elite...), MediaTek Dimensity e Intel (OpenVINO, Windows). En
//   Android es el que mejor va en los Pixel y en los moviles con NPU que llama.cpp no sabe usar.
// * ONNX Runtime GenAI (Microsoft): carpetas con genai_config.json. DirectML (cualquier GPU en Windows), QNN (NPU de
//   los Snapdragon X Elite/Plus en Windows ARM), OpenVINO (NPU Intel), VitisAI (NPU AMD Ryzen AI), CUDA y CPU.
// * Qualcomm Genie (QAIRT, 2.8): carpetas con genie_config.json (modelos de Qualcomm AI Hub compilados para la NPU
//   Hexagon). Android, Windows ARM y Linux ARM con Snapdragon.
//
// Ninguno se enlaza al compilar: su biblioteca (liblitert-lm.so, onnxruntime-genai.dll...) va en
// bundled/runtimes/<motor>/<variante>/ y se carga al ejecutar. Cada motor corre en un proceso aparte
// («nexus-office motor ...», el mismo ejecutable) con una API compatible con OpenAI, como llama-server: si el driver
// de la NPU o de la GPU falla, cae ese proceso y no Nexus-Office, y se reintenta en la siguiente (NPU -> GPU -> CPU).
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llm/llm.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx::motores {

struct Info {
  std::string id;        // litert | onnx | genie
  std::string nombre;    // para mostrar
  std::string formato;   // formato de modelo del catalogo: litertlm | onnx-genai
  std::string lib;       // biblioteca (sin prefijo ni extension)
};
const std::vector<Info>& lista();
const Info* info(const std::string& id);
std::string motor_de_formato(const std::string& formato);   // "" si ninguno

// carpeta con la biblioteca del motor: NEXUS_RUNTIME_<MOTOR> (carpeta o fichero), data/runtimes/<motor>/<variante>,
// bundled/runtimes/<motor>/<variante> o, en Android, la carpeta de bibliotecas de la app. nullopt si no hay.
// Con variante, primero la de esa carpeta (onnx/cuda, onnx/qnn...): cada proveedor de ONNX Runtime va en la suya.
std::optional<fs::path> lib_path(const std::string& motor, const std::string& variante = "");
std::vector<std::string> variantes(const std::string& motor);   // carpetas incluidas con la biblioteca del motor
json estado();   // {litert: {lib, variante, variantes}, onnx: {...}, genie: {...}} para Sistema > Runtimes

// aceleradores que se pueden pedir a mano para un motor (selector de inicio), con la variante que los trae
struct Acelerador {
  std::string id, nombre, variante;
};
std::vector<Acelerador> aceleradores(const std::string& motor);
// variante (carpeta) con la que se arranca un acelerador; "" = la primera que haya
std::string variante_para(const std::string& motor, const std::string& acelerador, const json& entry);

// aceleradores que se prueban, en orden, para un motor y un modelo con este perfil (p. ej. {"npu", "gpu", "cpu"})
std::vector<std::string> orden(const std::string& motor, const json& profile, const json& entry);

// ---------------------------------------------------------------- dentro del proceso del motor
struct Config {
  fs::path lib, modelo, cache;
  std::string acelerador = "cpu";   // cpu | gpu | npu (LiteRT) o el proveedor de ONNX Runtime (dml, qnn...)
  fs::path dispatch;                // LiteRT: carpeta de las bibliotecas «dispatch» de la NPU
  int hilos = 0, n_ctx = 4096;
};

struct Params {
  int max_tokens = 1024, top_k = 40;
  double temperature = 0.7, top_p = 0.95;
  long long seed = -1;
};

class Motor {
 public:
  virtual ~Motor() = default;
  virtual std::string acelerador() const = 0;
  // genera la respuesta a la conversacion; on_piece devuelve false para cortar
  virtual void generar(const json& messages, const Params& p, const std::function<bool(const std::string&)>& on_piece) = 0;
};

std::unique_ptr<Motor> abrir_litert(const Config& c);   // std::runtime_error si no se puede
std::unique_ptr<Motor> abrir_onnx(const Config& c);
std::unique_ptr<Motor> abrir_genie(const Config& c);

// «nexus-office motor <litert|onnx|genie> --modelo X --puerto N [--acelerador gpu] [--lib DIR] [--hilos N] [--ctx N]»:
// sirve /health, /v1/models y /v1/chat/completions (con y sin stream)
int main_motor(const std::vector<std::string>& argv);

// mensajes de Nexus-Office (role/content) a texto plano por turno, sin imagenes
json mensajes_simples(const json& messages);

}  // namespace nx::motores

namespace nx::llm {

// backend que arranca «nexus-office motor» y le habla como a un servidor OpenAI
class MotorBackend : public OpenAICompatBackend {
 public:
  using Resolver = LlamaServerBackend::Resolver;
  MotorBackend(std::string motor, Resolver resolve, std::function<json()> profile);
  ~MotorBackend() override;
  std::string name() const override { return motor_; }
  bool available() override;
  std::vector<std::string> models() override;
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;
  void stop();
  json effective();
  int port = 0;

 private:
  void ensure(const std::string& model, const json& opts);
  std::string motor_;
  Resolver resolve_;
  std::function<json()> profile_;
  std::unique_ptr<proc::Process> proc_;
  std::string key_;
  json effective_ = json::object();
  std::vector<std::string> fallidos_;   // aceleradores que no arrancaron con este modelo
  std::recursive_mutex mu_;
};

}  // namespace nx::llm
