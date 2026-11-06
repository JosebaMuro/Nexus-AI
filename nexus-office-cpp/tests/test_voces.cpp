// Voces: la general, la de cada agente (propia o distinta segun su id), tono y volumen sobre el WAV.
#include <cstring>

#include "agents/agents.h"
#include "agents/spec.h"
#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "media/media.h"
#include "util/fs.h"

using namespace nx;

static std::string wav_prueba(uint32_t rate, int16_t muestra, int n) {
  std::string w = "RIFF....WAVEfmt ";
  auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w += (char)((v >> (8 * i)) & 0xff); };
  auto p16 = [&](uint16_t v) { w += (char)(v & 0xff); w += (char)(v >> 8); };
  p32(16); p16(1); p16(1); p32(rate); p32(rate * 2); p16(2); p16(16);
  w += "data"; p32((uint32_t)n * 2);
  for (int i = 0; i < n; ++i) p16((uint16_t)muestra);
  return w;
}
static uint32_t rd32(const std::string& w, size_t o) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)w[o + i] << (8 * i);
  return v;
}

TEST("voces: tono y volumen sobre el WAV") {
  std::string w = wav_prueba(22050, 1000, 10);
  media::tono_volumen(w, 1.2, 0.5);
  CHECK_EQ(rd32(w, 24), (uint32_t)26460);   // muestras por segundo x1,2
  CHECK_EQ(rd32(w, 28), (uint32_t)52920);
  CHECK_EQ((int16_t)((uint8_t)w[44] | ((uint8_t)w[45] << 8)), (int16_t)500);
  std::string sat = wav_prueba(16000, 30000, 2);
  media::tono_volumen(sat, 1.0, 2.0);
  CHECK_EQ((int16_t)((uint8_t)sat[44] | ((uint8_t)sat[45] << 8)), (int16_t)32767);
  std::string roto = "no es un wav";
  media::tono_volumen(roto, 1.5, 1.5);
  CHECK_EQ(roto, std::string("no es un wav"));
}

TEST("voces: la general, la propia de un agente y una distinta para cada uno") {
  auto dir = std::filesystem::temp_directory_path() / ("nexus-voces-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  Services s(false);
  s.config.set("user.voz", json{{"velocidad", 1.1}, {"nombre", "es"}});
  // sin voz propia: la general con un tono propio del agente, siempre el mismo
  json a = s.voice->voz_de("contable", json::object());
  json b = s.voice->voz_de("juridico", json::object());
  CHECK_EQ(a["velocidad"].get<double>(), 1.1);
  CHECK(a["auto"].get<bool>());
  CHECK(a["tono"].get<double>() >= 0.85 && a["tono"].get<double>() <= 1.35);
  // con su puesto, dos agentes seguidos nunca comparten tono
  CHECK(s.voice->voz_de("a", json::object(), 0)["tono"] != s.voice->voz_de("b", json::object(), 1)["tono"]);
  CHECK_EQ(jdump(a), jdump(s.voice->voz_de("contable", json::object())));
  CHECK(jdump(a) != jdump(b));
  // el asistente principal habla con la general tal cual
  CHECK(!s.voice->voz_de("nexus", json::object()).contains("tono"));
  // voz propia: manda sobre la general y no se toca
  json p = s.voice->voz_de("contable", json{{"tono", 0.7}, {"motor", "sistema"}});
  CHECK_EQ(p["tono"].get<double>(), 0.7);
  CHECK_EQ(jstr(p, "motor"), std::string("sistema"));
  CHECK_EQ(jstr(p, "nombre"), std::string("es"));
  CHECK(!p.contains("auto"));
  // sin voces distintas: todos con la general
  s.config.set("user.voz_distinta", false);
  CHECK(!s.voice->voz_de("contable", json::object()).contains("tono"));
  CHECK(s.voice->voces().contains("motores"));

  // la voz se guarda con el agente
  auto spec = agents::AgentSpec::from_json({{"id", "x"}, {"name", "X"}, {"voz", {{"tono", 1.3}}}});
  CHECK_EQ(spec.to_json()["voz"]["tono"].get<double>(), 1.3);
  CHECK(!agents::AgentSpec::from_json({{"id", "y"}, {"name", "Y"}}).to_json().contains("voz"));
  s.shutdown();
}
