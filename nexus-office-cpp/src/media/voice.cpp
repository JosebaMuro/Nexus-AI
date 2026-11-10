// Voz (media/voice.py): texto->voz (Piper neural offline > voz del sistema > navegador) y voz->texto (Whisper local
// > navegador). Todo con degradacion: si no hay motor local, la interfaz usa la Web Speech API.
//
// Edicion C++: Piper y Whisper son los ejecutables piper y whisper-cli (whisper.cpp) incluidos en el paquete o en el
// PATH; la "voz del sistema" sustituye a pyttsx3 (SAPI en Windows, say en macOS, espeak-ng en Linux). El motor de
// dictado se anuncia como "faster-whisper" para que la interfaz web, compartida con la version Python, lo use igual.
#include <algorithm>
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
// ---------------------------------------------------------------- voces de los agentes
// numero de 0 a 1 estable para cada agente (FNV-1a de su id)
static double semilla_voz(const std::string& id) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : id) h = (h ^ c) * 1099511628211ULL;
  return (double)(h % 10007) / 10006.0;
}

static double num_voz(const json& v, const char* k, double def, double lo, double hi) {
  if (!v.is_object() || !v.contains(k) || !v[k].is_number()) return def;
  return std::clamp(v[k].get<double>(), lo, hi);
}

json VoiceEngine::voz_de(const std::string& agente, const json& voz_agente, int indice) {
  json general = config_.get("user.voz", json::object());
  json v = general.is_object() ? general : json::object();
  bool propia = voz_agente.is_object() && !voz_agente.empty();
  if (propia)
    for (auto& [k, x] : voz_agente.items())
      if (!x.is_null() && !(x.is_string() && x.get<std::string>().empty())) v[k] = x;
  if (!propia && !agente.empty() && agente != "nexus" && config_.flag("user.voz_distinta", true)) {
    // sin voz propia: un tono distinto por agente alrededor del general y, si el modelo Piper tiene varios hablantes,
    // uno de ellos; con espeak, una variante
    // tonos bien separados (alternando grave y agudo) y, con indice, consecutivos para que no se repitan
    static const double tonos[] = {0.86, 1.14, 0.94, 1.06, 0.80, 1.22, 0.90, 1.10, 0.98, 1.18};
    static const char* variantes[] = {"m3", "f2", "m1", "f4", "m7", "f3", "m2", "f1", "m4", "f5"};
    int i = indice >= 0 ? indice : (int)(semilla_voz(agente) * 1000);
    double base = num_voz(v, "tono", 1.0, 0.5, 2.0);
    v["tono"] = std::round(base * tonos[i % 10] * 100) / 100;
    v["auto"] = true;
    json m = jstr(v, "modelo").empty() ? piper_model() : catalog_.get(jstr(v, "modelo"));
    json hs = m.is_null() ? json::array() : piper_hablantes(m);
    if (hs.size() > 1 && !v.contains("hablante")) v["hablante"] = hs[i % (int)hs.size()]["id"];
    bool espeak = !sys::is_windows() && !sys::is_macos() && !system_tts_cmd().empty();
    if (espeak && jstr(v, "nombre").find('+') == std::string::npos) v["variante"] = variantes[i % 10];
  }
  return v;
}

json VoiceEngine::piper_hablantes(const json& modelo) {
  json out = json::array();
  std::string path = jstr(modelo, "path");
  if (path.empty()) return out;
  json cfg = jparse(read_file(P(path + ".json")).value_or(""));
  if (cfg.is_discarded() || !cfg.is_object()) {
    cfg = json();
    fs::path pp = P(path);
    if (is_dir(pp))
      for (auto& f : glob(pp, "*.onnx.json")) {
        cfg = jparse(read_file(f).value_or(""));
        break;
      }
  }
  if (cfg.is_discarded() || !cfg.is_object()) return out;
  json mapa = cfg.value("speaker_id_map", json::object());
  if (mapa.is_object() && !mapa.empty()) {
    std::vector<std::pair<int, std::string>> l;
    for (auto& [n, id] : mapa.items())
      if (id.is_number()) l.push_back({id.get<int>(), n});
    std::sort(l.begin(), l.end());
    if (l.size() > 1)
      for (auto& [id, n] : l) out.push_back({{"id", id}, {"nombre", n}});
  } else {
    int n = cfg.value("num_speakers", 1);
    for (int i = 0; n > 1 && i < n; ++i) out.push_back({{"id", i}, {"nombre", "Hablante " + std::to_string(i + 1)}});
  }
  return out;
}

json VoiceEngine::voces() {
  json piper = json::array();
  for (auto& m : catalog_.list())
    if (jstr(m, "kind") == "tts")
      piper.push_back({{"id", jstr(m, "id")}, {"nombre", jstr(m, "name", jstr(m, "id"))}, {"hablantes", piper_hablantes(m)}});
  json sistema = json::array(), variantes = json::array();
  auto cmd = system_tts_cmd();
  try {
    if (!cmd.empty() && sys::is_windows()) {
      auto r = proc::run({cmd[0], "-NoProfile", "-NonInteractive", "-Command",
                          "Add-Type -AssemblyName System.Speech;(New-Object System.Speech.Synthesis.SpeechSynthesizer)."
                          "GetInstalledVoices()|%{$_.VoiceInfo.Name+'|'+$_.VoiceInfo.Culture.Name}"},
                         30, {.capture = true});
      for (auto& l : splitlines(r.out)) {
        auto pos = l.find('|');
        if (pos != std::string::npos) sistema.push_back({{"nombre", trim(l.substr(0, pos))}, {"idioma", trim(l.substr(pos + 1))}});
      }
    } else if (!cmd.empty() && sys::is_macos()) {
      auto r = proc::run({cmd[0], "-v", "?"}, 30, {.capture = true});
      for (auto& l : splitlines(r.out)) {
        auto pos = l.find("  ");
        if (pos == std::string::npos) continue;
        std::string resto = trim(l.substr(pos));
        sistema.push_back({{"nombre", trim(l.substr(0, pos))}, {"idioma", resto.substr(0, resto.find(' '))}});
      }
    } else if (!cmd.empty()) {
      // espeak-ng --voices: Pty Language Age/Gender VoiceName File Other
      auto r = proc::run({cmd[0], "--voices"}, 30, {.capture = true});
      bool cab = true;
      for (auto& l : splitlines(r.out)) {
        if (cab) {
          cab = false;
          continue;
        }
        std::vector<std::string> c;
        for (auto& x : split(l, " "))
          if (!trim(x).empty()) c.push_back(trim(x));
        if (c.size() >= 4) sistema.push_back({{"nombre", c[1]}, {"idioma", c[1]}, {"descripcion", c[3]}});
      }
      for (const char* x : {"m1", "m2", "m3", "m4", "m5", "m6", "m7", "f1", "f2", "f3", "f4", "f5", "croak", "whisper"})
        variantes.push_back(x);
    }
  } catch (const std::exception& e) {
    L.warning("No se pudieron listar las voces del sistema: ", e.what());
  }
  return {{"piper", piper}, {"sistema", sistema}, {"variantes", variantes}, {"motores", tts_engines()}};
}

// tono y volumen sobre el WAV de 16 bits: el tono cambia la frecuencia de muestreo de la cabecera (sale mas agudo y
// mas rapido; quien llama ya lo sintetizo mas lento en la misma proporcion) y el volumen escala las muestras
void tono_volumen(std::string& wav, double tono, double volumen) {
  if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0) return;
  auto u16 = [&](size_t o) { return (uint16_t)((uint8_t)wav[o] | ((uint8_t)wav[o + 1] << 8)); };
  auto u32 = [&](size_t o) { return (uint32_t)u16(o) | ((uint32_t)u16(o + 2) << 16); };
  auto put32 = [&](size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) wav[o + i] = (char)((v >> (8 * i)) & 0xff);
  };
  size_t pos = 12;
  int bits = 0;
  while (pos + 8 <= wav.size()) {
    std::string id = wav.substr(pos, 4);
    uint32_t len = u32(pos + 4);
    size_t body = pos + 8;
    if (id == "fmt " && body + 16 <= wav.size()) {
      bits = u16(body + 14);
      if (std::abs(tono - 1) > 0.005) {
        put32(body + 4, (uint32_t)std::lround(u32(body + 4) * tono));   // muestras por segundo
        put32(body + 8, (uint32_t)std::lround(u32(body + 8) * tono));   // bytes por segundo
      }
    } else if (id == "data" && bits == 16 && std::abs(volumen - 1) > 0.005) {
      size_t fin = std::min<size_t>(body + (size_t)len, wav.size());
      for (size_t o = body; o + 1 < fin; o += 2) {
        int s = (int16_t)u16(o);
        s = std::clamp((int)std::lround(s * volumen), -32768, 32767);
        wav[o] = (char)(s & 0xff);
        wav[o + 1] = (char)((s >> 8) & 0xff);
      }
    }
    if (len > wav.size()) break;
    pos = body + len + (len & 1);
  }
}

std::optional<std::pair<std::string, std::string>> VoiceEngine::tts(const std::string& raw, double rate, const json& voz_in) {
  json voz = voz_in.is_object() ? voz_in : json::object();
  std::string engine = jstr(info(), "tts_active");
  std::string pedido = jstr(voz, "motor");
  if (!pedido.empty() && pedido != "auto") {
    auto l = tts_engines();
    if (std::find(l.begin(), l.end(), pedido) != l.end()) engine = pedido;
  }
  std::string text = uleft(trim(raw), 2000);
  if (text.empty() || engine == "browser") return std::nullopt;
  double tono = num_voz(voz, "tono", 1.0, 0.5, 2.0), volumen = num_voz(voz, "volumen", 1.0, 0.0, 2.0);
  // se sintetiza a velocidad/tono para que, al subir el tono en la cabecera, la duracion quede en la pedida
  double vel = std::clamp(rate * num_voz(voz, "velocidad", 1.0, 0.3, 3.0) / tono, 0.25, 4.0);
  try {
    std::string wav;
    if (engine == "piper") wav = tts_piper(text, vel, voz);
    else if (engine == "sistema") wav = tts_system(text, vel, voz);
    else return std::nullopt;
    tono_volumen(wav, tono, volumen);
    return std::make_pair(std::move(wav), engine);
  } catch (const std::exception& e) {
    L.warning("TTS ", engine, " fallo: ", e.what());
  }
  return std::nullopt;
}

static fs::path temp_file(const std::string& suffix) { return paths::sub({"cache"}) / P("voz-" + token_hex(6) + suffix); }

std::string VoiceEngine::tts_piper(const std::string& text, double rate, const json& voz) {
  json entry = jstr(voz, "modelo").empty() ? json() : catalog_.get(jstr(voz, "modelo"));
  if (entry.is_null() || jstr(entry, "kind") != "tts") entry = piper_model();
  auto exe = piper_exe();
  if (!exe || entry.is_null()) throw std::runtime_error("Piper no disponible");
  std::lock_guard<std::mutex> lk(mu_);
  fs::path out = temp_file(".wav");
  std::vector<std::string> a = {S(*exe), "--model", jstr(entry, "path"), "--output_file", S(out), "--length_scale",
                                pyfloat(1.0 / std::max(0.25, rate))};
  if (voz.contains("hablante") && voz["hablante"].is_number())
    a.insert(a.end(), {"--speaker", std::to_string(voz["hablante"].get<int>())});
  auto r = proc::run(a, 120, {.cwd = exe->parent_path(), .env = runtimes::lib_env(*exe), .capture = true, .input = text + "\n"});
  auto data = read_file(out);
  remove_file(out);
  if (!r.ok() || !data || data->empty()) throw std::runtime_error(uright(r.err.empty() ? r.out : r.err, 300));
  return *data;
}

std::string VoiceEngine::tts_system(const std::string& text, double rate, const json& voz) {
  auto cmd = system_tts_cmd();
  if (cmd.empty()) throw std::runtime_error("No hay voz del sistema");
  std::lock_guard<std::mutex> lk(mu_);
  fs::path out = temp_file(".wav");
  std::string lang = config_.str("user.language", "es"), wanted = jstr(voz, "nombre");
  if (wanted.empty()) wanted = config_.str("user.voice_name", "");
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
    std::string v = wanted.empty() ? lang : wanted;
    if (!jstr(voz, "variante").empty() && v.find('+') == std::string::npos) v += "+" + jstr(voz, "variante");
    std::vector<std::string> a = {cmd[0], "-s", std::to_string((int)(170 * rate)), "-v", v, "-w", S(out), text};
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
