// Multimedia (nexus/media y nexus/prompt_enhancer.py): imagen, musica, voz y mejorador de prompts.
//
// Edicion C++: la imagen neuronal usa stable-diffusion.cpp (sd-cli) igual que la version Python; sin modelo se
// genera arte procedural. La voz usa Piper (ejecutable) o la voz del sistema, y el dictado whisper.cpp
// (whisper-cli). La musica procedural es la misma teoria musical sintetizada en C++.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/config.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx::models {
class Catalog;
}
namespace nx::runtimes {
class RuntimeManager;
}
namespace nx::llm {
class LLMManager;
}

namespace nx::prompt {
extern const std::vector<std::pair<std::string, std::string>> STYLES;   // orden de la version Python
std::string style(const std::string& key);                              // STYLES.get(key, key)
extern const char* QUALITY;
extern const char* NEGATIVE;
bool looks_spanish(const std::string& text);
std::string translate_rules(const std::string& text);
json enhance_rules(const std::string& text, const std::string& target = "image", const std::string& style = "");
json enhance(llm::LLMManager* llm, std::string text, std::string target = "image", const std::string& style = "",
             bool use_llm = true);
}  // namespace nx::prompt

namespace nx::media {

using ProfileGetter = std::function<json()>;

// lienzo RGB minimo (lineas con grosor, elipses, desenfoque y PNG)
struct Canvas {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  Canvas(int width, int height, uint32_t rgb = 0);
  void plot(int x, int y, const uint8_t c[3]);
  void disc(double cx, double cy, double r, const uint8_t c[3]);
  void line(const std::vector<std::pair<double, double>>& pts, const uint8_t c[3], int width = 1);
  void ellipse_outline(double x0, double y0, double x1, double y1, const uint8_t c[3], int width = 1);
  void gaussian_blur(double radius);
  bool save_png(const fs::path& p) const;
  std::string png() const;
};

std::string slug_name(const std::string& t, size_t max, const std::string& fallback);   // _slug de image.py / music.py

class ImageEngine {
 public:
  ImageEngine(Config& config, models::Catalog& catalog, runtimes::RuntimeManager& rt, ProfileGetter profile,
              llm::LLMManager* llm = nullptr);
  json model();
  json status();
  json generate(const std::string& prompt, const std::string& negative = "", std::optional<int> size = std::nullopt,
                std::optional<int> steps = std::nullopt, std::optional<long long> seed = std::nullopt,
                const std::string& idea = "");
  std::string english(const std::string& prompt);
  static Canvas procedural(const std::string& prompt, long long seed, int size = 512);
  json analyze(const std::string& data, const std::string& question = "");
  static json describe_stats(const std::vector<uint8_t>& rgb, int w, int h);

 private:
  Config& config_;
  models::Catalog& catalog_;
  runtimes::RuntimeManager& rt_;
  ProfileGetter profile_;
  llm::LLMManager* llm_;
  json sdcpp(const fs::path& exe, const std::string& variant, const json& model, const std::string& prompt,
             const std::string& negative, std::optional<int> size, std::optional<int> steps, long long seed,
             const fs::path& out);
  bool vision_available();
};

// ---------------------------------------------------------------- musica
std::string detect_mood(const std::string& prompt);
void write_midi(const fs::path& path, const std::vector<std::tuple<double, double, int, int>>& notes, int tempo, int tpq = 480);

class Composer {
 public:
  explicit Composer(int bars = 16) : bars_(bars) {}
  json compose(const std::string& prompt, std::optional<double> seconds = std::nullopt,
               std::optional<long long> seed = std::nullopt);

 private:
  int bars_;
};

class MusicEngine {
 public:
  MusicEngine(Config& config, ProfileGetter profile);
  std::vector<std::string> engines();
  json generate(const std::string& prompt, double seconds = 20, std::string engine = "");
  Composer composer;

 private:
  Config& config_;
  ProfileGetter profile_;
};

// ---------------------------------------------------------------- voz
class VoiceEngine {
 public:
  VoiceEngine(Config& config, models::Catalog& catalog, ProfileGetter profile, runtimes::RuntimeManager* rt = nullptr);
  std::vector<std::string> tts_engines();
  std::vector<std::string> stt_engines();
  json info();
  // (wav, motor) o nullopt (la interfaz usa la voz del navegador)
  std::optional<std::pair<std::string, std::string>> tts(const std::string& text, double rate = 1.0);
  json stt(const std::string& audio, const std::string& suffix = ".webm");

 private:
  Config& config_;
  models::Catalog& catalog_;
  ProfileGetter profile_;
  runtimes::RuntimeManager* rt_;
  std::mutex mu_;
  json piper_model();
  std::optional<fs::path> piper_exe();
  std::optional<fs::path> whisper_exe();
  std::vector<std::string> system_tts_cmd();
  std::string tts_piper(const std::string& text, double rate);
  std::string tts_system(const std::string& text, double rate);
};

}  // namespace nx::media
