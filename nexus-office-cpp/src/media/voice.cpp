// Voz (media/voice.py): texto->voz (Piper neural offline > voz del sistema > navegador) y voz->texto (Whisper local
// > navegador). Todo con degradacion: si no hay motor local, la interfaz usa la Web Speech API.
//
// Edicion C++: Piper y Whisper son los ejecutables piper y whisper-cli (whisper.cpp) incluidos en el paquete o en el
// PATH; la "voz del sistema" sustituye a pyttsx3 (SAPI en Windows, say en macOS, espeak-ng en Linux). El motor de
// dictado se anuncia como "faster-whisper" para que la interfaz web, compartida con la version Python, lo use igual.
#include <cmath>

#include "media/media.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::media {

static auto L = log::get("voice");

VoiceEngine::VoiceEngine(Config& config, models::Catalog& catalog, ProfileGetter profile, runtimes::RuntimeManager* rt)
    : config_(config), catalog_(catalog), profile_(std::move(profile)), rt_(rt) {}

static std::optional<fs::path> find_tool(const std::vector<std::string>& names, const std::string& folder) {
  std::string suffix = sys::is_windows() ? ".exe" : "";
  for (const fs::path& base : {runtimes::bundled_dir() / folder, paths::data_dir() / "runtimes" / folder})
    if (is_dir(base))
      for (auto& n : names)
        for (auto& p : glob(base, n + suffix, true))
          if (is_file(p)) return p;
  for (auto& n : names) {
    std::string w = proc::which(n);
    if (!w.empty()) return P(w);
  }
  return std::nullopt;
}

json VoiceEngine::piper_model() {
  std::string mid = config_.str("media.tts_model", "");
  json entry = mid.empty() ? json() : catalog_.get(mid);
  if (!entry.is_null()) return entry;
  for (auto& m : catalog_.list())
    if (jstr(m, "kind") == "tts") return m;
  return nullptr;
}

std::optional<fs::path> VoiceEngine::piper_exe() { return find_tool({"piper"}, "piper"); }
std::optional<fs::path> VoiceEngine::whisper_exe() { return find_tool({"whisper-cli", "whisper-cpp", "main"}, "whisper"); }

std::vector<std::string> VoiceEngine::system_tts_cmd() {
  if (sys::is_windows()) {
    std::string ps = proc::which("powershell");
    return ps.empty() ? std::vector<std::string>{} : std::vector<std::string>{ps};
  }
  if (sys::is_macos()) {
    std::string say = proc::which("say");
    return say.empty() ? std::vector<std::string>{} : std::vector<std::string>{say};
  }
  for (const char* n : {"espeak-ng", "espeak"}) {
    std::string e = proc::which(n);
    if (!e.empty()) return {e};
  }
  return {};
}

std::vector<std::string> VoiceEngine::tts_engines() {
  std::vector<std::string> out;
  if (piper_exe() && !piper_model().is_null()) out.push_back("piper");
  if (!system_tts_cmd().empty()) out.push_back("sistema");
  out.push_back("browser");
  return out;
}

std::vector<std::string> VoiceEngine::stt_engines() {
  std::vector<std::string> out;
  if (whisper_exe()) out.push_back("faster-whisper");
  out.push_back("browser");
  return out;
}

json VoiceEngine::info() {
  auto tts_l = tts_engines();
  auto stt_l = stt_engines();
  std::string pt = config_.str("media.tts_engine", "auto"), ps = config_.str("media.stt_engine", "auto");
  if (pt == "pyttsx3") pt = "sistema";   // configuraciones de la version Python
  auto pick = [](const std::vector<std::string>& l, const std::string& pref) {
    if (pref == "auto") return l[0];
    return std::find(l.begin(), l.end(), pref) != l.end() ? pref : std::string("browser");
  };
  json prof = profile_();
  return {{"tts", tts_l},
          {"stt", stt_l},
          {"tts_active", pick(tts_l, pt)},
          {"stt_active", pick(stt_l, ps)},
          {"stt_model", prof["stt"]["model"]}};
}

std::optional<std::pair<std::string, std::string>> VoiceEngine::tts(const std::string& raw, double rate) {
  std::string engine = jstr(info(), "tts_active");
  std::string text = uleft(trim(raw), 2000);
  if (text.empty() || engine == "browser") return std::nullopt;
  try {
    if (engine == "piper") return std::make_pair(tts_piper(text, rate), std::string("piper"));
    if (engine == "sistema") return std::make_pair(tts_system(text, rate), std::string("sistema"));
  } catch (const std::exception& e) {
    L.warning("TTS ", engine, " fallo: ", e.what());
  }
  return std::nullopt;
}

static fs::path temp_file(const std::string& suffix) { return paths::sub({"cache"}) / P("voz-" + token_hex(6) + suffix); }

std::string VoiceEngine::tts_piper(const std::string& text, double rate) {
  json entry = piper_model();
  auto exe = piper_exe();
  if (!exe || entry.is_null()) throw std::runtime_error("Piper no disponible");
  std::lock_guard<std::mutex> lk(mu_);
  fs::path out = temp_file(".wav");
  auto r = proc::run({S(*exe), "--model", jstr(entry, "path"), "--output_file", S(out), "--length_scale",
                      pyfloat(1.0 / std::max(0.5, rate))},
                     120, {.cwd = exe->parent_path(), .env = runtimes::lib_env(*exe), .capture = true, .input = text + "\n"});
  auto data = read_file(out);
  remove_file(out);
  if (!r.ok() || !data || data->empty()) throw std::runtime_error(uright(r.err.empty() ? r.out : r.err, 300));
  return *data;
}

std::string VoiceEngine::tts_system(const std::string& text, double rate) {
  auto cmd = system_tts_cmd();
  if (cmd.empty()) throw std::runtime_error("No hay voz del sistema");
  std::lock_guard<std::mutex> lk(mu_);
  fs::path out = temp_file(".wav");
  std::string lang = config_.str("user.language", "es"), wanted = config_.str("user.voice_name", "");
  proc::Result r;
  if (sys::is_windows()) {
    fs::path txt = temp_file(".txt");
    write_file(txt, text);
    int sapi_rate = std::clamp((int)std::lround((rate - 1.0) * 10), -10, 10);
    std::string sel = wanted.empty()
                          ? "foreach($v in $s.GetInstalledVoices()){if($v.VoiceInfo.Culture.Name -like '" + lang +
                                "*'){$s.SelectVoice($v.VoiceInfo.Name);break}};"
                          : "try{$s.SelectVoice('" + replace_all(wanted, "'", "''") + "')}catch{};";
    std::string script = "Add-Type -AssemblyName System.Speech;$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;" +
                         sel + "$s.Rate=" + std::to_string(sapi_rate) + ";$s.SetOutputToWaveFile('" +
                         replace_all(S(out), "'", "''") + "');$s.Speak([IO.File]::ReadAllText('" +
                         replace_all(S(txt), "'", "''") + "',[Text.Encoding]::UTF8));$s.Dispose()";
    r = proc::run({cmd[0], "-NoProfile", "-NonInteractive", "-Command", script}, 120, {.capture = true});
    remove_file(txt);
  } else if (sys::is_macos()) {
    std::vector<std::string> a = {cmd[0], "-r", std::to_string((int)(180 * rate)), "--data-format=LEI16@22050", "-o", S(out)};
    if (!wanted.empty()) a.insert(a.end(), {"-v", wanted});
    a.push_back(text);
    r = proc::run(a, 120, {.capture = true});
  } else {
    std::vector<std::string> a = {cmd[0], "-s", std::to_string((int)(170 * rate)), "-v", wanted.empty() ? lang : wanted,
                                  "-w", S(out), text};
    r = proc::run(a, 120, {.capture = true});
  }
  auto data = read_file(out);
  remove_file(out);
  if (!data || data->empty()) throw std::runtime_error(uright(r.err.empty() ? r.out : r.err, 300));
  return *data;
}

// WAV PCM de 16 bits (el que graba la interfaz) -> mono 16 kHz para whisper.cpp
static std::optional<std::string> to_wav16k(const std::string& wav) {
  if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0) return std::nullopt;
  auto u16 = [&](size_t o) { return (uint16_t)((uint8_t)wav[o] | ((uint8_t)wav[o + 1] << 8)); };
  auto u32 = [&](size_t o) { return (uint32_t)u16(o) | ((uint32_t)u16(o + 2) << 16); };
  size_t pos = 12;
  int channels = 0, rate = 0, bits = 0, fmt = 0;
  std::string_view data;
  while (pos + 8 <= wav.size()) {
    std::string id = wav.substr(pos, 4);
    uint32_t len = u32(pos + 4);
    size_t body = pos + 8;
    if (id == "fmt " && body + 16 <= wav.size()) {
      fmt = u16(body);
      channels = u16(body + 2);
      rate = (int)u32(body + 4);
      bits = u16(body + 14);
    } else if (id == "data") {
      data = std::string_view(wav).substr(body, std::min<size_t>(len, wav.size() - body));
      break;
    }
    pos = body + len + (len & 1);
  }
  if ((fmt != 1 && fmt != 3) || channels < 1 || rate < 1000 || data.empty()) return std::nullopt;
  size_t frame = (size_t)channels * (bits / 8);
  if (!frame) return std::nullopt;
  size_t frames = data.size() / frame;
  std::vector<float> mono(frames);
  for (size_t i = 0; i < frames; ++i) {
    double acc = 0;
    for (int c = 0; c < channels; ++c) {
      const char* p = data.data() + i * frame + c * (bits / 8);
      if (fmt == 3 && bits == 32) {
        float f;
        memcpy(&f, p, 4);
        acc += f;
      } else if (bits == 16) {
        acc += (int16_t)((uint8_t)p[0] | ((uint8_t)p[1] << 8)) / 32768.0;
      } else if (bits == 8) {
        acc += ((uint8_t)p[0] - 128) / 128.0;
      }
    }
    mono[i] = (float)(acc / channels);
  }
  size_t out_n = (size_t)((double)frames * 16000 / rate);
  std::vector<int16_t> pcm(out_n);
  for (size_t i = 0; i < out_n; ++i) {
    double src = (double)i * rate / 16000;
    size_t a = (size_t)src;
    double t = src - a;
    double v = a + 1 < frames ? mono[a] * (1 - t) + mono[a + 1] * t : (a < frames ? mono[a] : 0);
    pcm[i] = (int16_t)std::clamp(v * 32767, -32768.0, 32767.0);
  }
  auto le32 = [](uint32_t v) { return std::string{(char)(v & 255), (char)((v >> 8) & 255), (char)((v >> 16) & 255), (char)(v >> 24)}; };
  auto le16 = [](uint16_t v) { return std::string{(char)(v & 255), (char)(v >> 8)}; };
  uint32_t bytes = (uint32_t)pcm.size() * 2;
  std::string h = "RIFF" + le32(36 + bytes) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(16000) + le32(32000) +
                  le16(2) + le16(16) + "data" + le32(bytes);
  h.append((const char*)pcm.data(), bytes);
  return h;
}

json VoiceEngine::stt(const std::string& audio, const std::string& suffix) {
  auto exe = whisper_exe();
  if (!exe) return {{"error", "No hay motor de voz local; usa el reconocimiento del navegador"}, {"engine", "browser"}};
  json prof = profile_();
  std::string model = jstr(prof["stt"], "model");
  if (model.empty()) model = "base";
  fs::path mdir = paths::models_dir() / "whisper";
  fs::path mfile = mdir / P("ggml-" + model + ".bin");
  if (!path_exists(mfile)) {
    auto found = glob(mdir, "ggml-" + model + "*.bin");
    if (!found.empty()) {
      mfile = found[0];
    } else {
      L.info("Descargando el modelo de Whisper '", model, "'");
      try {
        mkdirs(mdir);
        http::download("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-" + model + ".bin", mfile);
      } catch (const std::exception& e) {
        return {{"error", "Falta el modelo de Whisper (" + S(mfile.filename()) + ") y no se pudo descargar: " + e.what()},
                {"engine", "faster-whisper"}};
      }
    }
  }
  auto wav = to_wav16k(audio);
  if (!wav) return {{"error", "Formato de audio no soportado (se espera WAV PCM)"}, {"engine", "faster-whisper"}};
  fs::path in = temp_file(suffix == ".wav" ? ".wav" : ".wav");
  write_file(in, *wav);
  std::string lang = config_.str("user.language", "es");
  int threads = (int)jnum(prof["llm"]["n_threads"], 4);
  proc::Result r;
  {
    std::lock_guard<std::mutex> lk(mu_);
    r = proc::run({S(*exe), "-m", S(mfile), "-f", S(in), "-l", lang.empty() ? "auto" : lang, "-t",
                   std::to_string(std::max(1, threads)), "-nt", "-np", "-bs", "1"},
                  300, {.cwd = exe->parent_path(), .env = runtimes::lib_env(*exe), .capture = true});
  }
  remove_file(in);
  if (!r.ok()) return {{"error", "whisper.cpp fallo: " + uright(r.err.empty() ? r.out : r.err, 300)}, {"engine", "faster-whisper"}};
  std::vector<std::string> parts;
  for (auto& line : splitlines(r.out))
    if (!trim(line).empty()) parts.push_back(trim(line));
  return {{"text", join(parts, " ")}, {"language", lang}, {"engine", "faster-whisper"}};
}

}  // namespace nx::media
