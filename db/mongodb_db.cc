#include "mongodb_db.h"
#include "core/properties.h"
#include <bson/bson.h>
#include <iostream>

using namespace std;

namespace ycsbc {

MongoDB::MongoDB() : client_(NULL), database_(NULL), write_concern_(NULL) {}

MongoDB::MongoDB(utils::Properties props)
    : client_(NULL), database_(NULL), write_concern_(NULL), props_(props) {
}

MongoDB::~MongoDB() {
  Close();
}

void MongoDB::Init() {
  mongoc_init();
  const utils::Properties &props = *props_;

  // 1. 获取配置 (对应 Java: getProperties)
  string url = props.GetProperty("mongodb.url", "mongodb://localhost:27017");
  db_name_ = props.GetProperty("mongodb.database", "ycsb");
  string wc_type = props.GetProperty("mongodb.writeConcern", "normal");

  // 2. 连接 MongoDB
  // 注意：Java旧驱动需要去掉 "mongodb://" 前缀，但 libmongoc 需要这个前缀，所以这里直接使用
  client_ = mongoc_client_new(url.c_str());
  if (!client_) {
    cerr << "Failed to connect to MongoDB at " << url << endl;
    exit(1);
  }

  // 3. 处理 WriteConcern (对应 Java: "strict", "normal", "none")
  // libmongoc 使用 mongoc_write_concern_t
  write_concern_ = mongoc_write_concern_new();
  if (wc_type == "strict") {
    // 对应 Java WriteConcern.STRICT (等待主节点确认)
    mongoc_write_concern_set_w(write_concern_, 1);
  } else if (wc_type == "normal") {
    // 对应 Java WriteConcern.NORMAL (网络确认)
    mongoc_write_concern_set_w(write_concern_, 1);
  } else if (wc_type == "none") {
    // 对应 Java WriteConcern.NONE (不等待确认)
    mongoc_write_concern_set_w(write_concern_, 0);
  }

  // 将 WriteConcern 应用到 client
  mongoc_client_set_write_concern(client_, write_concern_);

  database_ = mongoc_client_get_database(client_, db_name_.c_str());
}

void MongoDB::Close() {
  if (write_concern_) mongoc_write_concern_destroy(write_concern_);
  if (database_) mongoc_database_destroy(database_);
  if (client_) mongoc_client_destroy(client_);
  mongoc_cleanup();
}

// 对应 Java: read(table, key, fields, result)
int MongoDB::Read(const string &table, const string &key,
                  const vector<string> *fields,
                  vector<KVPair> &result) {
  mongoc_collection_t *collection = mongoc_client_get_collection(client_, db_name_.c_str(), table.c_str());

  // 构建查询: { "_id": key }
  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));

  // 构建选项 opts (用于 Projection)
  bson_t *opts = bson_new();
  if (fields) {
    bson_t child;
    bson_append_document_begin(opts, "projection", -1, &child);
    // 类似 Java: fieldsToReturn.put(field, 1)
    for (const string &f : *fields) {
      bson_append_bool(&child, f.c_str(), -1, true);
    }
    bson_append_document_end(opts, &child);
  }

  // query
  mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

  const bson_t *doc;
  int ret = DB::kErrorNoData;

  // (Java: queryResult.toMap())
  if (mongoc_cursor_next(cursor, &doc)) {
    bson_iter_t iter;
    if (bson_iter_init(&iter, doc)) {
      while (bson_iter_next(&iter)) {
        // 忽略 _id 字段，或者根据需求保留
        // 这里为了匹配 YCSB 逻辑，通常返回所有 value 字段
        const char *k = bson_iter_key(&iter);
        if (strcmp(k, "_id") == 0) continue;

        if (BSON_ITER_HOLDS_UTF8(&iter)) {
          result.push_back(make_pair(k, bson_iter_utf8(&iter, NULL)));
        }
      }
    }
    ret = DB::kOK;
  }

  bson_destroy(query);
  bson_destroy(opts);
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(collection);
  return ret;
}

// 对应 Java: insert(table, key, values)
int MongoDB::Insert(const string &table, const string &key,
                    vector<KVPair> &values) {
  mongoc_collection_t *collection = mongoc_client_get_collection(client_, db_name_.c_str(), table.c_str());

  bson_t *doc = bson_new();
  BSON_APPEND_UTF8(doc, "_id", key.c_str());
  for (const auto &kv : values) {
    BSON_APPEND_UTF8(doc, kv.first.c_str(), kv.second.c_str());
  }

  bson_error_t error;
  // Java 代码通过 getLastError 判断 "n"==1
  // libmongoc insert_one 返回 true 即表示成功
  bool r = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);

  bson_destroy(doc);
  mongoc_collection_destroy(collection);

  return r ? DB::kOK : DB::kErrorConflict;
}

// 对应 Java: update(table, key, values) 使用 $set
int MongoDB::Update(const string &table, const string &key,
                    vector<KVPair> &values) {
  mongoc_collection_t *collection = mongoc_client_get_collection(client_, db_name_.c_str(), table.c_str());

  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));

  // 构建更新文档: { "$set": { ... } }
  bson_t *update = bson_new();
  bson_t child;
  bson_append_document_begin(update, "$set", 4, &child);
  for (const auto &kv : values) {
    bson_append_utf8(&child, kv.first.c_str(), -1, kv.second.c_str(), -1);
  }
  bson_append_document_end(update, &child);

  bson_error_t error;
  bool r = mongoc_collection_update_one(collection, query, update, NULL, NULL, &error);

  bson_destroy(query);
  bson_destroy(update);
  mongoc_collection_destroy(collection);

  return r ? DB::kOK : DB::kErrorConflict;
}

// 对应 Java: delete(table, key)
int MongoDB::Delete(const string &table, const string &key) {
  mongoc_collection_t *collection = mongoc_client_get_collection(client_, db_name_.c_str(), table.c_str());

  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));
  bson_error_t error;

  // Java代码里如果是 Strict 模式会加 $atomic，但在新版驱动中一般由 WriteConcern 保证原子性
  bool r = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);

  bson_destroy(query);
  mongoc_collection_destroy(collection);
  return r ? DB::kOK : DB::kErrorNoData;
}

// 对应 Java: scan(table, startkey, recordcount, fields, result)
int MongoDB::Scan(const string &table, const string &key,
                  int record_count, const std::vector<std::string> *fields,
                  vector<vector<KVPair>> &result) {
  mongoc_collection_t *collection = mongoc_client_get_collection(client_, db_name_.c_str(), table.c_str());

  // Java Query: { "_id": { "$gte": startkey } }
  bson_t *query = BCON_NEW("_id", "{", "$gte", BCON_UTF8(key.c_str()), "}");

  // Java: cursor.limit(recordcount)
  // C: use opts to set limit
  bson_t *opts = bson_new();
  bson_append_int64(opts, "limit", 5, record_count);

  // 如果 scan 也需要支持 fields projection (虽然Java Scan代码里传了 fields 参数但好像没用上?
  // 不过为了严谨，我们加上)
  if (fields) {
    bson_t child;
    bson_append_document_begin(opts, "projection", -1, &child);
    for (const string &f : *fields) {
      bson_append_bool(&child, f.c_str(), -1, true);
    }
    bson_append_document_end(opts, &child);
  }

  mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, opts, NULL);

  const bson_t *doc;
  while (mongoc_cursor_next(cursor, &doc)) {
      vector<KVPair> row;
      bson_iter_t iter;
      if (bson_iter_init(&iter, doc)) {
          while (bson_iter_next(&iter)) {
              // 同样跳过 _id
               const char *k = bson_iter_key(&iter);
              if (strcmp(k, "_id") == 0) continue;

              if (BSON_ITER_HOLDS_UTF8(&iter)) {
                  row.push_back(make_pair(k, bson_iter_utf8(&iter, NULL)));
              }
          }
      }
      result.push_back(row);
  }

  bson_destroy(query);
  bson_destroy(opts);
  mongoc_cursor_destroy(cursor);
  mongoc_collection_destroy(collection);
  return DB::kOK;
}

} // namespace ycsbc
