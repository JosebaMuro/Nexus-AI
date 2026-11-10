// Motor de difusion propio sobre ONNX Runtime (3.2): sustituye a stable-diffusion.cpp cuando el modelo de imagen es
// una carpeta ONNX (diffusers/optimum: text_encoder, unet, vae_decoder, tokenizer y scheduler).
//
// Corre DENTRO del proceso: ONNX Runtime se carga al ejecutar (dlopen de la misma biblioteca que trae ONNX Runtime
// GenAI) y cada red va en el proveedor del chip (matriz del dossier):
//   Snapdragon -> QNN (NPU Hexagon, HTP en fp16) | Tensor/Mali/Adreno sin QNN -> NNAPI o XNNPACK
//   NVIDIA -> TensorRT o CUDA | AMD -> ROCm o DirectML | Intel -> OpenVINO (NPU/GPU) | Windows -> DirectML
//   Apple -> CoreML | CPU -> XNNPACK o el de la CPU de ONNX Runtime
// Pipeline: tokenizador CLIP (BPE) -> codificador de texto -> bucle UNet con el planificador Euler -> VAE. Cada paso
// produce una vista previa (proyeccion lineal del latente a RGB, sin pasar por el VAE) que va directa al bufer de
// pantalla. Los tensores viven en bloques del asignador zero-copy (memoria del acelerador: CUDA pinned, dma-buf...)
// creados una vez por imagen: el bucle no reserva memoria y la salida de la UNet se escribe en un tensor preasignado.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hal/zerocopy.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx::difusion {

// carpeta de modelo ONNX de difusion (tiene unet/model.onnx y vae_decoder/model.onnx)
bool es_modelo(const fs::path& carpeta);

// biblioteca onnxruntime (onnxruntime.dll / libonnxruntime.so / .dylib): NEXUS_ORT_LIB, la carpeta de ONNX Runtime
// GenAI de esa variante (runtimes/onnx/<variante>) o, en Android, la de las bibliotecas de la app
std::optional<fs::path> biblioteca_ort(const std::string& variante = "");

// proveedores que se prueban, en orden, para este perfil de hardware (el ultimo siempre es «cpu»)
std::vector<std::string> proveedores_para(const json& perfil);
// esa biblioteca de ONNX Runtime trae compilado el proveedor (cpu siempre; nnapi y dml se miran por su funcion)
bool proveedor_disponible(const fs::path& lib, const std::string& proveedor);
// carpeta de ONNX Runtime que trae ese proveedor (cuda, dml, qnn, openvino...; "" = cualquiera)
std::string variante_de(const std::string& proveedor);

// ---------------------------------------------------------------- tokenizador CLIP (BPE de bytes)
class TokenizadorClip {
 public:
  bool cargar(const fs::path& carpeta);   // vocab.json, merges.txt y special_tokens_map.json
  // [inicio] + tokens (como mucho largo-2) + [fin] + relleno hasta largo
  std::vector<int32_t> codificar(const std::string& texto, int largo = 77) const;
  int inicio = 49406, fin = 49407, relleno = 49407;
  size_t vocabulario() const { return vocab_.size(); }

 private:
  std::vector<std::string> bpe(const std::string& palabra) const;
  std::map<std::string, int> vocab_;
  std::map<std::string, int> rangos_;   // "a b" -> orden de la fusion
  mutable std::map<std::string, std::vector<std::string>> cache_;
};

// ---------------------------------------------------------------- planificador Euler (EulerDiscreteScheduler)
struct Planificador {
  double beta_inicio = 0.00085, beta_fin = 0.012;
  std::string betas = "scaled_linear", espaciado = "leading", prediccion = "epsilon";
  int pasos_entrenamiento = 1000, desplazamiento = 1;
  std::vector<double> timesteps, sigmas;   // sigmas tiene un elemento mas (el 0 final)
  double sigma_inicial = 1;

  static Planificador leer(const fs::path& scheduler_config);   // valores por defecto si no esta
  void preparar(int pasos);
};

// ---------------------------------------------------------------- motor
struct Opciones {
  fs::path lib;                    // onnxruntime
  fs::path modelo;                 // carpeta del modelo
  fs::path cache;                  // contextos compilados (QNN) y motores de TensorRT
  std::string proveedor = "cpu";   // cpu | xnnpack | cuda | tensorrt | rocm | dml | qnn | openvino[-npu|-gpu] | nnapi | coreml | webgpu
  int hilos = 0;                   // 0 = los nucleos grandes
  hal::Memoria memoria = hal::Memoria::Host;
};

struct Peticion {
  std::string prompt, negativo;
  int ancho = 512, alto = 512, pasos = 4;
  double guia = 0;                 // <= 1: sin guia libre de clasificador (modelos turbo)
  uint64_t semilla = 0;
  // vista previa de cada paso (PNG en un bloque del asignador) y paso actual (1..total)
  std::function<void(const hal::Buffer& png, int paso, int total)> vista;
  const std::atomic<bool>* cancelar = nullptr;
};

struct Resultado {
  hal::Buffer png;
  std::vector<uint8_t> rgb;
  int ancho = 0, alto = 0, pasos = 0;
  std::string proveedor;
  json tiempos = json::object();   // ms: texto, unet (total y por paso), vae
};

class Motor {
 public:
  virtual ~Motor() = default;
  // carga las tres redes en el proveedor pedido; std::runtime_error si no se puede
  static std::unique_ptr<Motor> abrir(const Opciones& o);
  virtual Resultado generar(const Peticion& p) = 0;
  virtual std::string proveedor() const = 0;
  virtual json estado() const = 0;
};

// ---------------------------------------------------------------- codificador de vision (percepcion)
// CLIP o SigLIP exportados a ONNX (carpeta con onnx/vision_model*.onnx y, en CLIP, onnx/text_model*.onnx con
// vocab.json y merges.txt): la imagen a un vector de la base vectorial local y, con CLIP, tambien el texto de la
// busqueda al mismo espacio (buscar «una persona con casco» entre los frames sin pasar por una descripcion).
bool es_modelo_vision(const fs::path& carpeta);

class CodificadorVision {
 public:
  virtual ~CodificadorVision() = default;
  // std::runtime_error si no se puede cargar
  static std::unique_ptr<CodificadorVision> abrir(const fs::path& lib, const fs::path& carpeta, const std::string& proveedor,
                                                  int hilos = 0);
  // RGB (w x h) -> vector normalizado (norma 1)
  virtual std::vector<float> imagen(const uint8_t* rgb, int w, int h) = 0;
  // texto -> vector en el mismo espacio; vacio si el modelo no tiene codificador de texto que se pueda usar aqui
  virtual std::vector<float> texto(const std::string& t) = 0;
  virtual bool tiene_texto() const = 0;
  virtual std::string id() const = 0;   // para no mezclar vectores de modelos distintos
  virtual std::string proveedor() const = 0;
};

// version de ONNX Runtime de una biblioteca ("" si no carga) y proveedores que trae compilados
json info_ort(const fs::path& lib);

// utilidades (publicas para las pruebas)
uint16_t a_fp16(float f);
float de_fp16(uint16_t h);
// latente [4, h, w] -> RGB aproximado (h x w), sin VAE: la vista previa de cada paso
std::vector<uint8_t> latente_a_rgb(const float* lat, int h, int w);

}  // namespace nx::difusion
