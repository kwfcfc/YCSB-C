//
//  mongodb_db.h
//  YCSB-C
//

#ifndef YCSB_C_MONGODB_DB_H_
#define YCSB_C_MONGODB_DB_H_

#include "core/db.h"
#include <mongoc/mongoc.h>
#include <bson/bson.h>
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

  // 1. 将 pool_ 改为静态成员，让所有实例共享
  static mongoc_client_pool_t *pool_;

  // 2. 引入静态计数器和锁，用于管理全局生命周期
  static int instance_count_;
  static std::mutex mutex_;

  mongoc_write_concern_t *write_concern_;

  std::string url_;
  std::string db_name_;
  std::string wc_type_;
};

} // namespace ycsbc

#endif // YCSB_C_MONGODB_DB_H_
