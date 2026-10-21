#include "core/db.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>

#include "util/crypto.h"
#include "util/text.h"

namespace nx {

static const char* SCHEMA = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY, title TEXT, created REAL, updated REAL);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT, role TEXT, content TEXT,
    agent TEXT, ts REAL, meta TEXT);
CREATE INDEX IF NOT EXISTS ix_messages_session ON messages(session_id, id);
CREATE TABLE IF NOT EXISTS memories (
    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, content TEXT, importance REAL,
    embedding BLOB, embedder TEXT, created REAL, last_access REAL, access_count INTEGER DEFAULT 0,
    source TEXT, agent TEXT);
CREATE INDEX IF NOT EXISTS ix_memories_kind ON memories(kind);
CREATE TABLE IF NOT EXISTS entities (
    id INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL DEFAULT '', key TEXT, name TEXT, type TEXT,
    weight REAL DEFAULT 1, mentions INTEGER DEFAULT 1, first_seen REAL, last_seen REAL,
    category TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', source TEXT NOT NULL DEFAULT '', UNIQUE(scope, key));
CREATE TABLE IF NOT EXISTS relations (
    id INTEGER PRIMARY KEY AUTOINCREMENT, src INTEGER, dst INTEGER, rel TEXT,
    weight REAL DEFAULT 1, last_seen REAL, scope TEXT NOT NULL DEFAULT '', UNIQUE(src, dst, rel));
CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT, message_id INTEGER, score INTEGER, comment TEXT, ts REAL);
CREATE TABLE IF NOT EXISTS profile (key TEXT PRIMARY KEY, value TEXT, updated REAL);
CREATE TABLE IF NOT EXISTS lounge (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, agent TEXT, kind TEXT, content TEXT, meta TEXT);
CREATE TABLE IF NOT EXISTS agent_skills (
    agent TEXT, topic TEXT, ok REAL DEFAULT 0, fail REAL DEFAULT 0, last REAL, PRIMARY KEY(agent, topic));
CREATE TABLE IF NOT EXISTS agent_proposals (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, spec TEXT, reason TEXT, status TEXT, proposer TEXT);
CREATE TABLE IF NOT EXISTS rag_docs (
    id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, source TEXT, path TEXT, kind TEXT, pages INTEGER,
    chars INTEGER, added REAL, meta TEXT, agent TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS rag_chunks (
    id INTEGER PRIMARY KEY AUTOINCREMENT, doc_id INTEGER, idx INTEGER, page INTEGER, text TEXT,
    embedding BLOB, embedder TEXT);
CREATE INDEX IF NOT EXISTS ix_rag_chunks_doc ON rag_chunks(doc_id);
CREATE TABLE IF NOT EXISTS entity_vecs (
    entity_id INTEGER PRIMARY KEY, embedder TEXT, key TEXT, vec BLOB);
CREATE TABLE IF NOT EXISTS tool_calls (
    id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, agent TEXT, session_id TEXT, tool TEXT, args TEXT,
    risk TEXT, decision TEXT, approver TEXT, status TEXT, result TEXT, duration REAL);
)SQL";

static const char* INDEXES = R"SQL(
CREATE INDEX IF NOT EXISTS ix_entities_scope ON entities(scope, weight DESC);
CREATE INDEX IF NOT EXISTS ix_relations_src ON relations(src);
CREATE INDEX IF NOT EXISTS ix_relations_dst ON relations(dst);
CREATE INDEX IF NOT EXISTS ix_relations_scope ON relations(scope, weight DESC);
CREATE INDEX IF NOT EXISTS ix_rag_docs_agent ON rag_docs(agent);
CREATE INDEX IF NOT EXISTS ix_entities_category ON entities(scope, category);
CREATE INDEX IF NOT EXISTS ix_rag_chunks_embedder ON rag_chunks(embedder);
)SQL";

json blob(const std::string& bytes) { return json::binary(json::binary_t::container_type(bytes.begin(), bytes.end())); }

json blob(const std::vector<float>& v) {
  json::binary_t::container_type c(v.size() * 4);
  if (!v.empty()) std::memcpy(c.data(), v.data(), c.size());
  return json::binary(std::move(c));
}

std::vector<float> floats(const json& b) {
  std::vector<float> out;
  if (b.is_binary()) {
    auto& c = b.get_binary();
    out.resize(c.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), c.data(), out.size() * 4);
  } else if (b.is_string()) {   // por si una columna BLOB vino como texto
    auto& s = b.get_ref<const std::string&>();
    out.resize(s.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), s.data(), out.size() * 4);
  }
  return out;
}

DB::DB(const fs::path& path) : path_(path.empty() ? paths::db_file() : path) {
  mkdirs(path_.parent_path());
  if (sqlite3_open_v2(S(path_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK)
    throw DbError("no se pudo abrir la base de datos " + S(path_));
  sqlite3_busy_timeout(db_, 5000);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  script("PRAGMA journal_mode=WAL");
  script("PRAGMA synchronous=NORMAL");
  script(SCHEMA);
  migrate();
  script(INDEXES);
}

DB::~DB() {
  if (db_) sqlite3_close_v2(db_);
}

std::vector<std::string> DB::columns(const std::string& table) {
  std::vector<std::string> out;
  for (auto& r : query("PRAGMA table_info(" + table + ")")) out.push_back(r["name"].get<std::string>());
  return out;
}

void DB::migrate() {
  auto has = [&](const std::string& t, const std::string& c) {
    auto cols = columns(t);
    return std::find(cols.begin(), cols.end(), c) != cols.end();
  };
  if (!has("entities", "scope")) {
    script(R"SQL(
      BEGIN;
      ALTER TABLE entities RENAME TO entities_v1;
      CREATE TABLE entities (
          id INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL DEFAULT '', key TEXT, name TEXT, type TEXT,
          weight REAL DEFAULT 1, mentions INTEGER DEFAULT 1, first_seen REAL, last_seen REAL, UNIQUE(scope, key));
      INSERT INTO entities(id, scope, key, name, type, weight, mentions, first_seen, last_seen)
          SELECT id, '', key, name, type, weight, mentions, first_seen, last_seen FROM entities_v1;
      DROP TABLE entities_v1;
      COMMIT;)SQL");
  }
  if (!has("relations", "scope")) execute("ALTER TABLE relations ADD COLUMN scope TEXT NOT NULL DEFAULT ''");
  if (!has("rag_docs", "agent")) execute("ALTER TABLE rag_docs ADD COLUMN agent TEXT NOT NULL DEFAULT ''");
  for (const char* col : {"category", "note", "source"})
    if (!has("entities", col)) execute(cat("ALTER TABLE entities ADD COLUMN ", col, " TEXT NOT NULL DEFAULT ''"));
}

void DB::script(const std::string& sql) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string e = err ? err : "error SQL";
    sqlite3_free(err);
    throw DbError(e);
  }
}

static void bind(sqlite3_stmt* st, int i, const json& v) {
  if (v.is_null()) sqlite3_bind_null(st, i);
  else if (v.is_boolean()) sqlite3_bind_int64(st, i, v.get<bool>() ? 1 : 0);
  else if (v.is_number_integer()) sqlite3_bind_int64(st, i, v.get<long long>());
  else if (v.is_number_unsigned()) sqlite3_bind_int64(st, i, (long long)v.get<unsigned long long>());
  else if (v.is_number_float()) sqlite3_bind_double(st, i, v.get<double>());
  else if (v.is_string()) {
    auto& s = v.get_ref<const std::string&>();
    sqlite3_bind_text(st, i, s.data(), (int)s.size(), SQLITE_TRANSIENT);
  } else if (v.is_binary()) {
    auto& b = v.get_binary();
    sqlite3_bind_blob(st, i, b.empty() ? "" : (const void*)b.data(), (int)b.size(), SQLITE_TRANSIENT);
  } else {
    std::string s = jdump(v);
    sqlite3_bind_text(st, i, s.data(), (int)s.size(), SQLITE_TRANSIENT);
  }
}

static json column(sqlite3_stmt* st, int i) {
  switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER: return (long long)sqlite3_column_int64(st, i);
    case SQLITE_FLOAT: return sqlite3_column_double(st, i);
    case SQLITE_TEXT: {
      const char* t = (const char*)sqlite3_column_text(st, i);
      return std::string(t, (size_t)sqlite3_column_bytes(st, i));
    }
    case SQLITE_BLOB: {
      const unsigned char* b = (const unsigned char*)sqlite3_column_blob(st, i);
      int n = sqlite3_column_bytes(st, i);
      return json::binary(json::binary_t::container_type(b, b + n));
    }
    default: return nullptr;
  }
}

std::vector<Row> DB::run(const std::string& sql, const Params& p, bool want_rows, int* changes) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &st, nullptr) != SQLITE_OK)
    throw DbError(std::string(sqlite3_errmsg(db_)) + " en: " + sql.substr(0, 200));
  for (size_t i = 0; i < p.size(); ++i) bind(st, (int)i + 1, p[i]);
  std::vector<Row> rows;
  int ncol = sqlite3_column_count(st);
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (!want_rows) continue;
    Row r = json::object();
    for (int c = 0; c < ncol; ++c) r[sqlite3_column_name(st, c)] = column(st, c);
    rows.push_back(std::move(r));
  }
  if (rc != SQLITE_DONE) {
    std::string e = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    throw DbError(e);
  }
  sqlite3_finalize(st);
  if (changes) *changes = sqlite3_changes(db_);
  return rows;
}

void DB::commit_if_needed() {
  if (!depth_ && !sqlite3_get_autocommit(db_)) script("COMMIT");
}

int DB::execute(const std::string& sql, const Params& p) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  int ch = 0;
  run(sql, p, false, &ch);
  commit_if_needed();
  return ch;
}

long long DB::last_insert_id() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return sqlite3_last_insert_rowid(db_);
}

std::optional<Row> DB::returning(const std::string& sql, const Params& p) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto rows = run(sql, p, true, nullptr);
  commit_if_needed();
  if (rows.empty()) return std::nullopt;
  return rows[0];
}

void DB::executemany(const std::string& sql, const std::vector<Params>& rows) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  bool own = depth_ == 0 && sqlite3_get_autocommit(db_);
  if (own) script("BEGIN");
  try {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &st, nullptr) != SQLITE_OK)
      throw DbError(sqlite3_errmsg(db_));
    for (auto& p : rows) {
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
      for (size_t i = 0; i < p.size(); ++i) bind(st, (int)i + 1, p[i]);
      int rc;
      while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      }
      if (rc != SQLITE_DONE) {
        std::string e = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        throw DbError(e);
      }
    }
    sqlite3_finalize(st);
  } catch (...) {
    if (own) script("ROLLBACK");
    throw;
  }
  if (own) script("COMMIT");
}

std::vector<Row> DB::query(const std::string& sql, const Params& p) { return run(sql, p, true, nullptr); }

std::optional<Row> DB::one(const std::string& sql, const Params& p) {
  auto rows = query(sql, p);
  if (rows.empty()) return std::nullopt;
  return rows[0];
}

json DB::scalar(const std::string& sql, const Params& p) {
  auto r = one(sql, p);
  if (!r || r->empty()) return nullptr;
  return r->begin().value();
}

void DB::transaction(const std::function<void()>& f) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (depth_ == 0 && sqlite3_get_autocommit(db_)) script("BEGIN");
  ++depth_;
  try {
    f();
  } catch (...) {
    --depth_;
    if (depth_ == 0 && !sqlite3_get_autocommit(db_)) script("ROLLBACK");
    throw;
  }
  --depth_;
  if (depth_ == 0 && !sqlite3_get_autocommit(db_)) script("COMMIT");
}

json DB::get_profile() {
  json out = json::object();
  for (auto& r : query("SELECT key, value FROM profile")) {
    json v = jparse(r["value"].is_string() ? r["value"].get<std::string>() : "null");
    out[r["key"].get<std::string>()] = v.is_discarded() ? json(nullptr) : v;
  }
  return out;
}

void DB::set_profile(const std::string& key, const json& value) {
  execute(
      "INSERT INTO profile(key, value, updated) VALUES(?,?,?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated=excluded.updated",
      {key, jdump(value), now()});
}

}  // namespace nx
