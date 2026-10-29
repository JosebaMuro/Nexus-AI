// Catalogo curado (models/recommended.py): los datos vienen de la version Python (gen/data.cpp).
#include <map>
#include <set>

#include "gen/data.h"
#include "hardware/hardware.h"
#include "models/models.h"

namespace nx::models {

const json& recommended_list() {
  static const json R = data::get("recommended");
  return R;
}

const json& legacy_llm() {
  static const json L = data::get("legacy_llm");
  return L;
}

std::string fit(const json& rec, const json& profile) {
  double ram = jnum(profile["ram_gb"]), vram = jnum(profile.value("vram_gb", json(0)));
  bool gpu = jnum(profile["llm"]["gpu_layers"]) != 0;
  if (jnum(rec.value("min_ram_gb", json(0))) > ram && !(gpu && vram >= jnum(rec.value("min_vram_gb", json(99)))))
    return "no_cabe";
  std::string kind = jstr(rec, "kind");
  if (kind == "llm") {
    double max_b = jnum(profile["llm"]["max_model_b"]), rec_b = jnum(profile["llm"]["recommended_model_b"]);
    double p = jnum(rec["params_b"]);
    if (p > max_b) return "pesado";
    return p >= rec_b * 0.99 || p == max_b ? "ideal" : "ok";
  }
  if (kind == "image") {
    bool neural = truthy(profile["image"]["neural"]);
    if (truthy(rec.value("min_vram_gb", json())) && !(neural && vram >= jnum(rec["min_vram_gb"]))) return "pesado";
    bool base = truthy(rec.value("baseline", json(false)));
    return (base && !neural) || (neural && !base) ? "ideal" : "ok";
  }
  return "ideal";
}

json recommend(const json& profile, const json& sector) {
  json out = json::array();
  for (auto& r : recommended_list()) {
    std::string kind = jstr(r, "kind");
    if (hw::android() && kind != "llm" && kind != "embed") continue;
    std::string id = jstr(r, "id");
    if (truthy(r.value("sector_only", json(false))) && !sector.contains(id)) continue;
    json item = r;
    item["fit"] = fit(r, profile);
    if (sector.contains(id)) item["sector"] = sector[id];
    out.push_back(item);
  }
  std::set<std::string> kinds;
  std::vector<json*> general;
  for (auto& r : out)
    if (!truthy(r.value("sector_only", json(false))) && !truthy(r.value("previous", json(false)))) {
      general.push_back(&r);
      kinds.insert(jstr(r, "kind"));
    }
  for (auto& kind : kinds) {
    json* best = nullptr;
    for (auto* r : general)
      if (jstr(*r, "kind") == kind && jstr(*r, "fit") == "ideal" &&
          (!best || jnum((*r)["params_b"]) > jnum((*best)["params_b"])))
        best = r;
    if (best) {
      (*best)["best"] = true;
    } else if (kind == "llm") {
      for (auto* r : general) {
        std::string f = jstr(*r, "fit");
        if (jstr(*r, "kind") == kind && (f == "ok" || f == "pesado") &&
            (!best || jnum((*r)["params_b"]) < jnum((*best)["params_b"])))
          best = r;
      }
      if (best) (*best)["best"] = true;
    }
  }
  // IA especializada de la oficina (finanzas...): la mas grande que va con fluidez; si ninguna, la mas ligera que cabe
  std::map<std::string, json*> esp_best;
  for (auto& r : out) {
    std::string esp = jstr(r, "especialidad"), f = jstr(r, "fit");
    if (esp.empty() || f == "no_cabe") continue;
    json*& b = esp_best[esp];
    bool ok = f != "pesado", b_ok = b && jstr(*b, "fit") != "pesado";
    double p = jnum(r["params_b"]);
    if (!b || (ok && !b_ok) || (ok == b_ok && (ok ? p > jnum((*b)["params_b"]) : p < jnum((*b)["params_b"])))) b = &r;
  }
  for (auto& [esp, r] : esp_best) (*r)["best_especialidad"] = true;
  return out;
}

}  // namespace nx::models
