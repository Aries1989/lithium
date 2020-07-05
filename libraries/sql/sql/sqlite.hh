#pragma once

#if defined(_MSC_VER)
#include <ciso646>
#endif

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <li/callable_traits/callable_traits.hh>
#include <li/metamap/metamap.hh>
#include <li/sql/sql_common.hh>
#include <li/sql/symbols.hh>
#include <li/sql/type_hashmap.hh>
#include <sqlite3.h>

namespace li {

struct sqlite_tag {};

template <typename T> struct tuple_remove_references_and_const;
template <typename... T> struct tuple_remove_references_and_const<std::tuple<T...>> {
  typedef std::tuple<std::remove_const_t<std::remove_reference_t<T>>...> type;
};

template <typename T>
using tuple_remove_references_and_const_t = typename tuple_remove_references_and_const<T>::type;

void free_sqlite3_statement(void* s) { sqlite3_finalize((sqlite3_stmt*)s); }

struct sqlite_statement {
  typedef std::shared_ptr<sqlite3_stmt> stmt_sptr;

  sqlite_statement() {}

  sqlite_statement(sqlite3* db, sqlite3_stmt* s)
      : db_(db), stmt_(s), stmt_sptr_(stmt_sptr(s, free_sqlite3_statement)) {}

  void flush_results() {}

  // Bind arguments to the request unknowns (marked with ?)
  template <typename... T> sql_result<sqlite_statement> operator()(T&&... args) {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    int i = 1;
    li::tuple_map(std::forward_as_tuple(args...), [&](auto& m) {
      int err;
      if ((err = this->bind(stmt_, i, m)) != SQLITE_OK)
        throw std::runtime_error(std::string("Sqlite error during binding: ") +
                                 sqlite3_errmsg(db_));
      i++;
    });

    last_step_ret_ = sqlite3_step(stmt_);
    if (last_step_ret_ != SQLITE_ROW and last_step_ret_ != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errstr(last_step_ret_));

    return sql_result<sqlite_statement>{*this};
  }

  // Read a tuple or a metamap.
  template <typename B> template <typename T> bool read(T&& t1) {

    // Throw is nothing to read.
    if (last_step_ret_ != SQLITE_ROW)
      return false;

    // Tuple
    if constexpr (is_tuple<T>::value) {
      int ncols = sqlite3_column_count(stmt_);
      if (ncols != sizeof...(A)) {
        std::ostringstream ss;
        ss << "Invalid number of parameters: SQL request has " << ncols
           << " fields but the function to process it has " << sizeof...(A) << " parameters.";
        throw std::runtime_error(ss.str());
      }
      int i = 0;
      auto read_elt = [&](auto& v) {
        this->read_column(i, v);
        i++;
      };
      ::li::tuple_map(std::forward<T>(o), read_elt);
    } else // Metamap
    {
      int ncols = sqlite3_column_count(stmt_);
      int filled[sizeof...(A)];
      for (unsigned i = 0; i < sizeof...(A); i++)
        filled[i] = 0;

      for (int i = 0; i < ncols; i++) {
        const char* cname = sqlite3_column_name(stmt_, i);
        bool found = false;
        int j = 0;
        li::map(o, [&](auto k, auto& v) {
          if (!found and !filled[j] and !strcmp(cname, symbol_string(k))) {
            this->read_column(i, v);
            filled[j] = 1;
          }
          j++;
        });
      }
    }

    // Go to the next row.
    last_step_ret_ = sqlite3_step(stmt_);
    if (last_step_ret_ != SQLITE_ROW and last_step_ret_ != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errstr(last_step_ret_));

    return true;
  }

  long long int last_insert_id() { return sqlite3_last_insert_rowid(db_); }

  void read_column(int pos, int& v) { v = sqlite3_column_int(stmt_, pos); }
  void read_column(int pos, float& v) { v = float(sqlite3_column_double(stmt_, pos)); }
  void read_column(int pos, double& v) { v = sqlite3_column_double(stmt_, pos); }
  void read_column(int pos, int64_t& v) { v = sqlite3_column_int64(stmt_, pos); }
  void read_column(int pos, std::string& v) {
    auto str = sqlite3_column_text(stmt_, pos);
    auto n = sqlite3_column_bytes(stmt_, pos);
    v = std::move(std::string((const char*)str, n));
  }
  // Todo: Date types
  // template <typename C, typename D>
  // void read_column(int pos, std::chrono::time_point<C, D>& v)
  // {
  //   v = std::chrono::time_point<C, D>(sqlite3_column_int(stmt_, pos));
  // }

  int bind(sqlite3_stmt* stmt, int pos, double d) const {
    return sqlite3_bind_double(stmt, pos, d);
  }

  int bind(sqlite3_stmt* stmt, int pos, int d) const { return sqlite3_bind_int(stmt, pos, d); }
  int bind(sqlite3_stmt* stmt, int pos, long int d) const {
    return sqlite3_bind_int64(stmt, pos, d);
  }
  int bind(sqlite3_stmt* stmt, int pos, long long int d) const {
    return sqlite3_bind_int64(stmt, pos, d);
  }
  void bind(sqlite3_stmt* stmt, int pos, sql_null_t) { sqlite3_bind_null(stmt, pos); }
  int bind(sqlite3_stmt* stmt, int pos, const char* s) const {
    return sqlite3_bind_text(stmt, pos, s, strlen(s), nullptr);
  }
  int bind(sqlite3_stmt* stmt, int pos, const std::string& s) const {
    return sqlite3_bind_text(stmt, pos, s.data(), s.size(), nullptr);
  }
  int bind(sqlite3_stmt* stmt, int pos, const std::string_view& s) const {
    return sqlite3_bind_text(stmt, pos, s.data(), s.size(), nullptr);
  }
  int bind(sqlite3_stmt* stmt, int pos, const sql_blob& b) const {
    return sqlite3_bind_blob(stmt, pos, b.data(), b.size(), nullptr);
  }

  sqlite3* db_;
  sqlite3_stmt* stmt_;
  stmt_sptr stmt_sptr_;
  int last_step_ret_;
};

void free_sqlite3_db(void* db) { sqlite3_close_v2((sqlite3*)db); }

struct sqlite_connection {
  typedef sqlite_tag db_tag;

  typedef std::shared_ptr<sqlite3> db_sptr;
  typedef std::unordered_map<std::string, sqlite_statement> stmt_map;
  typedef std::shared_ptr<std::unordered_map<std::string, sqlite_statement>> stmt_map_ptr;
  typedef std::shared_ptr<std::mutex> mutex_ptr;

  sqlite_connection()
      : db_(nullptr), stm_cache_(new stmt_map()), cache_mutex_(new std::mutex()) // FIXME
  {}

  void connect(const std::string& filename,
               int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
    int r = sqlite3_open_v2(filename.c_str(), &db_, flags, nullptr);
    if (r != SQLITE_OK)
      throw std::runtime_error(std::string("Cannot open database ") + filename + " " +
                               sqlite3_errstr(r));

    db_sptr_ = db_sptr(db_, free_sqlite3_db);
  }

  template <typename E> inline void format_error(E&) const {}

  template <typename E, typename T1, typename... T>
  inline void format_error(E& err, T1 a, T... args) const {
    err << a;
    format_error(err, args...);
  }

  template <typename F> sqlite_statement cached_statement(F f) {
    if (statements_hashmap(f).stmt_sptr_.get() == nullptr)
      return prepare(f());
    else
      return statements_hashmap(f);
  }

  sqlite_statement prepare(const std::string& req) {
    // std::cout << req << std::endl;
    auto it = stm_cache_->find(req);
    if (it != stm_cache_->end())
      return it->second;

    sqlite3_stmt* stmt;

    int err = sqlite3_prepare_v2(db_, req.c_str(), req.size(), &stmt, nullptr);
    if (err != SQLITE_OK)
      throw std::runtime_error(std::string("Sqlite error during prepare: ") + sqlite3_errmsg(db_) +
                               " statement was: " + req);

    cache_mutex_->lock();
    auto it2 = stm_cache_->insert(it, std::make_pair(req, sqlite_statement(db_, stmt)));
    cache_mutex_->unlock();
    return it2->second;
  }
  sqlite_statement operator()(const std::string& req) { return prepare(req)(); }

  template <typename T>
  inline std::string type_to_string(const T&, std::enable_if_t<std::is_integral<T>::value>* = 0) {
    return "INTEGER";
  }
  template <typename T>
  inline std::string type_to_string(const T&,
                                    std::enable_if_t<std::is_floating_point<T>::value>* = 0) {
    return "REAL";
  }
  inline std::string type_to_string(const std::string&) { return "TEXT"; }
  inline std::string type_to_string(const sql_blob&) { return "BLOB"; }
  template <unsigned SIZE> inline std::string type_to_string(const sql_varchar<SIZE>&) {
    std::ostringstream ss;
    ss << "VARCHAR(" << SIZE << ')';
    return ss.str();
  }

  mutex_ptr cache_mutex_;
  sqlite3* db_;
  db_sptr db_sptr_;
  stmt_map_ptr stm_cache_;
  type_hashmap<sqlite_statement> statements_hashmap;
};

struct sqlite_database {
  typedef sqlite_tag db_tag;

  typedef sqlite_connection connection_type;

  sqlite_database() {}

  template <typename... O> sqlite_database(const std::string& path, O... options_) {
    auto options = mmm(options_...);

    path_ = path;
    con_.connect(path, SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (has_key(options, s::synchronous)) {
      std::ostringstream ss;
      ss << "PRAGMA synchronous=" << li::get_or(options, s::synchronous, 2);
      con_(ss.str())();
    }
  }

  template <typename Y> inline sqlite_connection connect(Y& y) { return con_; }
  inline sqlite_connection connect() { return con_; }

  sqlite_connection con_;
  std::string path_;
};

} // namespace li
