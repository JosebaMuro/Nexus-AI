// Compositor musical (media/music.py): teoria musical + sintesis, siempre disponible en CPU y sin modelos.
// Del prompt deduce el estado de animo -> tonalidad, modo, tempo, progresion, instrumentos y bateria.
// Exporta WAV estereo y MIDI.
#include <algorithm>
#include <cmath>
#include <map>

#include "media/media.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::media {

static auto L = log::get("music");
static constexpr int SR = 32000;
static const char* NOTE_NAMES[12] = {"Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"};
static const std::map<std::string, std::vector<int>> SCALES = {{"major", {0, 2, 4, 5, 7, 9, 11}},
                                                               {"minor", {0, 2, 3, 5, 7, 8, 10}},
                                                               {"dorian", {0, 2, 3, 5, 7, 9, 10}},
                                                               {"phrygian", {0, 1, 3, 5, 7, 8, 10}}};
struct Mood {
  std::string mode;
  int t0, t1;
  std::vector<int> prog;
  std::string lead, pad, drums;
  bool arp;
};
static const std::vector<std::pair<std::string, Mood>> MOODS = {
    {"feliz", {"major", 112, 128, {0, 4, 5, 3}, "pluck", "soft", "pop", false}},
    {"triste", {"minor", 64, 78, {0, 5, 2, 6}, "sine", "warm", "none", false}},
    {"epica", {"minor", 88, 100, {0, 5, 2, 6}, "brass", "strings", "epic", true}},
    {"calma", {"major", 70, 84, {0, 5, 1, 4}, "sine", "warm", "soft", false}},
    {"misterio", {"phrygian", 80, 92, {0, 1, 0, 6}, "sine", "dark", "soft", true}},
    {"electronica", {"minor", 120, 128, {0, 5, 3, 4}, "saw", "soft", "edm", true}},
    {"jazz", {"dorian", 96, 116, {1, 4, 0, 5}, "sine", "warm", "soft", false}},
};
static const std::vector<std::pair<std::string, std::string>> KEYWORDS = {
    {"feliz", "feliz alegre happy alegria fiesta verano sol positiva upbeat divertida pop"},
    {"triste", "triste sad melancolica melancolia lluvia nostalgia llanto lenta"},
    {"epica", "epica epic heroica batalla cine cinematografica orquestal aventura poderosa trailer"},
    {"calma", "calma relajante tranquila relax chill paz meditacion dormir lofi suave ambient"},
    {"misterio", "misterio misteriosa oscura terror suspense tensa dark horror noche"},
    {"electronica", "electronica cyberpunk futurista synthwave techno edm dance robot neon espacial"},
    {"jazz", "jazz swing blues bar cafe"},
};

std::string detect_mood(const std::string& prompt) {
  std::string p = strip_accents(lower(prompt));
  std::string best;
  int best_n = -1;
  for (auto& [m, kw] : KEYWORDS) {
    int n = 0;
    for (auto& w : split_ws(kw)) n += contains(p, w) ? 1 : 0;
    if (n > best_n) {   // max(): el primero con mas coincidencias
      best = m;
      best_n = n;
    }
  }
  return best_n > 0 ? best : "calma";
}

static double midi_freq(double n) { return 440.0 * std::pow(2.0, (n - 69) / 12); }

static std::vector<double> linspace(double a, double b, int n, bool endpoint = true) {
  std::vector<double> v(std::max(0, n));
  if (n <= 0) return v;
  double div = endpoint ? std::max(1, n - 1) : n;
  for (int i = 0; i < n; ++i) v[i] = a + (b - a) * i / div;
  if (endpoint && n > 1) v[n - 1] = b;
  return v;
}

static std::vector<double> adsr(int n, double a = 0.01, double d = 0.1, double s = 0.7, double r = 0.2) {
  int a_n = (int)(a * SR), d_n = (int)(d * SR), r_n = (int)(r * SR);
  int s_n = std::max(0, n - a_n - d_n - r_n);
  std::vector<double> env;
  for (double x : linspace(0, 1, a_n, false)) env.push_back(x);
  for (double x : linspace(1, s, d_n, false)) env.push_back(x);
  env.insert(env.end(), s_n, s);
  for (double x : linspace(s, 0, r_n)) env.push_back(x);
  env.resize(n, 0.0);
  return env;
}

using Rng = std::mt19937_64;
static double uniform(Rng& rng, double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
static double normal(Rng& rng) { return std::normal_distribution<double>(0, 1)(rng); }

static std::vector<double> osc(const std::string& kind, double freq, int n, Rng& rng) {
  std::vector<double> out(std::max(0, n));
  double rph[3] = {0, 0, 0};
  if (kind == "strings")
    for (double& p : rph) p = uniform(rng, 0, 6);
  for (int i = 0; i < n; ++i) {
    double t = (double)i / SR, ph = 2 * M_PI * freq * t, v = 0;
    if (kind == "saw") {
      for (int k = 1; k < 9; ++k) v += std::sin(k * ph) / k;
      v *= 0.5;
    } else if (kind == "square") {
      for (int k = 1; k < 10; k += 2) v += std::sin(k * ph) / k;
      v *= 0.6;
    } else if (kind == "brass") {
      v = (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.3 * std::sin(3 * ph) + 0.15 * std::sin(4 * ph)) * 0.5;
    } else if (kind == "strings") {
      const double det[3] = {-0.004, 0, 0.005};
      for (int k = 0; k < 3; ++k) v += std::sin(2 * M_PI * freq * (1 + det[k]) * t + rph[k]);
      v /= 3;
    } else if (kind == "pluck") {
      v = (std::sin(ph) + 0.4 * std::sin(2 * ph)) * std::exp(-t * 6);
    } else if (kind == "dark") {
      v = (std::sin(ph) + 0.3 * std::sin(1.5 * ph)) * 0.7;
    } else {
      v = std::sin(ph) + 0.15 * std::sin(2 * ph);
    }
    out[i] = v;
  }
  return out;
}

// np.convolve(x, kernel, mode="same")
static std::vector<double> convolve_same(const std::vector<double>& x, const std::vector<double>& k) {
  size_t n = x.size(), m = k.size();
  std::vector<double> out(n, 0.0);
  if (!n || !m) return out;
  size_t off = (m - 1) / 2;
  for (size_t i = 0; i < n; ++i) {
    size_t full = i + off;
    double acc = 0;
    for (size_t j = 0; j < m; ++j) {
      if (full < j) break;
      size_t xi = full - j;
      if (xi < n) acc += x[xi] * k[j];
    }
    out[i] = acc;
  }
  return out;
}

static std::vector<double> lowpass(const std::vector<double>& x, double alpha) {
  int k = std::max(1, (int)(1 / alpha));
  std::vector<double> kernel(k * 4);
  double sum = 0;
  for (int i = 0; i < k * 4; ++i) sum += kernel[i] = std::exp(-(double)i / k);
  for (double& v : kernel) v /= sum;
  return convolve_same(x, kernel);
}

void write_midi(const fs::path& path, const std::vector<std::tuple<double, double, int, int>>& notes, int tempo, int tpq) {
  struct Ev {
    long t;
    int status, note, vel;
  };
  std::vector<Ev> events;
  for (auto& [start, dur, note, ch] : notes) {
    events.push_back({(long)(start * tpq), 0x90 | ch, note, 90});
    events.push_back({(long)((start + dur) * tpq), 0x80 | ch, note, 0});
  }
  std::stable_sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) {
    bool ona = (a.status & 0xF0) == 0x90, onb = (b.status & 0xF0) == 0x90;
    return std::tie(a.t, ona) < std::tie(b.t, onb);
  });
  auto vlq = [](long v) {
    std::string out(1, (char)(v & 0x7F));
    while (v > 0x7F) {
      v >>= 7;
      out.insert(out.begin(), (char)((v & 0x7F) | 0x80));
    }
    return out;
  };
  long us = (long)(60000000.0 / tempo);
  std::string track = std::string("\x00\xff\x51\x03", 4);
  track += (char)((us >> 16) & 255);
  track += (char)((us >> 8) & 255);
  track += (char)(us & 255);
  long last = 0;
  for (auto& e : events) {
    track += vlq(e.t - last);
    track += (char)e.status;
    track += (char)(e.note & 0x7F);
    track += (char)e.vel;
    last = e.t;
  }
  track += std::string("\x00\xff\x2f\x00", 4);
  auto be32 = [](uint32_t v) {
    return std::string{(char)(v >> 24), (char)((v >> 16) & 255), (char)((v >> 8) & 255), (char)(v & 255)};
  };
  auto be16 = [](uint16_t v) { return std::string{(char)(v >> 8), (char)(v & 255)}; };
  std::string data = "MThd" + be32(6) + be16(0) + be16(1) + be16((uint16_t)tpq) + "MTrk" + be32((uint32_t)track.size()) + track;
  write_file(path, data);
}

static std::string wav_bytes(const std::vector<int16_t>& samples, int channels, int rate) {
  auto le32 = [](uint32_t v) { return std::string{(char)(v & 255), (char)((v >> 8) & 255), (char)((v >> 16) & 255), (char)(v >> 24)}; };
  auto le16 = [](uint16_t v) { return std::string{(char)(v & 255), (char)(v >> 8)}; };
  uint32_t bytes = (uint32_t)samples.size() * 2;
  std::string h = "RIFF" + le32(36 + bytes) + "WAVEfmt " + le32(16) + le16(1) + le16((uint16_t)channels) + le32(rate) +
                  le32(rate * channels * 2) + le16((uint16_t)(channels * 2)) + le16(16) + "data" + le32(bytes);
  h.append((const char*)samples.data(), bytes);   // little endian en todas las plataformas soportadas
  return h;
}

json Composer::compose(const std::string& prompt, std::optional<double> seconds, std::optional<long long> seed_opt) {
  long long seed = seed_opt ? *seed_opt : (long long)(crc32(prompt) ^ ((uint32_t)(long long)now() & 0xFFFF));
  Rng rng((uint64_t)seed);
  std::string mood = detect_mood(prompt);
  const Mood* mp = nullptr;
  for (auto& [k, v] : MOODS)
    if (k == mood) mp = &v;
  const Mood& m = *mp;
  int tempo = std::uniform_int_distribution<int>(m.t0, m.t1 - 1)(rng);
  static const int ROOTS[6] = {57, 60, 62, 64, 65, 67};   // La3..Sol4
  int root = ROOTS[std::uniform_int_distribution<int>(0, 5)(rng)];
  const auto& scale = SCALES.at(m.mode);
  double beat = 60.0 / tempo;
  int bars = !seconds || !*seconds ? bars_ : std::max(4, (int)(*seconds / (beat * 4)));
  size_t total = (size_t)(bars * 4 * beat * SR) + SR * 2;
  std::vector<double> left(total, 0.0), right(total, 0.0);
  std::vector<std::tuple<double, double, int, int>> notes_midi;   // (inicio_beats, duracion_beats, nota, canal)

  auto deg = [&](int d, int octave = 0) {
    int q = d >= 0 ? d / 7 : (d - 6) / 7;   // division entera de Python
    int r = d - q * 7;
    return root + scale[r] + 12 * (q + octave);
  };
  auto put = [&](const std::vector<double>& sig, double start_beats, double pan = 0.0, double gain = 1.0) {
    size_t i = (size_t)(start_beats * beat * SR);
    if (i >= total) return;
    size_t j = std::min(total, i + sig.size());
    double gl = gain * (1 - std::max(0.0, pan)), gr = gain * (1 + std::min(0.0, pan));
    for (size_t k = i; k < j; ++k) {
      left[k] += sig[k - i] * gl;
      right[k] += sig[k - i] * gr;
    }
  };
  auto mul = [](std::vector<double> a, const std::vector<double>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] *= b[i];
    return a;
  };
  auto choice = [&](const std::vector<double>& v) { return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)]; };

  int prev = 7;
  for (int bar = 0; bar < bars; ++bar) {
    int chord_deg = m.prog[bar % m.prog.size()];
    int chord[3] = {deg(chord_deg), deg(chord_deg + 2), deg(chord_deg + 4)};
    double b0 = bar * 4;
    // pad / acordes
    int n = (int)(4 * beat * SR);
    for (int note : chord) {
      auto sig = mul(osc(m.pad == "strings" ? "strings" : m.pad == "dark" ? "dark" : "sine", midi_freq(note - 12), n, rng),
                     adsr(n, 0.3, 0.5, 0.6, 0.6));
      put(sig, b0, uniform(rng, -0.4, 0.4), 0.10);
      notes_midi.emplace_back(b0, 4, note - 12, 1);
    }
    // bajo
    int stepq = tempo < 90 ? 2 : 1;
    for (int q = 0; q < 4; q += stepq) {
      int nb = (int)(beat * SR * (tempo < 90 ? 2 : 1) * 0.9);
      put(mul(osc("square", midi_freq(chord[0] - 24), nb, rng), adsr(nb, 0.005, 0.1, 0.5, 0.08)), b0 + q, 0, 0.16);
      notes_midi.emplace_back(b0 + q, 0.9, chord[0] - 24, 2);
    }
    // arpegio
    if (m.arp)
      for (int s = 0; s < 8; ++s) {
        int note = chord[s % 3] + (s >= 4 ? 12 : 0);
        int na = (int)(beat * SR * 0.45);
        put(mul(osc("pluck", midi_freq(note), na, rng), adsr(na, 0.002, 0.05, 0.3, 0.1)), b0 + s * 0.5, s % 2 ? 0.5 : -0.5, 0.08);
      }
    // melodia: paseo aleatorio por la escala, tonos del acorde en tiempos fuertes
    if (bar % 8 < 6 || bars < 8) {
      double t_beat = 0;
      while (t_beat < 4) {
        double dur = tempo < 100 ? choice({0.5, 1, 1, 1.5, 2}) : choice({0.5, 0.5, 1, 1});
        if (std::uniform_real_distribution<double>(0, 1)(rng) < 0.15) {
          t_beat += dur;
          continue;
        }
        int target;
        if (t_beat == 0 || t_beat == 2)
          target = chord_deg + (int)choice({0, 2, 4}) + 7;
        else
          target = prev + (int)choice({-2, -1, 1, 2, 0});
        prev = std::clamp(target, 4, 13);
        int note = deg(prev);
        int nm = (int)(dur * beat * SR);
        auto sig = mul(osc(m.lead, midi_freq(note), nm, rng), adsr(nm, 0.01, 0.12, 0.6, std::min(0.25, dur * beat * 0.4)));
        put(sig, b0 + t_beat, 0.1, 0.22);
        notes_midi.emplace_back(b0 + t_beat, dur, note, 0);
        t_beat += dur;
      }
    }
    // bateria
    if (m.drums != "none") {
      const std::string& style = m.drums;
      auto kick = [&] {
        int nk = (int)(0.35 * SR);
        std::vector<double> v(nk);
        for (int i = 0; i < nk; ++i) {
          double t = (double)i / SR;
          v[i] = std::sin(2 * M_PI * (50 + 120 * std::exp(-t * 30)) * t) * std::exp(-t * 9);
        }
        return v;
      };
      auto snare = [&] {
        int ns = (int)(0.2 * SR);
        std::vector<double> v(ns);
        for (int i = 0; i < ns; ++i) {
          double t = (double)i / SR;
          v[i] = (normal(rng) * 0.6 + std::sin(2 * M_PI * 190 * t) * 0.4) * std::exp(-t * 18);
        }
        return v;
      };
      auto hat = [&] {
        int nh = (int)(0.05 * SR);
        std::vector<double> x(nh);
        for (double& v : x) v = normal(rng);
        auto sm = convolve_same(x, {0.25, 0.25, 0.25, 0.25});
        for (int i = 0; i < nh; ++i) x[i] = (x[i] - sm[i]) * std::exp(-(double)i / SR * 80);
        return x;
      };
      double g = style == "soft" ? 0.35 : style == "pop" ? 0.6 : style == "edm" ? 0.75 : style == "epic" ? 0.8 : 0.5;
      for (int q = 0; q < 4; ++q) {
        if (style == "edm" || q == 0 || q == 2 || (style == "epic" && q == 3)) put(kick(), b0 + q, 0, g);
        if ((q == 1 || q == 3) && style != "soft") put(snare(), b0 + q, 0, g * 0.5);
        if (style == "pop" || style == "edm" || style == "soft") put(hat(), b0 + q + 0.5, 0.3, g * 0.25);
      }
      if (style == "epic" && bar % 4 == 3)
        for (int i = 0; i < 4; ++i) put(snare(), b0 + 3 + i * 0.25, 0, g * 0.35);
    }
  }
  auto lp_l = lowpass(left, 0.5), lp_r = lowpass(right, 0.5);
  for (size_t i = 0; i < total; ++i) {
    left[i] = lp_l[i] * 0.5 + left[i] * 0.5;
    right[i] = lp_r[i] * 0.5 + right[i] * 0.5;
  }
  size_t fade = (size_t)(1.5 * SR);
  auto ramp = linspace(1, 0, (int)fade);
  for (size_t i = 0; i < fade && i < total; ++i) {
    left[total - fade + i] *= ramp[i];
    right[total - fade + i] *= ramp[i];
  }
  double peak = 0;
  for (size_t i = 0; i < total; ++i) peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
  double k = 0.9 / std::max(1e-6, peak);
  std::vector<int16_t> pcm(total * 2);
  for (size_t i = 0; i < total; ++i) {
    pcm[i * 2] = (int16_t)(left[i] * k * 32767);
    pcm[i * 2 + 1] = (int16_t)(right[i] * k * 32767);
  }
  std::string name = std::to_string((long long)now()) + "_" + slug_name(prompt, 30, "tema");
  fs::path out_dir = paths::sub({"outputs", "music"});
  fs::path wav_path = out_dir / P(name + ".wav"), midi_path = out_dir / P(name + ".mid");
  write_file(wav_path, wav_bytes(pcm, 2, SR));
  write_midi(midi_path, notes_midi, tempo);
  std::string key = std::string(NOTE_NAMES[root % 12]) + " " +
                    (m.mode == "major" ? "mayor" : m.mode == "minor" ? "menor" : m.mode);
  return {{"engine", "procedural"},
          {"path", S(wav_path)},
          {"url", "/outputs/music/" + S(wav_path.filename())},
          {"midi_url", "/outputs/music/" + S(midi_path.filename())},
          {"mood", mood},
          {"tempo", tempo},
          {"key", key},
          {"duration", round_to((double)total / SR, 1)},
          {"bars", bars},
          {"seed", seed}};
}

MusicEngine::MusicEngine(Config& config, ProfileGetter profile) : config_(config), profile_(std::move(profile)) {}

// MusicGen (transformers + torch) no forma parte de la edicion C++: solo el compositor procedural
std::vector<std::string> MusicEngine::engines() { return {"procedural"}; }

json MusicEngine::generate(const std::string& prompt, double seconds, std::string engine) {
  if (engine.empty()) engine = config_.str("media.music_engine", "auto");
  if (engine == "musicgen") L.warning("MusicGen no esta disponible en la edicion C++; usando compositor procedural");
  return composer.compose(prompt, seconds);
}

}  // namespace nx::media
