// Asignador zero-copy (ver zerocopy.h).
#include "hal/zerocopy.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

#include "hal/ring.h"
#include "util/dynlib.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif
#endif

namespace nx::hal {

const char* nombre(Memoria m) {
  switch (m) {
    case Memoria::Compartida: return "compartida";
    case Memoria::DmaBuf: return "dma-buf";
    case Memoria::CudaPinned: return "cuda-pinned";
    case Memoria::HipRegistrada: return "hip-registrada";
    default: return "host";
  }
}

namespace {

std::atomic<long long> g_vivos{0}, g_bytes{0}, g_reservas{0}, g_compartidos{0};

size_t a_pagina(size_t n) { return ((n ? n : 1) + kPagina - 1) / kPagina * kPagina; }

}  // namespace

// de donde salio la memoria del bloque (para devolverla igual)
enum class Origen { Alineada, Mapeada, Rpcmem, Cuda, Hip };

struct Bloque {
  std::atomic<long> refs{1};
  Origen origen = Origen::Alineada;
  uint8_t* ptr = nullptr;
  size_t cap = 0, len = 0;
  Memoria mem = Memoria::Host;
  PoolImpl* pool = nullptr;
#ifdef _WIN32
  HANDLE map = nullptr;
#else
  int fd = -1;
#endif
};

namespace {

// ---------------------------------------------------------------- bibliotecas de los aceleradores (dlopen)
// rpcmem (libcdsprpc.so, Qualcomm): la memoria que la NPU Hexagon usa sin copiar (QNN HTP «shared buffer»)
struct Rpcmem {
  DynLib lib;
  void* (*alloc)(int, unsigned, int) = nullptr;
  void (*free)(void*) = nullptr;
  int (*to_fd)(void*) = nullptr;
  bool ok = false;
};
Rpcmem& rpcmem() {
  static Rpcmem* r = [] {
    auto* x = new Rpcmem;
#if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
    if (x->lib.open("libcdsprpc.so") && x->lib.get("rpcmem_alloc", x->alloc) && x->lib.get("rpcmem_free", x->free))
      x->lib.get("rpcmem_to_fd", x->to_fd), x->ok = true;
#endif
    return x;
  }();
  return *r;
}
// CUDA: cudaHostAlloc / cudaFreeHost del runtime que trae el paquete de CUDA (o el del sistema)
struct Cuda {
  DynLib lib;
  int (*host_alloc)(void**, size_t, unsigned) = nullptr;
  int (*free_host)(void*) = nullptr;
  bool ok = false;
};
Cuda& cuda() {
  static Cuda* c = [] {
    auto* x = new Cuda;
#ifdef _WIN32
    const char* nombres[] = {"cudart64_13.dll", "cudart64_12.dll", "cudart64_110.dll"};
#else
    const char* nombres[] = {"libcudart.so.13", "libcudart.so.12", "libcudart.so"};
#endif
    for (const char* n : nombres)
      if (x->lib.open(n) && x->lib.get("cudaHostAlloc", x->host_alloc) && x->lib.get("cudaFreeHost", x->free_host)) {
        x->ok = true;
        break;
      }
    return x;
  }();
  return *c;
}
// HIP (AMD ROCm): hipHostRegister sobre memoria alineada normal
struct Hip {
  DynLib lib;
  int (*reg)(void*, size_t, unsigned) = nullptr;
  int (*unreg)(void*) = nullptr;
  bool ok = false;
};
Hip& hip() {
  static Hip* h = [] {
    auto* x = new Hip;
#ifdef _WIN32
    const char* nombres[] = {"amdhip64_6.dll", "amdhip64.dll"};
#else
    const char* nombres[] = {"libamdhip64.so.6", "libamdhip64.so"};
#endif
    for (const char* n : nombres)
      if (x->lib.open(n) && x->lib.get("hipHostRegister", x->reg) && x->lib.get("hipHostUnregister", x->unreg)) {
        x->ok = true;
        break;
      }
    return x;
  }();
  return *h;
}

#if defined(__linux__)
// /dev/dma_heap (Linux 5.6+, Android 12+): struct dma_heap_allocation_data y DMA_HEAP_IOCTL_ALLOC de linux/dma-heap.h
struct DmaHeapAlloc {
  uint64_t len;
  uint32_t fd, fd_flags;
  uint64_t heap_flags;
};
#define NX_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, DmaHeapAlloc)
int dma_heap_fd() {
  static int fd = [] {
    for (const char* p : {"/dev/dma_heap/qcom,system", "/dev/dma_heap/system", "/dev/dma_heap/mtk_mm"}) {
      int f = open(p, O_RDONLY | O_CLOEXEC);
      if (f >= 0) return f;
    }
    return -1;
  }();
  return fd;
}
#endif

bool alinear(Bloque& b) {
#ifdef _WIN32
  b.ptr = (uint8_t*)_aligned_malloc(b.cap, kPagina);
#else
  void* p = nullptr;
  if (posix_memalign(&p, kPagina, b.cap) != 0) p = nullptr;
  b.ptr = (uint8_t*)p;
#endif
  b.origen = Origen::Alineada;
  return b.ptr != nullptr;
}

// memoria del acelerador; false si no se pudo (el llamador cae a la compartida)
bool asignar_acelerador(Bloque& b) {
  if (b.mem == Memoria::CudaPinned && cuda().ok) {
    void* p = nullptr;
    if (cuda().host_alloc(&p, b.cap, 1u /* cudaHostAllocPortable */) == 0 && p) {
      b.ptr = (uint8_t*)p;
      b.origen = Origen::Cuda;
      return true;
    }
  }
  if (b.mem == Memoria::HipRegistrada && hip().ok && alinear(b)) {
    if (hip().reg(b.ptr, b.cap, 1u /* hipHostRegisterPortable */) == 0) {
      b.origen = Origen::Hip;
      return true;
    }
#ifdef _WIN32
    _aligned_free(b.ptr);
#else
    free(b.ptr);
#endif
    b.ptr = nullptr;
  }
  if (b.mem == Memoria::DmaBuf) {
    if (rpcmem().ok) {
      void* p = rpcmem().alloc(25 /* RPCMEM_HEAP_ID_SYSTEM */, 1u /* RPCMEM_DEFAULT_FLAGS */, (int)b.cap);
      if (p) {
        b.ptr = (uint8_t*)p;
        b.origen = Origen::Rpcmem;
#ifndef _WIN32
        if (rpcmem().to_fd) b.fd = rpcmem().to_fd(p);
#endif
        return true;
      }
    }
#if defined(__linux__)
    if (dma_heap_fd() >= 0) {
      DmaHeapAlloc d{b.cap, 0, O_RDWR | O_CLOEXEC, 0};
      if (ioctl(dma_heap_fd(), NX_DMA_HEAP_IOCTL_ALLOC, &d) == 0) {
        void* p = mmap(nullptr, b.cap, PROT_READ | PROT_WRITE, MAP_SHARED, (int)d.fd, 0);
        if (p != MAP_FAILED) {
          b.ptr = (uint8_t*)p;
          b.fd = (int)d.fd;
          b.origen = Origen::Mapeada;
          return true;
        }
        close((int)d.fd);
      }
    }
#endif
  }
  return false;
}

// reserva la memoria del bloque; si la pedida no esta disponible, cae a la compartida y despues a la normal
bool asignar(Bloque& b, size_t bytes, Memoria m) {
  b.cap = a_pagina(bytes);
  b.mem = m;
  if (m == Memoria::DmaBuf || m == Memoria::CudaPinned || m == Memoria::HipRegistrada) {
    if (asignar_acelerador(b)) return true;
    b.mem = m = Memoria::Compartida;
  }
  if (m == Memoria::Compartida) {
#ifdef _WIN32
    const unsigned long long cap = b.cap;
    b.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, nullptr);
    if (b.map) {
      b.ptr = (uint8_t*)MapViewOfFile(b.map, FILE_MAP_ALL_ACCESS, 0, 0, b.cap);
      if (b.ptr) {
        b.origen = Origen::Mapeada;
        return true;
      }
      CloseHandle(b.map);
      b.map = nullptr;
    }
#elif defined(__linux__) && defined(SYS_memfd_create)
    // memfd: Linux 3.17+ y todos los Android con ARM64 que soporta la app (API 28+)
    int fd = (int)syscall(SYS_memfd_create, "nexus-zerocopy", 1u /* MFD_CLOEXEC */);
    if (fd >= 0 && ftruncate(fd, (off_t)b.cap) == 0) {
      void* p = mmap(nullptr, b.cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED) {
        b.ptr = (uint8_t*)p;
        b.fd = fd;
        b.origen = Origen::Mapeada;
        return true;
      }
    }
    if (fd >= 0) close(fd);
#endif
    b.mem = Memoria::Host;
  }
  return alinear(b);
}

void liberar(Bloque* b) {
  if (!b) return;
  if (b->ptr) {
    switch (b->origen) {
      case Origen::Cuda: cuda().free_host(b->ptr); break;
      case Origen::Rpcmem: rpcmem().free(b->ptr); break;
      case Origen::Hip: hip().unreg(b->ptr); [[fallthrough]];
      case Origen::Alineada:
#ifdef _WIN32
        _aligned_free(b->ptr);
#else
        free(b->ptr);
#endif
        break;
      case Origen::Mapeada:
#ifdef _WIN32
        UnmapViewOfFile(b->ptr);
        CloseHandle(b->map);
#else
        munmap(b->ptr, b->cap);
        close(b->fd);
#endif
        break;
    }
  }
  g_vivos.fetch_sub(1, std::memory_order_relaxed);
  g_bytes.fetch_sub((long long)b->cap, std::memory_order_relaxed);
  if (b->mem != Memoria::Host) g_compartidos.fetch_sub(1, std::memory_order_relaxed);
  delete b;
}

Bloque* nuevo(size_t bytes, Memoria m) {
  auto* b = new Bloque;
  if (!asignar(*b, bytes, m)) {
    delete b;
    throw std::bad_alloc();
  }
  g_vivos.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add((long long)b->cap, std::memory_order_relaxed);
  g_reservas.fetch_add(1, std::memory_order_relaxed);
  if (b->mem != Memoria::Host) g_compartidos.fetch_add(1, std::memory_order_relaxed);
  return b;
}

}  // namespace

class PoolImpl {
 public:
  PoolImpl(size_t tam, size_t maxb, Memoria m) : tam(a_pagina(tam)), max_bloques(maxb), mem(m), libres(maxb) {}
  ~PoolImpl() {
    while (auto b = libres.pop()) liberar(*b);
  }
  Buffer pedir(size_t bytes) {
    if (bytes > tam) {   // no cabe en un bloque del pool: suelto
      sueltos.fetch_add(1, std::memory_order_relaxed);
      return reservar(bytes, mem);
    }
    Bloque* b = nullptr;
    if (auto r = libres.pop()) {
      b = *r;
      reutilizados.fetch_add(1, std::memory_order_relaxed);
    } else if (creados.fetch_add(1, std::memory_order_relaxed) < (long long)max_bloques) {
      b = nuevo(tam, mem);
      b->pool = this;
    } else {
      creados.fetch_sub(1, std::memory_order_relaxed);
      sueltos.fetch_add(1, std::memory_order_relaxed);
      return reservar(bytes, mem);
    }
    b->refs.store(1, std::memory_order_relaxed);
    b->len = bytes;
    en_uso.fetch_add(1, std::memory_order_relaxed);
    return Buffer(b);
  }
  void devolver(Bloque* b) {
    en_uso.fetch_sub(1, std::memory_order_relaxed);
    if (!libres.push(b)) {   // no deberia pasar (la lista libre tiene sitio para todos)
      creados.fetch_sub(1, std::memory_order_relaxed);
      liberar(b);
    }
  }
  const size_t tam, max_bloques;
  const Memoria mem;
  MpmcRing<Bloque*> libres;
  std::atomic<long long> creados{0}, en_uso{0}, reutilizados{0}, sueltos{0};
};

// ------------------------------------------------------------------ Buffer
Buffer::Buffer(const Buffer& o) noexcept : b_(o.b_) {
  if (b_) b_->refs.fetch_add(1, std::memory_order_relaxed);
}
Buffer::Buffer(Buffer&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }
Buffer& Buffer::operator=(const Buffer& o) noexcept {
  if (this != &o) {
    if (o.b_) o.b_->refs.fetch_add(1, std::memory_order_relaxed);
    soltar();
    b_ = o.b_;
  }
  return *this;
}
Buffer& Buffer::operator=(Buffer&& o) noexcept {
  if (this != &o) {
    soltar();
    b_ = o.b_;
    o.b_ = nullptr;
  }
  return *this;
}
Buffer::~Buffer() { soltar(); }

void Buffer::soltar() noexcept {
  if (!b_) return;
  if (b_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (b_->pool)
      b_->pool->devolver(b_);
    else
      liberar(b_);
  }
  b_ = nullptr;
}

uint8_t* Buffer::data() { return b_ ? b_->ptr : nullptr; }
const uint8_t* Buffer::data() const { return b_ ? b_->ptr : nullptr; }
size_t Buffer::size() const { return b_ ? b_->len : 0; }
size_t Buffer::capacidad() const { return b_ ? b_->cap : 0; }
bool Buffer::resize(size_t n) {
  if (!b_ || n > b_->cap) return false;
  b_->len = n;
  return true;
}
Memoria Buffer::memoria() const { return b_ ? b_->mem : Memoria::Host; }
long Buffer::referencias() const { return b_ ? b_->refs.load(std::memory_order_relaxed) : 0; }

std::string Buffer::descriptor() const {
  if (!b_) return "";
#ifdef _WIN32
  return b_->map ? "win:" + std::to_string((unsigned long long)(uintptr_t)b_->map) : "";
#else
  return b_->fd >= 0 ? "fd:" + std::to_string(b_->fd) : "";
#endif
}

Buffer reservar(size_t bytes, Memoria m) {
  Bloque* b = nuevo(bytes, m);
  b->len = bytes;
  return Buffer(b);
}

Buffer copiar(std::string_view datos, Memoria m) {
  Buffer b = reservar(datos.size(), m);
  if (!datos.empty()) std::memcpy(b.data(), datos.data(), datos.size());
  return b;
}

// ------------------------------------------------------------------ Pool
Pool::Pool(size_t tam, size_t maxb, Memoria m) : impl_(new PoolImpl(tam, maxb, m)) {}
Pool::~Pool() { delete impl_; }
Buffer Pool::pedir(size_t bytes) { return impl_->pedir(bytes); }
json Pool::estado() const {
  return {{"tam_bloque", impl_->tam},
          {"max_bloques", impl_->max_bloques},
          {"memoria", nombre(impl_->mem)},
          {"creados", impl_->creados.load()},
          {"en_uso", impl_->en_uso.load()},
          {"libres", impl_->libres.size()},
          {"reutilizados", impl_->reutilizados.load()},
          {"sueltos", impl_->sueltos.load()}};
}

// los pools globales no se destruyen nunca: puede quedar algun buffer vivo en otro objeto global al salir
Pool& pool_frames() {
  static Pool* p = new Pool(4u << 20, 16, Memoria::Compartida);
  return *p;
}
Pool& pool_imagenes() {
  static Pool* p = new Pool(16u << 20, 6, Memoria::Compartida);
  return *p;
}

namespace {
std::atomic<int> g_preferida{(int)Memoria::Compartida};
}
Memoria memoria_preferida() { return (Memoria)g_preferida.load(std::memory_order_relaxed); }
void fijar_memoria_preferida(Memoria m) { g_preferida.store((int)m, std::memory_order_relaxed); }

Memoria memoria_para(const json& perfil) {
  std::string acc = perfil.is_object() ? perfil.value("accelerator", std::string()) : "";
  std::string gpu = perfil.is_object() ? perfil.value("id", std::string()) : "";   // «cuda-nvidia-dgpu»...
  for (auto& c : gpu) c = (char)std::tolower((unsigned char)c);
  if (acc == "cuda" || gpu.find("nvidia") != std::string::npos) return Memoria::CudaPinned;
  if (acc == "rocm" || acc == "hip" || (gpu.find("amd") != std::string::npos && acc != "cpu" && acc != "vulkan"))
    return Memoria::HipRegistrada;
#if defined(__ANDROID__) || defined(__linux__)
  if (acc == "npu" || acc == "qnn" || acc == "hexagon" || acc == "opencl" || acc == "vulkan") return Memoria::DmaBuf;
#endif
#if defined(__ANDROID__)
  return Memoria::DmaBuf;
#else
  return Memoria::Compartida;
#endif
}

json memorias_disponibles() {
  json j = json::object();
  for (Memoria m : {Memoria::Host, Memoria::Compartida, Memoria::DmaBuf, Memoria::CudaPinned, Memoria::HipRegistrada}) {
    bool ok = false;
    try {
      ok = reservar(kPagina, m).memoria() == m;
    } catch (...) {
    }
    j[nombre(m)] = ok;
  }
  return j;
}

Buffer leer_fichero(const std::string& ruta) {
  FILE* f = nullptr;
#ifdef _WIN32
  int n16 = MultiByteToWideChar(CP_UTF8, 0, ruta.c_str(), -1, nullptr, 0);
  std::wstring w(n16 > 0 ? n16 : 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, ruta.c_str(), -1, w.data(), n16);
  f = _wfopen(w.c_str(), L"rb");
#else
  f = std::fopen(ruta.c_str(), "rb");
#endif
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  Buffer b;
  if (n > 0) {
    b = pool_imagenes().pedir((size_t)n);
    if (std::fread(b.data(), 1, (size_t)n, f) != (size_t)n) b = Buffer();
  }
  std::fclose(f);
  return b;
}

json estado_asignador() {
  return {{"bloques_vivos", g_vivos.load()},
          {"bytes", g_bytes.load()},
          {"reservas", g_reservas.load()},
          {"compartidos", g_compartidos.load()},
          {"alineacion", kPagina},
          {"preferida", nombre(memoria_preferida())},
          {"frames", pool_frames().estado()},
          {"imagenes", pool_imagenes().estado()}};
}

}  // namespace nx::hal
