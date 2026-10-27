// CPU, GPU y NPU del movil en la app Android autonoma (hardware/android_accel.py). Ver android_accel.cpp.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::hw::android_accel {

inline const char* GPU_EXE = "libllama-server-gpu.so";
using LibFinder = std::function<std::string(const std::vector<std::string>&)>;

json props();
json device_info(const fs::path& path = {});
std::string vendor_lib(const std::vector<std::string>& names, const std::vector<std::string>& dirs = {});
json soc(const json& props, const json& info);
json npu(const json& soc, const LibFinder& lib = nullptr);
std::map<std::string, std::string> gpu_env(const std::string& libs);
json parse_devices(const std::string& out);
std::string gpu_vendor(const std::string& name);
bool is_npu_dev(const std::string& dev);
json pick(const json& devs, bool use_npu = true, bool use_gpu = true);
json probe_gpu(const std::string& libs, const std::string& exe_name = GPU_EXE);
json detect(const std::optional<std::string>& libs = std::nullopt, bool use_cache = true);
void mark_gpu_broken(const std::string& reason, const std::string& device = "");
bool is_gpu_exe(const fs::path& exe);

}  // namespace nx::hw::android_accel
