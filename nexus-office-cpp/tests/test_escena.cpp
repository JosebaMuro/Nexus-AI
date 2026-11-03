// Compositor de escenas (tests/test_image_prompt.py): la imagen sin modelo depende del prompt; prompts distintos dan
// imagenes distintas y con lo que dice el texto (hora, lugar, color y sujeto).
#include <cmath>
#include <functional>

#include "check.h"
#include "media/scene.h"

using namespace nx;
using namespace nx::media;

namespace {

constexpr int N = 192;

struct Img {
  Canvas c;
  // media de los canales (ch < 0: los tres) entre las filas y0 e y1
  double mean(int y0 = 0, int y1 = N, int ch = -1) const {
    double acc = 0;
    long n = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < c.w; ++x)
        for (int k = 0; k < 3; ++k)
          if (ch < 0 || ch == k) {
            acc += c.px[((size_t)y * c.w + x) * 3 + k];
            ++n;
          }
    return n ? acc / n : 0;
  }
  double frac(const std::function<bool(int, int, int)>& cond, int y0 = 0, int y1 = N) const {
    long hit = 0, n = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < c.w; ++x, ++n) {
        const uint8_t* p = &c.px[((size_t)y * c.w + x) * 3];
        if (cond(p[0], p[1], p[2])) ++hit;
      }
    return n ? (double)hit / n : 0;
  }
};

Img arr(const std::string& prompt, long long seed = 1) { return {scene::compose(prompt, seed, N)}; }

double diff(const Img& a, const Img& b) {
  double acc = 0;
  for (size_t i = 0; i < a.c.px.size(); ++i) acc += std::abs((int)a.c.px[i] - (int)b.c.px[i]);
  return acc / a.c.px.size();
}

}  // namespace

TEST("escena: entiende espanol e ingles") {
  auto s = scene::parse("Dos gatos negros bajo la luna en una ciudad");
  CHECK_EQ(s.time, std::string("night"));
  CHECK(s.places == std::vector<std::string>{"city"});
  CHECK(std::find(s.sky.begin(), s.sky.end(), "moon") != s.sky.end());
  CHECK_EQ(s.subjects.size(), (size_t)1);
  CHECK_EQ(s.subjects[0].kind, std::string("cat"));
  CHECK_EQ(s.subjects[0].count, 2);
  CHECK(s.subjects[0].color == (scene::Rgb{28, 28, 32}));
  s = scene::parse("a red car on the beach at sunset");
  CHECK_EQ(s.time, std::string("sunset"));
  CHECK(s.places == std::vector<std::string>{"beach"});
  CHECK(s.subjects[0].kind == "car" && s.subjects[0].color == (scene::Rgb{210, 40, 45}));
  s = scene::parse("Castillo en la montaña con nieve y un árbol");
  CHECK(s.places == std::vector<std::string>{"mountain"});
  CHECK(std::find(s.weather.begin(), s.weather.end(), "snow") != s.weather.end());
  CHECK(s.subjects.size() == 2 && s.subjects[0].kind == "castle" && s.subjects[1].kind == "tree");
  CHECK_EQ(scene::parse("un ramo de rosas rojas").subjects[0].kind, std::string("flower"));
  CHECK(scene::parse("un gato rosa").subjects[0].color == (scene::Rgb{240, 110, 170}));
  CHECK(scene::parse("un fractal abstracto morado").abstract);
  CHECK(!scene::understood("xyzzy qwerty"));
  CHECK(scene::understood("de noche"));
}

TEST("escena: prompts distintos dan imagenes distintas") {
  std::vector<std::string> prompts = {"un gato negro bajo la luna",        "una casa en la playa al atardecer",
                                      "a red car in a city at night",      "castillo en la montaña con nieve",
                                      "dos perros en un campo de flores amarillas", "un astronauta en el espacio",
                                      "un barco en el mar con tormenta",   "robot en el desierto al amanecer"};
  std::vector<Img> imgs;
  for (auto& p : prompts) imgs.push_back(arr(p));
  for (size_t i = 0; i < imgs.size(); ++i)
    for (size_t j = i + 1; j < imgs.size(); ++j) {
      double d = diff(imgs[i], imgs[j]);
      if (d <= 20) std::cerr << "  se parecen demasiado: " << prompts[i] << " / " << prompts[j] << " (" << d << ")\n";
      CHECK(d > 20);
    }
}

TEST("escena: mismo prompt y semilla es reproducible") {
  CHECK(arr("un faro en la costa de noche", 5).c.px == arr("un faro en la costa de noche", 5).c.px);
}

TEST("escena: la imagen refleja la hora") {
  auto noche = arr("una casa en el campo de noche"), dia = arr("una casa en el campo de dia");
  CHECK(noche.mean(0, N / 3) < 60);
  CHECK(dia.mean(0, N / 3) > 60);
  auto tarde = arr("una casa en el campo al atardecer");
  CHECK(tarde.mean(0, N / 3, 0) > tarde.mean(0, N / 3, 2));   // cielo calido al atardecer
}

TEST("escena: la imagen refleja el lugar") {
  auto mar = arr("el mar en calma");
  CHECK(mar.mean((int)(N * 0.65), N, 2) > mar.mean((int)(N * 0.65), N, 0) + 40);   // agua azul abajo
  auto des = arr("un desierto");
  CHECK(des.mean((int)(N * 0.75), N, 0) > des.mean((int)(N * 0.75), N, 2) + 50);   // arena
  CHECK(arr("una montaña con nieve").mean((int)(N * 0.75), N) > 200);              // suelo blanco
  CHECK(arr("el espacio").mean() < 70);
}

TEST("escena: la imagen refleja el color del sujeto") {
  auto red = [](int r, int g, int b) { return r > 150 && g < 80 && b < 80; };
  auto blue = [](int r, int g, int b) { return b > 150 && r < 80 && g < 120; };
  auto rojo = arr("un coche rojo en la ciudad"), azul = arr("un coche azul en la ciudad");
  CHECK(rojo.frac(red) > 0.01 && azul.frac(red) < rojo.frac(red) / 3);
  CHECK(azul.frac(blue) > 0.01 && rojo.frac(blue) < azul.frac(blue) / 3);
}

TEST("escena: la imagen contiene el sujeto") {
  // un gato naranja aparece en la mitad de abajo y, con el mismo fondo, la diferencia con un perro esta solo donde
  // van los animales
  auto orange = [](int r, int g, int b) { return r > 200 && g > 110 && g < 160 && b < 80; };
  auto gato = arr("un gato naranja en un prado", 3), perro = arr("un perro en un prado", 3);
  CHECK(gato.frac(orange, N / 2, N) > 0.01);
  CHECK(perro.frac(orange) < 0.001);
  long changed = 0;
  int ymin = N;
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      int s = 0;
      for (int k = 0; k < 3; ++k)
        s += std::abs((int)gato.c.px[((size_t)y * N + x) * 3 + k] - (int)perro.c.px[((size_t)y * N + x) * 3 + k]);
      if (s > 60) {
        ++changed;
        ymin = std::min(ymin, y);
      }
    }
  CHECK(changed > N * N * 0.01);
  CHECK(ymin > N * 0.4);   // el cielo y el horizonte no cambian
}
