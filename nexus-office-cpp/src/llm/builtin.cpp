// Motor integrado sin modelo (llm/builtin.py): reglas que emiten llamadas a herramientas con el mismo protocolo.
#include <chrono>
#include <thread>

#include "llm/llm.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::llm {

static const Rx& URL() {
  static const Rx r(R"(https?://\S+)");
  return r;
}

static std::string call(const std::string& name, const json& args = json::object()) { return tool_call_text(name, args); }

static std::string rstrip_chars(std::string s, const std::string& chars) { return rtrim(s, chars); }

std::optional<std::pair<std::string, json>> detect_intent(const std::string& text, const std::set<std::string>& tools) {
  static const Rx MATH(R"(^[\d\s\.\,\+\-\*/\^\(\)%a-z_]*\d[\d\s\.\,\+\-\*/\^\(\)%a-z_]*$)", Rx::I);
  static const Rx IMG(
      R"(\b(?:genera|crea|dibuja|pinta|haz|hazme|draw|generate|create|make)\b.*?\b(?:imagen|dibujo|ilustraci[oó]n|cuadro|foto|image|picture)\b(?:\s+(?:de|del|sobre|of|with|con))?\s*(.*))",
      Rx::I | Rx::S);
  static const Rx MUS(
      R"(\b(?:comp[oó]n|componme|crea|genera|haz|hazme|toca|compose|make|generate)\b.*?\b(?:m[uú]sica|canci[oó]n|melod[ií]a|tema musical|music|song|melody|beat)\b\s*(.*))",
      Rx::I | Rx::S);
  static const Rx DOC(
      R"(\b(pdf|documento|informe|dossier|extrae|extraer|procesa|procesar|archiva|archivar|guarda|guardar|maqueta|resume|resumir|aprende|estudia)\b)",
      Rx::I);
  static const Rx CRAWL(
      R"(\b(rastrea|rastrear|rastreo|crawl\w*|ara[ñn]a|spider|indexa (?:toda|todo|el sitio|el dominio|la documentaci[oó]n)|todo el (?:dominio|sitio)|toda la documentaci[oó]n|documentaci[oó]n (?:completa|entera)|aprende de (?:la|esta) documentaci[oó]n|lee (?:toda|la) documentaci[oó]n)\b)",
      Rx::I);
  static const Rx DOMAIN(R"(\b(todo el (dominio|sitio|site)|dominio completo|whole (site|domain))\b)");
  static const Rx WHOLE(R"((toda la documentaci[oó]n|documentaci[oó]n (completa|entera)|entera\b|completa\b))");
  static const Rx CONT(R"(\b(sigue|segu[ií]|contin[uú]a|continuar|reanuda|reanudar)\b)");
  static const Rx PREFIX(R"(^(cu[aá]nto es|cu[aá]nto da|calcula|calculate|what is|resuelve)\s*)");
  static const Rx OPS(R"([\+\-\*/\^%]|sqrt|sin|cos|log)");
  static const Rx TIME(R"(\b(qu[eé] hora es|qu[eé] d[ií]a es hoy|qu[eé] fecha es|what time is it)\b)");
  std::string low = lower(trim(text));
  auto url = URL().search(text);
  if (tools.count("crawl_docs") && url && CRAWL.test(text)) {
    json args = {{"url", rstrip_chars(url->str(), ".,;)")}, {"scope", DOMAIN.test(low) ? "domain" : "path"}};
    if (args["scope"] == "domain" || WHOLE.test(low)) {
      args["max_pages"] = 100000;
      args["max_depth"] = 50;
      args["background"] = true;
    }
    if (CONT.test(low)) args["continue"] = true;
    return std::make_pair(std::string("crawl_docs"), args);
  }
  if (tools.count("url_to_pdf") && url && DOC.test(text))
    return std::make_pair(std::string("url_to_pdf"), json{{"url", rstrip_chars(url->str(), ".,;)")}});
  std::string expr = strip_u(PREFIX.sub(low, ""), U" ?¿=");
  if (tools.count("calculator") && !expr.empty() && MATH.match(expr) && MATH.fullmatch(expr) && OPS.test(expr))
    return std::make_pair(std::string("calculator"), json{{"expression", expr}});
  if (tools.count("datetime_now") && TIME.test(low)) return std::make_pair(std::string("datetime_now"), json::object());
  if (auto m = IMG.search(text); tools.count("generate_image") && m) {
    std::string p = strip_u(m->str(1), U" .");
    return std::make_pair(std::string("generate_image"), json{{"prompt", p.empty() ? text : p}});
  }
  if (auto m = MUS.search(text); tools.count("compose_music") && m) {
    std::string p = strip_u(m->str(1), U" .");
    return std::make_pair(std::string("compose_music"), json{{"prompt", p.empty() ? text : p}});
  }
  return std::nullopt;
}

void BuiltinBackend::stream_chat(const json& messages, const std::string&, const Opts& opts, const Sink& out) {
  std::set<std::string> tools;
  for (auto& t : opts.o.value("tools", json::array())) tools.insert(t.get<std::string>());
  std::string reply = respond(messages, tools);
  double t0 = now(), first = 0;
  long n = 0;
  static const Rx ws(R"((\s+))");
  for (auto& w : ws.split(reply)) {
    if (w.empty()) continue;
    if (!first) first = now() - t0;
    ++n;
    out(w);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  last_stats = {n, now() - t0, first};
}

std::string BuiltinBackend::respond(const json& messages, const std::set<std::string>& tools) {
  std::string system = !messages.empty() && messages[0]["role"] == "system" ? jstr(messages[0], "content") : "";
  std::string last = messages.empty() ? "" : jstr(messages.back(), "content");
  static const Rx RESULT(R"rx(<tool_result name="([^"]+)">(.*?)</tool_result>)rx", Rx::S);
  if (auto m = RESULT.search(last)) return after_tool(m->str(1), m->str(2));
  std::string text = trim(last), low = lower(text);
  static const Rx NAME(R"([Uu]suario se llama ([^\n.]+))");
  auto nm = NAME.search(system);
  std::string user = nm ? trim(nm->str(1)) : "";
  if (auto intent = detect_intent(text, tools)) return call(intent->first, intent->second);
  static const Rx HORA(R"(\b(hora|qu[eé] d[ií]a|fecha|what time|date today)\b)");
  static const Rx SABES(R"(\b(qu[eé] sabes|qu[eé] recuerdas|recuerdas|what do you know|remember about)\b)");
  static const Rx SABES_Q(R"(.*?(sabes|recuerdas|know|remember)\s*(de|sobre|about)?\s*)");
  static const Rx SYS(R"(\b(hardware|rendimiento|sistema|gpu|npu|cpu|memoria ram|performance)\b)");
  static const Rx BUSCA(R"(^(busca|buscar|search|investiga)\b)");
  static const Rx BUSCA_Q(R"(^(busca|buscar|search|investiga)\s*(en internet|online)?\s*)");
  static const Rx HOLA(R"(^(hola|buenas|hey|hello|hi|saludos)\b)");
  static const Rx RECUERDA(R"(^(recuerda|apunta|no olvides|remember)\b)");
  static const Rx GUSTA(R"(\b(me llamo|me gusta|vivo en|trabajo)\b)");
  if (tools.count("datetime_now") && HORA.test(low)) return call("datetime_now");
  if (tools.count("web_fetch") && URL().test(text)) return call("web_fetch", {{"url", URL().search(text)->str()}});
  if (tools.count("memory_search") && SABES.test(low)) {
    std::string q = strip_u(SABES_Q.sub(low, "", 1), U" ?¿");
    if (q.empty()) q = "usuario";
    return call("memory_search", {{"query", q == "mi" || q == "mí" || q == "me" ? "usuario" : q}});
  }
  if (tools.count("system_info") && SYS.test(low)) return call("system_info");
  if (tools.count("web_search") && BUSCA.match(low)) return call("web_search", {{"query", BUSCA_Q.sub(low, "", 1)}});
  if (HOLA.match(low)) {
    std::string who = user.empty() ? "" : ", " + user;
    return "¡Hola" + who +
           "! Soy Nexus-Office. Ahora mismo funciono con mi núcleo integrado (sin modelo de lenguaje), pero ya puedo "
           "calcular, decirte la hora, generar imágenes y música, recordar lo que me cuentas y mostrarte mi grafo de "
           "conocimiento. Para conversar con libertad, descarga un modelo en **Modelos** (te recomiendo uno adaptado a "
           "tu hardware).";
  }
  if (RECUERDA.match(low) || GUSTA.test(low)) return "Entendido, lo he guardado en mi memoria y en mi grafo de conocimiento. 🧠";
  const std::string FRAG = "Fragmentos de tu biblioteca";
  if (contains(system, FRAG)) {
    std::string part = system.substr(system.rfind(FRAG));
    static const Rx F(R"(^\[(\d+)\] «([^»]+)» p\.(\d+): (.+)$)", Rx::M);
    auto frags = F.finditer(part);
    if (!frags.empty()) {
      std::vector<std::string> items;
      for (size_t i = 0; i < frags.size() && i < 3; ++i)
        items.push_back("> " + uleft(frags[i].str(4), 350) + "…\n> — *" + frags[i].str(2) + "*, p. " + frags[i].str(3) +
                        " [" + frags[i].str(1) + "]");
      return "Esto es lo que dice mi biblioteca de documentos:\n\n" + join(items, "\n\n");
    }
  }
  const std::string REC = "Recuerdos relevantes:";
  if (contains(system, REC)) {
    static const Rx M(R"(^- (.+)$)", Rx::M);
    auto mems = M.findall(system.substr(system.rfind(REC)));
    if (!mems.empty()) {
      std::vector<std::string> listed;
      for (size_t i = 0; i < mems.size() && i < 4; ++i) listed.push_back("- " + mems[i]);
      return "Sin un modelo de lenguaje no puedo razonar libremente, pero esto es lo que recuerdo relacionado:\n" +
             join(listed, "\n");
    }
  }
  return "Todavía no tengo un modelo de lenguaje cargado, así que mis respuestas abiertas son limitadas. Prueba: "
         "*«cuánto es 2^10 / 3»*, *«genera una imagen de una ciudad de neón»*, *«compón una música épica»*, *«qué sabes "
         "de mí»* o *«info del sistema»*. Desde **Modelos** puedes descargar un LLM recomendado para tu equipo.";
}

static std::string S_(const json& d, const char* k) {
  if (!d.is_object() || !d.contains(k)) return "None";
  const json& v = d[k];
  if (v.is_null()) return "None";
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_number_float()) return pyfloat(v.get<double>());
  return pydump(v);
}

std::string BuiltinBackend::after_tool(const std::string& name, const std::string& body) {
  json data = jparse(body);
  if (data.is_discarded()) data = body;
  if (data.is_object() && truthy(data.value("error", json())))
    return "La herramienta **" + name + "** no pudo completarse: " + S_(data, "error");
  if (name == "calculator") return "El resultado es **" + S_(data, "result") + "**.";
  if (name == "datetime_now")
    return "Ahora son las **" + S_(data, "time") + "** del " + S_(data, "weekday") + " " + S_(data, "date") + ".";
  if (name == "generate_image") return "Aquí tienes la imagen (" + S_(data, "engine") + "):\n\n![imagen](" + S_(data, "url") + ")";
  if (name == "compose_music")
    return "He compuesto una pieza en **" + S_(data, "key") + "**, " + S_(data, "tempo") + " BPM, estilo *" +
           S_(data, "mood") + "*:\n\n[audio](" + S_(data, "url") + ")";
  if (name == "memory_search") {
    json items = data.is_object() ? data.value("results", json::array()) : json::array();
    if (items.empty()) return "Aún no tengo recuerdos sobre eso. Cuéntame más y lo aprenderé.";
    std::vector<std::string> l;
    for (size_t i = 0; i < items.size() && i < 6; ++i) l.push_back("- " + S_(items[i], "content"));
    return "Esto es lo que recuerdo:\n" + join(l, "\n");
  }
  if (name == "system_info") {
    json p = data.is_object() ? data.value("profile", json::object()) : json::object();
    json llm = p.value("llm", json::object());
    bool npu = truthy(p.value("npu", json::object()).value("present", json(false)));
    return "Perfil **" + S_(p, "id") + "** (" + S_(p, "tier") + "): acelerador `" + S_(p, "accelerator") + "` en " +
           S_(p, "device_name") + ". RAM " + S_(p, "ram_gb") + " GB. Modelo recomendado ≤ " +
           S_(llm, "recommended_model_b") + "B, contexto " + S_(llm, "n_ctx") + ", " + S_(llm, "n_threads") +
           " hilos. NPU: " + (npu ? "sí" : "no") + ".";
  }
  if (name == "rag_search") {
    json items = data.is_object() ? data.value("results", json::array()) : json::array();
    if (items.empty()) return "No he encontrado nada sobre eso en mi biblioteca de documentos.";
    std::vector<std::string> l;
    for (size_t i = 0; i < items.size() && i < 3; ++i)
      l.push_back("> " + uleft(S_(items[i], "text"), 300) + "…\n> — *" + S_(items[i], "document") + "*, p. " +
                  S_(items[i], "page"));
    return "En mi biblioteca:\n\n" + join(l, "\n\n");
  }
  if (name == "crawl_docs" && data.is_object() && jstr(data, "status") == "en curso")
    return "He empezado a rastrear el sitio completo en segundo plano, por lotes correlativos. Cada lote se añade a mi "
           "biblioteca RAG en cuanto termina (lo verás en *Biblioteca → Rastreos por lotes*), así que ya puedes "
           "preguntarme por lo que vaya leyendo.";
  if (name == "web_fetch" || name == "web_search") {
    std::string txt = data.is_object() && truthy(data.value("text", json())) ? S_(data, "text")
                      : data.is_object() ? pydump(data.value("results", data)) : pydump(data);
    return "Esto es lo que he encontrado:\n\n" + uleft(txt, 1200);
  }
  return "Resultado de **" + name + "**:\n```\n" + uleft(data.is_string() ? data.get<std::string>() : data.dump(1), 1500) +
         "\n```";
}

}  // namespace nx::llm
