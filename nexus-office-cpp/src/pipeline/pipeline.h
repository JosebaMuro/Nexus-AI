// Pipelines desacoplados del dossier de arquitectura: percepcion -> orquestador LLM -> difusion -> pantalla.
//
//   frames (camara, API)  --MpmcRing-->  [hilo percepcion]  red de vision en GPU/NPU (llama-server con mmproj) o
//                                                           huella visual en la CPU -> embeddings -> base vectorial
//   orquestador LLM (agentes)  lee la base vectorial (herramienta «percepcion») y, ante una instruccion multimodal
//                              (generate_image), envia un DescriptorTrabajo
//   DescriptorTrabajo  --MpmcRing-->  [hilo difusion]  sd.cpp en la NPU/GPU, paso a paso (vista previa por paso)
//   FramePantalla      --TripleBuffer-->  [hilo presentador]  buffer de pantalla + aviso por WebSocket
//
// Ningun pipeline corre en el hilo de quien lo llama (servidor HTTP, agente): solo se encola. Los datos (frames e
// imagenes) van en bloques del asignador zero-copy de hal/ y entre hilos solo viajan referencias.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hal/ring.h"
#include "hal/zerocopy.h"
#include "memory/memory.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx {
class DB;
}

namespace nx::pipeline {

struct Frame {
  hal::Buffer datos;   // imagen codificada (JPEG, PNG, WebP...) tal como llego
  std::string fuente;
  double ts = 0;
};

// encargo del orquestador al modulo de difusion
struct DescriptorTrabajo {
  uint64_t id = 0;
  std::string prompt, negativo, idea, origen;
  int tam = 0, pasos = 0;   // 0 = lo que diga el perfil de hardware
  std::optional<long long> semilla;
  double creado = 0;
};

// frame del buffer de pantalla
struct FramePantalla {
  hal::Buffer imagen;   // PNG
  uint64_t trabajo = 0, version = 0;
  int paso = 0;
  bool final = false;
};

// vista previa de un paso de la difusion: fichero con la imagen y numero de paso
using VistaPrevia = std::function<void(const hal::Buffer& png, int paso)>;   // PNG en un bloque del asignador

struct Dependencias {
  DB* db = nullptr;
  std::function<mem::EmbedderPtr()> embedder;
  std::function<bool()> vision_disponible;              // hay un modelo con vision (mmproj) en el acelerador
  std::function<json(const std::string&)> analizar;     // descripcion de la imagen con la red de vision
  std::function<json(const DescriptorTrabajo&, const VistaPrevia&)> generar;   // devuelve {"path", "url"...}
  std::function<void(const json&)> emitir;              // aviso a las interfaces (WebSocket)
};

struct Opciones {
  double vision_cada_s = 10;      // como mucho una descripcion con la red de vision cada tantos segundos
  double umbral_repetido = 0.985; // frames casi iguales al anterior se descartan
  size_t cola_frames = 32, cola_trabajos = 64;
  bool guardar_frames = true;
};

// huella visual compacta de un frame RGB (rejilla 8x8 de color, centrada y normalizada): 192 valores
mem::Vec huella(const uint8_t* rgb, int w, int h);

class Pipeline {
 public:
  Pipeline(Dependencias d, Opciones o = {});
  ~Pipeline();
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  void start();
  void stop();

  // ---- percepcion
  // copia unica de los bytes a un bloque zero-copy y a la cola; false si la cola esta llena (el frame se descarta)
  bool enviar_frame(std::string_view bytes, const std::string& fuente = "api");
  bool enviar_frame(hal::Buffer datos, const std::string& fuente = "api");
  json buscar(const std::string& q, int k = 5);   // base vectorial de lo percibido
  json recientes(int n = 20);
  // espera a que la percepcion vacie su cola (pruebas y ordenes de linea)
  bool esperar_percepcion(double timeout_s = 30);

  // ---- difusion
  uint64_t difundir(DescriptorTrabajo d);         // encola y devuelve el id (lanza si la cola esta llena)
  std::shared_future<json> resultado(uint64_t id);
  json trabajo(uint64_t id);
  json trabajos(int n = 20);

  // ---- pantalla
  FramePantalla pantalla();   // ultimo frame presentado (comparte el bloque, sin copiarlo)

  json estado();

 private:
  struct Trabajo {
    DescriptorTrabajo d;
    std::promise<json> promesa;
    std::shared_future<json> futuro;
    std::string estado = "en cola";
    int paso = 0;
    json resultado;
    std::string error;
    double inicio = 0, fin = 0;
  };

  void bucle_percepcion();
  void bucle_difusion();
  void bucle_presentador();
  void percibir(Frame& f);
  void publicar(uint64_t trabajo, int paso, bool final, const hal::Buffer& png);
  void emitir(const json& e);
  json fila(const json& r, double score = -1);

  Dependencias dep_;
  Opciones opt_;
  std::atomic<bool> quit_{false}, started_{false};

  hal::MpmcRing<Frame> frames_;
  hal::Senal senal_frames_;
  std::atomic<long long> recibidos_{0}, descartados_{0}, repetidos_{0}, guardados_{0}, en_proceso_{0};
  double ultima_vision_ = 0;
  mem::Vec ultima_huella_;
  double ultimo_guardado_ = 0;
  hal::Senal senal_vacia_;

  hal::MpmcRing<uint64_t> cola_;
  hal::Senal senal_cola_;
  std::mutex mu_trabajos_;
  std::map<uint64_t, std::shared_ptr<Trabajo>> trabajos_;
  std::atomic<uint64_t> siguiente_{1};

  hal::TripleBuffer<FramePantalla> pantalla_buf_;
  hal::Senal senal_pantalla_;
  std::atomic<uint64_t> version_{0};
  std::mutex mu_pantalla_;   // solo para las lecturas HTTP del ultimo frame (copiar una referencia)
  FramePantalla actual_;

  std::thread t_percepcion_, t_difusion_, t_presentador_;
};

}  // namespace nx::pipeline
