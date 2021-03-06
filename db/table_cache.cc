// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "leveldb/mirror.h"
#include "util/coding.h"


namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname,
                       const Options* options,
                       int entries)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)),
      mcache_(NewLRUCache(entries)) {
}

TableCache::~TableCache() {
  delete cache_;
  delete mcache_;
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle, bool mirror) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));

  DEBUG_INFO2(file_number, mirror);

  if (mirror)
    *handle = mcache_->Lookup(key);
  else
    *handle = cache_->Lookup(key);

  if (*handle == NULL) {
    std::string fname = TableFileName(MIRROR_PATH, file_number);
    if (!(mirror && MIRROR_ENABLE && file_size > 65536 && !FileNameHash::inuse(fname))) {
      fname = TableFileName(dbname_, file_number);
    }

    DEBUG_INFO2(fname, mirror);
    RandomAccessFile* file = NULL;
    Table* table = NULL;
    s = env_->NewRandomAccessFile(fname, &file, mirror);
    if (s.ok()) {
      s = Table::Open(*options_, file, file_size, &table);
    }
    DEBUG_INFO2(fname, mirror);

    if (!s.ok()) {
      assert(table == NULL);
      DEBUG_INFO2(fname, mirror);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      if (mirror)
        *handle = mcache_->Insert(key, tf, 1, &DeleteEntry);
      else
        *handle = cache_->Insert(key, tf, 1, &DeleteEntry);

      DEBUG_INFO2(fname, mirror);
    }
  }
  DEBUG_INFO2("End of FindTable", file_number);
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size,
                                  Table** tableptr, bool mirror) {
  DEBUG_INFO2(file_number, mirror);
  if (tableptr != NULL) {
    *tableptr = NULL;
  }

  Cache::Handle* handle = NULL;
  Status s = FindTable(file_number, file_size, &handle, mirror);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table;
  if (mirror)
    table = reinterpret_cast<TableAndFile*>(mcache_->Value(handle))->table;
  else
    table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;

  Iterator* result = table->NewIterator(options);

  if (mirror)
    result->RegisterCleanup(&UnrefEntry, mcache_, handle);
  else
    result->RegisterCleanup(&UnrefEntry, cache_, handle);

  if (tableptr != NULL) {
    *tableptr = table;
  }

  DEBUG_INFO2("End", file_number);
  return result;
}

Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,
                       void* arg,
                       void (*saver)(void*, const Slice&, const Slice&)) {
  DEBUG_INFO2(file_number, file_size);
  Cache::Handle* handle = NULL;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, saver);
    cache_->Release(handle);
  }
  DEBUG_INFO2("End", file_number);
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
  mcache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
