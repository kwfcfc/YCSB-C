#include "mongodb_db.h"
#include <iostream>

using namespace std;

namespace ycsbc {

MongoDB::MongoDB(const string &url, const string &db_name, const string &wc_type)
    : pool_(NULL), write_concern_(NULL),
      url_(url), db_name_(db_name), wc_type_(wc_type) {
}

MongoDB::~MongoDB() {
  Close();
}

void MongoDB::Init() {
  mongoc_init();

  // 1. 解析 URI
  bson_error_t error;
  mongoc_uri_t *uri = mongoc_uri_new_with_error(url_.c_str(), &error);
  if (!uri) {
    cerr << "Failed to parse MongoDB URI: " << url_ << endl;
    cerr << "Error: " << error.message << endl;
    exit(1);
  }

  // 2. 创建连接池
  pool_ = mongoc_client_pool_new(uri);
  mongoc_uri_destroy(uri); // pool 建立后，uri 对象就可以释放了

  if (!pool_) {
      cerr << "Failed to create MongoDB client pool." << endl;
      exit(1);
  }

  // 3. 预先初始化 WriteConcern 对象
  write_concern_ = mongoc_write_concern_new();
  if (wc_type_ == "strict") {
    mongoc_write_concern_set_w(write_concern_, 1);
  } else if (wc_type_ == "normal") {
    mongoc_write_concern_set_w(write_concern_, 1);
  } else if (wc_type_ == "none") {
    mongoc_write_concern_set_w(write_concern_, 0);
  }
}

void MongoDB::Close() {
if (write_concern_) {
      mongoc_write_concern_destroy(write_concern_);
      write_concern_ = NULL;
  }
  if (pool_) {
      mongoc_client_pool_destroy(pool_);
      pool_ = NULL;
  }
  mongoc_cleanup();
}

// 辅助函数：给 Collection 设置 Write Concern
void MongoDB::SetWriteConcern(mongoc_collection_t *collection) {
    if (write_concern_) {
        mongoc_collection_set_write_concern(collection, write_concern_);
    }
}

// 对应 Java: read(table, key, fields, result)
int MongoDB::Read(const string &table, const string &key,
                  const vector<string> *fields,
                  vector<KVPair> &result) {
// 1. 从池中借出一个 client
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  // 2. 获取 collection (这是轻量级操作)
  mongoc_collection_t *collection = mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());

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

  // 3. 将 client 还回池中
  mongoc_client_pool_push(pool_, client);

  return ret;
}

// 对应 Java: insert(table, key, values)
int MongoDB::Insert(const string &table, const string &key,
                    vector<KVPair> &values) {
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());
  SetWriteConcern(collection);

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

  mongoc_client_pool_push(pool_, client);

  return r ? DB::kOK : DB::kErrorConflict;
}

// 对应 Java: update(table, key, values) 使用 $set
int MongoDB::Update(const string &table, const string &key,
                    vector<KVPair> &values) {
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());
  SetWriteConcern(collection);

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
  mongoc_client_pool_push(pool_, client);

  return r ? DB::kOK : DB::kErrorConflict;
}

// 对应 Java: delete(table, key)
int MongoDB::Delete(const string &table, const string &key) {
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());
  SetWriteConcern(collection);

  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));
  bson_error_t error;

  // Java代码里如果是 Strict 模式会加 $atomic，但在新版驱动中一般由 WriteConcern 保证原子性
  bool r = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);

  bson_destroy(query);
  mongoc_collection_destroy(collection);
  mongoc_client_pool_push(pool_, client);
  return r ? DB::kOK : DB::kErrorNoData;
}

// 对应 Java: scan(table, startkey, recordcount, fields, result)
int MongoDB::Scan(const string &table, const string &key,
                  int record_count, const std::vector<std::string> *fields,
                  vector<vector<KVPair>> &result) {
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());

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
  mongoc_client_pool_push(pool_, client);
  return DB::kOK;
}

} // namespace ycsbc
