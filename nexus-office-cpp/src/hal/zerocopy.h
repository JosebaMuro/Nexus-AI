// Capa de abstraccion de hardware: asignador zero-copy.
//
// Los datos grandes (frames de la camara, imagenes de la difusion, tensores de entrada) se escriben una sola vez en un
// bloque de este asignador y despues solo viajan referencias: el ring buffer, el motor de percepcion, el buffer de
// pantalla y la respuesta HTTP comparten el mismo bloque sin copiarlo.
//
// - Los bloques estan alineados a pagina (4096 bytes). Es lo que piden los aceleradores para usar memoria de la CPU
//   sin copiarla: Vulkan (VK_EXT_external_memory_host), OpenCL (CL_MEM_USE_HOST_PTR, Adreno y Mali), CUDA
//   (cudaHostRegister) y el DMA de las NPU (Hexagon, rpcmem).
// - Memoria::Compartida respalda el bloque con memoria compartida del sistema (memfd en Linux y Android,
//   CreateFileMapping en Windows). El descriptor se puede pasar a un proceso hijo (llama-server, sd.cpp, motores en
//   proceso aparte) para que lo mapee en vez de recibir los datos copiados por una tuberia.
// - Memoria del acelerador (3.2): DmaBuf (rpcmem de Qualcomm o /dev/dma_heap en Linux y Android: la NPU Hexagon y
//   las GPU Adreno/Mali la leen sin copia), CudaPinned (cudaHostAlloc: copias a la GPU NVIDIA por DMA, asincronas) y
//   HipRegistrada (hipHostRegister, igual para AMD). Si el driver o la biblioteca no estan, el bloque cae a memoria
//   compartida o normal; Buffer::memoria() dice la que se consiguio.
// - Pool recicla bloques del mismo tamano con una lista libre lock-free (MpmcRing): pedir y devolver un bloque no
//   toca el asignador del sistema ni ningun mutex.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "util/json.h"

namespace nx::hal {

enum class Memoria { Host, Compartida, DmaBuf, CudaPinned, HipRegistrada };
const char* nombre(Memoria m);
// memoria que se pide para los tensores y bloques de los motores segun el hardware (perfil de hardware/profile)
Memoria memoria_para(const json& perfil);
// memoria por defecto de los tensores de los motores (la fija Services al arrancar con memoria_para)
Memoria memoria_preferida();
void fijar_memoria_preferida(Memoria m);
// que tipos de memoria del acelerador hay en este equipo (se prueba a reservar una pagina de cada uno)
json memorias_disponibles();

inline constexpr size_t kPagina = 4096;

class PoolImpl;
struct Bloque;

// referencia contada a un bloque (copiar = compartir, sin copiar los datos)
class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer& o) noexcept;
  Buffer(Buffer&& o) noexcept;
  Buffer& operator=(const Buffer& o) noexcept;
  Buffer& operator=(Buffer&& o) noexcept;
  ~Buffer();

  explicit operator bool() const { return b_ != nullptr; }
  uint8_t* data();
  const uint8_t* data() const;
  size_t size() const;                // bytes utiles
  size_t capacidad() const;           // bytes reservados (multiplo de la pagina)
  bool resize(size_t n);              // sin realojar: false si n > capacidad
  std::string_view vista() const { return {(const char*)data(), size()}; }
  Memoria memoria() const;
  bool compartida() const { return b_ && memoria() == Memoria::Compartida; }
  // descriptor para otro proceso: "fd:<n>" (memfd o dma-buf) (Linux/Android, mapeable en /proc/<pid>/fd/<n>), "win:<handle>" o ""
  std::string descriptor() const;
  long referencias() const;
  uintptr_t direccion() const { return (uintptr_t)data(); }

 private:
  friend class PoolImpl;
  friend Buffer reservar(size_t, Memoria);
  explicit Buffer(Bloque* b) : b_(b) {}
  void soltar() noexcept;
  Bloque* b_ = nullptr;
};

// bloque suelto (no reciclado)
Buffer reservar(size_t bytes, Memoria m = Memoria::Host);
// copia unica de datos externos a un bloque (la entrada de una peticion HTTP, un fichero leido)
Buffer copiar(std::string_view datos, Memoria m = Memoria::Host);

class Pool {
 public:
  // bloques de tam_bloque bytes; hasta max_bloques vivos a la vez (los que pasen se reservan sueltos)
  Pool(size_t tam_bloque, size_t max_bloques, Memoria m = Memoria::Host);
  ~Pool();   // el pool tiene que vivir mas que sus buffers (los pools del programa son globales)
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Buffer pedir(size_t bytes);
  json estado() const;

 private:
  PoolImpl* impl_;
};

// lee un fichero entero en un bloque del pool de imagenes (sin pasar por un std::string); vacio si no se puede
Buffer leer_fichero(const std::string& ruta_utf8);

// pools globales del programa: frames (hasta 4 MB: 1080p RGBA o un JPEG grande) e imagenes (hasta 16 MB)
Pool& pool_frames();
Pool& pool_imagenes();

// totales del asignador (bloques vivos, bytes, reutilizaciones...)
json estado_asignador();

}  // namespace nx::hal
