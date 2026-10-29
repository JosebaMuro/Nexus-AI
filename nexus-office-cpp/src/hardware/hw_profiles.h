// Perfiles de hardware configurables (nexus/hardware/hw_profiles.py): motor, acelerador, GPU, modelo y donde va
// cada cosa (VRAM o RAM).
//
// motor: auto · llamaserver · llamacpp; acelerador: auto · cpu · gpu · npu; gpu: cual usar si hay varias (nombre o
// dispositivo de llama.cpp, p. ej. «Vulkan1»); modelo: el de texto que se activa (vacio = el que haya); capas_gpu:
// «auto», «todas», «ninguna» o un numero de capas; kv: «auto», «vram» o «ram»; mlock: el modelo fijo en la RAM;
// contexto, hilos y lote (0 = automatico; contexto -1 = el maximo; hilos negativos = porcentaje de los automaticos).
// Hay perfiles de serie (Automatico, Rendimiento, Equilibrado, Modelo grande, Ahorro, Solo CPU) y los del usuario.
#pragma once
#include <stdexcept>
#include <string>

#include "core/config.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::hw::profiles {

extern const json CAMPOS;
extern const json DE_SERIE;

json norm(const json& p);
json lista(Config& cfg);
json get(Config& cfg, const std::string& pid);   // null si no existe
json guardar(Config& cfg, const json& p);
bool borrar(Config& cfg, const std::string& pid);
json& superponer(json& profile, const json& p);   // ajustes del perfil sobre el perfil calculado
void al_arrancar(Config& cfg);                    // antes de construir el perfil: la GPU elegida
void aplicar_al_perfil(Services& s);              // despues: superpone el perfil activo
json activar(const std::string& pid, Config& cfg, Services* s = nullptr);   // std::invalid_argument si no existe
json elegir_gpu(Services& s, const std::string& gpu);
// capas del modelo en la GPU o la NPU elegidas en la configuracion (hardware.capas): «auto», «todas», «ninguna» o un
// numero. Van por encima del perfil activo; solo cuentan si hay GPU o NPU (si no, todo sigue en la CPU)
json norm_capas(const json& v);                    // valor valido o std::invalid_argument
void aplicar_capas(json& profile, Config& cfg);
json gpus(Services& s);

}  // namespace nx::hw::profiles
