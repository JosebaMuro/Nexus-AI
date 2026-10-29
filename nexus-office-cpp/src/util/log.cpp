#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>

#include "util/fs.h"

namespace nx::log {

static std::mutex mu;
static std::deque<std::string> ring;
static bool configured = false;
static bool quiet = false;
static fs::path file;

void setup() {
  std::lock_guard<std::mutex> lk(mu);
  if (configured) return;
  file = paths::logs_dir() / "nexus.log";
  configured = true;
}

void set_quiet(bool q) { quiet = q; }

static std::string stamp() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
  return buf;
}

static void rotate() {
  std::error_code ec;
  if (file.empty() || !fs::exists(file, ec) || fs::file_size(file, ec) < 2'000'000) return;
  for (int i = 3; i >= 1; --i) {
    fs::path src = file;
    if (i > 1) src += "." + std::to_string(i - 1);
    fs::path dst = file;
    dst += "." + std::to_string(i);
    if (fs::exists(src, ec)) fs::rename(src, dst, ec);
  }
}

void write(const char* level, const std::string& name, const std::string& msg) {
  if (std::string(level) == "DEBUG" && env("NEXUS_DEBUG").empty()) return;
  std::string lvl = level;
  lvl.resize(7, ' ');
  std::string line = stamp() + " " + lvl + " " + name + ": " + msg;
  std::lock_guard<std::mutex> lk(mu);
  ring.push_back(line);
  if (ring.size() > 500) ring.pop_front();
  if (!quiet) {
    std::cerr << line << "\n";
    std::cerr.flush();
  }
  if (configured) {
    rotate();
    std::ofstream f(file, std::ios::app | std::ios::binary);
    f << line << "\n";
  }
}

std::vector<std::string> recent(size_t n) {
  std::lock_guard<std::mutex> lk(mu);
  size_t start = ring.size() > n ? ring.size() - n : 0;
  return std::vector<std::string>(ring.begin() + (long)start, ring.end());
}

}  // namespace nx::log
