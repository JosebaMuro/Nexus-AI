// Video a partir de una secuencia de imagenes, sin dependencias: cada fotograma se compone en C++ (encuadre, zoom
// lento tipo Ken Burns y fundido entre imagenes) y se guarda como JPEG dentro de un AVI (Motion JPEG). Lo abren VLC,
// el reproductor de Windows, QuickTime con VLC, Android y los editores de video. Para WebM/MP4 con titulos y musica
// esta el editor web (recursos/web/video.html), que hace lo mismo en el navegador con MediaRecorder.
#pragma once
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::video {

struct Opts {
  int w = 1280, h = 720;
  int fps = 24;
  double segundos = 3.0;      // por imagen
  double transicion = 0.8;    // segundos de fundido entre imagenes (0 = corte)
  bool kenburns = true;       // zoom y desplazamiento lentos
  int calidad = 85;           // JPEG
};

// formato por nombre: "16:9" (1280x720), "1080p", "9:16" (vertical, redes), "1:1", "4:3"
void formato(Opts& o, const std::string& nombre);

// compone el video; devuelve {frames, seconds, width, height, fps, bytes}. Lanza si no hay imagenes legibles.
json render_avi(const std::vector<fs::path>& imagenes, const Opts& o, const fs::path& out);

// cabecera AVI + fotogramas JPEG (expuesto para las pruebas)
class AviWriter {
 public:
  AviWriter(const fs::path& out, int w, int h, int fps);
  ~AviWriter();
  bool ok() const { return f_ != nullptr; }
  void frame(const std::string& jpeg);
  json close();

 private:
  FILE* f_ = nullptr;
  int w_, h_, fps_;
  long movi_pos_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> index_;   // desplazamiento y tamano de cada fotograma
  uint32_t max_frame_ = 0;
};

}  // namespace nx::video
