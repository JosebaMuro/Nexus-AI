// CPU, GPU y NPU del movil en la app Android autonoma, y como acelerar el modelo con ellos
// (hardware/android_accel.py).
//
// * CPU: el SoC (ro.soc.model, ro.board.platform...) y sus nucleos (core/android_tune).
// * GPU: la app lleva, ademas de los llama-server de CPU (estaticos), libllama-server-gpu.so: llama.cpp enlazado con
//   la libc de Android y con los backends Vulkan y OpenCL, que se cargan solo si el movil tiene sus drivers. Ese
//   binario, con --list-devices, dice que GPU puede usar de verdad (Adreno por OpenCL; Mali, Immortalis, Xclipse,
//   PowerVR... por Vulkan). El nombre de la GPU llega tambien de la app (EGL, device_info.json).
// * NPU: se reconoce por el SoC y por las bibliotecas del fabricante. La Hexagon de los Snapdragon 8 Gen 2 o
//   posteriores (HTP v73+) la usa llama.cpp con su backend Hexagon: aparece en --list-devices como HTP0 y se prefiere
//   a la GPU. Las demas NPU no tienen backend en llama.cpp: se informa de ellas y el modelo va en la GPU o la CPU.
//
// El resultado de la sonda se guarda (android_accel.json) por version de la app y huella del sistema. Si la NPU falla
// al arrancar llama-server, se marca y se pasa a la GPU; si falla la GPU, a la CPU.
#include "hardware/android_accel.h"

#include <map>

#include "util/fs.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::hw::android_accel {

namespace {

const std::vector<std::string> VENDOR_LIBS = {"/vendor/lib64", "/system/vendor/lib64", "/system/lib64"};
const std::vector<std::string> PROPS = {"ro.soc.manufacturer", "ro.soc.model", "ro.board.platform", "ro.hardware",
                                        "ro.hardware.egl", "ro.hardware.vulkan", "ro.product.manufacturer",
                                        "ro.product.model", "ro.build.fingerprint", "ro.build.version.sdk"};
const std::vector<std::string> DSP_DIRS = {"/vendor/dsp/cdsp", "/vendor/lib/rfsa/adsp", "/system/lib/rfsa/adsp", "/dsp"};

// nombre comercial de algunos SoC Qualcomm (el resto se muestra con su codigo)
const std::map<std::string, std::string> SNAPDRAGON = {
    {"sm8850", "Snapdragon 8 Elite Gen 5"}, {"sm8750", "Snapdragon 8 Elite"}, {"sm8650", "Snapdragon 8 Gen 3"},
    {"sm8635", "Snapdragon 8s Gen 3"}, {"sm8550", "Snapdragon 8 Gen 2"}, {"sm8475", "Snapdragon 8+ Gen 1"},
    {"sm8450", "Snapdragon 8 Gen 1"}, {"sm8350", "Snapdragon 888"}, {"sm8250", "Snapdragon 865"},
    {"sm8150", "Snapdragon 855"}, {"sm7675", "Snapdragon 7+ Gen 3"}, {"sm7550", "Snapdragon 7 Gen 3"},
    {"sm7475", "Snapdragon 7+ Gen 2"}, {"sm7450", "Snapdragon 7 Gen 1"}, {"sm6450", "Snapdragon 6 Gen 1"},
    {"pineapple", "Snapdragon 8 Gen 3"}, {"kalama", "Snapdragon 8 Gen 2"}, {"taro", "Snapdragon 8 Gen 1"},
    {"sun", "Snapdragon 8 Elite"}, {"lahaina", "Snapdragon 888"}, {"kona", "Snapdragon 865"}};
const std::map<std::string, std::string> TENSOR = {{"gs101", "Google Tensor"}, {"gs201", "Google Tensor G2"},
                                                   {"zuma", "Google Tensor G3"}, {"zumapro", "Google Tensor G4"},
                                                   {"laguna", "Google Tensor G5"}};

std::string get(const std::map<std::string, std::string>& m, const std::string& k) {
  auto it = m.find(k);
  return it == m.end() ? "" : it->second;
}

std::string run_text(const std::vector<std::string>& cmd, double timeout,
                     const std::map<std::string, std::string>& env = {}) {
  proc::Options o;
  o.capture = true;
  o.env = env;
  auto r = proc::run(cmd, timeout, o);
  return r.out + r.err;
}

fs::path cache_file() { return paths::data_dir() / "android_accel.json"; }

std::string cache_key(const json& p, const std::string& libs) {
  fs::path exe = libs.empty() ? fs::path() : P(libs) / GPU_EXE;
  uint64_t size = !libs.empty() && is_file(exe) ? fsize(exe) : 0;
  return cat(NEXUS_VERSION, "|", jstr(p, "ro.build.fingerprint"), "|", size);
}

json load_cache() {
  auto t = read_file(cache_file());
  json j = t ? jparse(*t) : json();
  return j.is_object() ? j : json::object();
}

void save_cache(const json& c) {
  try {
    write_file(cache_file(), jdump(c, 1));
  } catch (...) {
  }
}

}  // namespace

json props() {
  json out = json::object();
  const std::string getprop = "/system/bin/getprop";
  if (!path_exists(getprop)) return out;
  static const Rx LINE(R"(^\[([^\]]+)\]: \[(.*)\]$)");
  for (auto& line : splitlines(run_text({getprop}, 10))) {
    auto m = LINE.match(line);
    if (m && std::find(PROPS.begin(), PROPS.end(), m->str(1)) != PROPS.end()) out[m->str(1)] = m->str(2);
  }
  return out;
}

json device_info(const fs::path& path) {
  fs::path f = !path.empty() ? path
               : !env("NEXUS_DEVICE_INFO").empty() ? P(env("NEXUS_DEVICE_INFO"))
                                                   : P(env("NEXUS_DATA_DIR", ".")).parent_path() / "device_info.json";
  auto t = read_file(f);
  json j = t ? jparse(*t) : json();
  return j.is_object() ? j : json::object();
}

std::string vendor_lib(const std::vector<std::string>& names, const std::vector<std::string>& dirs) {
  for (auto& d : dirs.empty() ? VENDOR_LIBS : dirs)
    for (auto& n : names)
      if (path_exists(P(d) / n)) return S(P(d) / n);
  return "";
}

json soc(const json& p, const json& info) {
  auto pick = [&](const char* ik, const char* pk) {
    std::string a = trim(jstr(info, ik));
    return a.empty() ? trim(jstr(p, pk)) : a;
  };
  std::string mfr = pick("soc_manufacturer", "ro.soc.manufacturer");
  std::string model = pick("soc_model", "ro.soc.model");
  std::string platform = trim(jstr(p, "ro.board.platform"));
  if (platform.empty()) platform = trim(jstr(info, "board"));
  std::string hardware = trim(jstr(p, "ro.hardware"));
  if (hardware.empty()) hardware = trim(jstr(info, "hardware"));
  std::string key = lower(mfr + " " + model + " " + platform + " " + hardware);
  static const Rx QC(R"(qti|qualcomm|qcom|\bsm\d{4}|\bsdm\d|\bmsm\d|snapdragon|pineapple|kalama|taro|lahaina|kona|\bsun\b)");
  static const Rx GG(R"(google|tensor|gs101|gs201|zuma|laguna)");
  static const Rx MT(R"(mediatek|\bmt\d{4}|dimensity|helio)");
  static const Rx SS(R"(samsung|exynos|s5e\d|universal\d)");
  static const Rx UN(R"(unisoc|spreadtrum|\bums\d|\bt\d{3}\b)");
  std::string family = QC.test(key) ? "qualcomm" : GG.test(key) ? "google" : MT.test(key) ? "mediatek"
                       : SS.test(key) ? "samsung" : UN.test(key) ? "unisoc" : "";
  std::string commercial;
  for (auto& code : {lower(model), lower(platform)}) {
    if (commercial.empty()) commercial = get(SNAPDRAGON, code);
    if (commercial.empty()) commercial = get(TENSOR, code);
  }
  std::string lm = lower(mfr);
  std::string first = lm != "qti" && !lm.empty() ? mfr : family == "qualcomm" ? "Qualcomm" : "";
  std::string second = !model.empty() ? model : platform;
  std::vector<std::string> parts;
  if (!first.empty()) parts.push_back(first);
  if (!second.empty()) parts.push_back(second);
  std::string label = join(parts, " ");
  if (!commercial.empty() && !contains(label, commercial)) label = label.empty() ? commercial : label + " (" + commercial + ")";
  std::string name = !label.empty() ? label : !hardware.empty() ? hardware : platform;
  return {{"family", family}, {"manufacturer", mfr}, {"model", model}, {"platform", platform},
          {"hardware", hardware}, {"name", name}};
}

json npu(const json& s, const LibFinder& lib_in) {
  LibFinder lib = lib_in ? lib_in : [](const std::vector<std::string>& n) { return vendor_lib(n); };
  std::string fam = jstr(s, "family");
  auto mk = [](const std::string& vendor, const std::string& name, const std::string& hint, const std::string& found) {
    return json{{"vendor", vendor}, {"name", name}, {"runtime_hint", hint},
                {"source", found.empty() ? "soc" : "vendor-lib"}, {"lib", found}};
  };
  if (fam == "qualcomm") {
    std::string n = jstr(s, "name");
    return mk("qualcomm", "Qualcomm Hexagon NPU" + (n.empty() ? "" : " · " + n), "QNN / Hexagon SDK",
              lib({"libQnnHtp.so", "libcdsprpc.so", "libSNPE.so"}));
  }
  if (fam == "mediatek")
    return mk("mediatek", "MediaTek APU (NeuroPilot)", "NeuroPilot",
              lib({"libneuron_adapter.so", "libneuron_adapter_mgvi.so", "libapu_mdw.so", "libneuronusdk_adapter.mtk.so"}));
  if (fam == "google") {
    std::string t = get(TENSOR, lower(jstr(s, "platform")));
    return mk("google", (t.empty() ? "Google Tensor" : t) + " TPU", "Edge TPU / NNAPI",
              lib({"libedgetpu_util.so", "libedgetpu_client.google.so", "libdarwinn_client.so"}));
  }
  if (fam == "samsung") {
    std::string found = lib({"libeden_nn_on_system.so", "libenn_user_driver.so", "libenn_public_api_cpp.so"});
    static const Rx EX(R"(s5e9|exynos(9|2)\d{2,3}|exynos1[0-9]{3})");
    if (!found.empty() || EX.test(lower(jstr(s, "model") + jstr(s, "platform"))))
      return mk("samsung", "Samsung Exynos NPU", "Exynos AI Studio (ENN)", found);
  }
  return nullptr;
}

std::map<std::string, std::string> gpu_env(const std::string& libs) {
  std::vector<std::string> ps = {libs};
  for (auto d : {"/system/lib64", "/vendor/lib64", "/system/vendor/lib64"})
    if (is_dir(P(d))) ps.push_back(d);
  std::vector<std::string> dsp = {libs};
  dsp.insert(dsp.end(), DSP_DIRS.begin(), DSP_DIRS.end());
  return {{"LD_PRELOAD", ""}, {"LD_LIBRARY_PATH", join(ps, ":")}, {"ADSP_LIBRARY_PATH", join(dsp, ";")}};
}

bool is_npu_dev(const std::string& dev) {
  static const Rx R(R"(HTP\d)");
  return R.match(dev).has_value();
}

json parse_devices(const std::string& out) {
  static const Rx DEV(R"(^\s+([\w.:\-]+): (.*?) \((\d+) MiB, (\d+) MiB free\)\s*$)");
  json devs = json::array();
  for (auto& line : splitlines(out)) {
    auto m = DEV.match(line);
    if (!m) continue;
    std::string name = m->str(1), ln = lower(name);
    std::string backend = contains(ln, "opencl") ? "opencl" : starts_with(ln, "vulkan") ? "vulkan"
                          : is_npu_dev(name) ? "hexagon" : ln;
    devs.push_back({{"dev", name}, {"backend", backend}, {"name", trim(m->str(2))},
                    {"mem_mb", std::stol(m->str(3))}, {"free_mb", std::stol(m->str(4))}});
  }
  return devs;
}

std::string gpu_vendor(const std::string& name) {
  std::string n = lower(name);
  static const std::vector<std::pair<const char*, const char*>> K = {
      {"adreno", "qualcomm"}, {"qualcomm", "qualcomm"}, {"mali", "arm"}, {"immortalis", "arm"},
      {"xclipse", "samsung"}, {"powervr", "imagination"}, {"img ", "imagination"}, {"maleoon", "huawei"},
      {"nvidia", "nvidia"}, {"intel", "intel"}};
  for (auto& [k, v] : K)
    if (contains(n, k)) return v;
  return "unknown";
}

json pick(const json& devs, bool use_npu, bool use_gpu) {
  if (use_npu)
    for (auto& d : devs)
      if (jstr(d, "backend") == "hexagon") return d;
  json rest = json::array();
  if (use_gpu)
    for (auto& d : devs)
      if (jstr(d, "backend") != "hexagon") rest.push_back(d);
  if (rest.empty()) return nullptr;
  bool adreno = false;
  for (auto& d : rest) adreno |= contains(lower(jstr(d, "name")), "adreno");
  if (adreno) {   // Adreno por OpenCL (kernels de Qualcomm); su Vulkan suele ir peor que la CPU
    for (auto& d : rest)
      if (contains(lower(jstr(d, "name")), "adreno") && jstr(d, "backend") == "opencl") return d;
    return nullptr;
  }
  for (auto& d : rest)
    if (jstr(d, "backend") == "vulkan") return d;
  return nullptr;
}

json probe_gpu(const std::string& libs, const std::string& exe_name) {
  fs::path exe = libs.empty() ? fs::path() : P(libs) / exe_name;
  if (libs.empty() || !is_file(exe))
    return {{"binary", false}, {"devices", json::array()}, {"error", "la APK no trae llama-server con GPU"}};
  std::string out = run_text({S(exe), "--list-devices"}, 45, gpu_env(libs));
  json devs = parse_devices(out);
  json res = {{"binary", true}, {"devices", devs}};
  if (devs.empty()) {
    res["error"] = "ningun backend GPU (Vulkan/OpenCL) encontro una GPU utilizable";
    std::vector<std::string> tail;
    for (auto& ln : splitlines(out))
      if (!trim(ln).empty() && !contains(ln, "linker")) tail.push_back(ln);
    if (tail.size() > 4) tail.erase(tail.begin(), tail.end() - 4);
    if (!tail.empty()) {
      std::string t = join(tail, " | ");
      res["log"] = t.size() > 400 ? t.substr(t.size() - 400) : t;
    }
  }
  return res;
}

json detect(const std::optional<std::string>& libs_in, bool use_cache) {
  std::string libs = libs_in ? *libs_in : env("NEXUS_ANDROID_LIBS");
  json p = props(), info = device_info();
  std::string key = cache_key(p, libs);
  json cache = use_cache ? load_cache() : json::object();
  json probe;
  if (jstr(cache, "key") == key && cache.contains("gpu") && cache["gpu"].is_object()) {
    probe = cache["gpu"].value("probe", json::object());
  } else {
    probe = probe_gpu(libs);
    cache = {{"key", key}};
  }
  json s = soc(p, info);
  json devs = probe.value("devices", json::array());
  std::string broken = jstr(cache, "gpu_broken"), npu_broken = jstr(cache, "npu_broken");
  json use = pick(devs, npu_broken.empty(), broken.empty());
  json gpus = json::array();
  for (auto& d : devs)
    if (jstr(d, "backend") != "hexagon") gpus.push_back(d);
  json gpu_dev = pick(gpus);
  std::string egl = trim(jstr(info, "gl_renderer"));
  std::string name = !gpu_dev.is_null() ? jstr(gpu_dev, "name") : !gpus.empty() ? jstr(gpus[0], "name") : "";
  if (name.empty()) name = egl;
  if (name.empty()) {
    std::string e = lower(jstr(p, "ro.hardware.egl"));
    name = e == "adreno" ? "Qualcomm Adreno" : e == "mali" ? "ARM Mali" : "";
  }
  std::string vk = jstr(info, "vulkan_version");
  if (vk.empty()) vk = jstr(p, "ro.hardware.vulkan");
  json gpu = {{"name", name}, {"vendor", gpu_vendor(!name.empty() ? name : jstr(p, "ro.hardware.egl"))},
              {"egl", egl}, {"gl_vendor", jstr(info, "gl_vendor")}, {"vulkan", vk}, {"devices", devs},
              {"use", use}, {"broken", broken}, {"npu_broken", npu_broken}, {"probe", probe},
              {"binary", probe.value("binary", false)}};
  if (!gpus.empty() && gpu_dev.is_null() && broken.empty()) {
    bool adreno = false;
    for (auto& d : gpus) adreno |= contains(lower(jstr(d, "name")), "adreno");
    gpu["note"] = adreno ? "Adreno sin OpenCL: su Vulkan es mas lento que la CPU en llama.cpp" : "";
  }
  json n = npu(s);
  json htp;
  for (auto& d : devs)
    if (jstr(d, "backend") == "hexagon") {
      htp = d;
      break;
    }
  if (!n.is_null() && !htp.is_null()) {   // llama.cpp la ve: el modelo puede ir en ella
    n["usable"] = npu_broken.empty();
    n["device"] = htp["dev"];
    n["runtime_hint"] = "llama.cpp Hexagon";
    n["broken"] = npu_broken;
  }
  json pr = json::object();
  for (auto& [k, v] : p.items())
    if (k != "ro.build.fingerprint") pr[k] = v;
  json res = {{"soc", s}, {"npu", n}, {"gpu", gpu}, {"props", pr}};
  cache["key"] = key;
  cache["gpu"] = gpu;
  cache["soc"] = s;
  cache["npu"] = n;
  save_cache(cache);
  return res;
}

void mark_gpu_broken(const std::string& reason, const std::string& device) {
  json cache = load_cache();
  cache[is_npu_dev(device) ? "npu_broken" : "gpu_broken"] = reason.empty() ? "fallo al arrancar" : uleft(reason, 300);
  save_cache(cache);
}

bool is_gpu_exe(const fs::path& exe) { return S(exe.filename()) == GPU_EXE; }

}  // namespace nx::hw::android_accel
