// Explorador de archivos para importar modelos (core/explorador).
#include <chrono>
#include <map>

#include "check.h"
#include "core/explorador.h"
#include "util/fs.h"
#include "util/json.h"

using namespace nx;
namespace ex = nx::explorador;

TEST("explorador: lista carpetas y ficheros de modelo y reconoce las carpetas de ONNX GenAI y Genie") {
  auto d = std::filesystem::temp_directory_path() /
           ("nexus-fx-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  mkdirs(d / "phi4-onnx");
  mkdirs(d / "llama-genie");
  mkdirs(d / "fotos");
  mkdirs(d / ".oculta");
  write_file(d / "phi4-onnx" / "genai_config.json", "{}");
  write_file(d / "llama-genie" / "genie_config.json", "{}");
  write_file(d / "Qwen3-4B-Q4_K_M.gguf", "GGUF");
  write_file(d / "gemma.litertlm", "x");
  write_file(d / "notas.txt", "hola");

  json r = ex::listar(S(d));
  CHECK(r["dirs"].size() == 3);   // sin la oculta
  CHECK(r["files"].size() == 2);  // sin notas.txt
  CHECK_EQ(jstr(r["files"][0], "formato"), std::string("litertlm"));   // por nombre sin mayusculas: gemma, Qwen
  CHECK_EQ(jstr(r["files"][1], "formato"), std::string("gguf"));
  std::map<std::string, std::string> fmt;
  for (auto& x : r["dirs"]) fmt[jstr(x, "name")] = jstr(x, "modelo");
  CHECK(fmt["phi4-onnx"] == "onnx-genai");
  CHECK(fmt["llama-genie"] == "genie");
  CHECK(fmt["fotos"].empty());
  CHECK(!jstr(r, "parent").empty());
  CHECK(!r["roots"].empty());

  CHECK(ex::listar(S(d), true)["files"].size() == 3);
  CHECK(jstr(ex::listar(S(d / "phi4-onnx")), "modelo") == "onnx-genai");
  CHECK(jstr(ex::listar(S(d / "gemma.litertlm")), "path") == S(resolve(d)));   // un fichero abre su carpeta
  bool err = false;
  try { ex::listar(S(d / "no-existe")); } catch (const std::runtime_error&) { err = true; }
  CHECK(err);
  rm_rf(d);
}
