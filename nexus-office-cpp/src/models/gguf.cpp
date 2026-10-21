// Lector minimo de metadatos GGUF (models/gguf.py): la cabecera hasta los datos del tokenizer.
#include <cstring>
#include <fstream>
#include <map>

#include "models/models.h"
#include "util/text.h"

namespace nx::models {

namespace {
struct Reader {
  std::ifstream f;
  template <class T> T rd() {
    T v{};
    f.read((char*)&v, sizeof v);
    if (!f) throw std::runtime_error("fichero GGUF truncado");
    return v;
  }
  std::string str() {
    uint64_t n = rd<uint64_t>();
    if (n > (1u << 26)) throw std::runtime_error("cadena GGUF demasiado larga");
    std::string s(n, '\0');
    f.read(s.data(), (std::streamsize)n);
    return valid_utf8(s);
  }
  size_t scalar_size(uint32_t t) {
    switch (t) {
      case 0: case 1: case 7: return 1;
      case 2: case 3: return 2;
      case 4: case 5: case 6: return 4;
      case 10: case 11: case 12: return 8;
      default: return 0;
    }
  }
  json scalar(uint32_t t) {
    switch (t) {
      case 0: return rd<uint8_t>();
      case 1: return rd<int8_t>();
      case 2: return rd<uint16_t>();
      case 3: return rd<int16_t>();
      case 4: return rd<uint32_t>();
      case 5: return rd<int32_t>();
      case 6: return (double)rd<float>();
      case 7: return rd<uint8_t>() != 0;
      case 10: return rd<uint64_t>();
      case 11: return rd<int64_t>();
      case 12: return rd<double>();
    }
    throw std::runtime_error(cat("tipo GGUF desconocido ", t));
  }
  json value(uint32_t t) {
    if (scalar_size(t)) return scalar(t);
    if (t == 8) return str();
    if (t == 9) {
      uint32_t it = rd<uint32_t>();
      uint64_t n = rd<uint64_t>();
      if (scalar_size(it)) {
        f.seekg((std::streamoff)(scalar_size(it) * n), std::ios::cur);
      } else if (it == 8) {
        for (uint64_t i = 0; i < n; ++i) f.seekg((std::streamoff)rd<uint64_t>(), std::ios::cur);
      } else {
        for (uint64_t i = 0; i < n; ++i) value(it);
      }
      return cat("[array ", n, "]");
    }
    throw std::runtime_error(cat("tipo GGUF desconocido ", t));
  }
};
}  // namespace

json gguf_metadata(const fs::path& p, size_t max_keys) {
  Reader r;
  r.f.open(p, std::ios::binary);
  if (!r.f) throw std::runtime_error("no se puede abrir " + S(p));
  char magic[4];
  r.f.read(magic, 4);
  if (!r.f || std::memcmp(magic, "GGUF", 4) != 0) throw std::runtime_error("no es un fichero GGUF");
  json meta = json::object();
  uint32_t version = r.rd<uint32_t>();
  uint64_t n_tensors = r.rd<uint64_t>(), n_kv = r.rd<uint64_t>();
  meta["gguf.version"] = version;
  meta["gguf.tensors"] = n_tensors;
  for (uint64_t i = 0; i < std::min<uint64_t>(n_kv, max_keys); ++i) {
    std::string key = r.str();
    uint32_t vt = r.rd<uint32_t>();
    json v = r.value(vt);
    if (!starts_with(key, "tokenizer.") || (vt != 8 && vt != 9)) meta[key] = v;
  }
  return meta;
}

json gguf_summarize(const fs::path& p) {
  json m;
  try {
    m = gguf_metadata(p);
  } catch (const std::exception& e) {
    return {{"error", e.what()}};
  }
  static const std::map<int, std::string> FT = {
      {0, "F32"},     {1, "F16"},     {2, "Q4_0"},    {3, "Q4_1"},    {7, "Q8_0"},    {8, "Q5_0"},
      {9, "Q5_1"},    {10, "Q2_K"},   {11, "Q3_K_S"}, {12, "Q3_K_M"}, {13, "Q3_K_L"}, {14, "Q4_K_S"},
      {15, "Q4_K_M"}, {16, "Q5_K_S"}, {17, "Q5_K_M"}, {18, "Q6_K"},   {32, "BF16"}};
  std::string arch = jstr(m, "general.architecture");
  std::string quant;
  if (m.contains("general.file_type")) {
    int ft = m["general.file_type"].get<int>();
    quant = FT.count(ft) ? FT.at(ft) : std::to_string(ft);
  }
  auto ak = [&](const std::string& k) { return arch.empty() ? json(nullptr) : m.value(arch + "." + k, json(nullptr)); };
  std::string gname = lower(jstr(m, "general.name")), base = lower(jstr(m, "general.basename"));
  bool is_emb = !arch.empty() && (m.contains(arch + ".pooling_type") || contains(gname, "embed") || contains(base, "embed"));
  return {{"architecture", arch},
          {"name", jstr(m, "general.name")},
          {"quant", quant},
          {"context_length", ak("context_length")},
          {"size_label", jstr(m, "general.size_label")},
          {"is_projector", arch == "clip" || m.contains("clip.has_vision_encoder") || m.contains("clip.projector_type")},
          {"pooling", ak("pooling_type")},
          {"embedding_length", ak("embedding_length")},
          {"is_embedding", is_emb},
          {"add_eos", m.value("tokenizer.ggml.add_eos_token", json(nullptr))},
          {"tensors", m.value("gguf.tensors", json(nullptr))}};
}

}  // namespace nx::models
