//
//  mongodb_db.h
//  YCSB-C
//

#ifndef YCSB_C_MONGODB_DB_H_
#define YCSB_C_MONGODB_DB_H_

#include "core/db.h"
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <mutex>
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
  void SetWriteConcern(mongoc_collection_t *collection);

  // 1. use pool_ as static member shared for all instances
  static mongoc_client_pool_t *pool_;

  // 2. use instance count and lock for global life cycle
  static int instance_count_;
  static std::mutex mutex_;

  mongoc_write_concern_t *write_concern_;

  std::string url_;
  std::string db_name_;
  std::string wc_type_;
};

} // namespace ycsbc

#endif // YCSB_C_MONGODB_DB_H_
