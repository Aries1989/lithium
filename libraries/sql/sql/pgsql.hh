#pragma once

#include <unistd.h>

#include <atomic>
#include <iostream>
#include <cassert>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <sys/epoll.h>
#include <boost/lexical_cast.hpp>
#include "libpq-fe.h"


#include <li/callable_traits/callable_traits.hh>
#include <li/metamap/metamap.hh>
#include <li/sql/sql_common.hh>
#include <li/sql/symbols.hh>
#include <li/sql/pgsql_statement.hh>
#include <li/sql/type_hashmap.hh>


namespace li {

int max_pgsql_connections_per_thread = 200;
thread_local int total_number_of_pgsql_connections = 0;

struct pgsql_tag {};

struct pgsql_database;


struct pgsql_connection_data {

  ~pgsql_connection_data() {
    if (connection)
    {
      cancel();
      PQfinish(connection);
      // std::cerr << " DISCONNECT " << fd << std::endl;
      total_number_of_pgsql_connections--;
    }
  }
  void cancel() {
    if (connection)
    {
      // Cancel any pending request.
      PGcancel* cancel = PQgetCancel(connection);
      char x[256];
      if (cancel)
      {
        PQcancel(cancel, x, 256);
        PQfreeCancel(cancel);
      }
    }
  } 

  PGconn* connection = nullptr;
  int fd = -1;
  std::unordered_map<std::string, std::shared_ptr<pgsql_statement_data>> statements;
  type_hashmap<std::shared_ptr<pgsql_statement_data>> statements_hashmap;
};

thread_local std::deque<std::shared_ptr<pgsql_connection_data>> pgsql_connection_pool;

template <typename Y>
void pq_wait(Y& yield, PGconn* con)
{
  while (PQisBusy(con)) yield();
}


template <typename Y>
struct pgsql_connection {

  Y& fiber_;
  std::shared_ptr<pgsql_connection_data> data_;
  std::unordered_map<std::string, std::shared_ptr<pgsql_statement_data>>& stm_cache_;
  PGconn* connection_;
  std::shared_ptr<int> connection_status_;

  typedef pgsql_tag db_tag;

  inline pgsql_connection(const pgsql_connection&) = delete;
  inline pgsql_connection& operator=(const pgsql_connection&) = delete;
  inline pgsql_connection(pgsql_connection&&) = default;
  
  inline pgsql_connection(Y& fiber, std::shared_ptr<li::pgsql_connection_data> data)
      : fiber_(fiber), data_(data), stm_cache_(data->statements),
        connection_(data->connection){

    connection_status_ = std::shared_ptr<int>(new int(0), [data](int* p) mutable {
      if (*p or pgsql_connection_pool.size() > max_pgsql_connections_per_thread)
      {
        assert(total_number_of_pgsql_connections >= 1);
        //yield.unsubscribe(data->fd);
        //std::cerr << "Discarding broken pgsql connection." << std::endl;
      }
      else
      {
         pgsql_connection_pool.push_back(data);
      }
    });
  }

  ~pgsql_connection() {
    if (connection_status_ && *connection_status_ == 0)
    {
      // flush results if needed.
      // while (PGresult* res = wait_for_next_result())
      //   PQclear(res);
    }
  }

  //FIXME long long int last_insert_rowid() { return pgsql_insert_id(connection_); }

  //pgsql_statement<Y> operator()(const std::string& rq) { return prepare(rq)(); }

  // Read request returning 1 scalar. Use prepared statement for more advanced read functions.
  template <typename T>
  T read()
  {
    PGresult* res = wait_for_next_result();
    return boost::lexical_cast<T>(PQgetvalue(res, 0, 0));
  }

  auto& operator()(const std::string& rq) {
    wait();
    if (!PQsendQuery(connection_, rq.c_str()))
      throw std::runtime_error(std::string("Postresql error:") + PQerrorMessage(connection_));
    return *this;
  }

  void wait () {
    while (PGresult* res = wait_for_next_result())
      PQclear(res);
  }

  PGresult* wait_for_next_result() {
    while (true)
    {
      if (PQconsumeInput(connection_) == 0)
        throw std::runtime_error(std::string("PQconsumeInput() failed: ") + PQerrorMessage(connection_));

      if (PQisBusy(connection_))
      {
        try {
          fiber_.yield();
        } catch (typename Y::exception_type& e) {
          // Yield thrown a exception (probably because a closed connection).
          // Mark the connection as broken because it is left in a undefined state.
          *connection_status_ = 1;
          throw std::move(e);
        }
      }
      else 
        return PQgetResult(connection_);
    }
  }

  template <typename F, typename... K>
  pgsql_statement<Y> cached_statement(F f, K... keys) {
    if (data_->statements_hashmap(f, keys...).get() == nullptr)
    {
      pgsql_statement<Y> res = prepare(f());
      data_->statements_hashmap(f, keys...) = res.data_.shared_from_this();
      return res;
    }
    else
      return pgsql_statement<Y>{connection_, fiber_, 
                               *data_->statements_hashmap(f, keys...),
                               connection_status_};
  }

  pgsql_statement<Y> prepare(const std::string& rq) {
    auto it = stm_cache_.find(rq);
    if (it != stm_cache_.end())
    {
      //pgsql_wrapper_.pgsql_stmt_free_result(it->second->stmt_);
      //pgsql_wrapper_.pgsql_stmt_reset(it->second->stmt_);
      return pgsql_statement<Y>{connection_, fiber_, *it->second, connection_status_};
    }
    std::stringstream stmt_name;
    stmt_name << (void*)connection_ << stm_cache_.size();
    //std::cout << "prepare " << rq << " NAME: " << stmt_name.str() << std::endl;

    while (PGresult* res = wait_for_next_result())
      PQclear(res);

    if (!PQsendPrepare(connection_, stmt_name.str().c_str(), rq.c_str(), 0, nullptr)) { 
      throw std::runtime_error(std::string("PQsendPrepare error") + PQerrorMessage(connection_)); 
    }
    // flush results.
    while(PGresult* ret = PQgetResult(connection_))
    {
      if (PQresultStatus(ret) == PGRES_FATAL_ERROR)
        throw std::runtime_error(std::string("Postresql fatal error:") + PQerrorMessage(connection_));
      if (PQresultStatus(ret) == PGRES_NONFATAL_ERROR)
        std::cerr << "Postgresql non fatal error: " << PQerrorMessage(connection_) << std::endl;
      PQclear(ret);
    }
    //pq_wait(yield_, connection_);

    auto pair = stm_cache_.emplace(rq, std::make_shared<pgsql_statement_data>(stmt_name.str()));
    return pgsql_statement<Y>{connection_, fiber_, *pair.first->second, connection_status_};
  }

  template <typename T>
  inline std::string type_to_string(const T&, std::enable_if_t<std::is_integral<T>::value>* = 0) {
    return "INT";
  }
  template <typename T>
  inline std::string type_to_string(const T&,
                                    std::enable_if_t<std::is_floating_point<T>::value>* = 0) {
    return "DOUBLE";
  }
  inline std::string type_to_string(const std::string&) { return "TEXT"; }
  inline std::string type_to_string(const sql_blob&) { return "BLOB"; }
  template <unsigned S> inline std::string type_to_string(const sql_varchar<S>) {
    std::ostringstream ss;
    ss << "VARCHAR(" << S << ')';
    return ss.str();
  }

};


struct pgsql_database : std::enable_shared_from_this<pgsql_database> {

  typedef pgsql_tag db_tag;

  std::mutex mutex_;
  std::string host_, user_, passwd_, database_;
  unsigned int port_;
  std::string character_set_;

  template <typename... O> inline pgsql_database(O... opts) {

    auto options = mmm(opts...);
    static_assert(has_key(options, s::host), "open_pgsql_connection requires the s::host argument");
    static_assert(has_key(options, s::database),
                  "open_pgsql_connection requires the s::databaser argument");
    static_assert(has_key(options, s::user), "open_pgsql_connection requires the s::user argument");
    static_assert(has_key(options, s::password),
                  "open_pgsql_connection requires the s::password argument");

    host_ = options.host;
    database_ = options.database;
    user_ = options.user;
    passwd_ = options.password;
    port_ = get_or(options, s::port, 0);
    character_set_ = get_or(options, s::charset, "utf8");

    if (!PQisthreadsafe())
      throw std::runtime_error("LibPQ is not threadsafe.");
  }

  template <typename Y>
  inline pgsql_connection<Y> connect(Y& fiber) {

    //std::cout << "nconnection " << total_number_of_pgsql_connections << std::endl;
    std::shared_ptr<pgsql_connection_data> data = nullptr;
    while (!data)
    {

      if (!pgsql_connection_pool.empty()) {
        data = pgsql_connection_pool.back();
        pgsql_connection_pool.pop_back();
        fiber.epoll_add(data->fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
      }
      else
      {
        // std::cout << total_number_of_pgsql_connections << " connections. "<< std::endl;
        if (total_number_of_pgsql_connections >= max_pgsql_connections_per_thread)
        {
          //std::cout << "Waiting for a free pgsql connection..." << std::endl;
          fiber.yield();
          continue;
        }
        total_number_of_pgsql_connections++;
        PGconn* connection = nullptr;
        int pgsql_fd = -1;
        std::stringstream coninfo;
        coninfo << "postgresql://" << user_ << ":" << passwd_ << "@" << host_ << ":" << port_ << "/" << database_;
        // connection = PQconnectdb(coninfo.str().c_str());
        connection = PQconnectStart(coninfo.str().c_str());
        if (!connection)
        {
          std::cerr << "Warning: PQconnectStart returned null." << std::endl;
          total_number_of_pgsql_connections--;
          fiber.yield();
          continue;
        }
        if (PQsetnonblocking(connection, 1) == -1)
        {
          std::cerr << "Warning: PQsetnonblocking returned -1: " << PQerrorMessage(connection) << std::endl;
          PQfinish(connection);
          total_number_of_pgsql_connections--;
          fiber.yield();
          continue;
        }


        int status = PQconnectPoll(connection);

        pgsql_fd = PQsocket(connection);
        if (pgsql_fd == -1)
        {
          std::cerr << "Warning: PQsocket returned -1: " << PQerrorMessage(connection) << std::endl;
          // If PQsocket return -1, retry later.
          PQfinish(connection);
          total_number_of_pgsql_connections--;
          fiber.yield();
          continue;
        }
        fiber.epoll_add(pgsql_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);

        try
        {
          while (status != PGRES_POLLING_FAILED and status != PGRES_POLLING_OK)
          {
            int new_pgsql_fd = PQsocket(connection);
            if (new_pgsql_fd != pgsql_fd)
            {
              pgsql_fd = new_pgsql_fd;
              fiber.epoll_add(pgsql_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
            }
            fiber.yield();
            status = PQconnectPoll(connection);
          }
        } catch (typename Y::exception_type& e) {
          // Yield thrown a exception (probably because a closed connection).
          total_number_of_pgsql_connections--;
          PQfinish(connection);
          throw std::move(e);
        }
        // std::cout << "CONNECTED " << std::endl;
        fiber.epoll_mod(pgsql_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
        if (status != PGRES_POLLING_OK)
        {
          std::cerr << "Warning: cannot connect to the postgresql server " << host_  << ": " << PQerrorMessage(connection) << std::endl;
          std::cerr<< "thread allocated connection == " << total_number_of_pgsql_connections <<  std::endl;
          std::cerr<< "Maximum is " << max_pgsql_connections_per_thread <<  std::endl;
          total_number_of_pgsql_connections--;
          PQfinish(connection);
          fiber.yield();
          continue;
        }


        //pgsql_set_character_set(pgsql, character_set_.c_str());
        data = std::shared_ptr<pgsql_connection_data>(new pgsql_connection_data{connection, pgsql_fd});
      }
    }
    assert(data);
    return pgsql_connection(fiber, data);
  }

  struct active_yield {
    typedef std::runtime_error exception_type;
    void epoll_add(int, int) {}
    void epoll_mod(int, int) {}
    void yield() {}
  };

  inline pgsql_connection<active_yield> connect() {
    return connect(*(active_yield*)0); // hack to make a reference to active_yield that holds no data.
  }
};


} // namespace li
