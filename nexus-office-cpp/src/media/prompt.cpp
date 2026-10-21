// Mejorador de prompts para imagen, musica y chat (prompt_enhancer.py).
// Con LLM reescribe el prompt con una plantilla por destino; sin el, reglas (diccionario ES->EN, estilos, calidad).
#include <map>

#include "llm/llm.h"
#include "media/media.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::prompt {

const std::vector<std::pair<std::string, std::string>> STYLES = {
    {"", ""},
    {"cinematografico", "cinematic still, dramatic lighting, shallow depth of field, film grain, anamorphic"},
    {"fotorrealista", "photorealistic, ultra detailed, natural lighting, 85mm lens, high dynamic range"},
    {"anime", "anime style, clean line art, vibrant colors, cel shading, studio quality"},
    {"oleo", "oil painting, thick brush strokes, rich textures, classical composition"},
    {"acuarela", "watercolor painting, soft washes, paper texture, delicate colors"},
    {"cyberpunk", "cyberpunk, neon lights, rain, reflections, futuristic city, high contrast"},
    {"fantasia", "epic fantasy art, magical atmosphere, volumetric light, intricate details"},
    {"3d", "3d render, octane render, global illumination, subsurface scattering, physically based"},
    {"pixel", "pixel art, 16-bit, limited palette, crisp pixels"},
    {"gotico", "dark gothic, moody, chiaroscuro, ornate details, eclipse light"},
};
const char* QUALITY = "highly detailed, sharp focus, masterpiece, best quality";
const char* NEGATIVE =
    "blurry, low quality, lowres, deformed, bad anatomy, extra fingers, watermark, text, signature, jpeg artifacts";

static const std::map<std::string, std::string> ES_EN = {
    {"un", "a"}, {"una", "a"}, {"el", "the"}, {"la", "the"}, {"los", "the"}, {"las", "the"}, {"de", "of"},
    {"del", "of the"}, {"con", "with"}, {"y", "and"}, {"en", "in"}, {"sobre", "on"}, {"bajo", "under"},
    {"entre", "between"}, {"al", "at the"}, {"noche", "night"}, {"dia", "day"}, {"ciudad", "city"},
    {"bosque", "forest"}, {"mar", "sea"}, {"playa", "beach"}, {"montana", "mountain"}, {"montaña", "mountain"},
    {"cielo", "sky"}, {"lluvia", "rain"}, {"nieve", "snow"}, {"niebla", "fog"}, {"sol", "sun"}, {"luna", "moon"},
    {"estrellas", "stars"}, {"estrella", "star"}, {"eclipse", "eclipse"}, {"rio", "river"}, {"lago", "lake"},
    {"castillo", "castle"}, {"templo", "temple"}, {"pagoda", "pagoda"}, {"casa", "house"}, {"calle", "street"},
    {"jardin", "garden"}, {"flores", "flowers"}, {"flor", "flower"}, {"arbol", "tree"}, {"arboles", "trees"},
    {"mujer", "woman"}, {"hombre", "man"}, {"nina", "girl"}, {"niño", "boy"}, {"nino", "boy"}, {"niña", "girl"},
    {"gato", "cat"}, {"perro", "dog"}, {"dragon", "dragon"}, {"caballo", "horse"}, {"pajaro", "bird"},
    {"robot", "robot"}, {"astronauta", "astronaut"}, {"guerrero", "warrior"}, {"samurai", "samurai"},
    {"geisha", "geisha"}, {"retrato", "portrait"}, {"paisaje", "landscape"}, {"cristal", "crystal"},
    {"fuego", "fire"}, {"agua", "water"}, {"hielo", "ice"}, {"oro", "gold"}, {"plata", "silver"}, {"rojo", "red"},
    {"roja", "red"}, {"azul", "blue"}, {"verde", "green"}, {"negro", "black"}, {"negra", "black"},
    {"blanco", "white"}, {"blanca", "white"}, {"amarillo", "yellow"}, {"morado", "purple"}, {"violeta", "violet"},
    {"rosa", "pink"}, {"naranja", "orange"}, {"gris", "gray"}, {"oscuro", "dark"}, {"oscura", "dark"},
    {"brillante", "glowing"}, {"brillantes", "glowing"}, {"antiguo", "ancient"}, {"antigua", "ancient"},
    {"futurista", "futuristic"}, {"magico", "magical"}, {"mágico", "magical"}, {"grande", "huge"},
    {"pequeño", "small"}, {"pequeno", "small"}, {"hermoso", "beautiful"}, {"hermosa", "beautiful"},
    {"neon", "neon"}, {"neón", "neon"}, {"tormenta", "storm"}, {"atardecer", "sunset"}, {"amanecer", "sunrise"},
    {"volando", "flying"}, {"nave", "spaceship"}, {"espacio", "outer space"}, {"planeta", "planet"},
    {"galaxia", "galaxy"}, {"nebulosa", "nebula"}, {"kimono", "kimono"}, {"tatuajes", "tattoos"},
    {"tatuaje", "tattoo"}, {"maquillaje", "makeup"}, {"cara", "face"}, {"ojos", "eyes"},
};
static const std::map<std::string, std::string> MUSIC_HINTS = {
    {"epica", "orchestral, epic taiko drums, brass, choir, 90 bpm, minor key, cinematic build-up"},
    {"triste", "solo piano, soft strings, 70 bpm, minor key, melancholic, reverb"},
    {"feliz", "upbeat pop, acoustic guitar, claps, 120 bpm, major key, bright"},
    {"calma", "ambient pads, soft piano, 75 bpm, major key, lo-fi, relaxing"},
    {"misterio", "dark ambient, low drones, sparse percussion, 85 bpm, phrygian mode, suspense"},
    {"electronica", "synthwave, analog synths, arpeggios, 124 bpm, four-on-the-floor, neon"},
    {"jazz", "jazz trio, upright bass, brushed drums, 110 bpm, dorian, swing"},
};
static const std::map<std::string, std::string> TEMPLATES = {
    {"image",
     "Eres un experto en prompts para generadores de imagen (Stable Diffusion). Reescribe la idea del usuario como UN "
     "prompt en ingles, conciso y visual: sujeto, accion, entorno, estilo, iluminacion, color, composicion y calidad, "
     "separados por comas. Despues una linea 'NEGATIVE:' con lo que hay que evitar. Formato exacto:\nPROMPT: ...\n"
     "NEGATIVE: ..."},
    {"music",
     "Eres productor musical. Convierte la idea en una descripcion para un generador de musica: genero, estado de "
     "animo, instrumentos, tempo (BPM), tonalidad y estructura. Una sola linea en espanol empezando por 'PROMPT:'."},
    {"chat",
     "Mejora la peticion del usuario para un asistente de IA sin cambiar su intencion: aclara el objetivo, anade "
     "contexto util, el formato de respuesta deseado y restricciones. Responde solo con 'PROMPT:' seguido del prompt "
     "mejorado, en el mismo idioma del usuario."},
};

std::string style(const std::string& key) {
  for (auto& [k, v] : STYLES)
    if (k == key) return v;
  return key;
}

static bool has_style(const std::string& key) {
  for (auto& [k, v] : STYLES)
    if (k == key) return !v.empty();
  return false;
}

bool looks_spanish(const std::string& text) {
  static const Rx R(R"(\b(de|una|un|con|el|la|los|las|del|que|y|en|sobre|muy)\b|[áéíóúñ¿¡])", Rx::I);
  return R.test(text);
}

std::string translate_rules(const std::string& text) {
  static const Rx WORDS(R"(\w+|[^\w\s])");
  std::vector<std::string> out;
  std::string low_text = lower(text);
  for (auto& m : WORDS.finditer(low_text)) {
    std::string w = m.str();
    auto it = ES_EN.find(w);
    if (it == ES_EN.end()) it = ES_EN.find(normalize(w));
    out.push_back(it != ES_EN.end() && !it->second.empty() ? it->second : w);
  }
  std::string s = join(out, " ");
  static const Rx P1(R"(\s+([,.;:!?]))"), P2(R"(\b(a|the) (a|the)\b)"), P3(R"(\ba ([aeiou]))");
  s = P1.sub(s, "\\1");
  s = P2.sub(s, "\\1");
  s = P3.sub(s, "an \\1");
  return s;
}

json enhance_rules(const std::string& raw, const std::string& target, const std::string& st) {
  std::string text = trim(raw);
  if (target == "image") {
    std::string subject = looks_spanish(text) ? translate_rules(text) : text;
    std::vector<std::string> parts;
    for (auto& p : {subject, style(st), std::string("dramatic lighting, rich colors, centered composition"), std::string(QUALITY)})
      if (!p.empty()) parts.push_back(p);
    return {{"prompt", join(parts, ", ")}, {"negative", NEGATIVE}, {"engine", "reglas"}};
  }
  if (target == "music") {
    std::string mood = media::detect_mood(text);
    auto it = MUSIC_HINTS.find(mood);
    return {{"prompt", trim(rtrim(text, ".") + ". " + (it == MUSIC_HINTS.end() ? "" : it->second))},
            {"negative", ""},
            {"engine", "reglas"},
            {"mood", mood}};
  }
  std::string prompt = "Objetivo: " + rtrim(text, ".") +
                       ".\nContexto: responde teniendo en cuenta lo que sabes de mi y de la conversacion.\n"
                       "Formato: estructura la respuesta con puntos claros y un resumen final breve.\n"
                       "Restricciones: si falta informacion, indica los supuestos; no inventes datos.";
  return {{"prompt", prompt}, {"negative", ""}, {"engine", "reglas"}};
}

json enhance(llm::LLMManager* llm, std::string text, std::string target, const std::string& st, bool use_llm) {
  text = trim(text);
  if (text.empty()) return {{"prompt", ""}, {"negative", ""}, {"engine", "vacio"}};
  if (!TEMPLATES.count(target)) target = "image";
  if (use_llm && llm && llm->has_real_llm()) {
    try {
      std::string extra = st.empty() ? "" : "\nEstilo deseado: " + style(st);
      llm::Opts o;
      o.o = {{"max_tokens", 220}, {"temperature", 0.4}};
      std::string raw = replace_all(llm->complete(TEMPLATES.at(target) + extra + "\n\nIdea del usuario: " + text, "default", o),
                                    "**", "");
      static const Rx PR(R"(PROMPT:\s*(.+?)(?:\n\s*NEGATIVE:|\z))", Rx::S | Rx::I), NG(R"(NEGATIVE:\s*(.+))", Rx::I);
      auto m = PR.search(raw);
      std::string prompt = trim(trim(m ? m->str(1) : raw), "\"");
      auto neg = NG.search(raw);
      if (ulen(prompt) > 8) {
        json out = {{"prompt", uleft(prompt, 900)},
                    {"negative", neg ? trim(neg->str(1)) : (target == "image" ? std::string(NEGATIVE) : std::string())},
                    {"engine", "llm"}};
        if (target == "image" && !st.empty() && has_style(st) && !contains(out["prompt"].get<std::string>(), style(st)))
          out["prompt"] = out["prompt"].get<std::string>() + ", " + style(st);
        return out;
      }
    } catch (...) {
    }
  }
  return enhance_rules(text, target, st);
}

}  // namespace nx::prompt
