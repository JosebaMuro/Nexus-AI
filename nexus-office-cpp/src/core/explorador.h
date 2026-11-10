// Explorador de archivos para importar modelos (vista Modelos > Importar modelo local).
//
// * dialogo(): el selector de archivos del sistema (Windows: IFileOpenDialog; macOS: choose file; Linux: zenity o
//   kdialog). Solo si Nexus-Office corre en el escritorio de quien lo usa: no desde el puerto remoto, ni como servicio
//   (sesion 0 en Windows, sin DISPLAY en Linux), ni en Android.
// * listar(): el explorador propio de la interfaz, que va en todas partes (Android, servicio, acceso remoto): carpetas
//   y ficheros de modelo de una carpeta, con las unidades y carpetas habituales como atajos.
#pragma once
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx::explorador {

// formato de modelo de un fichero (gguf, onnx, safetensors, litertlm...) o de una carpeta (onnx-genai, genie,
// safetensors con config.json); "" si no lo es
std::string formato_modelo(const fs::path& p);

// {path, parent, dirs:[{name, path, modelo}], files:[{name, path, size, formato}], roots:[{name, path}]}.
// Sin ruta: la carpeta personal. todos=false deja solo los ficheros de modelo.
json listar(const std::string& ruta, bool todos = false);
json raices();

bool dialogo_disponible();
// {path} | {cancelled:true} | {unavailable:true, detail}; carpeta=true elige una carpeta (ONNX GenAI, Genie...)
json dialogo(bool carpeta);

}  // namespace nx::explorador
