// Capa de abstraccion de hardware: nucleos del procesador.
//
// Los moviles (big.LITTLE / DynamIQ) y los Intel hibridos (P-cores y E-cores) tienen nucleos de distinta velocidad. Los
// hilos de inferencia (difusion, motores dentro del proceso, percepcion) van mas rapido fijados a los nucleos grandes:
// si el planificador los pone en un nucleo pequeno, el paso entero espera al hilo mas lento.
#pragma once
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::hal {

struct Nucleos {
  std::vector<int> grandes;    // indices de CPU logica de los nucleos rapidos
  std::vector<int> pequenos;   // el resto
  std::vector<unsigned long> ids_grandes;   // Windows: Ids de cpu-set de los grandes (SetThreadSelectedCpuSets)
  int logicos = 0;
  std::string fuente;          // cpufreq | cpu-sets | perflevel | ninguna
};

// nucleos del equipo (se lee una vez)
const Nucleos& nucleos();
// cuantos hilos de calculo conviene usar: los nucleos grandes (minimo 1)
int hilos_calculo();
// fija el hilo actual a los nucleos grandes; false si el sistema no lo permite o no hay distincion
bool fijar_hilo_grandes();
json estado_nucleos();
// mascara hexadecimal de los nucleos grandes para llama.cpp (--cpu-mask); "" si no hay distincion
std::string mascara_grandes();

}  // namespace nx::hal
