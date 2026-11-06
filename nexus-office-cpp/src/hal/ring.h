// Capa de abstraccion de hardware: buffers circulares lock-free entre hilos (solo operaciones atomicas, sin mutex).
//
// - SpscRing<T, N>: un productor y un consumidor (frames de la camara -> percepcion, difusion -> presentador).
//   Indices en lineas de cache distintas y cache local del indice contrario: empujar y sacar cuestan unas decenas de
//   nanosegundos y el paso entre hilos queda por debajo del microsegundo en un equipo normal.
// - MpmcRing<T>: varios productores y consumidores (cola de trabajos de difusion, lista libre del asignador). Es la
//   cola acotada de Dmitry Vyukov: cada celda lleva su numero de secuencia y nadie espera a nadie.
//
// Ninguno de los dos bloquea: si esta lleno, push devuelve false (el que llama decide si descarta o reintenta). Para
// dormir mientras no hay nada se usa Senal (std::atomic::wait de C++20, un futex en Linux/Android).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace nx::hal {

// 64 bytes: linea de cache de x86-64 y de los ARM64 de movil (Cortex-A/X); evita el falso compartir entre hilos
inline constexpr size_t kLinea = 64;

constexpr bool potencia_de_dos(size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

template <class T, size_t N>
class SpscRing {
  static_assert(potencia_de_dos(N), "la capacidad del ring debe ser potencia de dos");
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_default_constructible_v<T>);

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // solo el hilo productor
  bool push(T v) {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_cache_ == N) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h - tail_cache_ == N) return false;   // lleno
    }
    slots_[h & (N - 1)] = std::move(v);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // solo el hilo consumidor
  std::optional<T> pop() {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t == head_cache_) return std::nullopt;   // vacio
    }
    T v = std::move(slots_[t & (N - 1)]);
    slots_[t & (N - 1)] = T{};   // suelta lo que tuviera (referencias a buffers)
    tail_.store(t + 1, std::memory_order_release);
    return v;
  }

  size_t size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  static constexpr size_t capacity() { return N; }

 private:
  alignas(kLinea) std::atomic<size_t> head_{0};
  size_t tail_cache_ = 0;   // copia del indice del consumidor que guarda el productor
  alignas(kLinea) std::atomic<size_t> tail_{0};
  size_t head_cache_ = 0;   // copia del indice del productor que guarda el consumidor
  alignas(kLinea) T slots_[N]{};
};

template <class T>
class MpmcRing {
  static_assert(std::is_nothrow_move_constructible_v<T> && std::is_default_constructible_v<T>);

 public:
  explicit MpmcRing(size_t capacidad) : mask_(redondear(capacidad) - 1), cells_(new Cell[mask_ + 1]) {
    for (size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  MpmcRing(const MpmcRing&) = delete;
  MpmcRing& operator=(const MpmcRing&) = delete;

  bool push(T v) {
    size_t pos = enq_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = (intptr_t)seq - (intptr_t)pos;
      if (dif == 0) {
        if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.val = std::move(v);
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;   // lleno
      } else {
        pos = enq_.load(std::memory_order_relaxed);
      }
    }
  }

  std::optional<T> pop() {
    size_t pos = deq_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
      if (dif == 0) {
        if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          T v = std::move(c.val);
          c.val = T{};
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return v;
        }
      } else if (dif < 0) {
        return std::nullopt;   // vacio
      } else {
        pos = deq_.load(std::memory_order_relaxed);
      }
    }
  }

  // aproximado (otros hilos pueden estar empujando o sacando a la vez)
  size_t size() const {
    const size_t e = enq_.load(std::memory_order_acquire), d = deq_.load(std::memory_order_acquire);
    return e >= d ? e - d : 0;
  }
  size_t capacity() const { return mask_ + 1; }

 private:
  struct alignas(kLinea) Cell {
    std::atomic<size_t> seq{0};
    T val{};
  };
  static size_t redondear(size_t n) {
    size_t p = 2;
    while (p < n) p <<= 1;
    return p;
  }
  const size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(kLinea) std::atomic<size_t> enq_{0};
  alignas(kLinea) std::atomic<size_t> deq_{0};
};

// despertar a un consumidor dormido sin mutex ni variable de condicion: un contador atomico con wait/notify
class Senal {
 public:
  void notificar() {
    gen_.fetch_add(1, std::memory_order_release);
    gen_.notify_one();
  }
  void notificar_todos() {
    gen_.fetch_add(1, std::memory_order_release);
    gen_.notify_all();
  }
  uint32_t generacion() const { return gen_.load(std::memory_order_acquire); }
  // duerme hasta que la generacion cambie respecto a «vista»
  void esperar(uint32_t vista) const { gen_.wait(vista, std::memory_order_acquire); }

 private:
  std::atomic<uint32_t> gen_{0};
};

// ultimo valor publicado, sin bloqueos (triple buffer): un escritor y un lector. El escritor nunca espera al lector;
// el lector siempre obtiene el frame mas reciente completo. Es el «buffer de pantalla» del motor de difusion.
template <class T>
class TripleBuffer {
 public:
  // escritor: deja v como frame nuevo
  void publicar(T v) {
    slots_[back_] = std::move(v);
    const uint8_t prev = mid_.exchange(uint8_t(back_ | kNuevo), std::memory_order_acq_rel);
    back_ = prev & kIndice;
  }
  // lector: true si habia un frame nuevo (queda en actual())
  bool recoger() {
    if (!(mid_.load(std::memory_order_relaxed) & kNuevo)) return false;
    const uint8_t prev = mid_.exchange(front_, std::memory_order_acq_rel);
    front_ = prev & kIndice;
    return true;
  }
  const T& actual() const { return slots_[front_]; }

 private:
  static constexpr uint8_t kNuevo = 0x4, kIndice = 0x3;
  T slots_[3]{};
  alignas(kLinea) std::atomic<uint8_t> mid_{1};
  alignas(kLinea) uint8_t back_ = 0;    // solo el escritor
  alignas(kLinea) uint8_t front_ = 2;   // solo el lector
};

}  // namespace nx::hal
