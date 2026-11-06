// Asignador zero-copy (ver zerocopy.h).
#include "hal/zerocopy.h"

#include <cstdlib>
#include <cstring>

#include "hal/ring.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace nx::hal {

const char* nombre(Memoria m) { return m == Memoria::Compartida ? "compartida" : "host"; }

namespace {

std::atomic<long long> g_vivos{0}, g_bytes{0}, g_reservas{0}, g_compartidos{0};

size_t a_pagina(size_t n) { return ((n ? n : 1) + kPagina - 1) / kPagina * kPagina; }

}  // namespace

struct Bloque {
  std::atomic<long> refs{1};
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

// reserva la memoria del bloque; si la compartida no esta disponible, cae a memoria normal alineada
bool asignar(Bloque& b, size_t bytes, Memoria m) {
  b.cap = a_pagina(bytes);
  b.mem = m;
  if (m == Memoria::Compartida) {
#ifdef _WIN32
    const unsigned long long cap = b.cap;
    b.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(cap >> 32), (DWORD)cap, nullptr);
    if (b.map) {
      b.ptr = (uint8_t*)MapViewOfFile(b.map, FILE_MAP_ALL_ACCESS, 0, 0, b.cap);
      if (b.ptr) return true;
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
        return true;
      }
    }
    if (fd >= 0) close(fd);
#endif
    b.mem = Memoria::Host;
  }
#ifdef _WIN32
  b.ptr = (uint8_t*)_aligned_malloc(b.cap, kPagina);
#else
  void* p = nullptr;
  if (posix_memalign(&p, kPagina, b.cap) != 0) p = nullptr;
  b.ptr = (uint8_t*)p;
#endif
  return b.ptr != nullptr;
}

void liberar(Bloque* b) {
  if (!b) return;
  if (b->ptr) {
#ifdef _WIN32
    if (b->map) {
      UnmapViewOfFile(b->ptr);
      CloseHandle(b->map);
    } else {
      _aligned_free(b->ptr);
    }
#else
    if (b->fd >= 0) {
      munmap(b->ptr, b->cap);
      close(b->fd);
    } else {
      free(b->ptr);
    }
#endif
  }
  g_vivos.fetch_sub(1, std::memory_order_relaxed);
  g_bytes.fetch_sub((long long)b->cap, std::memory_order_relaxed);
  if (b->mem == Memoria::Compartida) g_compartidos.fetch_sub(1, std::memory_order_relaxed);
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
  if (b->mem == Memoria::Compartida) g_compartidos.fetch_add(1, std::memory_order_relaxed);
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
  if (!b_ || b_->mem != Memoria::Compartida) return "";
#ifdef _WIN32
  return "win:" + std::to_string((unsigned long long)(uintptr_t)b_->map);
#else
  return "fd:" + std::to_string(b_->fd);
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

json estado_asignador() {
  return {{"bloques_vivos", g_vivos.load()},
          {"bytes", g_bytes.load()},
          {"reservas", g_reservas.load()},
          {"compartidos", g_compartidos.load()},
          {"alineacion", kPagina},
          {"frames", pool_frames().estado()},
          {"imagenes", pool_imagenes().estado()}};
}

}  // namespace nx::hal
