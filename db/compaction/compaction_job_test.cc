//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "db/compaction/compaction_job.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <map>
#include <string>
#include <tuple>

#include "db/blob/blob_index.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/error_handler.h"
#include "db/version_set.h"
#include "file/random_access_file_reader.h"
#include "file/writable_file_writer.h"
#include "options/options_helper.h"
#include "rocksdb/cache.h"
#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/file_system.h"
#include "rocksdb/options.h"
#include "rocksdb/write_buffer_manager.h"
#include "table/mock_table.h"
#include "table/unique_id_impl.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/string_util.h"
#include "utilities/merge_operators.h"

namespace ROCKSDB_NAMESPACE {

namespace {

void VerifyInitializationOfCompactionJobStats(
      const CompactionJobStats& compaction_job_stats) {
#if !defined(IOS_CROSS_COMPILE)
  ASSERT_EQ(compaction_job_stats.elapsed_micros, 0U);

  ASSERT_EQ(compaction_job_stats.num_input_records, 0U);
  ASSERT_EQ(compaction_job_stats.num_input_files, 0U);
  ASSERT_EQ(compaction_job_stats.num_input_files_at_output_level, 0U);

  ASSERT_EQ(compaction_job_stats.num_output_records, 0U);
  ASSERT_EQ(compaction_job_stats.num_output_files, 0U);

  ASSERT_EQ(compaction_job_stats.is_manual_compaction, true);

  ASSERT_EQ(compaction_job_stats.total_input_bytes, 0U);
  ASSERT_EQ(compaction_job_stats.total_output_bytes, 0U);

  ASSERT_EQ(compaction_job_stats.total_input_raw_key_bytes, 0U);
  ASSERT_EQ(compaction_job_stats.total_input_raw_value_bytes, 0U);

  ASSERT_EQ(compaction_job_stats.smallest_output_key_prefix[0], 0);
  ASSERT_EQ(compaction_job_stats.largest_output_key_prefix[0], 0);

  ASSERT_EQ(compaction_job_stats.num_records_replaced, 0U);

  ASSERT_EQ(compaction_job_stats.num_input_deletion_records, 0U);
  ASSERT_EQ(compaction_job_stats.num_expired_deletion_records, 0U);

  ASSERT_EQ(compaction_job_stats.num_corrupt_keys, 0U);
#endif  // !defined(IOS_CROSS_COMPILE)
}

// Mock FSWritableFile for testing io priority.
// Only override the essential functions for testing compaction io priority.
class MockTestWritableFile : public FSWritableFileOwnerWrapper {
 public:
  MockTestWritableFile(std::unique_ptr<FSWritableFile>&& file,
                       Env::IOPriority io_priority)
      : FSWritableFileOwnerWrapper(std::move(file)),
        write_io_priority_(io_priority) {}
  IOStatus Append(const Slice& data, const IOOptions& options,
                  IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Append(data, options, dbg);
  }
  IOStatus Append(const Slice& data, const IOOptions& options,
                  const DataVerificationInfo& verification_info,
                  IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Append(data, options, verification_info, dbg);
  }
  IOStatus Close(const IOOptions& options, IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Close(options, dbg);
  }
  IOStatus Flush(const IOOptions& options, IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Flush(options, dbg);
  }
  IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Sync(options, dbg);
  }
  IOStatus Fsync(const IOOptions& options, IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Fsync(options, dbg);
  }
  uint64_t GetFileSize(const IOOptions& options, IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->GetFileSize(options, dbg);
  }
  IOStatus RangeSync(uint64_t offset, uint64_t nbytes, const IOOptions& options,
                     IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->RangeSync(offset, nbytes, options, dbg);
  }

  void PrepareWrite(size_t offset, size_t len, const IOOptions& options,
                    IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    target()->PrepareWrite(offset, len, options, dbg);
  }

  IOStatus Allocate(uint64_t offset, uint64_t len, const IOOptions& options,
                    IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, write_io_priority_);
    return target()->Allocate(offset, len, options, dbg);
  }

 private:
  Env::IOPriority write_io_priority_;
};

// Mock FSRandomAccessFile for testing io priority.
// Only override the essential functions for testing compaction io priority.
class MockTestRandomAccessFile : public FSRandomAccessFileOwnerWrapper {
 public:
  MockTestRandomAccessFile(std::unique_ptr<FSRandomAccessFile>&& file,
                           Env::IOPriority io_priority)
      : FSRandomAccessFileOwnerWrapper(std::move(file)),
        read_io_priority_(io_priority) {}

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override {
    EXPECT_EQ(options.rate_limiter_priority, read_io_priority_);
    return target()->Read(offset, n, options, result, scratch, dbg);
  }
  IOStatus Prefetch(uint64_t offset, size_t n, const IOOptions& options,
                    IODebugContext* dbg) override {
    EXPECT_EQ(options.rate_limiter_priority, read_io_priority_);
    return target()->Prefetch(offset, n, options, dbg);
  }

 private:
  Env::IOPriority read_io_priority_;
};

// Mock FileSystem for testing io priority.
class MockTestFileSystem : public FileSystemWrapper {
 public:
  explicit MockTestFileSystem(const std::shared_ptr<FileSystem>& base,
                              Env::IOPriority read_io_priority,
                              Env::IOPriority write_io_priority)
      : FileSystemWrapper(base),
        read_io_priority_(read_io_priority),
        write_io_priority_(write_io_priority) {}

  static const char* kClassName() { return "MockTestFileSystem"; }
  const char* Name() const override { return kClassName(); }

  IOStatus NewRandomAccessFile(const std::string& fname,
                               const FileOptions& file_opts,
                               std::unique_ptr<FSRandomAccessFile>* result,
                               IODebugContext* dbg) override {
    IOStatus s = target()->NewRandomAccessFile(fname, file_opts, result, dbg);
    EXPECT_OK(s);
    result->reset(
        new MockTestRandomAccessFile(std::move(*result), read_io_priority_));
    return s;
  }
  IOStatus NewWritableFile(const std::string& fname,
                           const FileOptions& file_opts,
                           std::unique_ptr<FSWritableFile>* result,
                           IODebugContext* dbg) override {
    IOStatus s = target()->NewWritableFile(fname, file_opts, result, dbg);
    EXPECT_OK(s);
    result->reset(
        new MockTestWritableFile(std::move(*result), write_io_priority_));
    return s;
  }

 private:
  Env::IOPriority read_io_priority_;
  Env::IOPriority write_io_priority_;
};

enum TableTypeForTest : uint8_t { kMockTable = 0, kBlockBasedTable = 1 };

}  // namespace

class CompactionJobTestBase : public testing::Test {
 protected:
  CompactionJobTestBase(std::string dbname, const Comparator* ucmp,
                        std::function<std::string(uint64_t)> encode_u64_ts,
                        bool test_io_priority, TableTypeForTest table_type)
      : dbname_(std::move(dbname)),
        ucmp_(ucmp),
        db_options_(),
        mutable_cf_options_(cf_options_),
        mutable_db_options_(),
        table_cache_(NewLRUCache(50000, 16)),
        write_controller_(db_options_.use_dynamic_delay),
        write_buffer_manager_(db_options_.db_write_buffer_size),
        versions_(new VersionSet(
            dbname_, &db_options_, env_options_, table_cache_.get(),
            &write_buffer_manager_, &write_controller_,
            /*block_cache_tracer=*/nullptr,
            /*io_tracer=*/nullptr, /*db_id*/ "", /*db_session_id*/ "")),
        shutting_down_(false),
        mock_table_factory_(new mock::MockTableFactory()),
        error_handler_(nullptr, db_options_, &mutex_),
        encode_u64_ts_(std::move(encode_u64_ts)),
        test_io_priority_(test_io_priority),
        table_type_(table_type) {
    Env* base_env = Env::Default();
    EXPECT_OK(
        test::CreateEnvFromSystem(ConfigOptions(), &base_env, &env_guard_));
    env_ = base_env;
    fs_ = env_->GetFileSystem();
  }

  void SetUp() override {
    EXPECT_OK(env_->CreateDirIfMissing(dbname_));
    db_options_.env = env_;
    db_options_.fs = fs_;
    db_options_.db_paths.emplace_back(dbname_,
                                      std::numeric_limits<uint64_t>::max());
    cf_options_.comparator = ucmp_;
    if (table_type_ == TableTypeForTest::kBlockBasedTable) {
      BlockBasedTableOptions table_options;
      cf_options_.table_factory.reset(NewBlockBasedTableFactory(table_options));
    } else if (table_type_ == TableTypeForTest::kMockTable) {
      cf_options_.table_factory = mock_table_factory_;
    } else {
      assert(false);
    }
  }

  std::string GenerateFileName(uint64_t file_number) {
    FileMetaData meta;
    std::vector<DbPath> db_paths;
    db_paths.emplace_back(dbname_, std::numeric_limits<uint64_t>::max());
    meta.fd = FileDescriptor(file_number, 0, 0);
    return TableFileName(db_paths, meta.fd.GetNumber(), meta.fd.GetPathId());
  }

  std::string KeyStr(const std::string& user_key, const SequenceNumber seq_num,
                     const ValueType t, uint64_t ts = 0) {
    std::string user_key_with_ts = user_key + encode_u64_ts_(ts);
    return InternalKey(user_key_with_ts, seq_num, t).Encode().ToString();
  }

  static std::string BlobStr(uint64_t blob_file_number, uint64_t offset,
                             uint64_t size) {
    std::string blob_index;
    BlobIndex::EncodeBlob(&blob_index, blob_file_number, offset, size,
                          kNoCompression);
    return blob_index;
  }

  static std::string BlobStrTTL(uint64_t blob_file_number, uint64_t offset,
                                uint64_t size, uint64_t expiration) {
    std::string blob_index;
    BlobIndex::EncodeBlobTTL(&blob_index, expiration, blob_file_number, offset,
                             size, kNoCompression);
    return blob_index;
  }

  static std::string BlobStrInlinedTTL(const Slice& value,
                                       uint64_t expiration) {
    std::string blob_index;
    BlobIndex::EncodeInlinedTTL(&blob_index, expiration, value);
    return blob_index;
  }

  // Creates a table with the specificied key value pairs.
  void CreateTable(const std::string& table_name,
                   const mock::KVVector& contents, uint64_t& file_size) {
    std::unique_ptr<WritableFileWriter> file_writer;
    Status s = WritableFileWriter::Create(fs_, table_name, FileOptions(),
                                          &file_writer, nullptr);
    ASSERT_OK(s);
    std::unique_ptr<TableBuilder> table_builder(
        cf_options_.table_factory->NewTableBuilder(
            TableBuilderOptions(*cfd_->ioptions(), mutable_cf_options_,
                                cfd_->internal_comparator(),
                                cfd_->int_tbl_prop_collector_factories(),
                                CompressionType::kNoCompression,
                                CompressionOptions(), 0 /* column_family_id */,
                                kDefaultColumnFamilyName, -1 /* level */),
            file_writer.get()));
    // Build table.
    for (auto kv : contents) {
      std::string key;
      std::string value;
      std::tie(key, value) = kv;
      table_builder->Add(key, value);
    }
    ASSERT_OK(table_builder->Finish());
    file_size = table_builder->FileSize();
  }

  void AddMockFile(const mock::KVVector& contents, int level = 0) {
    assert(contents.size() > 0);

    bool first_key = true;
    std::string smallest, largest;
    InternalKey smallest_key, largest_key;
    SequenceNumber smallest_seqno = kMaxSequenceNumber;
    SequenceNumber largest_seqno = 0;
    uint64_t oldest_blob_file_number = kInvalidBlobFileNumber;
    for (auto kv : contents) {
      ParsedInternalKey key;
      std::string skey;
      std::string value;
      std::tie(skey, value) = kv;
      const Status pik_status =
          ParseInternalKey(skey, &key, true /* log_err_key */);

      smallest_seqno = std::min(smallest_seqno, key.sequence);
      largest_seqno = std::max(largest_seqno, key.sequence);

      if (first_key ||
          cfd_->user_comparator()->Compare(key.user_key, smallest) < 0) {
        smallest.assign(key.user_key.data(), key.user_key.size());
        smallest_key.DecodeFrom(skey);
      }
      if (first_key ||
          cfd_->user_comparator()->Compare(key.user_key, largest) > 0) {
        largest.assign(key.user_key.data(), key.user_key.size());
        largest_key.DecodeFrom(skey);
      }

      first_key = false;

      if (pik_status.ok() && key.type == kTypeBlobIndex) {
        BlobIndex blob_index;
        const Status s = blob_index.DecodeFrom(value);
        if (!s.ok()) {
          continue;
        }

        if (blob_index.IsInlined() || blob_index.HasTTL() ||
            blob_index.file_number() == kInvalidBlobFileNumber) {
          continue;
        }

        if (oldest_blob_file_number == kInvalidBlobFileNumber ||
            oldest_blob_file_number > blob_index.file_number()) {
          oldest_blob_file_number = blob_index.file_number();
        }
      }
    }

    uint64_t file_number = versions_->NewFileNumber();

    uint64_t file_size = 0;
    if (table_type_ == TableTypeForTest::kBlockBasedTable) {
      CreateTable(GenerateFileName(file_number), contents, file_size);
    } else if (table_type_ == TableTypeForTest::kMockTable) {
      file_size = 10;
      EXPECT_OK(mock_table_factory_->CreateMockTable(
          env_, GenerateFileName(file_number), std::move(contents)));
    } else {
      assert(false);
    }

    VersionEdit edit;
    edit.AddFile(level, file_number, 0, file_size, smallest_key, largest_key,
                 smallest_seqno, largest_seqno, false, Temperature::kUnknown,
                 oldest_blob_file_number, kUnknownOldestAncesterTime,
                 kUnknownFileCreationTime, kUnknownFileChecksum,
                 kUnknownFileChecksumFuncName, kNullUniqueId64x2);

    mutex_.Lock();
    EXPECT_OK(
        versions_->LogAndApply(versions_->GetColumnFamilySet()->GetDefault(),
                               mutable_cf_options_, &edit, &mutex_, nullptr));
    mutex_.Unlock();
  }

  void VerifyTables(int output_level,
                    const std::vector<mock::KVVector>& expected_results,
                    std::vector<uint64_t> expected_oldest_blob_file_numbers) {
    if (expected_results.empty()) {
      ASSERT_EQ(compaction_job_stats_.num_output_files, 0U);
      return;
    }
    int expected_output_file_num = 0;
    for (const auto& e : expected_results) {
      if (!e.empty()) {
        ++expected_output_file_num;
      }
    }
    ASSERT_EQ(expected_output_file_num, compaction_job_stats_.num_output_files);
    if (expected_output_file_num == 0) {
      return;
    }

    if (expected_oldest_blob_file_numbers.empty()) {
      expected_oldest_blob_file_numbers.resize(expected_output_file_num,
                                               kInvalidBlobFileNumber);
    }

    auto cfd = versions_->GetColumnFamilySet()->GetDefault();
    if (table_type_ == TableTypeForTest::kMockTable) {
      assert(expected_results.size() == 1);
      mock_table_factory_->AssertLatestFile(expected_results[0]);
    } else {
      assert(table_type_ == TableTypeForTest::kBlockBasedTable);
    }

    auto output_files =
        cfd->current()->storage_info()->LevelFiles(output_level);
    ASSERT_EQ(expected_output_file_num, output_files.size());

    if (table_type_ == TableTypeForTest::kMockTable) {
      assert(output_files.size() == 1);
      const FileMetaData* const output_file = output_files[0];
      ASSERT_EQ(output_file->oldest_blob_file_number,
                expected_oldest_blob_file_numbers[0]);
      return;
    }

    for (size_t i = 0; i < expected_results.size(); ++i) {
      const FileMetaData* const output_file = output_files[i];
      std::string file_name = GenerateFileName(output_file->fd.GetNumber());
      const auto& fs = env_->GetFileSystem();
      std::unique_ptr<RandomAccessFileReader> freader;
      IOStatus ios = RandomAccessFileReader::Create(
          fs, file_name, FileOptions(), &freader, nullptr);
      ASSERT_OK(ios);
      std::unique_ptr<TableReader> table_reader;
      uint64_t file_size = output_file->fd.GetFileSize();
      ReadOptions read_opts;
      Status s = cf_options_.table_factory->NewTableReader(
          read_opts,
          TableReaderOptions(*cfd->ioptions(), nullptr, FileOptions(),
                             cfd_->internal_comparator()),
          std::move(freader), file_size, &table_reader, false);
      ASSERT_OK(s);
      assert(table_reader);
      std::unique_ptr<InternalIterator> iiter(
          table_reader->NewIterator(read_opts, nullptr, nullptr, true,
                                    TableReaderCaller::kUncategorized));
      assert(iiter);

      mock::KVVector from_db;
      for (iiter->SeekToFirst(); iiter->Valid(); iiter->Next()) {
        const Slice key = iiter->key();
        const Slice value = iiter->value();
        from_db.emplace_back(
            make_pair(key.ToString(false), value.ToString(false)));
      }
      ASSERT_EQ(expected_results[i], from_db);
    }
  }

  void SetLastSequence(const SequenceNumber sequence_number) {
    versions_->SetLastAllocatedSequence(sequence_number + 1);
    versions_->SetLastPublishedSequence(sequence_number + 1);
    versions_->SetLastSequence(sequence_number + 1);
  }

  // returns expected result after compaction
  mock::KVVector CreateTwoFiles(bool gen_corrupted_keys) {
    stl_wrappers::KVMap expected_results;
    constexpr int kKeysPerFile = 10000;
    constexpr int kCorruptKeysPerFile = 200;
    constexpr int kMatchingKeys = kKeysPerFile / 2;
    SequenceNumber sequence_number = 0;

    auto corrupt_id = [&](int id) {
      return gen_corrupted_keys && id > 0 && id <= kCorruptKeysPerFile;
    };

    for (int i = 0; i < 2; ++i) {
      auto contents = mock::MakeMockFile();
      for (int k = 0; k < kKeysPerFile; ++k) {
        auto key = std::to_string(i * kMatchingKeys + k);
        auto value = std::to_string(i * kKeysPerFile + k);
        InternalKey internal_key(key, ++sequence_number, kTypeValue);

        // This is how the key will look like once it's written in bottommost
        // file
        InternalKey bottommost_internal_key(
            key, 0, kTypeValue);

        if (corrupt_id(k)) {
          test::CorruptKeyType(&internal_key);
          test::CorruptKeyType(&bottommost_internal_key);
        }
        contents.push_back({internal_key.Encode().ToString(), value});
        if (i == 1 || k < kMatchingKeys || corrupt_id(k - kMatchingKeys)) {
          expected_results.insert(
              {bottommost_internal_key.Encode().ToString(), value});
        }
      }
      mock::SortKVVector(&contents, ucmp_);

      AddMockFile(contents);
    }

    SetLastSequence(sequence_number);

    mock::KVVector expected_results_kvvector;
    for (auto& kv : expected_results) {
      expected_results_kvvector.push_back({kv.first, kv.second});
    }

    return expected_results_kvvector;
  }

  void NewDB() {
    EXPECT_OK(DestroyDB(dbname_, Options()));
    EXPECT_OK(env_->CreateDirIfMissing(dbname_));

    std::shared_ptr<Logger> info_log;
    DBOptions db_opts = BuildDBOptions(db_options_, mutable_db_options_);
    Status s = CreateLoggerFromOptions(dbname_, db_opts, &info_log);
    ASSERT_OK(s);
    db_options_.info_log = info_log;

    versions_.reset(
        new VersionSet(dbname_, &db_options_, env_options_, table_cache_.get(),
                       &write_buffer_manager_, &write_controller_,
                       /*block_cache_tracer=*/nullptr, /*io_tracer=*/nullptr,
                       /*db_id*/ "", /*db_session_id*/ ""));
    compaction_job_stats_.Reset();
    ASSERT_OK(SetIdentityFile(env_, dbname_));

    VersionEdit new_db;
    new_db.SetLogNumber(0);
    new_db.SetNextFile(2);
    new_db.SetLastSequence(0);

    const std::string manifest = DescriptorFileName(dbname_, 1);
    std::unique_ptr<WritableFileWriter> file_writer;
    const auto& fs = env_->GetFileSystem();
    s = WritableFileWriter::Create(fs, manifest,
                                   fs->OptimizeForManifestWrite(env_options_),
                                   &file_writer, nullptr);

    ASSERT_OK(s);
    {
      log::Writer log(std::move(file_writer), 0, false);
      std::string record;
      new_db.EncodeTo(&record);
      s = log.AddRecord(record);
    }
    ASSERT_OK(s);
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(fs_.get(), dbname_, 1, nullptr);

    ASSERT_OK(s);

    cf_options_.merge_operator = merge_op_;
    cf_options_.compaction_filter = compaction_filter_.get();
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(kDefaultColumnFamilyName, cf_options_);

    ASSERT_OK(versions_->Recover(column_families, false));
    cfd_ = versions_->GetColumnFamilySet()->GetDefault();
  }

  // input_files[i] on input_levels[i]
  void RunLastLevelCompaction(
      const std::vector<std::vector<FileMetaData*>>& input_files,
      const std::vector<int> input_levels,
      std::function<void(Compaction& comp)>&& verify_func,
      const std::vector<SequenceNumber>& snapshots = {}) {
    const int kLastLevel = cf_options_.num_levels - 1;
    verify_per_key_placement_ = std::move(verify_func);
    mock::KVVector empty_map;
    RunCompaction(input_files, input_levels, {empty_map}, snapshots,
                  kMaxSequenceNumber, kLastLevel, false);
  }

  // input_files[i] on input_levels[i]
  void RunCompaction(
      const std::vector<std::vector<FileMetaData*>>& input_files,
      const std::vector<int>& input_levels,
      const std::vector<mock::KVVector>& expected_results,
      const std::vector<SequenceNumber>& snapshots = {},
      SequenceNumber earliest_write_conflict_snapshot = kMaxSequenceNumber,
      int output_level = 1, bool verify = true,
      std::vector<uint64_t> expected_oldest_blob_file_numbers = {},
      bool check_get_priority = false,
      Env::IOPriority read_io_priority = Env::IO_TOTAL,
      Env::IOPriority write_io_priority = Env::IO_TOTAL,
      int max_subcompactions = 0) {
    // For compaction, set fs as MockTestFileSystem to check the io_priority.
    if (test_io_priority_) {
      db_options_.fs.reset(
          new MockTestFileSystem(fs_, read_io_priority, write_io_priority));
    }

    auto cfd = versions_->GetColumnFamilySet()->GetDefault();

    size_t num_input_files = 0;
    std::vector<CompactionInputFiles> compaction_input_files;
    for (size_t i = 0; i < input_files.size(); ++i) {
      auto level_files = input_files[i];
      CompactionInputFiles compaction_level;
      compaction_level.level = input_levels[i];
      compaction_level.files.insert(compaction_level.files.end(),
          level_files.begin(), level_files.end());
      compaction_input_files.push_back(compaction_level);
      num_input_files += level_files.size();
    }

    Compaction compaction(
        cfd->current()->storage_info(), *cfd->ioptions(),
        *cfd->GetLatestMutableCFOptions(), mutable_db_options_,
        compaction_input_files, output_level, 1024 * 1024, 10 * 1024 * 1024, 0,
        kNoCompression, cfd->GetLatestMutableCFOptions()->compression_opts,
        Temperature::kUnknown, max_subcompactions, {}, true);
    compaction.SetInputVersion(cfd->current());

    assert(db_options_.info_log);
    LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL, db_options_.info_log.get());
    mutex_.Lock();
    EventLogger event_logger(db_options_.info_log.get());
    // TODO(yiwu) add a mock snapshot checker and add test for it.
    SnapshotChecker* snapshot_checker = nullptr;
    ASSERT_TRUE(full_history_ts_low_.empty() ||
                ucmp_->timestamp_size() == full_history_ts_low_.size());
    const std::atomic<bool> kManualCompactionCanceledFalse{false};
    CompactionJob compaction_job(
        0, &compaction, db_options_, mutable_db_options_, env_options_,
        versions_.get(), &shutting_down_, &log_buffer, nullptr, nullptr,
        nullptr, nullptr, &mutex_, &error_handler_, snapshots,
        earliest_write_conflict_snapshot, snapshot_checker, nullptr,
        table_cache_, &event_logger, false, false, dbname_,
        &compaction_job_stats_, Env::Priority::USER, nullptr /* IOTracer */,
        /*manual_compaction_canceled=*/kManualCompactionCanceledFalse,
        env_->GenerateUniqueId(), DBImpl::GenerateDbSessionId(nullptr),
        full_history_ts_low_);
    VerifyInitializationOfCompactionJobStats(compaction_job_stats_);

    compaction_job.Prepare();
    mutex_.Unlock();
    Status s = compaction_job.Run();
    ASSERT_OK(s);
    ASSERT_OK(compaction_job.io_status());
    mutex_.Lock();
    ASSERT_OK(compaction_job.Install(*cfd->GetLatestMutableCFOptions()));
    ASSERT_OK(compaction_job.io_status());
    mutex_.Unlock();
    log_buffer.FlushBufferToLog();

    if (verify) {
      ASSERT_GE(compaction_job_stats_.elapsed_micros, 0U);
      ASSERT_EQ(compaction_job_stats_.num_input_files, num_input_files);

      VerifyTables(output_level, expected_results,
                   expected_oldest_blob_file_numbers);
    }

    if (check_get_priority) {
      CheckGetRateLimiterPriority(compaction_job);
    }

    if (verify_per_key_placement_) {
      // Verify per_key_placement compaction
      assert(compaction.SupportsPerKeyPlacement());
      verify_per_key_placement_(compaction);
    }
  }

  void CheckGetRateLimiterPriority(CompactionJob& compaction_job) {
    // When the state from WriteController is normal.
    ASSERT_EQ(compaction_job.GetRateLimiterPriority(), Env::IO_LOW);

    WriteController* write_controller =
        compaction_job.versions_->GetColumnFamilySet()->write_controller();

    {
      // When the state from WriteController is Delayed.
      std::unique_ptr<WriteControllerToken> delay_token =
          write_controller->GetDelayToken(1000000);
      ASSERT_EQ(compaction_job.GetRateLimiterPriority(), Env::IO_USER);
    }

    {
      // When the state from WriteController is Stopped.
      std::unique_ptr<WriteControllerToken> stop_token =
          write_controller->GetStopToken();
      ASSERT_EQ(compaction_job.GetRateLimiterPriority(), Env::IO_USER);
    }
  }

  std::shared_ptr<Env> env_guard_;
  Env* env_;
  std::shared_ptr<FileSystem> fs_;
  std::string dbname_;
  const Comparator* const ucmp_;
  EnvOptions env_options_;
  ImmutableDBOptions db_options_;
  ColumnFamilyOptions cf_options_;
  MutableCFOptions mutable_cf_options_;
  MutableDBOptions mutable_db_options_;
  std::shared_ptr<Cache> table_cache_;
  WriteController write_controller_;
  WriteBufferManager write_buffer_manager_;
  std::unique_ptr<VersionSet> versions_;
  InstrumentedMutex mutex_;
  std::atomic<bool> shutting_down_;
  std::shared_ptr<mock::MockTableFactory> mock_table_factory_;
  CompactionJobStats compaction_job_stats_;
  ColumnFamilyData* cfd_;
  std::unique_ptr<CompactionFilter> compaction_filter_;
  std::shared_ptr<MergeOperator> merge_op_;
  ErrorHandler error_handler_;
  std::string full_history_ts_low_;
  const std::function<std::string(uint64_t)> encode_u64_ts_;
  const bool test_io_priority_;
  std::function<void(Compaction& comp)> verify_per_key_placement_;
  const TableTypeForTest table_type_ = kMockTable;
};

// TODO(icanadi) Make it simpler once we mock out VersionSet
class CompactionJobTest : public CompactionJobTestBase {
 public:
  CompactionJobTest()
      : CompactionJobTestBase(
            test::PerThreadDBPath("compaction_job_test"), BytewiseComparator(),
            [](uint64_t /*ts*/) { return ""; }, /*test_io_priority=*/false,
            TableTypeForTest::kMockTable) {}
};

TEST_F(CompactionJobTest, Simple) {
  NewDB();

  auto expected_results = CreateTwoFiles(false);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  ASSERT_EQ(2U, files.size());
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, DISABLED_SimpleCorrupted) {
  NewDB();

  auto expected_results = CreateTwoFiles(true);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
  ASSERT_EQ(compaction_job_stats_.num_corrupt_keys, 400U);
}

TEST_F(CompactionJobTest, SimpleDeletion) {
  NewDB();

  auto file1 = mock::MakeMockFile({{KeyStr("c", 4U, kTypeDeletion), ""},
                                   {KeyStr("c", 3U, kTypeValue), "val"}});
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({{KeyStr("b", 2U, kTypeValue), "val"},
                                   {KeyStr("b", 1U, kTypeValue), "val"}});
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("b", 0U, kTypeValue), "val"}});

  SetLastSequence(4U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, OutputNothing) {
  NewDB();

  auto file1 = mock::MakeMockFile({{KeyStr("a", 1U, kTypeValue), "val"}});

  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({{KeyStr("a", 2U, kTypeDeletion), ""}});

  AddMockFile(file2);

  auto expected_results = mock::MakeMockFile();

  SetLastSequence(4U);

  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, SimpleOverwrite) {
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("a", 3U, kTypeValue), "val2"},
      {KeyStr("b", 4U, kTypeValue), "val3"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({{KeyStr("a", 1U, kTypeValue), "val"},
                                   {KeyStr("b", 2U, kTypeValue), "val"}});
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 0U, kTypeValue), "val2"},
                          {KeyStr("b", 0U, kTypeValue), "val3"}});

  SetLastSequence(4U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, SimpleNonLastLevel) {
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("a", 5U, kTypeValue), "val2"},
      {KeyStr("b", 6U, kTypeValue), "val3"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({{KeyStr("a", 3U, kTypeValue), "val"},
                                   {KeyStr("b", 4U, kTypeValue), "val"}});
  AddMockFile(file2, 1);

  auto file3 = mock::MakeMockFile({{KeyStr("a", 1U, kTypeValue), "val"},
                                   {KeyStr("b", 2U, kTypeValue), "val"}});
  AddMockFile(file3, 2);

  // Because level 1 is not the last level, the sequence numbers of a and b
  // cannot be set to 0
  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 5U, kTypeValue), "val2"},
                          {KeyStr("b", 6U, kTypeValue), "val3"}});

  SetLastSequence(6U);
  const std::vector<int> input_levels = {0, 1};
  auto lvl0_files =
      cfd_->current()->storage_info()->LevelFiles(input_levels[0]);
  auto lvl1_files =
      cfd_->current()->storage_info()->LevelFiles(input_levels[1]);
  RunCompaction({lvl0_files, lvl1_files}, input_levels, {expected_results});
}

TEST_F(CompactionJobTest, SimpleMerge) {
  merge_op_ = MergeOperators::CreateStringAppendOperator();
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("a", 5U, kTypeMerge), "5"},
      {KeyStr("a", 4U, kTypeMerge), "4"},
      {KeyStr("a", 3U, kTypeValue), "3"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile(
      {{KeyStr("b", 2U, kTypeMerge), "2"}, {KeyStr("b", 1U, kTypeValue), "1"}});
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 0U, kTypeValue), "3,4,5"},
                          {KeyStr("b", 0U, kTypeValue), "1,2"}});

  SetLastSequence(5U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, NonAssocMerge) {
  merge_op_ = MergeOperators::CreateStringAppendTESTOperator();
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("a", 5U, kTypeMerge), "5"},
      {KeyStr("a", 4U, kTypeMerge), "4"},
      {KeyStr("a", 3U, kTypeMerge), "3"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile(
      {{KeyStr("b", 2U, kTypeMerge), "2"}, {KeyStr("b", 1U, kTypeMerge), "1"}});
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 0U, kTypeValue), "3,4,5"},
                          {KeyStr("b", 0U, kTypeValue), "1,2"}});

  SetLastSequence(5U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

// Filters merge operands with value 10.
TEST_F(CompactionJobTest, MergeOperandFilter) {
  merge_op_ = MergeOperators::CreateUInt64AddOperator();
  compaction_filter_.reset(new test::FilterNumber(10U));
  NewDB();

  auto file1 = mock::MakeMockFile(
      {{KeyStr("a", 5U, kTypeMerge), test::EncodeInt(5U)},
       {KeyStr("a", 4U, kTypeMerge), test::EncodeInt(10U)},  // Filtered
       {KeyStr("a", 3U, kTypeMerge), test::EncodeInt(3U)}});
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({
      {KeyStr("b", 2U, kTypeMerge), test::EncodeInt(2U)},
      {KeyStr("b", 1U, kTypeMerge), test::EncodeInt(10U)}  // Filtered
  });
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 0U, kTypeValue), test::EncodeInt(8U)},
                          {KeyStr("b", 0U, kTypeValue), test::EncodeInt(2U)}});

  SetLastSequence(5U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, FilterSomeMergeOperands) {
  merge_op_ = MergeOperators::CreateUInt64AddOperator();
  compaction_filter_.reset(new test::FilterNumber(10U));
  NewDB();

  auto file1 = mock::MakeMockFile(
      {{KeyStr("a", 5U, kTypeMerge), test::EncodeInt(5U)},
       {KeyStr("a", 4U, kTypeMerge), test::EncodeInt(10U)},  // Filtered
       {KeyStr("a", 3U, kTypeValue), test::EncodeInt(5U)},
       {KeyStr("d", 8U, kTypeMerge), test::EncodeInt(10U)}});
  AddMockFile(file1);

  auto file2 =
      mock::MakeMockFile({{KeyStr("b", 2U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 1U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("c", 2U, kTypeMerge), test::EncodeInt(3U)},
                          {KeyStr("c", 1U, kTypeValue), test::EncodeInt(7U)},
                          {KeyStr("d", 1U, kTypeValue), test::EncodeInt(6U)}});
  AddMockFile(file2);

  auto file3 =
      mock::MakeMockFile({{KeyStr("a", 1U, kTypeMerge), test::EncodeInt(3U)}});
  AddMockFile(file3, 2);

  auto expected_results = mock::MakeMockFile({
      {KeyStr("a", 5U, kTypeValue), test::EncodeInt(10U)},
      {KeyStr("c", 2U, kTypeValue), test::EncodeInt(10U)},
      {KeyStr("d", 1U, kTypeValue), test::EncodeInt(6U)}
      // b does not appear because the operands are filtered
  });

  SetLastSequence(5U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

// Test where all operands/merge results are filtered out.
TEST_F(CompactionJobTest, FilterAllMergeOperands) {
  merge_op_ = MergeOperators::CreateUInt64AddOperator();
  compaction_filter_.reset(new test::FilterNumber(10U));
  NewDB();

  auto file1 =
      mock::MakeMockFile({{KeyStr("a", 11U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("a", 10U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("a", 9U, kTypeMerge), test::EncodeInt(10U)}});
  AddMockFile(file1);

  auto file2 =
      mock::MakeMockFile({{KeyStr("b", 8U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 7U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 6U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 5U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 4U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 3U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 2U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("c", 2U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("c", 1U, kTypeMerge), test::EncodeInt(10U)}});
  AddMockFile(file2);

  auto file3 =
      mock::MakeMockFile({{KeyStr("a", 2U, kTypeMerge), test::EncodeInt(10U)},
                          {KeyStr("b", 1U, kTypeMerge), test::EncodeInt(10U)}});
  AddMockFile(file3, 2);

  SetLastSequence(11U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);

  mock::KVVector empty_map;
  RunCompaction({files}, {input_level}, {empty_map});
}

TEST_F(CompactionJobTest, SimpleSingleDelete) {
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("a", 5U, kTypeDeletion), ""},
      {KeyStr("b", 6U, kTypeSingleDeletion), ""},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({{KeyStr("a", 3U, kTypeValue), "val"},
                                   {KeyStr("b", 4U, kTypeValue), "val"}});
  AddMockFile(file2);

  auto file3 = mock::MakeMockFile({
      {KeyStr("a", 1U, kTypeValue), "val"},
  });
  AddMockFile(file3, 2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 5U, kTypeDeletion), ""}});

  SetLastSequence(6U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, SingleDeleteSnapshots) {
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("A", 12U, kTypeSingleDeletion), ""},
      {KeyStr("a", 12U, kTypeSingleDeletion), ""},
      {KeyStr("b", 21U, kTypeSingleDeletion), ""},
      {KeyStr("c", 22U, kTypeSingleDeletion), ""},
      {KeyStr("d", 9U, kTypeSingleDeletion), ""},
      {KeyStr("f", 21U, kTypeSingleDeletion), ""},
      {KeyStr("j", 11U, kTypeSingleDeletion), ""},
      {KeyStr("j", 9U, kTypeSingleDeletion), ""},
      {KeyStr("k", 12U, kTypeSingleDeletion), ""},
      {KeyStr("k", 11U, kTypeSingleDeletion), ""},
      {KeyStr("l", 3U, kTypeSingleDeletion), ""},
      {KeyStr("l", 2U, kTypeSingleDeletion), ""},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({
      {KeyStr("0", 2U, kTypeSingleDeletion), ""},
      {KeyStr("a", 11U, kTypeValue), "val1"},
      {KeyStr("b", 11U, kTypeValue), "val2"},
      {KeyStr("c", 21U, kTypeValue), "val3"},
      {KeyStr("d", 8U, kTypeValue), "val4"},
      {KeyStr("e", 2U, kTypeSingleDeletion), ""},
      {KeyStr("f", 1U, kTypeValue), "val1"},
      {KeyStr("g", 11U, kTypeSingleDeletion), ""},
      {KeyStr("h", 2U, kTypeSingleDeletion), ""},
      {KeyStr("m", 12U, kTypeValue), "val1"},
      {KeyStr("m", 11U, kTypeSingleDeletion), ""},
      {KeyStr("m", 8U, kTypeValue), "val2"},
  });
  AddMockFile(file2);

  auto file3 = mock::MakeMockFile({
      {KeyStr("A", 1U, kTypeValue), "val"},
      {KeyStr("e", 1U, kTypeValue), "val"},
  });
  AddMockFile(file3, 2);

  auto expected_results = mock::MakeMockFile({
      {KeyStr("A", 12U, kTypeSingleDeletion), ""},
      {KeyStr("a", 12U, kTypeSingleDeletion), ""},
      {KeyStr("a", 11U, kTypeValue), ""},
      {KeyStr("b", 21U, kTypeSingleDeletion), ""},
      {KeyStr("b", 11U, kTypeValue), "val2"},
      {KeyStr("c", 22U, kTypeSingleDeletion), ""},
      {KeyStr("c", 21U, kTypeValue), ""},
      {KeyStr("e", 2U, kTypeSingleDeletion), ""},
      {KeyStr("f", 21U, kTypeSingleDeletion), ""},
      {KeyStr("f", 1U, kTypeValue), "val1"},
      {KeyStr("g", 11U, kTypeSingleDeletion), ""},
      {KeyStr("j", 11U, kTypeSingleDeletion), ""},
      {KeyStr("k", 11U, kTypeSingleDeletion), ""},
      {KeyStr("m", 12U, kTypeValue), "val1"},
      {KeyStr("m", 11U, kTypeSingleDeletion), ""},
      {KeyStr("m", 8U, kTypeValue), "val2"},
  });

  SetLastSequence(22U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results}, {10U, 20U}, 10U);
}

TEST_F(CompactionJobTest, EarliestWriteConflictSnapshot) {
  NewDB();

  // Test multiple snapshots where the earliest snapshot is not a
  // write-conflic-snapshot.

  auto file1 = mock::MakeMockFile({
      {KeyStr("A", 24U, kTypeSingleDeletion), ""},
      {KeyStr("A", 23U, kTypeValue), "val"},
      {KeyStr("B", 24U, kTypeSingleDeletion), ""},
      {KeyStr("B", 23U, kTypeValue), "val"},
      {KeyStr("D", 24U, kTypeSingleDeletion), ""},
      {KeyStr("G", 32U, kTypeSingleDeletion), ""},
      {KeyStr("G", 31U, kTypeValue), "val"},
      {KeyStr("G", 24U, kTypeSingleDeletion), ""},
      {KeyStr("G", 23U, kTypeValue), "val2"},
      {KeyStr("H", 31U, kTypeValue), "val"},
      {KeyStr("H", 24U, kTypeSingleDeletion), ""},
      {KeyStr("H", 23U, kTypeValue), "val"},
      {KeyStr("I", 35U, kTypeSingleDeletion), ""},
      {KeyStr("I", 34U, kTypeValue), "val2"},
      {KeyStr("I", 33U, kTypeSingleDeletion), ""},
      {KeyStr("I", 32U, kTypeValue), "val3"},
      {KeyStr("I", 31U, kTypeSingleDeletion), ""},
      {KeyStr("J", 34U, kTypeValue), "val"},
      {KeyStr("J", 33U, kTypeSingleDeletion), ""},
      {KeyStr("J", 25U, kTypeValue), "val2"},
      {KeyStr("J", 24U, kTypeSingleDeletion), ""},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({
      {KeyStr("A", 14U, kTypeSingleDeletion), ""},
      {KeyStr("A", 13U, kTypeValue), "val2"},
      {KeyStr("C", 14U, kTypeSingleDeletion), ""},
      {KeyStr("C", 13U, kTypeValue), "val"},
      {KeyStr("E", 12U, kTypeSingleDeletion), ""},
      {KeyStr("F", 4U, kTypeSingleDeletion), ""},
      {KeyStr("F", 3U, kTypeValue), "val"},
      {KeyStr("G", 14U, kTypeSingleDeletion), ""},
      {KeyStr("G", 13U, kTypeValue), "val3"},
      {KeyStr("H", 14U, kTypeSingleDeletion), ""},
      {KeyStr("H", 13U, kTypeValue), "val2"},
      {KeyStr("I", 13U, kTypeValue), "val4"},
      {KeyStr("I", 12U, kTypeSingleDeletion), ""},
      {KeyStr("I", 11U, kTypeValue), "val5"},
      {KeyStr("J", 15U, kTypeValue), "val3"},
      {KeyStr("J", 14U, kTypeSingleDeletion), ""},
  });
  AddMockFile(file2);

  auto expected_results = mock::MakeMockFile({
      {KeyStr("A", 24U, kTypeSingleDeletion), ""},
      {KeyStr("A", 23U, kTypeValue), ""},
      {KeyStr("B", 24U, kTypeSingleDeletion), ""},
      {KeyStr("B", 23U, kTypeValue), ""},
      {KeyStr("D", 24U, kTypeSingleDeletion), ""},
      {KeyStr("E", 12U, kTypeSingleDeletion), ""},
      {KeyStr("G", 32U, kTypeSingleDeletion), ""},
      {KeyStr("G", 31U, kTypeValue), ""},
      {KeyStr("H", 31U, kTypeValue), "val"},
      {KeyStr("I", 35U, kTypeSingleDeletion), ""},
      {KeyStr("I", 34U, kTypeValue), ""},
      {KeyStr("I", 31U, kTypeSingleDeletion), ""},
      {KeyStr("I", 13U, kTypeValue), "val4"},
      {KeyStr("J", 34U, kTypeValue), "val"},
      {KeyStr("J", 33U, kTypeSingleDeletion), ""},
      {KeyStr("J", 25U, kTypeValue), "val2"},
      {KeyStr("J", 24U, kTypeSingleDeletion), ""},
      {KeyStr("J", 15U, kTypeValue), "val3"},
      {KeyStr("J", 14U, kTypeSingleDeletion), ""},
  });

  SetLastSequence(24U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results}, {10U, 20U, 30U},
                20U);
}

TEST_F(CompactionJobTest, SingleDeleteZeroSeq) {
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("A", 10U, kTypeSingleDeletion), ""},
      {KeyStr("dummy", 5U, kTypeValue), "val2"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({
      {KeyStr("A", 0U, kTypeValue), "val"},
  });
  AddMockFile(file2);

  auto expected_results = mock::MakeMockFile({
      {KeyStr("dummy", 0U, kTypeValue), "val2"},
  });

  SetLastSequence(22U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results}, {});
}

TEST_F(CompactionJobTest, MultiSingleDelete) {
  // Tests three scenarios involving multiple single delete/put pairs:
  //
  // A: Put Snapshot SDel Put SDel -> Put Snapshot SDel
  // B: Snapshot Put SDel Put SDel Snapshot -> Snapshot SDel Snapshot
  // C: SDel Put SDel Snapshot Put -> Snapshot Put
  // D: (Put) SDel Snapshot Put SDel -> (Put) SDel Snapshot SDel
  // E: Put SDel Snapshot Put SDel -> Snapshot SDel
  // F: Put SDel Put Sdel Snapshot -> removed
  // G: Snapshot SDel Put SDel Put -> Snapshot Put SDel
  // H: (Put) Put SDel Put Sdel Snapshot -> Removed
  // I: (Put) Snapshot Put SDel Put SDel -> SDel
  // J: Put Put SDel Put SDel SDel Snapshot Put Put SDel SDel Put
  //      -> Snapshot Put
  // K: SDel SDel Put SDel Put Put Snapshot SDel Put SDel SDel Put SDel
  //      -> Snapshot Put Snapshot SDel
  // L: SDel Put SDel Put SDel Snapshot SDel Put SDel SDel Put SDel
  //      -> Snapshot SDel Put SDel
  // M: (Put) SDel Put SDel Put SDel Snapshot Put SDel SDel Put SDel SDel
  //      -> SDel Snapshot Put SDel
  NewDB();

  auto file1 = mock::MakeMockFile({
      {KeyStr("A", 14U, kTypeSingleDeletion), ""},
      {KeyStr("A", 13U, kTypeValue), "val5"},
      {KeyStr("A", 12U, kTypeSingleDeletion), ""},
      {KeyStr("B", 14U, kTypeSingleDeletion), ""},
      {KeyStr("B", 13U, kTypeValue), "val2"},
      {KeyStr("C", 14U, kTypeValue), "val3"},
      {KeyStr("D", 12U, kTypeSingleDeletion), ""},
      {KeyStr("D", 11U, kTypeValue), "val4"},
      {KeyStr("G", 15U, kTypeValue), "val"},
      {KeyStr("G", 14U, kTypeSingleDeletion), ""},
      {KeyStr("G", 13U, kTypeValue), "val"},
      {KeyStr("I", 14U, kTypeSingleDeletion), ""},
      {KeyStr("I", 13U, kTypeValue), "val"},
      {KeyStr("J", 15U, kTypeValue), "val"},
      {KeyStr("J", 14U, kTypeSingleDeletion), ""},
      {KeyStr("J", 13U, kTypeSingleDeletion), ""},
      {KeyStr("J", 12U, kTypeValue), "val"},
      {KeyStr("J", 11U, kTypeValue), "val"},
      {KeyStr("K", 16U, kTypeSingleDeletion), ""},
      {KeyStr("K", 15U, kTypeValue), "val1"},
      {KeyStr("K", 14U, kTypeSingleDeletion), ""},
      {KeyStr("K", 13U, kTypeSingleDeletion), ""},
      {KeyStr("K", 12U, kTypeValue), "val2"},
      {KeyStr("K", 11U, kTypeSingleDeletion), ""},
      {KeyStr("L", 16U, kTypeSingleDeletion), ""},
      {KeyStr("L", 15U, kTypeValue), "val"},
      {KeyStr("L", 14U, kTypeSingleDeletion), ""},
      {KeyStr("L", 13U, kTypeSingleDeletion), ""},
      {KeyStr("L", 12U, kTypeValue), "val"},
      {KeyStr("L", 11U, kTypeSingleDeletion), ""},
      {KeyStr("M", 16U, kTypeSingleDeletion), ""},
      {KeyStr("M", 15U, kTypeSingleDeletion), ""},
      {KeyStr("M", 14U, kTypeValue), "val"},
      {KeyStr("M", 13U, kTypeSingleDeletion), ""},
      {KeyStr("M", 12U, kTypeSingleDeletion), ""},
      {KeyStr("M", 11U, kTypeValue), "val"},
  });
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile({
      {KeyStr("A", 10U, kTypeValue), "val"},
      {KeyStr("B", 12U, kTypeSingleDeletion), ""},
      {KeyStr("B", 11U, kTypeValue), "val2"},
      {KeyStr("C", 10U, kTypeSingleDeletion), ""},
      {KeyStr("C", 9U, kTypeValue), "val6"},
      {KeyStr("C", 8U, kTypeSingleDeletion), ""},
      {KeyStr("D", 10U, kTypeSingleDeletion), ""},
      {KeyStr("E", 12U, kTypeSingleDeletion), ""},
      {KeyStr("E", 11U, kTypeValue), "val"},
      {KeyStr("E", 5U, kTypeSingleDeletion), ""},
      {KeyStr("E", 4U, kTypeValue), "val"},
      {KeyStr("F", 6U, kTypeSingleDeletion), ""},
      {KeyStr("F", 5U, kTypeValue), "val"},
      {KeyStr("F", 4U, kTypeSingleDeletion), ""},
      {KeyStr("F", 3U, kTypeValue), "val"},
      {KeyStr("G", 12U, kTypeSingleDeletion), ""},
      {KeyStr("H", 6U, kTypeSingleDeletion), ""},
      {KeyStr("H", 5U, kTypeValue), "val"},
      {KeyStr("H", 4U, kTypeSingleDeletion), ""},
      {KeyStr("H", 3U, kTypeValue), "val"},
      {KeyStr("I", 12U, kTypeSingleDeletion), ""},
      {KeyStr("I", 11U, kTypeValue), "val"},
      {KeyStr("J", 6U, kTypeSingleDeletion), ""},
      {KeyStr("J", 5U, kTypeSingleDeletion), ""},
      {KeyStr("J", 4U, kTypeValue), "val"},
      {KeyStr("J", 3U, kTypeSingleDeletion), ""},
      {KeyStr("J", 2U, kTypeValue), "val"},
      {KeyStr("K", 8U, kTypeValue), "val3"},
      {KeyStr("K", 7U, kTypeValue), "val4"},
      {KeyStr("K", 6U, kTypeSingleDeletion), ""},
      {KeyStr("K", 5U, kTypeValue), "val5"},
      {KeyStr("K", 2U, kTypeSingleDeletion), ""},
      {KeyStr("K", 1U, kTypeSingleDeletion), ""},
      {KeyStr("L", 5U, kTypeSingleDeletion), ""},
      {KeyStr("L", 4U, kTypeValue), "val"},
      {KeyStr("L", 3U, kTypeSingleDeletion), ""},
      {KeyStr("L", 2U, kTypeValue), "val"},
      {KeyStr("L", 1U, kTypeSingleDeletion), ""},
      {KeyStr("M", 10U, kTypeSingleDeletion), ""},
      {KeyStr("M", 7U, kTypeValue), "val"},
      {KeyStr("M", 5U, kTypeSingleDeletion), ""},
      {KeyStr("M", 4U, kTypeValue), "val"},
      {KeyStr("M", 3U, kTypeSingleDeletion), ""},
  });
  AddMockFile(file2);

  auto file3 = mock::MakeMockFile({
      {KeyStr("D", 1U, kTypeValue), "val"},
      {KeyStr("H", 1U, kTypeValue), "val"},
      {KeyStr("I", 2U, kTypeValue), "val"},
  });
  AddMockFile(file3, 2);

  auto file4 = mock::MakeMockFile({
      {KeyStr("M", 1U, kTypeValue), "val"},
  });
  AddMockFile(file4, 2);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("A", 14U, kTypeSingleDeletion), ""},
                          {KeyStr("A", 13U, kTypeValue), ""},
                          {KeyStr("A", 12U, kTypeSingleDeletion), ""},
                          {KeyStr("A", 10U, kTypeValue), "val"},
                          {KeyStr("B", 14U, kTypeSingleDeletion), ""},
                          {KeyStr("B", 13U, kTypeValue), ""},
                          {KeyStr("C", 14U, kTypeValue), "val3"},
                          {KeyStr("D", 12U, kTypeSingleDeletion), ""},
                          {KeyStr("D", 11U, kTypeValue), ""},
                          {KeyStr("D", 10U, kTypeSingleDeletion), ""},
                          {KeyStr("E", 12U, kTypeSingleDeletion), ""},
                          {KeyStr("E", 11U, kTypeValue), ""},
                          {KeyStr("G", 15U, kTypeValue), "val"},
                          {KeyStr("G", 12U, kTypeSingleDeletion), ""},
                          {KeyStr("I", 14U, kTypeSingleDeletion), ""},
                          {KeyStr("I", 13U, kTypeValue), ""},
                          {KeyStr("J", 15U, kTypeValue), "val"},
                          {KeyStr("K", 16U, kTypeSingleDeletion), ""},
                          {KeyStr("K", 15U, kTypeValue), ""},
                          {KeyStr("K", 11U, kTypeSingleDeletion), ""},
                          {KeyStr("K", 8U, kTypeValue), "val3"},
                          {KeyStr("L", 16U, kTypeSingleDeletion), ""},
                          {KeyStr("L", 15U, kTypeValue), ""},
                          {KeyStr("L", 11U, kTypeSingleDeletion), ""},
                          {KeyStr("M", 15U, kTypeSingleDeletion), ""},
                          {KeyStr("M", 14U, kTypeValue), ""},
                          {KeyStr("M", 3U, kTypeSingleDeletion), ""}});

  SetLastSequence(22U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results}, {10U}, 10U);
}

// This test documents the behavior where a corrupt key follows a deletion or a
// single deletion and the (single) deletion gets removed while the corrupt key
// gets written out. TODO(noetzli): We probably want a better way to treat
// corrupt keys.
TEST_F(CompactionJobTest, DISABLED_CorruptionAfterDeletion) {
  NewDB();

  auto file1 =
      mock::MakeMockFile({{test::KeyStr("A", 6U, kTypeValue), "val3"},
                          {test::KeyStr("a", 5U, kTypeDeletion), ""},
                          {test::KeyStr("a", 4U, kTypeValue, true), "val"}});
  AddMockFile(file1);

  auto file2 =
      mock::MakeMockFile({{test::KeyStr("b", 3U, kTypeSingleDeletion), ""},
                          {test::KeyStr("b", 2U, kTypeValue, true), "val"},
                          {test::KeyStr("c", 1U, kTypeValue), "val2"}});
  AddMockFile(file2);

  auto expected_results =
      mock::MakeMockFile({{test::KeyStr("A", 0U, kTypeValue), "val3"},
                          {test::KeyStr("a", 0U, kTypeValue, true), "val"},
                          {test::KeyStr("b", 0U, kTypeValue, true), "val"},
                          {test::KeyStr("c", 0U, kTypeValue), "val2"}});

  SetLastSequence(6U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, OldestBlobFileNumber) {
  NewDB();

  // Note: blob1 is inlined TTL, so it will not be considered for the purposes
  // of identifying the oldest referenced blob file. Similarly, blob6 will be
  // ignored because it has TTL and hence refers to a TTL blob file.
  const stl_wrappers::KVMap::value_type blob1(
      KeyStr("a", 1U, kTypeBlobIndex), BlobStrInlinedTTL("foo", 1234567890ULL));
  const stl_wrappers::KVMap::value_type blob2(KeyStr("b", 2U, kTypeBlobIndex),
                                              BlobStr(59, 123456, 999));
  const stl_wrappers::KVMap::value_type blob3(KeyStr("c", 3U, kTypeBlobIndex),
                                              BlobStr(138, 1000, 1 << 8));
  auto file1 = mock::MakeMockFile({blob1, blob2, blob3});
  AddMockFile(file1);

  const stl_wrappers::KVMap::value_type blob4(KeyStr("d", 4U, kTypeBlobIndex),
                                              BlobStr(199, 3 << 10, 1 << 20));
  const stl_wrappers::KVMap::value_type blob5(KeyStr("e", 5U, kTypeBlobIndex),
                                              BlobStr(19, 6789, 333));
  const stl_wrappers::KVMap::value_type blob6(
      KeyStr("f", 6U, kTypeBlobIndex),
      BlobStrTTL(5, 2048, 1 << 7, 1234567890ULL));
  auto file2 = mock::MakeMockFile({blob4, blob5, blob6});
  AddMockFile(file2);

  const stl_wrappers::KVMap::value_type expected_blob1(
      KeyStr("a", 0U, kTypeBlobIndex), blob1.second);
  const stl_wrappers::KVMap::value_type expected_blob2(
      KeyStr("b", 0U, kTypeBlobIndex), blob2.second);
  const stl_wrappers::KVMap::value_type expected_blob3(
      KeyStr("c", 0U, kTypeBlobIndex), blob3.second);
  const stl_wrappers::KVMap::value_type expected_blob4(
      KeyStr("d", 0U, kTypeBlobIndex), blob4.second);
  const stl_wrappers::KVMap::value_type expected_blob5(
      KeyStr("e", 0U, kTypeBlobIndex), blob5.second);
  const stl_wrappers::KVMap::value_type expected_blob6(
      KeyStr("f", 0U, kTypeBlobIndex), blob6.second);
  auto expected_results =
      mock::MakeMockFile({expected_blob1, expected_blob2, expected_blob3,
                          expected_blob4, expected_blob5, expected_blob6});

  SetLastSequence(6U);
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results},
                std::vector<SequenceNumber>(), kMaxSequenceNumber,
                /* output_level */ 1, /* verify */ true,
                /* expected_oldest_blob_file_numbers */ {19});
}

TEST_F(CompactionJobTest, VerifyPenultimateLevelOutput) {
  cf_options_.bottommost_temperature = Temperature::kCold;
  SyncPoint::GetInstance()->SetCallBack(
      "Compaction::SupportsPerKeyPlacement:Enabled", [&](void* arg) {
        auto supports_per_key_placement = static_cast<bool*>(arg);
        *supports_per_key_placement = true;
      });

  std::atomic_uint64_t latest_cold_seq = 0;

  SyncPoint::GetInstance()->SetCallBack(
      "CompactionIterator::PrepareOutput.context", [&](void* arg) {
        auto context = static_cast<PerKeyPlacementContext*>(arg);
        context->output_to_penultimate_level =
            context->seq_num > latest_cold_seq;
      });
  SyncPoint::GetInstance()->EnableProcessing();

  NewDB();

  // Add files on different levels that may overlap
  auto file0_1 = mock::MakeMockFile({{KeyStr("z", 12U, kTypeValue), "val"}});
  AddMockFile(file0_1);

  auto file1_1 = mock::MakeMockFile({{KeyStr("b", 10U, kTypeValue), "val"},
                                     {KeyStr("f", 11U, kTypeValue), "val"}});
  AddMockFile(file1_1, 1);
  auto file1_2 = mock::MakeMockFile({{KeyStr("j", 12U, kTypeValue), "val"},
                                     {KeyStr("k", 13U, kTypeValue), "val"}});
  AddMockFile(file1_2, 1);
  auto file1_3 = mock::MakeMockFile({{KeyStr("p", 14U, kTypeValue), "val"},
                                     {KeyStr("u", 15U, kTypeValue), "val"}});
  AddMockFile(file1_3, 1);

  auto file2_1 = mock::MakeMockFile({{KeyStr("f", 8U, kTypeValue), "val"},
                                     {KeyStr("h", 9U, kTypeValue), "val"}});
  AddMockFile(file2_1, 2);
  auto file2_2 = mock::MakeMockFile({{KeyStr("m", 6U, kTypeValue), "val"},
                                     {KeyStr("p", 7U, kTypeValue), "val"}});
  AddMockFile(file2_2, 2);

  auto file3_1 = mock::MakeMockFile({{KeyStr("g", 2U, kTypeValue), "val"},
                                     {KeyStr("k", 3U, kTypeValue), "val"}});
  AddMockFile(file3_1, 3);
  auto file3_2 = mock::MakeMockFile({{KeyStr("v", 4U, kTypeValue), "val"},
                                     {KeyStr("x", 5U, kTypeValue), "val"}});
  AddMockFile(file3_2, 3);

  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  const std::vector<int> input_levels = {0, 1, 2, 3};
  auto files0 = cfd->current()->storage_info()->LevelFiles(input_levels[0]);
  auto files1 = cfd->current()->storage_info()->LevelFiles(input_levels[1]);
  auto files2 = cfd->current()->storage_info()->LevelFiles(input_levels[2]);
  auto files3 = cfd->current()->storage_info()->LevelFiles(input_levels[3]);

  RunLastLevelCompaction(
      {files0, files1, files2, files3}, input_levels,
      /*verify_func=*/[&](Compaction& comp) {
        for (char c = 'a'; c <= 'z'; c++) {
          std::string c_str;
          c_str = c;
          const Slice key(c_str);
          if (c == 'a') {
            ASSERT_FALSE(comp.WithinPenultimateLevelOutputRange(key));
          } else {
            ASSERT_TRUE(comp.WithinPenultimateLevelOutputRange(key));
          }
        }
      });
}

TEST_F(CompactionJobTest, NoEnforceSingleDeleteContract) {
  db_options_.enforce_single_del_contracts = false;
  NewDB();

  auto file =
      mock::MakeMockFile({{KeyStr("a", 4U, kTypeSingleDeletion), ""},
                          {KeyStr("a", 3U, kTypeDeletion), "dontcare"}});
  AddMockFile(file);
  SetLastSequence(4U);

  auto expected_results = mock::MakeMockFile();
  constexpr int input_level = 0;
  auto files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTest, InputSerialization) {
  // Setup a random CompactionServiceInput
  CompactionServiceInput input;
  const int kStrMaxLen = 1000;
  Random rnd(static_cast<uint32_t>(time(nullptr)));
  Random64 rnd64(time(nullptr));
  input.column_family.name = rnd.RandomString(rnd.Uniform(kStrMaxLen));
  input.column_family.options.comparator = ReverseBytewiseComparator();
  input.column_family.options.max_bytes_for_level_base =
      rnd64.Uniform(UINT64_MAX);
  input.column_family.options.disable_auto_compactions = rnd.OneIn(2);
  input.column_family.options.compression = kZSTD;
  input.column_family.options.compression_opts.level = 4;
  input.db_options.max_background_flushes = 10;
  input.db_options.paranoid_checks = rnd.OneIn(2);
  input.db_options.statistics = CreateDBStatistics();
  input.db_options.env = env_;
  while (!rnd.OneIn(10)) {
    input.snapshots.emplace_back(rnd64.Uniform(UINT64_MAX));
  }
  while (!rnd.OneIn(10)) {
    input.input_files.emplace_back(rnd.RandomString(
        rnd.Uniform(kStrMaxLen - 1) +
        1));  // input file name should have at least one character
  }
  input.output_level = 4;
  input.has_begin = rnd.OneIn(2);
  if (input.has_begin) {
    input.begin = rnd.RandomBinaryString(rnd.Uniform(kStrMaxLen));
  }
  input.has_end = rnd.OneIn(2);
  if (input.has_end) {
    input.end = rnd.RandomBinaryString(rnd.Uniform(kStrMaxLen));
  }

  std::string output;
  ASSERT_OK(input.Write(&output));

  // Test deserialization
  CompactionServiceInput deserialized1;
  ASSERT_OK(CompactionServiceInput::Read(output, &deserialized1));
  ASSERT_TRUE(deserialized1.TEST_Equals(&input));

  // Test mismatch
  deserialized1.db_options.max_background_flushes += 10;
  std::string mismatch;
  ASSERT_FALSE(deserialized1.TEST_Equals(&input, &mismatch));
  ASSERT_EQ(mismatch, "db_options.max_background_flushes");

  // Test unknown field
  CompactionServiceInput deserialized2;
  output.clear();
  ASSERT_OK(input.Write(&output));
  output.append("new_field=123;");

  ASSERT_OK(CompactionServiceInput::Read(output, &deserialized2));
  ASSERT_TRUE(deserialized2.TEST_Equals(&input));

  // Test missing field
  CompactionServiceInput deserialized3;
  deserialized3.output_level = 0;
  std::string to_remove = "output_level=4;";
  size_t pos = output.find(to_remove);
  ASSERT_TRUE(pos != std::string::npos);
  output.erase(pos, to_remove.length());
  ASSERT_OK(CompactionServiceInput::Read(output, &deserialized3));
  mismatch.clear();
  ASSERT_FALSE(deserialized3.TEST_Equals(&input, &mismatch));
  ASSERT_EQ(mismatch, "output_level");

  // manually set the value back, should match the original structure
  deserialized3.output_level = 4;
  ASSERT_TRUE(deserialized3.TEST_Equals(&input));

  // Test invalid version
  output.clear();
  ASSERT_OK(input.Write(&output));

  uint32_t data_version = DecodeFixed32(output.data());
  const size_t kDataVersionSize = sizeof(data_version);
  ASSERT_EQ(data_version,
            1U);  // Update once the default data version is changed
  char buf[kDataVersionSize];
  EncodeFixed32(buf, data_version + 10);  // make sure it's not valid
  output.replace(0, kDataVersionSize, buf, kDataVersionSize);
  Status s = CompactionServiceInput::Read(output, &deserialized3);
  ASSERT_TRUE(s.IsNotSupported());
}

TEST_F(CompactionJobTest, ResultSerialization) {
  // Setup a random CompactionServiceResult
  CompactionServiceResult result;
  const int kStrMaxLen = 1000;
  Random rnd(static_cast<uint32_t>(time(nullptr)));
  Random64 rnd64(time(nullptr));
  std::vector<Status> status_list = {
      Status::OK(),
      Status::InvalidArgument("invalid option"),
      Status::Aborted("failed to run"),
      Status::NotSupported("not supported option"),
  };
  result.status =
      status_list.at(rnd.Uniform(static_cast<int>(status_list.size())));
  while (!rnd.OneIn(10)) {
    UniqueId64x2 id{rnd64.Uniform(UINT64_MAX), rnd64.Uniform(UINT64_MAX)};
    result.output_files.emplace_back(
        rnd.RandomString(rnd.Uniform(kStrMaxLen)), rnd64.Uniform(UINT64_MAX),
        rnd64.Uniform(UINT64_MAX),
        rnd.RandomBinaryString(rnd.Uniform(kStrMaxLen)),
        rnd.RandomBinaryString(rnd.Uniform(kStrMaxLen)),
        rnd64.Uniform(UINT64_MAX), rnd64.Uniform(UINT64_MAX),
        rnd64.Uniform(UINT64_MAX), rnd.OneIn(2), id);
  }
  result.output_level = rnd.Uniform(10);
  result.output_path = rnd.RandomString(rnd.Uniform(kStrMaxLen));
  result.num_output_records = rnd64.Uniform(UINT64_MAX);
  result.total_bytes = rnd64.Uniform(UINT64_MAX);
  result.bytes_read = 123;
  result.bytes_written = rnd64.Uniform(UINT64_MAX);
  result.stats.elapsed_micros = rnd64.Uniform(UINT64_MAX);
  result.stats.num_output_files = rnd.Uniform(1000);
  result.stats.is_full_compaction = rnd.OneIn(2);
  result.stats.num_single_del_mismatch = rnd64.Uniform(UINT64_MAX);
  result.stats.num_input_files = 9;

  std::string output;
  ASSERT_OK(result.Write(&output));

  // Test deserialization
  CompactionServiceResult deserialized1;
  ASSERT_OK(CompactionServiceResult::Read(output, &deserialized1));
  ASSERT_TRUE(deserialized1.TEST_Equals(&result));

  // Test mismatch
  deserialized1.stats.num_input_files += 10;
  std::string mismatch;
  ASSERT_FALSE(deserialized1.TEST_Equals(&result, &mismatch));
  ASSERT_EQ(mismatch, "stats.num_input_files");

  // Test unique id mismatch
  if (!result.output_files.empty()) {
    CompactionServiceResult deserialized_tmp;
    ASSERT_OK(CompactionServiceResult::Read(output, &deserialized_tmp));
    deserialized_tmp.output_files[0].unique_id[0] += 1;
    ASSERT_FALSE(deserialized_tmp.TEST_Equals(&result, &mismatch));
    ASSERT_EQ(mismatch, "output_files.unique_id");
    deserialized_tmp.status.PermitUncheckedError();
  }

  // Test unknown field
  CompactionServiceResult deserialized2;
  output.clear();
  ASSERT_OK(result.Write(&output));
  output.append("new_field=123;");

  ASSERT_OK(CompactionServiceResult::Read(output, &deserialized2));
  ASSERT_TRUE(deserialized2.TEST_Equals(&result));

  // Test missing field
  CompactionServiceResult deserialized3;
  deserialized3.bytes_read = 0;
  std::string to_remove = "bytes_read=123;";
  size_t pos = output.find(to_remove);
  ASSERT_TRUE(pos != std::string::npos);
  output.erase(pos, to_remove.length());
  ASSERT_OK(CompactionServiceResult::Read(output, &deserialized3));
  mismatch.clear();
  ASSERT_FALSE(deserialized3.TEST_Equals(&result, &mismatch));
  ASSERT_EQ(mismatch, "bytes_read");

  deserialized3.bytes_read = 123;
  ASSERT_TRUE(deserialized3.TEST_Equals(&result));

  // Test invalid version
  output.clear();
  ASSERT_OK(result.Write(&output));

  uint32_t data_version = DecodeFixed32(output.data());
  const size_t kDataVersionSize = sizeof(data_version);
  ASSERT_EQ(data_version,
            1U);  // Update once the default data version is changed
  char buf[kDataVersionSize];
  EncodeFixed32(buf, data_version + 10);  // make sure it's not valid
  output.replace(0, kDataVersionSize, buf, kDataVersionSize);
  Status s = CompactionServiceResult::Read(output, &deserialized3);
  ASSERT_TRUE(s.IsNotSupported());
  for (const auto& item : status_list) {
    item.PermitUncheckedError();
  }
}

class CompactionJobTimestampTest : public CompactionJobTestBase {
 public:
  CompactionJobTimestampTest()
      : CompactionJobTestBase(test::PerThreadDBPath("compaction_job_ts_test"),
                              test::BytewiseComparatorWithU64TsWrapper(),
                              test::EncodeInt, /*test_io_priority=*/false,
                              TableTypeForTest::kMockTable) {}
};

TEST_F(CompactionJobTimestampTest, GCDisabled) {
  NewDB();

  auto file1 =
      mock::MakeMockFile({{KeyStr("a", 10, ValueType::kTypeValue, 100), "a10"},
                          {KeyStr("a", 9, ValueType::kTypeValue, 99), "a9"},
                          {KeyStr("b", 8, ValueType::kTypeValue, 98), "b8"},
                          {KeyStr("d", 7, ValueType::kTypeValue, 97), "d7"}});

  AddMockFile(file1);

  auto file2 = mock::MakeMockFile(
      {{KeyStr("b", 6, ValueType::kTypeDeletionWithTimestamp, 96), ""},
       {KeyStr("c", 5, ValueType::kTypeDeletionWithTimestamp, 95), ""},
       {KeyStr("c", 4, ValueType::kTypeValue, 94), "c5"},
       {KeyStr("d", 3, ValueType::kTypeSingleDeletion, 93), ""}});
  AddMockFile(file2);

  SetLastSequence(10);

  auto expected_results = mock::MakeMockFile(
      {{KeyStr("a", 10, ValueType::kTypeValue, 100), "a10"},
       {KeyStr("a", 9, ValueType::kTypeValue, 99), "a9"},
       {KeyStr("b", 8, ValueType::kTypeValue, 98), "b8"},
       {KeyStr("b", 6, ValueType::kTypeDeletionWithTimestamp, 96), ""},
       {KeyStr("c", 5, ValueType::kTypeDeletionWithTimestamp, 95), ""},
       {KeyStr("c", 4, ValueType::kTypeValue, 94), "c5"},
       {KeyStr("d", 7, ValueType::kTypeValue, 97), "d7"},
       {KeyStr("d", 3, ValueType::kTypeSingleDeletion, 93), ""}});
  constexpr int input_level = 0;
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTimestampTest, NoKeyExpired) {
  NewDB();

  auto file1 =
      mock::MakeMockFile({{KeyStr("a", 6, ValueType::kTypeValue, 100), "a6"},
                          {KeyStr("b", 7, ValueType::kTypeValue, 101), "b7"},
                          {KeyStr("c", 5, ValueType::kTypeValue, 99), "c5"}});
  AddMockFile(file1);

  auto file2 =
      mock::MakeMockFile({{KeyStr("a", 4, ValueType::kTypeValue, 98), "a4"},
                          {KeyStr("c", 3, ValueType::kTypeValue, 97), "c3"}});
  AddMockFile(file2);

  SetLastSequence(101);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 6, ValueType::kTypeValue, 100), "a6"},
                          {KeyStr("a", 4, ValueType::kTypeValue, 98), "a4"},
                          {KeyStr("b", 7, ValueType::kTypeValue, 101), "b7"},
                          {KeyStr("c", 5, ValueType::kTypeValue, 99), "c5"},
                          {KeyStr("c", 3, ValueType::kTypeValue, 97), "c3"}});
  constexpr int input_level = 0;
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);

  full_history_ts_low_ = encode_u64_ts_(0);
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTimestampTest, AllKeysExpired) {
  NewDB();

  auto file1 = mock::MakeMockFile(
      {{KeyStr("a", 5, ValueType::kTypeDeletionWithTimestamp, 100), ""},
       {KeyStr("b", 6, ValueType::kTypeSingleDeletion, 99), ""},
       {KeyStr("c", 7, ValueType::kTypeValue, 98), "c7"}});
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile(
      {{KeyStr("a", 4, ValueType::kTypeValue, 97), "a4"},
       {KeyStr("b", 3, ValueType::kTypeValue, 96), "b3"},
       {KeyStr("c", 2, ValueType::kTypeDeletionWithTimestamp, 95), ""},
       {KeyStr("c", 1, ValueType::kTypeValue, 94), "c1"}});
  AddMockFile(file2);

  SetLastSequence(7);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("c", 0, ValueType::kTypeValue, 0), "c7"}});
  constexpr int input_level = 0;
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);

  full_history_ts_low_ = encode_u64_ts_(std::numeric_limits<uint64_t>::max());
  RunCompaction({files}, {input_level}, {expected_results});
}

TEST_F(CompactionJobTimestampTest, SomeKeysExpired) {
  NewDB();

  auto file1 =
      mock::MakeMockFile({{KeyStr("a", 5, ValueType::kTypeValue, 50), "a5"},
                          {KeyStr("b", 6, ValueType::kTypeValue, 49), "b6"}});
  AddMockFile(file1);

  auto file2 = mock::MakeMockFile(
      {{KeyStr("a", 3, ValueType::kTypeValue, 48), "a3"},
       {KeyStr("a", 2, ValueType::kTypeValue, 46), "a2"},
       {KeyStr("b", 4, ValueType::kTypeDeletionWithTimestamp, 47), ""}});
  AddMockFile(file2);

  SetLastSequence(6);

  auto expected_results =
      mock::MakeMockFile({{KeyStr("a", 5, ValueType::kTypeValue, 50), "a5"},
                          {KeyStr("a", 0, ValueType::kTypeValue, 0), "a3"},
                          {KeyStr("b", 6, ValueType::kTypeValue, 49), "b6"}});
  constexpr int input_level = 0;
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);

  full_history_ts_low_ = encode_u64_ts_(49);
  RunCompaction({files}, {input_level}, {expected_results});
}

class CompactionJobTimestampTestWithBbTable : public CompactionJobTestBase {
 public:
  // Block-based table is needed if we want to test subcompaction partitioning
  // with anchors.
  explicit CompactionJobTimestampTestWithBbTable()
      : CompactionJobTestBase(
            test::PerThreadDBPath("compaction_job_ts_bbt_test"),
            test::BytewiseComparatorWithU64TsWrapper(), test::EncodeInt,
            /*test_io_priority=*/false, TableTypeForTest::kBlockBasedTable) {}
};

TEST_F(CompactionJobTimestampTestWithBbTable, SubcompactionAnchorL1) {
  cf_options_.target_file_size_base = 20;
  mutable_cf_options_.target_file_size_base = 20;
  NewDB();

  const std::vector<std::string> keys = {
      KeyStr("a", 20, ValueType::kTypeValue, 200),
      KeyStr("b", 21, ValueType::kTypeValue, 210),
      KeyStr("b", 20, ValueType::kTypeValue, 200),
      KeyStr("b", 18, ValueType::kTypeValue, 180),
      KeyStr("c", 17, ValueType::kTypeValue, 170),
      KeyStr("c", 16, ValueType::kTypeValue, 160),
      KeyStr("c", 15, ValueType::kTypeValue, 150)};
  const std::vector<std::string> values = {"a20", "b21", "b20", "b18",
                                           "c17", "c16", "c15"};

  constexpr int input_level = 1;

  auto file1 = mock::MakeMockFile(
      {{keys[0], values[0]}, {keys[1], values[1]}, {keys[2], values[2]}});
  AddMockFile(file1, input_level);

  auto file2 = mock::MakeMockFile(
      {{keys[3], values[3]}, {keys[4], values[4]}, {keys[5], values[5]}});
  AddMockFile(file2, input_level);

  auto file3 = mock::MakeMockFile({{keys[6], values[6]}});
  AddMockFile(file3, input_level);

  SetLastSequence(20);

  auto output1 = mock::MakeMockFile({{keys[0], values[0]}});
  auto output2 = mock::MakeMockFile(
      {{keys[1], values[1]}, {keys[2], values[2]}, {keys[3], values[3]}});
  auto output3 = mock::MakeMockFile(
      {{keys[4], values[4]}, {keys[5], values[5]}, {keys[6], values[6]}});

  auto expected_results =
      std::vector<mock::KVVector>{output1, output2, output3};
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);

  constexpr int output_level = 2;
  constexpr int max_subcompactions = 4;
  RunCompaction({files}, {input_level}, expected_results, /*snapshots=*/{},
                /*earliest_write_conflict_snapshot=*/kMaxSequenceNumber,
                output_level, /*verify=*/true, {kInvalidBlobFileNumber},
                /*check_get_priority=*/false, Env::IO_TOTAL, Env::IO_TOTAL,
                max_subcompactions);
}

TEST_F(CompactionJobTimestampTestWithBbTable, SubcompactionL0) {
  cf_options_.target_file_size_base = 20;
  mutable_cf_options_.target_file_size_base = 20;
  NewDB();

  const std::vector<std::string> keys = {
      KeyStr("a", 20, ValueType::kTypeValue, 200),
      KeyStr("b", 20, ValueType::kTypeValue, 200),
      KeyStr("b", 19, ValueType::kTypeValue, 190),
      KeyStr("b", 18, ValueType::kTypeValue, 180),
      KeyStr("c", 17, ValueType::kTypeValue, 170),
      KeyStr("c", 16, ValueType::kTypeValue, 160),
      KeyStr("c", 15, ValueType::kTypeValue, 150)};
  const std::vector<std::string> values = {"a20", "b20", "b19", "b18",
                                           "c17", "c16", "c15"};

  constexpr int input_level = 0;

  auto file1 = mock::MakeMockFile({{keys[5], values[5]}, {keys[6], values[6]}});
  AddMockFile(file1, input_level);

  auto file2 = mock::MakeMockFile({{keys[3], values[3]}, {keys[4], values[4]}});
  AddMockFile(file2, input_level);

  auto file3 = mock::MakeMockFile(
      {{keys[0], values[0]}, {keys[1], values[1]}, {keys[2], values[2]}});
  AddMockFile(file3, input_level);

  SetLastSequence(20);

  auto output1 = mock::MakeMockFile({{keys[0], values[0]}});
  auto output2 = mock::MakeMockFile(
      {{keys[1], values[1]}, {keys[2], values[2]}, {keys[3], values[3]}});
  auto output3 = mock::MakeMockFile(
      {{keys[4], values[4]}, {keys[5], values[5]}, {keys[6], values[6]}});

  auto expected_results =
      std::vector<mock::KVVector>{output1, output2, output3};
  const auto& files = cfd_->current()->storage_info()->LevelFiles(input_level);

  constexpr int output_level = 1;
  constexpr int max_subcompactions = 4;
  RunCompaction({files}, {input_level}, expected_results, /*snapshots=*/{},
                /*earliest_write_conflict_snapshot=*/kMaxSequenceNumber,
                output_level, /*verify=*/true, {kInvalidBlobFileNumber},
                /*check_get_priority=*/false, Env::IO_TOTAL, Env::IO_TOTAL,
                max_subcompactions);
}

// The io priority of the compaction reads and writes are different from
// other DB reads and writes. To prepare the compaction input files, use the
// default filesystem from Env. To test the io priority of the compaction
// reads and writes, db_options_.fs is set as MockTestFileSystem.
class CompactionJobIOPriorityTest : public CompactionJobTestBase {
 public:
  CompactionJobIOPriorityTest()
      : CompactionJobTestBase(
            test::PerThreadDBPath("compaction_job_io_priority_test"),
            BytewiseComparator(), [](uint64_t /*ts*/) { return ""; },
            /*test_io_priority=*/true, TableTypeForTest::kBlockBasedTable) {}
};

TEST_F(CompactionJobIOPriorityTest, WriteControllerStateNormal) {
  // When the state from WriteController is normal.
  NewDB();
  mock::KVVector expected_results = CreateTwoFiles(false);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  ASSERT_EQ(2U, files.size());
  RunCompaction({files}, {input_level}, {expected_results}, {},
                kMaxSequenceNumber, 1, false, {kInvalidBlobFileNumber}, false,
                Env::IO_LOW, Env::IO_LOW);
}

TEST_F(CompactionJobIOPriorityTest, WriteControllerStateDelayed) {
  // When the state from WriteController is Delayed.
  NewDB();
  mock::KVVector expected_results = CreateTwoFiles(false);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  ASSERT_EQ(2U, files.size());
  {
    std::unique_ptr<WriteControllerToken> delay_token =
        write_controller_.GetDelayToken(1000000);
    RunCompaction({files}, {input_level}, {expected_results}, {},
                  kMaxSequenceNumber, 1, false, {kInvalidBlobFileNumber}, false,
                  Env::IO_USER, Env::IO_USER);
  }
}

TEST_F(CompactionJobIOPriorityTest, WriteControllerStateStalled) {
  // When the state from WriteController is Stalled.
  NewDB();
  mock::KVVector expected_results = CreateTwoFiles(false);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  ASSERT_EQ(2U, files.size());
  {
    std::unique_ptr<WriteControllerToken> stop_token =
        write_controller_.GetStopToken();
    RunCompaction({files}, {input_level}, {expected_results}, {},
                  kMaxSequenceNumber, 1, false, {kInvalidBlobFileNumber}, false,
                  Env::IO_USER, Env::IO_USER);
  }
}

TEST_F(CompactionJobIOPriorityTest, GetRateLimiterPriority) {
  NewDB();
  mock::KVVector expected_results = CreateTwoFiles(false);
  auto cfd = versions_->GetColumnFamilySet()->GetDefault();
  constexpr int input_level = 0;
  auto files = cfd->current()->storage_info()->LevelFiles(input_level);
  ASSERT_EQ(2U, files.size());
  RunCompaction({files}, {input_level}, {expected_results}, {},
                kMaxSequenceNumber, 1, false, {kInvalidBlobFileNumber}, true,
                Env::IO_LOW, Env::IO_LOW);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  RegisterCustomObjects(argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED as CompactionJobStats is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
