// Dispositivos de llama.cpp (llama-server --list-devices) y eleccion de UNA GPU cuando hay varias
// (nexus/hardware/gpu_devices.py).
//
// Con dos tarjetas (o una integrada y otra dedicada) llama.cpp reparte el modelo entre todas las que ve, y en Linux
// Vulkan ademas ofrece «llvmpipe», un renderizador por software mas lento que la CPU. Aqui se lee la lista real del
// binario que se va a usar, se descartan los de software y se elige uno: el que diga el usuario o, si no, la
// dedicada con mas memoria. Se pasa a llama-server con --device.
#pragma once
#include <map>
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx::hw::gpu_devices {

json parse(const std::string& text);   // [{id, backend, name, mem_mb, free_mb, software, integrated}]
json list_devices(const fs::path& exe, const std::map<std::string, std::string>& env = {});   // cacheado por binario
bool matches(const json& dev, const std::string& want);
json pick(const json& devs, const std::string& preferred = "");   // null si solo hay de software
bool needs_device_flag(const json& devs);

}  // namespace nx::hw::gpu_devices
