// Comprobacion y autorreparacion al arrancar.
//
// previa() va antes de cargar la configuracion y abrir la base de datos, para que un fichero danado no impida arrancar:
//   carpetas con permiso de escritura, recursos del paquete, config.yaml (se restaura la ultima buena), base de datos
//   (quick_check; si esta danada se rescatan sus tablas o se vuelve a la copia diaria), temporales viejos, registros
//   que crecen sin limite y espacio en disco.
// completa() va con los servicios en marcha, en segundo plano: modelos GGUF que no se pueden leer (se apartan a
//   models/danados), llama-server que de verdad arranque en este equipo (permisos, cuarentena de macOS, y si la variante
//   GPU no funciona se pasa a la CPU) y drivers USB-serie de las placas.
// Cuando todo va bien se guarda una copia de la configuracion y una copia diaria de la base de datos (las 3 ultimas).
// El informe queda en data/salud.json, en /api/salud y en `nexus-office comprobar`.
#pragma once
#include <string>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::salud {

// cada comprobacion: {id, nombre, estado: ok|reparado|aviso|error, detalle}
json previa(bool reparar = true);
json completa(Services& s, bool reparar = true);

// puerto para escuchar: el pedido si esta libre; si lo ocupa otro programa, el siguiente libre (y lo anota)
int puerto(const std::string& host, int port);

// copia de la configuracion buena y copia diaria de la base de datos
void guardar_copias();

// ultimo informe: {inicio, fin, comprobaciones: [...], resumen: {ok, reparado, aviso, error}}
json informe();

namespace detail {   // para las pruebas
json rescatar_bd(const fs::path& db, const fs::path& copia);   // {ok, tablas, perdidas: [...], copia}
fs::path carpeta_copias();
}

}  // namespace nx::salud
