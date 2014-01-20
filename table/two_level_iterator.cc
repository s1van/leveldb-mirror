// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <queue>
#include "table/two_level_iterator.h"

#include "leveldb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"
#include "leveldb/mirror.h"
#include "util/aio_wrapper.h"

namespace leveldb {

int AIOWrapper::prefetch_counter_(0);
port::Mutex AIOWrapper::mu_;

namespace {

typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&, const bool mirror);

class TwoLevelIterator: public Iterator {
 public:
  TwoLevelIterator(
    Iterator* index_iter,
    BlockFunction block_function,
    void* arg,
    const ReadOptions& options,
    bool mirror = false);

  virtual ~TwoLevelIterator();

  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();
  virtual void Next();
  virtual void Prev();

  virtual bool Valid() const {
    return data_iter_.Valid();
  }
  virtual Slice key() const {
    assert(Valid());
    return data_iter_.key();
  }
  virtual Slice value() const {
    assert(Valid());
    return data_iter_.value();
  }
  virtual Status status() const {
    // It'd be nice if status() returned a const Status& instead of a Status
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    } else if (data_iter_.iter() != NULL && !data_iter_.status().ok()) {
      return data_iter_.status();
    } else {
      return status_;
    }
  }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataBlock();
  void PrefetchDataBlock();
  void InitPrefetchedDataBlock();

  BlockFunction block_function_;
  void* arg_;
  const ReadOptions options_;
  Status status_;
  IteratorWrapper index_iter_;
  IteratorWrapper data_iter_; // May be NULL
  // If data_iter_ is non-NULL, then "data_block_handle_" holds the
  // "index_value" passed to block_function_ to create the data_iter_.
  std::string data_block_handle_;
  bool mirror_;
  bool prefetch_;
	std::queue<Iterator*> prefetch_data_iter_;
	std::queue<Slice> prefetch_handle_;
	static const int max_prefetch_num_ = 2;
	static const int max_op_before_prefetch_ = 1024;
	int op_after_prefetch_;
};

TwoLevelIterator::TwoLevelIterator(
    Iterator* index_iter,
    BlockFunction block_function,
    void* arg,
    const ReadOptions& options,
    bool mirror)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(NULL),
			op_after_prefetch_(0),
      mirror_(mirror),
			prefetch_(mirror && HLSM_CPREFETCH) {
}

TwoLevelIterator::~TwoLevelIterator() {
}

void TwoLevelIterator::Seek(const Slice& target) {
  index_iter_.Seek(target);
  InitDataBlock();
  if (data_iter_.iter() != NULL) data_iter_.Seek(target);
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToFirst() {
  index_iter_.SeekToFirst();
	if (prefetch_)  {
  	DEBUG_INFO("[Prefetch] SeekToFirst");
		InitPrefetchedDataBlock();
	} else
  	InitDataBlock();

  if (data_iter_.iter() != NULL) data_iter_.SeekToFirst();
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::SeekToLast() {
  index_iter_.SeekToLast();
  InitDataBlock();
  if (data_iter_.iter() != NULL) data_iter_.SeekToLast();
  SkipEmptyDataBlocksBackward();
}

void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_.Next();
	if (prefetch_) {
  	DEBUG_INFO("[Prefetch] Next");
		op_after_prefetch_++;
		if (op_after_prefetch_ > max_op_before_prefetch_) {
			PrefetchDataBlock();
			op_after_prefetch_ = 0;
		}
	}
  SkipEmptyDataBlocksForward();
}

void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}


void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (data_iter_.iter() == NULL || !data_iter_.Valid()) {
		if (prefetch_) {
  		DEBUG_INFO("[Prefetch] SkipEmptyDataBlocksForward");
			PrefetchDataBlock();
			if (prefetch_handle_.size() == 0) {
    	  SetDataIterator(NULL);
    	  return;
			}
			InitPrefetchedDataBlock();
		} else {
    	// Move to next block
    	if (!index_iter_.Valid()) {
    	  SetDataIterator(NULL);
    	  return;
    	}
    	index_iter_.Next();
    	InitDataBlock();
		}

    if (data_iter_.iter() != NULL) data_iter_.SeekToFirst();
  }
}

void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (data_iter_.iter() == NULL || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(NULL);
      return;
    }
    index_iter_.Prev();
    InitDataBlock();
    if (data_iter_.iter() != NULL) data_iter_.SeekToLast();
  }
}

void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
  if (data_iter_.iter() != NULL) SaveError(data_iter_.status());
  data_iter_.Set(data_iter);
}

void TwoLevelIterator::InitDataBlock() {
  if (!index_iter_.Valid()) {
    SetDataIterator(NULL);
  } else {
    Slice handle = index_iter_.value();
    if (data_iter_.iter() != NULL && handle.compare(data_block_handle_) == 0) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } else {
      Iterator* iter = (*block_function_)(arg_, options_, handle, mirror_);
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(iter);
    }
  }
}

void TwoLevelIterator::InitPrefetchedDataBlock() {
	PrefetchDataBlock();
	if (prefetch_handle_.size() == 0)
    SetDataIterator(NULL);
	else {
    Slice handle = prefetch_handle_.front();
    if (data_iter_.iter() != NULL && handle.compare(data_block_handle_) == 0) {
			// data_iter_ is already constructed
		} else {
			prefetch_handle_.pop();
			data_block_handle_.assign(handle.data(), handle.size());
			SetDataIterator(prefetch_data_iter_.front());
			prefetch_data_iter_.pop();
		}
	}
}
		

void TwoLevelIterator::PrefetchDataBlock() {
	if (!prefetch_) return;
	while ( AIOWrapper::getPrefetchingNum() < max_prefetch_num_ && index_iter_.Valid() ) {
		index_iter_.Next();
		
  	if (index_iter_.Valid()) {
    	Slice handle = index_iter_.value();
			DEBUG_INFO(handle.data());
      Iterator* iter = (*block_function_)(arg_, options_, handle, mirror_);
			prefetch_handle_.push(handle);
			prefetch_data_iter_.push(iter);
    }
  }

}

}  // namespace

Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    BlockFunction block_function,
    void* arg,
    const ReadOptions& options,
    const bool mirror) {
  return new TwoLevelIterator(index_iter, block_function, arg, options, mirror);
}

}  // namespace leveldb
