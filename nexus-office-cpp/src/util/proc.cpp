#include "util/proc.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

namespace nx::proc {

namespace {
std::mutex mu;
std::vector<Process*> live;

void track(Process* p) {
  std::lock_guard<std::mutex> lk(mu);
  live.push_back(p);
}
void untrack(Process* p) {
  std::lock_guard<std::mutex> lk(mu);
  std::erase(live, p);
}

#ifdef _WIN32
HANDLE job() {
  static HANDLE j = [] {
    HANDLE h = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof info);
    return h;
  }();
  return j;
}

std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring o = L"\"";
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
    if (i == a.size()) { o.append(bs * 2, L'\\'); break; }
    if (a[i] == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
    else { o.append(bs, L'\\'); o += a[i]; }
  }
  return o + L"\"";
}

std::wstring env_block(const std::map<std::string, std::string>& extra) {
  std::map<std::wstring, std::wstring, std::less<>> vars;
  wchar_t* cur = GetEnvironmentStringsW();
  for (wchar_t* p = cur; *p; p += wcslen(p) + 1) {
    std::wstring s(p);
    size_t eq = s.find(L'=', 1);
    if (eq != std::wstring::npos) vars[s.substr(0, eq)] = s.substr(eq + 1);
  }
  FreeEnvironmentStringsW(cur);
  for (auto& [k, v] : extra) {
    std::wstring wk = P(k).wstring();
    // las claves de entorno en Windows no distinguen mayusculas
    for (auto it = vars.begin(); it != vars.end();) {
      if (_wcsicmp(it->first.c_str(), wk.c_str()) == 0) it = vars.erase(it);
      else ++it;
    }
    if (!v.empty()) vars[wk] = P(v).wstring();   // vacio = quitar la variable
  }
  std::wstring b;
  for (auto& [k, v] : vars) b += k + L"=" + v + L'\0';
  b += L'\0';
  return b;
}
#endif
}  // namespace

Process::~Process() {
  untrack(this);
#ifdef _WIN32
  if (handle_) CloseHandle((HANDLE)handle_);
#endif
}

#ifdef _WIN32
struct RunImpl {
  static bool spawn(Process& pr, const std::vector<std::string>& args, const Options& opt, HANDLE in, HANDLE out,
                    HANDLE err) {
    std::wstring cmd;
    for (auto& a : args) {
      if (!cmd.empty()) cmd += L' ';
      cmd += quote_arg(P(a).wstring());
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    PROCESS_INFORMATION pi{};
    std::wstring envb = env_block(opt.env);
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (opt.hide_window) flags |= CREATE_NO_WINDOW;
    if (opt.detached) flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    std::wstring cwd = opt.cwd.empty() ? L"" : opt.cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, envb.data(),
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok && opt.detached) {   // sin permiso para salir del Job: sin BREAKAWAY
      flags &= ~CREATE_BREAKAWAY_FROM_JOB;
      ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, envb.data(),
                          cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    }
    if (!ok) {
      pr.err_ = cat("no se pudo ejecutar ", args.empty() ? "" : args[0], " (error ", GetLastError(), ")");
      return false;
    }
    if (!opt.detached) AssignProcessToJobObject(job(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    pr.handle_ = pi.hProcess;
    pr.pid_ = (long)pi.dwProcessId;
    return true;
  }
};

bool Process::start(const std::vector<std::string>& args, const Options& opt) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  HANDLE out;
  if (!opt.output_file.empty()) {
    mkdirs(opt.output_file.parent_path());
    out = CreateFileW(opt.output_file.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  } else {
    out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  }
  bool ok = RunImpl::spawn(*this, args, opt, nul_in, out, out);
  CloseHandle(nul_in);
  CloseHandle(out);
  if (ok && !opt.detached) track(this);
  return ok;
}

std::optional<int> Process::poll() {
  if (code_) return code_;
  if (!handle_) return -1;
  DWORD c;
  if (WaitForSingleObject((HANDLE)handle_, 0) == WAIT_OBJECT_0 && GetExitCodeProcess((HANDLE)handle_, &c)) {
    code_ = (int)c;
    untrack(this);
  }
  return code_;
}

std::optional<int> Process::wait(double timeout_s) {
  if (code_) return code_;
  if (!handle_) return -1;
  WaitForSingleObject((HANDLE)handle_, timeout_s < 0 ? INFINITE : (DWORD)(timeout_s * 1000));
  return poll();
}

void Process::terminate() {
  if (handle_ && !poll()) TerminateProcess((HANDLE)handle_, 1);
}
void Process::kill() { terminate(); }

#else  // ------------------------------------------------------------ POSIX

struct RunImpl {
  // fork + exec: en el hijo solo llamadas seguras tras fork (todo se prepara antes)
  static bool spawn(Process& pr, const std::vector<std::string>& args, const Options& opt, int in_fd, int out_fd,
                    int err_fd) {
    if (args.empty()) {
      pr.err_ = "orden vacia";
      return false;
    }
    std::vector<std::string> envs;
    for (char** e = environ; *e; ++e) {
      std::string s(*e);
      auto k = s.substr(0, s.find('='));
      if (!opt.env.count(k)) envs.push_back(s);
    }
    for (auto& [k, v] : opt.env)
      if (!v.empty()) envs.push_back(k + "=" + v);   // vacio = quitar la variable
    std::vector<char*> argv, envp;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (auto& e : envs) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    // resolver la ruta antes del fork (execvpe no existe en todas las libc)
    std::string exe = args[0];
    if (exe.find('/') == std::string::npos) {
      std::string w = which(exe);
      if (!w.empty()) exe = w;
    }
    std::string cwd = opt.cwd.empty() ? "" : S(opt.cwd);
    int errpipe[2];
    if (pipe(errpipe) != 0) return false;
    fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
      pr.err_ = "fork fallo";
      close(errpipe[0]);
      close(errpipe[1]);
      return false;
    }
    if (pid == 0) {
      close(errpipe[0]);
      if (opt.detached) setsid();
#ifdef __linux__
      else {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(127);
      }
#endif
      (void)parent;
      if (in_fd >= 0) dup2(in_fd, 0);
      if (out_fd >= 0) dup2(out_fd, 1);
      if (err_fd >= 0) dup2(err_fd, 2);
      for (int fd = 3; fd < 256; ++fd)
        if (fd != errpipe[1]) close(fd);
      if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
        int e = errno;
        (void)!::write(errpipe[1], &e, sizeof e);
        _exit(127);
      }
      execve(exe.c_str(), argv.data(), envp.data());
      int e = errno;
      (void)!::write(errpipe[1], &e, sizeof e);
      _exit(127);
    }
    close(errpipe[1]);
    int child_errno = 0;
    ssize_t n;
    do n = read(errpipe[0], &child_errno, sizeof child_errno);
    while (n < 0 && errno == EINTR);
    close(errpipe[0]);
    if (n > 0) {
      waitpid(pid, nullptr, 0);
      pr.err_ = cat("no se pudo ejecutar ", args[0], ": ", std::strerror(child_errno));
      return false;
    }
    pr.pid_ = pid;
    return true;
  }
};

bool Process::start(const std::vector<std::string>& args, const Options& opt) {
  int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
  int out;
  if (!opt.output_file.empty()) {
    mkdirs(opt.output_file.parent_path());
    out = open(S(opt.output_file).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  } else {
    out = open("/dev/null", O_WRONLY | O_CLOEXEC);
  }
  bool ok = RunImpl::spawn(*this, args, opt, in, out, out);
  if (in >= 0) close(in);
  if (out >= 0) close(out);
  if (ok && !opt.detached) track(this);
  return ok;
}

std::optional<int> Process::poll() {
  if (code_) return code_;
  if (pid_ <= 0) return -1;
  int st = 0;
  pid_t r = waitpid((pid_t)pid_, &st, WNOHANG);
  if (r == (pid_t)pid_) {
    code_ = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? -WTERMSIG(st) : -1);
    untrack(this);
  } else if (r < 0) {
    code_ = -1;
    untrack(this);
  }
  return code_;
}

std::optional<int> Process::wait(double timeout_s) {
  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    if (auto c = poll()) return c;
    if (timeout_s >= 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= timeout_s)
      return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Process::terminate() {
  if (pid_ > 0 && !poll()) ::kill((pid_t)pid_, SIGTERM);
}
void Process::kill() {
  if (pid_ > 0 && !poll()) ::kill((pid_t)pid_, SIGKILL);
}
#endif

// ---------------------------------------------------------------- run con captura
Result run(const std::vector<std::string>& args, double timeout_s, const Options& opt) {
  Result r;
  Process p;
  std::string out, err;
  std::thread to, te, ti;
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
  CreatePipe(&in_r, &in_w, &sa, 0);
  CreatePipe(&out_r, &out_w, &sa, 0);
  CreatePipe(&err_r, &err_w, &sa, 0);
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  bool ok = RunImpl::spawn(p, args, opt, in_r, out_w, err_w);
  CloseHandle(in_r);
  CloseHandle(out_w);
  CloseHandle(err_w);
  if (!ok) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(err_r);
    r.err = p.error();
    return r;
  }
  track(&p);
  auto reader = [](HANDLE h, std::string* dst) {
    char buf[8192];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) dst->append(buf, n);
    CloseHandle(h);
  };
  to = std::thread(reader, out_r, &out);
  te = std::thread(reader, err_r, &err);
  ti = std::thread([in_w, data = opt.input] {
    DWORD n;
    if (!data.empty()) WriteFile(in_w, data.data(), (DWORD)data.size(), &n, nullptr);
    CloseHandle(in_w);
  });
#else
  int pin[2], pout[2], perr[2];
  if (pipe(pin) || pipe(pout) || pipe(perr)) {
    r.err = "pipe fallo";
    return r;
  }
  for (int fd : {pin[1], pout[0], perr[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
  bool ok = RunImpl::spawn(p, args, opt, pin[0], pout[1], perr[1]);
  close(pin[0]);
  close(pout[1]);
  close(perr[1]);
  if (!ok) {
    close(pin[1]);
    close(pout[0]);
    close(perr[0]);
    r.err = p.error();
    return r;
  }
  track(&p);
  auto reader = [](int fd, std::string* dst) {
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
      if (n > 0) dst->append(buf, (size_t)n);
    close(fd);
  };
  to = std::thread(reader, pout[0], &out);
  te = std::thread(reader, perr[0], &err);
  ti = std::thread([fd = pin[1], data = opt.input] {
    signal(SIGPIPE, SIG_IGN);
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = write(fd, data.data() + off, data.size() - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
    close(fd);
  });
#endif
  r.started = true;
  auto code = p.wait(timeout_s);
  if (!code) {
    r.timed_out = true;
    p.kill();
    p.wait(5);
  }
  ti.join();
  to.join();
  te.join();
  r.code = code.value_or(-1);
  r.out = std::move(out);
  r.err = std::move(err);
  return r;
}

std::string which(const std::string& name) {
#ifdef _WIN32
  const char sep = ';';
  std::vector<std::string> exts = {"", ".exe", ".cmd", ".bat"};
#else
  const char sep = ':';
  std::vector<std::string> exts = {""};
#endif
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
    return is_file(P(name)) ? name : "";
  for (auto& dir : split(env("PATH"), std::string(1, sep))) {
    if (dir.empty()) continue;
    for (auto& e : exts) {
      fs::path c = P(dir) / P(name + e);
      if (is_file(c)) {
#ifndef _WIN32
        if (access(S(c).c_str(), X_OK) != 0) continue;
#endif
        return S(c);
      }
    }
  }
  return "";
}

void cleanup() {
  std::vector<Process*> ps;
  {
    std::lock_guard<std::mutex> lk(mu);
    ps = live;
  }
  for (auto* p : ps) p->terminate();
}

}  // namespace nx::proc
