// Subprocesos (nexus/proc.py): los runtimes hijos (llama-server, sd-cli, piper...) mueren con Nexus-Office.
// Windows: Job Object con KILL_ON_JOB_CLOSE. Linux/Android: PR_SET_PDEATHSIG. macOS: limpieza al salir.
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "util/fs.h"

namespace nx::proc {

struct Options {
  fs::path cwd;
  std::map<std::string, std::string> env;   // variables anadidas o sustituidas
  fs::path output_file;                     // stdout y stderr al final de este fichero (si no, se descartan)
  bool capture = false;                     // capturar stdout/stderr (solo run)
  std::string input;                        // stdin (solo run)
  bool detached = false;                    // no ligar a la vida del proceso (servicio, navegador)
  bool hide_window = true;
};

class Process {
 public:
  Process() = default;
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  bool start(const std::vector<std::string>& args, const Options& opt = {});
  bool started() const { return pid_ > 0; }
  long pid() const { return pid_; }
  std::optional<int> poll();        // codigo de salida si ha terminado
  bool running() { return started() && !poll().has_value(); }
  std::optional<int> wait(double timeout_s = -1);   // nullopt si vence el tiempo
  void terminate();                 // SIGTERM / TerminateProcess
  void kill();
  const std::string& error() const { return err_; }

 private:
  long pid_ = 0;
  void* handle_ = nullptr;          // HANDLE en Windows
  std::optional<int> code_;
  std::string err_;
  friend struct RunImpl;
};

struct Result {
  bool started = false;
  bool timed_out = false;
  int code = -1;
  std::string out;
  std::string err;
  bool ok() const { return started && !timed_out && code == 0; }
};

// subprocess.run(capture_output=True, text=True, timeout=...)
Result run(const std::vector<std::string>& args, double timeout_s = 60, const Options& opt = {});

// Busca un ejecutable en el PATH (shutil.which). "" si no esta.
std::string which(const std::string& name);

// Termina todos los hijos ligados (al salir).
void cleanup();

}  // namespace nx::proc
