#include "mongodb_db.h"
#include <iostream>

using namespace std;

namespace ycsbc {

// --- 1. Global Static Variable ---
mongoc_client_pool_t* MongoDB::pool_ = nullptr;
int MongoDB::instance_count_ = 0;
std::mutex MongoDB::mutex_;

MongoDB::MongoDB(const string &url, const string &db_name,
                 const string &wc_type)
    : write_concern_(nullptr), url_(url), db_name_(db_name), wc_type_(wc_type) {
}

MongoDB::~MongoDB() {
  Close();
}

void MongoDB::Init() {
  {
  // Lock for global clientpool
  lock_guard<mutex> lock(mutex_);

  if (instance_count_ == 0) {
    mongoc_init();

    // 1. Parse URI
    bson_error_t error;
    mongoc_uri_t *uri = mongoc_uri_new_with_error(url_.c_str(), &error);
    if (!uri) {
      cerr << "Failed to parse MongoDB URI: " << url_ << endl;
      cerr << "Error: " << error.message << endl;
      exit(1);
    }

    // 2. Create client pool
    pool_ = mongoc_client_pool_new(uri);
    mongoc_uri_destroy(uri); // Release URI after creating pool

    if (!pool_) {
      cerr << "Failed to create MongoDB client pool." << endl;
      exit(1);
    }
  }

  instance_count_++;
  }

  // 3. Initialize WriteConcern Object
  // Every instance has its own write_concern_
  if (write_concern_ == nullptr) {
    write_concern_ = mongoc_write_concern_new();
    if (wc_type_ == "strict" || wc_type_ == "normal") {
      mongoc_write_concern_set_w(write_concern_, 1);
    // } else if (wc_type_ == "normal") {
    //   mongoc_write_concern_set_w(write_concern_, 1);
    } else if (wc_type_ == "none") {
      mongoc_write_concern_set_w(write_concern_, 0);
    }
  }
}

void MongoDB::Close() {
  if (write_concern_) {
    mongoc_write_concern_destroy(write_concern_);
    write_concern_ = NULL;
  }

  {
  lock_guard<mutex> lock(mutex_);

  // instance_count_ is 0, meaning it's closed. Return
  if (instance_count_ <= 0) {
      return;
  }

  instance_count_--;

  // Last instance needs to clean up
  if (instance_count_ == 0) {
    if (pool_) {
      mongoc_client_pool_destroy(pool_);
      pool_ = nullptr;
    }
    mongoc_cleanup();
  }
  }
}

// helper function: set write conern for Collection
void MongoDB::SetWriteConcern(mongoc_collection_t *collection) {
    if (write_concern_) {
        mongoc_collection_set_write_concern(collection, write_concern_);
    }
}

// compared to the Java version: read(table, key, fields, result)
int MongoDB::Read(const string &table, const string &key,
                  const vector<string> *fields,
                  vector<KVPair> &result) {
  if (!pool_) {
    return DB::kErrorNoData;
  }

  // 1. borrow a client from pool
  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  // 2. get collection (这是轻量级操作)
  mongoc_collection_t *collection = mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());

  // construct query: { "_id": key }
  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));

  // construct opts (for Projection)
  bson_t *opts = bson_new();
  if (fields) {
    bson_t child;
    bson_append_document_begin(opts, "projection", -1, &child);
    // similar to Java: fieldsToReturn.put(field, 1)
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
        // ignore _id field
        // for YCSB, return value fields
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

  // 3. push client back to pool
  mongoc_client_pool_push(pool_, client);

  return ret;
}

// similar to Java version: insert(table, key, values)
int MongoDB::Insert(const string &table, const string &key,
                    vector<KVPair> &values) {
  if (!pool_) {
    return DB::kErrorNoData;
  }

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
  // Java version use getLastError to test "n"==1
  // libmongoc insert_one return true for success
  bool r = mongoc_collection_insert_one(collection, doc, NULL, NULL, &error);

  bson_destroy(doc);
  mongoc_collection_destroy(collection);

  mongoc_client_pool_push(pool_, client);

  return r ? DB::kOK : DB::kErrorConflict;
}

// Similar to Java version: update(table, key, values) use $set
int MongoDB::Update(const string &table, const string &key,
                    vector<KVPair> &values) {
  if (!pool_) {
    return DB::kErrorNoData;
  }

  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());
  SetWriteConcern(collection);

  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));

  // Construct update document: { "$set": { ... } }
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

// Similar to Java version: delete(table, key)
int MongoDB::Delete(const string &table, const string &key) {
  if (!pool_) {
    return DB::kErrorNoData;
  }

  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());
  SetWriteConcern(collection);

  bson_t *query = BCON_NEW("_id", BCON_UTF8(key.c_str()));
  bson_error_t error;

  // In Java code, if it is Strict mode it will set $atomic, but in new driver
  // it is usually up to WriteConcern for atomicity
  bool r = mongoc_collection_delete_one(collection, query, NULL, NULL, &error);

  bson_destroy(query);
  mongoc_collection_destroy(collection);
  mongoc_client_pool_push(pool_, client);
  return r ? DB::kOK : DB::kErrorNoData;
}

// Similar to Java: scan(table, startkey, recordcount, fields, result)
int MongoDB::Scan(const string &table, const string &key,
                  int record_count, const std::vector<std::string> *fields,
                  vector<vector<KVPair>> &result) {
  if (!pool_) {
    return DB::kErrorNoData;
  }

  mongoc_client_t *client = mongoc_client_pool_pop(pool_);
  mongoc_collection_t *collection =
      mongoc_client_get_collection(client, db_name_.c_str(), table.c_str());

  // Java Query: { "_id": { "$gte": startkey } }
  bson_t *query = BCON_NEW("_id", "{", "$gte", BCON_UTF8(key.c_str()), "}");

  // Java: cursor.limit(recordcount)
  // C: use opts to set limit
  bson_t *opts = bson_new();
  bson_append_int64(opts, "limit", 5, record_count);

  // if scan needs to support fields projection (Though in Java Scan code
  // fields seem unused)
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
              // Skip _id too
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
