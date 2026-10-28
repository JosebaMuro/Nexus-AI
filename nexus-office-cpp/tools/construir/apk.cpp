// APK «Nexus-Office autonomo» (ARM64, Android 9+): el servidor es nexus-office compilado con el NDK.
//
//   nexus-construir apk --exe <nexus-office para Android> --libs <carpeta con libllama-server*.so>
//                       --recursos <carpeta instalada con nexus/> [--gpu-libs <carpeta lib/>]
//
// La APK lleva:
//   lib/arm64-v8a/libnexusoffice.so          el ejecutable de Nexus-Office (Android solo deja ejecutar lo que va aqui)
//   lib/arm64-v8a/libllama-server*.so        llama-server estatico en 3 variantes de CPU
//   lib/arm64-v8a/libllama-server-gpu.so     llama.cpp con la libc de Android y los backends Vulkan, OpenCL y Hexagon
//   lib/arm64-v8a/libggml-*.so ...           (NPU de los Snapdragon 8 Gen 2+); de la rama android-npu-libs (--gpu-libs)
//   assets/runtime.zip + runtime.json        la carpeta nexus/ (interfaz web, perfiles, agentes, certificados)
// La app Java esta en android/ (identificador ai.nexus.office.cpp). Herramientas: aapt2, d8, zipalign y apksigner del
// SDK de Android (.cache/android/build-tools o el PATH), javac y android.jar (NEXUS_ANDROID_JAR o
// .cache/android/platform/android.jar). Sin firma propia (.cache/android/nexus-release.jks) se crea una con keytool.
#include "comun.h"
#include "util/archive.h"
#include "util/crypto.h"
#include "util/proc.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {

const std::string APP_ID = "ai.nexus.office.cpp";
const int MIN_SDK = 28;
const int VERSION_CODE = 20000 + 50;   // por encima de la 2.0.0 de Python (20000 + revision): es otra app
const char* LLAMA[] = {"libllama-server.so", "libllama-server-dotprod.so", "libllama-server-i8mm.so"};

fs::path sdk() { return raiz() / ".cache" / "android"; }

#ifdef _WIN32
const char* EXE = ".exe";
const char* BAT = ".bat";
#else
const char* EXE = "";
const char* BAT = "";
#endif

std::string tool(const std::string& name, bool bat = false) {
  fs::path p = sdk() / "build-tools" / (name + (bat ? BAT : EXE));
  if (is_file(p)) return S(p);
  return proc::which(name);
}

std::string jdk_tool(const std::string& name) {
  fs::path p = sdk() / "jdk" / "bin" / (name + EXE);
  if (is_file(p)) return S(p);
  std::string w = proc::which(name);
  return w.empty() ? name : w;
}

fs::path android_jar() {
  std::string e = env("NEXUS_ANDROID_JAR");
  if (!e.empty() && is_file(P(e))) return P(e);
  if (is_file(sdk() / "platform" / "android.jar")) return sdk() / "platform" / "android.jar";
  throw Fallo("falta android.jar (API 34): copia platforms/android-34/android.jar del SDK a .cache/android/platform/ "
              "o indica su ruta en NEXUS_ANDROID_JAR");
}

std::pair<fs::path, std::string> keystore() {
  fs::path ks = sdk() / "nexus-release.jks", pw_file = sdk() / "nexus-release.pass";
  if (!is_file(ks)) {
    fs::create_directories(ks.parent_path());
    std::string pw = token_urlsafe(16);
    ejecutar({jdk_tool("keytool"), "-genkeypair", "-keystore", S(ks), "-alias", "nexus", "-keyalg", "RSA", "-keysize",
              "2048", "-validity", "10000", "-storepass", pw, "-keypass", pw, "-dname",
              "CN=Nexus-Office, O=Nexus-Office, C=ES"});
    escribir(pw_file, pw, 0600);
  }
  return {ks, trim(read_text(pw_file))};
}

}  // namespace

int apk(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {});
  if (!a.tiene("exe") || !a.tiene("libs") || !a.tiene("recursos"))
    throw Fallo("uso: nexus-construir apk --exe E --libs D --recursos R [--gpu-libs G]");
  fs::path exe = P(a.val("exe")), libs = P(a.val("libs")), resources = P(a.val("recursos"));
  std::vector<std::string> missing;
  std::vector<fs::path> need = {exe, resources / "nexus" / "web" / "index.html"};
  for (const char* n : LLAMA) need.push_back(libs / n);
  for (auto& p : need)
    if (!is_file(p)) missing.push_back(S(p));
  if (!missing.empty()) throw Fallo("faltan: " + join(missing, ", "));
  std::string aapt2 = tool("aapt2"), zipalign = tool("zipalign"), apksigner = tool("apksigner", true), d8 = tool("d8", true);
  if (aapt2.empty() || zipalign.empty() || apksigner.empty() || d8.empty())
    throw Fallo("faltan herramientas de Android (aapt2, zipalign, apksigner, d8)");
  std::string javac = jdk_tool("javac");
  std::string api_jar = S(android_jar());

  fs::path out = raiz() / "build-paquetes" / "apk-autonomo";
  rm_rf(out);
  for (const char* d : {"gen", "classes", "dex"}) fs::create_directories(out / d);
  fs::path app = out / "app";
  copiar_arbol(raiz() / "android", app, [](const std::string& rel) { return rel == ".gitignore" || rel == "build"; });

  // la carpeta nexus/ comprimida y {"files", "bytes"}: la app lo comprueba tras descomprimir
  fs::path runtime = out / "runtime.zip";
  json meta;
  {
    archive::ZipWriter z(runtime);
    for (auto& f : ficheros(resources / "nexus")) z.add_file(fs::relative(f, resources).generic_string(), f, 9);
    meta = {{"files", z.count()}, {"bytes", z.bytes()}, {"version", version()}};
    if (!z.close()) throw Fallo("no puedo escribir " + S(runtime));
  }
  log("apk", cat("runtime: ", meta["files"].get<uint64_t>(), " ficheros, ",
                 fnum((double)meta["bytes"].get<uint64_t>() / 1e6, 0), " MB -> ", mb(runtime)));

  ejecutar({aapt2, "compile", "--dir", S(app / "res"), "-o", S(out / "res.zip")});
  ejecutar({aapt2, "link", "-o", S(out / "base.apk"), "-I", api_jar, "--manifest", S(app / "AndroidManifest.xml"),
            "--rename-manifest-package", APP_ID, "-A", S(app / "assets"), "--java", S(out / "gen"),
            "--min-sdk-version", std::to_string(MIN_SDK), "--target-sdk-version", "34", "--version-code",
            std::to_string(VERSION_CODE), "--version-name", version(), S(out / "res.zip")});
  std::vector<std::string> cmd = {javac, "-encoding", "UTF-8", "--release", "8", "-Xlint:-options", "-nowarn",
                                  "-cp", api_jar, "-d", S(out / "classes")};
  for (auto& dir : {app / "src", out / "gen"})
    for (auto& f : ficheros(dir))
      if (suffix_lower(f) == ".java") cmd.push_back(S(f));
  ejecutar(cmd);
  cmd = {d8, "--release", "--min-api", std::to_string(MIN_SDK), "--lib", api_jar, "--output", S(out / "dex")};
  for (auto& f : ficheros(out / "classes"))
    if (suffix_lower(f) == ".class") cmd.push_back(S(f));
  ejecutar(cmd);

  {
    archive::ZipWriter z(out / "base.apk", true);
    z.add_file("classes.dex", out / "dex" / "classes.dex");
    z.add_file("lib/arm64-v8a/libnexusoffice.so", exe);
    for (const char* n : LLAMA) z.add_file(std::string("lib/arm64-v8a/") + n, libs / n);
    if (a.tiene("gpu-libs")) {   // GPU y NPU: llama-server-gpu y los backends de ggml que carga a demanda
      fs::path g = P(a.val("gpu-libs"));
      auto so = glob(g, "*.so");
      std::sort(so.begin(), so.end());
      bool gpu = false;
      uint64_t bytes = 0;
      for (auto& f : so) {
        gpu = gpu || f.filename() == "libllama-server-gpu.so";
        bytes += fsize(f);
        z.add_file("lib/arm64-v8a/" + S(f.filename()), f);
      }
      if (!gpu) throw Fallo("falta libllama-server-gpu.so en " + S(g));
      log("apk", cat("GPU/NPU: ", so.size(), " bibliotecas (", fnum((double)bytes / 1e6, 0), " MB)"));
    }
    z.add_file("assets/runtime.zip", runtime, 0);
    z.add_bytes("assets/runtime.json", jdump(meta));
    z.add_bytes("assets/LICENSES/LEEME.txt",
                "Nexus-Office C++; llama.cpp y ggml (MIT), cpp-httplib (MIT), nlohmann/json (MIT), SQLite (dominio "
                "publico), miniz (MIT), PCRE2 (BSD), rapidyaml (MIT), stb (dominio publico/MIT), mbedTLS (Apache 2.0), "
                "LLVM libc++ (Apache 2.0 con excepcion LLVM), musl (MIT).\n");
    if (!z.close()) throw Fallo("no puedo anadir los ficheros a la APK");
  }
  ejecutar({zipalign, "-f", "-p", "4", S(out / "base.apk"), S(out / "aligned.apk")});
  auto [ks, pw] = keystore();
  fs::path dest = raiz() / "dist-apk" / cat("Nexus-Office-", version(), "-android-autonomo.apk");
  fs::create_directories(dest.parent_path());
  ejecutar({apksigner, "sign", "--ks", S(ks), "--ks-key-alias", "nexus", "--ks-pass", "pass:" + pw, "--key-pass",
            "pass:" + pw, "--min-sdk-version", std::to_string(MIN_SDK), "--out", S(dest), S(out / "aligned.apk")});
  ejecutar({apksigner, "verify", "--min-sdk-version", std::to_string(MIN_SDK), S(dest)});
  log("apk", "-> " + S(dest) + " (" + mb(dest) + ")");
  return 0;
}

}  // namespace nc
