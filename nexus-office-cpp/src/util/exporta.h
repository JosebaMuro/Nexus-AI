// Datos globales que se usan fuera del nucleo (nexus-office.exe, nexus-construir) cuando el nucleo es una DLL
// (NEXUS_CORE_SHARED en Windows): MSVC exporta solo funciones de forma automatica, los datos hay que marcarlos.
#pragma once
#if defined(_WIN32) && defined(NEXUS_CORE_SHARED)
#if defined(NEXUS_CORE_BUILD)
#define NX_DATOS __declspec(dllexport)
#else
#define NX_DATOS __declspec(dllimport)
#endif
#else
#define NX_DATOS
#endif
