//
//  mongodb_db.h
//  YCSB-C
//

#ifndef YCSB_C_MONGODB_DB_H_
#define YCSB_C_MONGODB_DB_H_

#include "core/db.h"
#include <mongoc/mongoc.h>
#include <string>
#include <vector>

namespace ycsbc {

class MongoDB : public DB {
 public:
   MongoDB(const std::string &url, const std::string &db_name,
           const std::string &wc_type);
  virtual ~MongoDB();

  void Init();
  void Close();

  int Read(const std::string &table, const std::string &key,
           const std::vector<std::string> *fields,
           std::vector<KVPair> &result);

  int Scan(const std::string &table, const std::string &key,
           int record_count, const std::vector<std::string> *fields,
           std::vector<std::vector<KVPair>> &result);

  int Update(const std::string &table, const std::string &key,
             std::vector<KVPair> &values);

  int Insert(const std::string &table, const std::string &key,
             std::vector<KVPair> &values);

  int Delete(const std::string &table, const std::string &key);

 private:
  mongoc_client_t *client_;
  mongoc_database_t *database_;
  mongoc_write_concern_t *write_concern_;

  std::string db_name_;
  std::string url_;
  std::string wc_type_;
};

} // namespace ycsbc

#endif // YCSB_C_MONGODB_DB_H_
