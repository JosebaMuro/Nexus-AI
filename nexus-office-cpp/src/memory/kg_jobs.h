// Trabajos de importacion de grafos y de razonado de archivos comprimidos (memory/kg_jobs.py _run_import y
// _run_archive). Se ejecutan en segundo plano desde KgJobs en server/routes.cpp.
#pragma once
#include <functional>
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::kg {

using StepFn = std::function<void(const std::string& text, long done, long total)>;

// ""/"general" -> general; "auto" -> el organizador decide; nombre de agente -> ese agente
std::string resolve_target(Services& s, const std::string& target);
json run_import(Services& s, const json& parsed, const std::string& name, const std::string& target, const StepFn& step);
json run_archive(Services& s, const fs::path& path, const std::string& name, const std::string& target, bool reason,
                 bool rag, int max_reason, const StepFn& step);

}  // namespace nx::kg
