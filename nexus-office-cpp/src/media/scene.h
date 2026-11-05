// Compositor de escenas (media/scene.py): imagen sin modelo neuronal que representa lo que dice el prompt.
//
// Lee el texto (espanol o ingles) y saca la hora (dia, noche, atardecer...), el clima, el lugar (mar, playa, montana,
// bosque, ciudad, espacio...), los sujetos (gato, casa, arbol, barco, persona...) con su color y cuantos hay, y dibuja
// esa escena con formas vectoriales. No compite con stable-diffusion.cpp, pero «un gato negro bajo la luna» da un gato
// negro bajo la luna y no un dibujo abstracto que se parece en todos los prompts.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "media/media.h"

namespace nx::media::scene {

using Rgb = std::array<int, 3>;

struct Subject {
  std::string kind;
  std::optional<Rgb> color;
  int count = 1;
};

struct Scene {
  std::string time;                   // day, night, sunset, sunrise
  std::vector<std::string> weather;   // rain, snow, fog, storm (pueden repetirse)
  std::vector<std::string> places;    // sea, beach, mountain, forest, city, space, desert, lake, garden, field
  std::vector<std::string> sky;       // moon, sun, star, cloud, planet
  std::vector<Subject> subjects;
  std::vector<Rgb> colors;
  bool abstract = false;
  json to_json() const;
};

Scene parse(const std::string& prompt);                          // descompone el prompt
Canvas compose(const std::string& prompt, long long seed = 0, int size = 512);   // determinista por prompt y semilla
bool understood(const std::string& prompt);                      // reconoce algo concreto del prompt

}  // namespace nx::media::scene
