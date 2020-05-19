// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#ifndef ROCKSDB_LITE

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "util/transaction_test_util.h"

#include <inttypes.h>
#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <sys/time.h>


#include "rocksdb/db.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"
#include "rocksdb/utilities/transaction.h"
#include "rocksdb/utilities/transaction_db.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/testutil.h"


namespace rocksdb {

RandomTransactionInserter::RandomTransactionInserter(
    Random64* rand, const WriteOptions& write_options,
    const ReadOptions& read_options, uint64_t num_keys, uint16_t num_sets,
    int readpercent, int deletepercent, int conflict_level )
    : rand_(rand),
      write_options_(write_options),
      read_options_(read_options),
      num_keys_(num_keys),
      num_sets_(num_sets),
      readpercent_(readpercent),
      deletepercent_(deletepercent),
      conflict_level_(conflict_level),
      txn_id_(0) {}


RandomTransactionInserter::~RandomTransactionInserter() {
  if (txn_ != nullptr) {
    delete txn_;
  }

  if (to_txn_ != nullptr) {
    delete to_txn_;
  }
  
  if (optimistic_txn_ != nullptr) {
    delete optimistic_txn_;
  }
}

bool RandomTransactionInserter::TransactionDBInsert(
    TransactionDB* db, const TransactionOptions& txn_options) {
  txn_ = db->BeginTransaction(write_options_, txn_options, txn_);

  std::hash<std::thread::id> hasher;
  char name[64];
  snprintf(name, 64, "txn%" ROCKSDB_PRIszt "-%d",
           hasher(std::this_thread::get_id()), txn_id_++);
  assert(strlen(name) < 64 - 1);
  txn_->SetName(name);

  bool take_snapshot = rand_->OneIn(2);
  if (take_snapshot) {
    txn_->SetSnapshot();
    read_options_.snapshot = txn_->GetSnapshot();
  }
  auto res = DoInsert(nullptr, txn_, false);
  if (take_snapshot) {
    read_options_.snapshot = nullptr;
  }
  return res;
}

bool RandomTransactionInserter::TOTransactionDBInsert(
    TOTransactionDB* db) {

  TOTransactionOptions txn_option;

  to_txn_ = db->BeginTransaction(write_options_, txn_option);

  Status s = to_txn_->SetReadTimeStamp(UINT64_MAX);
  assert(s.ok());
  auto res = DoInsert(nullptr, to_txn_);
  delete to_txn_;
  to_txn_ = nullptr;
  return res;
}

bool RandomTransactionInserter::TOTransactionDBWriteRandom(
    std::vector<ColumnFamilyHandle*> handles, TOTransactionDB* db) {

  TOTransactionOptions txn_option;

  to_txn_ = db->BeginTransaction(write_options_, txn_option);

  struct timeval tv;
  gettimeofday(&tv,NULL);
  Status s = to_txn_->SetCommitTimeStamp(tv.tv_sec);
  assert(s.ok());

  s = to_txn_->SetReadTimeStamp(UINT64_MAX);
  assert(s.ok());
  auto res = DoWriteRandom(handles, to_txn_);
  delete to_txn_;
  to_txn_ = nullptr;
  return res;
}


bool RandomTransactionInserter::OptimisticTransactionDBInsert(
    OptimisticTransactionDB* db,
    const OptimisticTransactionOptions& txn_options) {
  optimistic_txn_ =
      db->BeginTransaction(write_options_, txn_options, optimistic_txn_);

  return DoInsert(nullptr, optimistic_txn_, true);
}

bool RandomTransactionInserter::DBInsert(DB* db) {
  return DoInsert(db, nullptr, false);
}

Status RandomTransactionInserter::DBGet(
    DB* db, Transaction* txn, ReadOptions& read_options, uint16_t set_i,
    uint64_t ikey, bool get_for_update, uint64_t* int_value,
    std::string* full_key, bool* unexpected_error) {
  Status s;
  // Five digits (since the largest uint16_t is 65535) plus the NUL
  // end char.
  char prefix_buf[6];
  // Pad prefix appropriately so we can iterate over each set
  assert(set_i + 1 <= 9999);
  snprintf(prefix_buf, sizeof(prefix_buf), "%.4u", set_i + 1);
  // key format:  [SET#][random#]
  std::string skey = ToString(ikey);
  Slice base_key(skey);
  *full_key = std::string(prefix_buf) + base_key.ToString();
  Slice key(*full_key);

  std::string value;
  if (txn != nullptr) {
    if (get_for_update) {
      s = txn->GetForUpdate(read_options, key, &value);
    } else {
      s = txn->Get(read_options, key, &value);
    }
  } else {
    s = db->Get(read_options, key, &value);
  }

  if (s.ok()) {
    // Found key, parse its value
    *int_value = std::stoull(value);
    if (*int_value == 0 || *int_value == ULONG_MAX) {
      *unexpected_error = true;
      fprintf(stderr, "Get returned unexpected value: %s\n", value.c_str());
      s = Status::Corruption();
    }
  } else if (s.IsNotFound()) {
    // Have not yet written to this key, so assume its value is 0
    *int_value = 0;
    s = Status::OK();
  }
  return s;
}

Status RandomTransactionInserter::DBGet(
    DB* db, TOTransaction* txn, ReadOptions& read_options, uint16_t set_i,
    uint64_t ikey, uint64_t* int_value,
    std::string* full_key, bool* unexpected_error) {
  Status s;
  // Five digits (since the largest uint16_t is 65535) plus the NUL
  // end char.
  char prefix_buf[6];
  // Pad prefix appropriately so we can iterate over each set
  assert(set_i + 1 <= 9999);
  snprintf(prefix_buf, sizeof(prefix_buf), "%.4u", set_i + 1);
  // key format:  [SET#][random#]
  std::string skey = ToString(ikey);
  Slice base_key(skey);
  *full_key = std::string(prefix_buf) + base_key.ToString();
  Slice key(*full_key);

  std::string value;
  if (txn != nullptr) {
    s = txn->Get(read_options, key, &value);
  } else {
    s = db->Get(read_options, key, &value);
  }

  if (s.ok()) {
    // Found key, parse its value
    *int_value = std::stoull(value);
    if (*int_value == 0 || *int_value == ULONG_MAX) {
      *unexpected_error = true;
      fprintf(stderr, "Get returned unexpected value: %s\n", value.c_str());
      s = Status::Corruption();
    }
  } else if (s.IsNotFound()) {
    // Have not yet written to this key, so assume its value is 0
    *int_value = 0;
    s = Status::OK();
  }
  return s;
}

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  unsigned int pos_;
  const int value_size_ = 1000; 

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < (unsigned)std::max(1048576, value_size_)) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(unsigned int len) {
    assert(len <= data_.size());
    if (pos_ + len > data_.size()) {
      pos_ = 0;
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }

  Slice GenerateWithTTL(unsigned int len) {
    assert(len <= data_.size());
    if (pos_ + len > data_.size()) {
      pos_ = 0;
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

bool RandomTransactionInserter::DoWriteRandom(std::vector<ColumnFamilyHandle*> handles, TOTransaction* txn) {
    
  RandomGenerator gen;
    
  Status s;

  bool unexpected_error = false;

  uint16_t num_sets = rand_->Next() % num_sets_ + 1;
  std::vector<uint16_t> set_vec(num_sets);
  
  std::iota(set_vec.begin(), set_vec.end(), static_cast<uint16_t>(0));
  std::random_shuffle(set_vec.begin(), set_vec.end(),
                      [&](uint64_t r) { return rand_->Uniform(r); });
                      
  size_t bytes_inserted = 0;
  size_t bytes_read = 0;
  // For each set, pick a key at random and increment it
  for (uint16_t set_i : set_vec) {
    std::string value;
    int rand_value = rand_->Next() % 100;
    uint64_t rand_key = rand_->Next() % num_keys_;
    for(int i = 0; i < conflict_level_ ;i++) {
		rand_key = rand_key / 10;
    }
	
    std::string full_key;

    // We use same rand_num as seed for key and column family so that we
    // can deterministically find the cfh corresponding to a particular
    // key while reading the key.
    size_t columnFamilynum = handles.size();
    ColumnFamilyHandle* handle;
    if (columnFamilynum != 0 ) {
       handle = handles[rand_key % columnFamilynum];
    } else {
       handle = nullptr;
    }
    
    // Five digits (since the largest uint16_t is 65535) plus the NUL
    // end char.
    char prefix_buf[6];
    // Pad prefix appropriately so we can iterate over each set
    assert(set_i + 1 <= 9999);
    snprintf(prefix_buf, sizeof(prefix_buf), "%.4u", set_i + 1);
    // key format:  [SET#][random#]
    std::string skey = ToString(rand_key);
    Slice base_key(skey);
    full_key = std::string(prefix_buf) + base_key.ToString();
    Slice key(full_key);
    
    if(rand_value < readpercent_){
        if (nullptr == handle){
           s = txn->Get(read_options_, key, &value);
        } else{
           s = txn->Get(read_options_, handle, key, &value);
        }
        gets_done_++;
        if(!s.ok() && !s.IsNotFound()){
            fprintf(stderr, "totxn Get error: %s\n", s.ToString().c_str());
            break;
        } else if (s.ok()) {
            found_++;
            bytes_read += key.size() + value_size_;
        } else {
            s = Status::OK();
        }
    } else if(rand_value < readpercent_ + deletepercent_){
        if (nullptr == handle){
           s = txn->Delete(key);
        } else{
           s = txn->Delete(handle, key);
        }
        bytes_inserted += key.size() + value_size_;
        deletes_done_++;
    } else {
        if (nullptr == handle){
           s = txn->Put(key, gen.Generate(value_size_));
        } else{
           s = txn->Put(handle, key, gen.Generate(value_size_));
        }
        bytes_inserted += key.size() + value_size_;
        puts_done_++;   
    }

    if (bytes_inserted > 15000000){
      fprintf(stderr, "opsize exceed max \n");
      break;
    }
    
    if (!s.ok()) {
      // Since we did a GetForUpdate, Put should not fail.
      fprintf(stderr, "Put returned an unexpected error: %s\n",
              s.ToString().c_str());
      
      break;
    }
  }

  if(s.ok()){
     bytes_inserted_ += bytes_inserted;
     bytes_read_ += bytes_read;
  }
  
                      
  if (s.ok()) {
    if (txn != nullptr) {
      std::hash<std::thread::id> hasher;
      char name[64];
      snprintf(name, 64, "txn%" ROCKSDB_PRIszt "-%d", hasher(std::this_thread::get_id()),
               txn_id_++);
      assert(strlen(name) < 64 - 1);

      s = txn->Commit();
      assert(s.ok());
    } 
  } 

  if (s.ok()) {
    success_count_++;
  } else {
    failure_count_++;
    s = txn->Rollback();
    unexpected_error = true;
    assert(s.ok());
  }

  last_status_ = s;

  // return success if we didn't get any unexpected errors
  return !unexpected_error;

}

bool RandomTransactionInserter::DoInsert(DB* db, TOTransaction* txn) {

  Status s;
  WriteBatch batch;

  // pick a random number to use to increment a key in each set
  uint64_t incr = (rand_->Next() % 100) + 1;
  bool unexpected_error = false;

  std::vector<uint16_t> set_vec(num_sets_);
  std::iota(set_vec.begin(), set_vec.end(), static_cast<uint16_t>(0));
  std::random_shuffle(set_vec.begin(), set_vec.end(),
                      [&](uint64_t r) { return rand_->Uniform(r); });
                      
  // For each set, pick a key at random and increment it
  for (uint16_t set_i : set_vec) {
    uint64_t int_value = 0;
    std::string full_key;
    uint64_t rand_key = rand_->Next() % num_keys_;

    s = DBGet(db, txn, read_options_, set_i, rand_key, &int_value, &full_key, &unexpected_error);
    Slice key(full_key);
    if (!s.ok()) {
      // Optimistic transactions should never return non-ok status here.
      // Non-optimistic transactions may return write-coflict/timeout errors.
      if ( !(s.IsBusy() || s.IsTimedOut() || s.IsTryAgain())) {
        fprintf(stderr, "Get returned an unexpected error: %s\n",
                s.ToString().c_str());
        unexpected_error = true;
      }
      break;
    }
    
    if (s.ok()) {
      // Increment key
      std::string sum = ToString(int_value + incr);
      if (txn != nullptr) {
        s = txn->Put(key, sum);
        if ( s.IsBusy() || s.IsTimedOut()) {
          // If the initial get was not for update, then the key is not locked
          // before put and put could fail due to concurrent writes.
          break;
        } else if (!s.ok()) {
          // Since we did a GetForUpdate, Put should not fail.
          fprintf(stderr, "Put returned an unexpected error: %s\n",
                  s.ToString().c_str());
          unexpected_error = true;
        }
      }
      bytes_inserted_ += key.size() + sum.size();
    }
  }
                      
  if (s.ok()) {
    if (txn != nullptr) {
      std::hash<std::thread::id> hasher;
      char name[64];
      snprintf(name, 64, "txn%" ROCKSDB_PRIszt "-%d", hasher(std::this_thread::get_id()),
               txn_id_++);
      assert(strlen(name) < 64 - 1);

      struct timeval tv;
      gettimeofday(&tv,NULL);
      s = txn->SetCommitTimeStamp(tv.tv_sec);
      if (!rand_->OneIn(20)) {
        s = txn->Commit();
      } else {
        // Also try 5% rollback
        s = txn->Rollback();
        assert(s.ok());
      }
      assert(s.ok());
    } 
  } else {
    if (txn != nullptr) {
      assert(txn->Rollback().ok());
    }
  }

  if (s.ok()) {
    success_count_++;
  } else {
    failure_count_++;
  }

  last_status_ = s;

  // return success if we didn't get any unexpected errors
  return !unexpected_error;
}


bool RandomTransactionInserter::DoInsert(DB* db, Transaction* txn,
                                         bool is_optimistic) {
  Status s;
  WriteBatch batch;

  // pick a random number to use to increment a key in each set
  uint64_t incr = (rand_->Next() % 100) + 1;
  bool unexpected_error = false;

  std::vector<uint16_t> set_vec(num_sets_);
  std::iota(set_vec.begin(), set_vec.end(), static_cast<uint16_t>(0));
  std::shuffle(set_vec.begin(), set_vec.end(), std::random_device{});

  // For each set, pick a key at random and increment it
  for (uint16_t set_i : set_vec) {
    uint64_t int_value = 0;
    std::string full_key;
    uint64_t rand_key = rand_->Next() % num_keys_;
    const bool get_for_update = txn ? rand_->OneIn(2) : false;
    s = DBGet(db, txn, read_options_, set_i, rand_key, get_for_update,
              &int_value, &full_key, &unexpected_error);
    Slice key(full_key);
    if (!s.ok()) {
      // Optimistic transactions should never return non-ok status here.
      // Non-optimistic transactions may return write-coflict/timeout errors.
      if (is_optimistic || !(s.IsBusy() || s.IsTimedOut() || s.IsTryAgain())) {
        fprintf(stderr, "Get returned an unexpected error: %s\n",
                s.ToString().c_str());
        unexpected_error = true;
      }
      break;
    }

    if (s.ok()) {
      // Increment key
      std::string sum = ToString(int_value + incr);
      if (txn != nullptr) {
        s = txn->Put(key, sum);
        if (!get_for_update && (s.IsBusy() || s.IsTimedOut())) {
          // If the initial get was not for update, then the key is not locked
          // before put and put could fail due to concurrent writes.
          break;
        } else if (!s.ok()) {
          // Since we did a GetForUpdate, Put should not fail.
          fprintf(stderr, "Put returned an unexpected error: %s\n",
                  s.ToString().c_str());
          unexpected_error = true;
        }
      } else {
        batch.Put(key, sum);
      }
      bytes_inserted_ += key.size() + sum.size();
    }
  }

  if (s.ok()) {
    if (txn != nullptr) {
      if (!is_optimistic && !rand_->OneIn(10)) {
        // also try commit without prpare
        s = txn->Prepare();
        assert(s.ok());
      }
      if (!rand_->OneIn(20)) {
        s = txn->Commit();
      } else {
        // Also try 5% rollback
        s = txn->Rollback();
        assert(s.ok());
      }
      assert(is_optimistic || s.ok());

      if (!s.ok()) {
        if (is_optimistic) {
          // Optimistic transactions can have write-conflict errors on commit.
          // Any other error is unexpected.
          if (!(s.IsBusy() || s.IsTimedOut() || s.IsTryAgain())) {
            unexpected_error = true;
          }
        } else {
          // Non-optimistic transactions should only fail due to expiration
          // or write failures.  For testing purproses, we do not expect any
          // write failures.
          if (!s.IsExpired()) {
            unexpected_error = true;
          }
        }

        if (unexpected_error) {
          fprintf(stderr, "Commit returned an unexpected error: %s\n",
                  s.ToString().c_str());
        }
      }
    } else {
      s = db->Write(write_options_, &batch);
      if (!s.ok()) {
        unexpected_error = true;
        fprintf(stderr, "Write returned an unexpected error: %s\n",
                s.ToString().c_str());
      }
    }
  } else {
    if (txn != nullptr) {
      assert(txn->Rollback().ok());
    }
  }

  if (s.ok()) {
    success_count_++;
  } else {
    failure_count_++;
  }

  last_status_ = s;

  // return success if we didn't get any unexpected errors
  return !unexpected_error;
}

// Verify that the sum of the keys in each set are equal
Status RandomTransactionInserter::Verify(DB* db, uint16_t num_sets,
                                         uint64_t num_keys_per_set,
                                         bool take_snapshot, Random64* rand) {
  uint64_t prev_total = 0;
  uint32_t prev_i = 0;
  bool prev_assigned = false;

  ReadOptions roptions;
  if (take_snapshot) {
    roptions.snapshot = db->GetSnapshot();
  }

  std::vector<uint16_t> set_vec(num_sets);
  std::iota(set_vec.begin(), set_vec.end(), static_cast<uint16_t>(0));
  std::shuffle(set_vec.begin(), set_vec.end(), std::random_device{});

  // For each set of keys with the same prefix, sum all the values
  for (uint16_t set_i : set_vec) {
    // Five digits (since the largest uint16_t is 65535) plus the NUL
    // end char.
    char prefix_buf[6];
    assert(set_i + 1 <= 9999);
    snprintf(prefix_buf, sizeof(prefix_buf), "%.4u", set_i + 1);
    uint64_t total = 0;

    // Use either point lookup or iterator. Point lookups are slower so we use
    // it less often.
    if (num_keys_per_set != 0 && rand && rand->OneIn(10)) {  // use point lookup
      ReadOptions read_options;
      for (uint64_t k = 0; k < num_keys_per_set; k++) {
        std::string dont_care;
        uint64_t int_value = 0;
        bool unexpected_error = false;
        const bool FOR_UPDATE = false;
        Status s = DBGet(db, nullptr, roptions, set_i, k, FOR_UPDATE,
                         &int_value, &dont_care, &unexpected_error);
        assert(s.ok());
        assert(!unexpected_error);
        total += int_value;
      }
    } else {  // user iterators
      Iterator* iter = db->NewIterator(roptions);
      for (iter->Seek(Slice(prefix_buf, 4)); iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        // stop when we reach a different prefix
        if (key.ToString().compare(0, 4, prefix_buf) != 0) {
          break;
        }
        Slice value = iter->value();
        uint64_t int_value = std::stoull(value.ToString());
        if (int_value == 0 || int_value == ULONG_MAX) {
          fprintf(stderr, "Iter returned unexpected value: %s\n",
                  value.ToString().c_str());
          return Status::Corruption();
        }
        total += int_value;
      }
      delete iter;
    }

    if (prev_assigned && total != prev_total) {
      fprintf(stdout,
              "RandomTransactionVerify found inconsistent totals. "
              "Set[%" PRIu32 "]: %" PRIu64 ", Set[%" PRIu32 "]: %" PRIu64 " \n",
              prev_i, prev_total, set_i, total);
      return Status::Corruption();
    }
    prev_total = total;
    prev_i = set_i;
    prev_assigned = true;
  }
  if (take_snapshot) {
    db->ReleaseSnapshot(roptions.snapshot);
  }

  return Status::OK();
}

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
