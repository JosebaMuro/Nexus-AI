// SQLite compartido (WAL) con el mismo esquema que la version Python: las dos ediciones abren la misma base.
// Los parametros y filas son JSON; los BLOB (embeddings) van como json::binary.
#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

struct sqlite3;

namespace nx {

using Params = std::vector<json>;
using Row = json;   // objeto {columna: valor}

json blob(const std::string& bytes);
json blob(const std::vector<float>& v);
std::vector<float> floats(const json& b);   // np.frombuffer(b, float32)

class DbError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class DB {
 public:
  explicit DB(const fs::path& path = {});
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // ejecuta y confirma (salvo dentro de transaction); devuelve filas cambiadas
  int execute(const std::string& sql, const Params& p = {});
  long long last_insert_id();
  std::optional<Row> returning(const std::string& sql, const Params& p = {});
  void executemany(const std::string& sql, const std::vector<Params>& rows);
  std::vector<Row> query(const std::string& sql, const Params& p = {});
  std::optional<Row> one(const std::string& sql, const Params& p = {});
  json scalar(const std::string& sql, const Params& p = {});   // primera columna de la primera fila (o null)
  void script(const std::string& sql);

  // agrupa escrituras en una confirmacion; si f lanza, se deshace todo
  void transaction(const std::function<void()>& f);

  json get_profile();
  void set_profile(const std::string& key, const json& value);

  const fs::path& path() const { return path_; }
  std::recursive_mutex& mutex() { return mu_; }

 private:
  sqlite3* db_ = nullptr;
  fs::path path_;
  std::recursive_mutex mu_;
  int depth_ = 0;
  std::vector<Row> run(const std::string& sql, const Params& p, bool want_rows, int* changes);
  void commit_if_needed();
  void migrate();
  std::vector<std::string> columns(const std::string& table);
};

}  // namespace nx
