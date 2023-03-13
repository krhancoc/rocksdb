//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/db_impl/db_impl_secondary.h"

#include <cinttypes>

#include "db/arena_wrapped_db_iter.h"
#include "db/merge_context.h"
#include "logging/auto_roll_logger.h"
#include "monitoring/perf_context_imp.h"
#include "util/cast_util.h"

namespace ROCKSDB_NAMESPACE {

#ifndef ROCKSDB_LITE
DBImplSecondary::DBImplSecondary(const DBOptions& db_options,
                                 const std::string& dbname)
    : DBImpl(db_options, dbname) {
  ROCKS_LOG_INFO(immutable_db_options_.info_log,
                 "Opening the db in secondary mode");
  LogFlush(immutable_db_options_.info_log);
}

DBImplSecondary::~DBImplSecondary() {}

Status DBImplSecondary::Recover(
    const std::vector<ColumnFamilyDescriptor>& column_families,
    bool /*readonly*/, bool /*error_if_log_file_exist*/,
    bool /*error_if_data_exists_in_logs*/, uint64_t*) {
  mutex_.AssertHeld();

  JobContext job_context(0);
  Status s;
  s = static_cast<ReactiveVersionSet*>(versions_.get())
          ->Recover(column_families, &manifest_reader_, &manifest_reporter_,
                    &manifest_reader_status_);
  if (!s.ok()) {
    return s;
  }
  if (immutable_db_options_.paranoid_checks && s.ok()) {
    s = CheckConsistency();
  }
  // Initial max_total_in_memory_state_ before recovery logs.
  max_total_in_memory_state_ = 0;
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    auto* mutable_cf_options = cfd->GetLatestMutableCFOptions();
    max_total_in_memory_state_ += mutable_cf_options->write_buffer_size *
                                  mutable_cf_options->max_write_buffer_number;
  }
  if (s.ok()) {
    default_cf_handle_ = new ColumnFamilyHandleImpl(
        versions_->GetColumnFamilySet()->GetDefault(), this, &mutex_);
    default_cf_internal_stats_ = default_cf_handle_->cfd()->internal_stats();
    single_column_family_mode_ =
        versions_->GetColumnFamilySet()->NumberOfColumnFamilies() == 1;

    std::unordered_set<ColumnFamilyData*> cfds_changed;
    s = FindAndRecoverLogFiles(&cfds_changed, &job_context);
  }

  if (s.IsPathNotFound()) {
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "Secondary tries to read WAL, but WAL file(s) have already "
                   "been purged by primary.");
    s = Status::OK();
  }
  // TODO: update options_file_number_ needed?

  job_context.Clean();
  return s;
}

// find new WAL and apply them in order to the secondary instance
Status DBImplSecondary::FindAndRecoverLogFiles(
    std::unordered_set<ColumnFamilyData*>* cfds_changed,
    JobContext* job_context) {
  assert(nullptr != cfds_changed);
  assert(nullptr != job_context);
  Status s;
  std::vector<uint64_t> logs;
  s = FindNewLogNumbers(&logs);
  if (s.ok() && !logs.empty()) {
    SequenceNumber next_sequence(kMaxSequenceNumber);
    s = RecoverLogFiles(logs, &next_sequence, cfds_changed, job_context);
  }
  return s;
}

// List all new WALs, return these log numbers
Status DBImplSecondary::FindNewLogNumbers(std::vector<uint64_t>* logs) {
  assert(logs != nullptr);
  return Status::OK();
}

// After manifest recovery, replay WALs and refresh log_readers_ if necessary
// REQUIRES: log_numbers are sorted in ascending order
Status DBImplSecondary::RecoverLogFiles(
    const std::vector<uint64_t>& log_numbers, SequenceNumber* next_sequence,
    std::unordered_set<ColumnFamilyData*>* cfds_changed,
    JobContext* job_context) {
  return Status::OK();
}

// Implementation of the DB interface
Status DBImplSecondary::Get(const ReadOptions& read_options,
                            ColumnFamilyHandle* column_family, const Slice& key,
                            PinnableSlice* value) {
  return GetImpl(read_options, column_family, key, value);
}

Status DBImplSecondary::GetImpl(const ReadOptions& read_options,
                                ColumnFamilyHandle* column_family,
                                const Slice& key, PinnableSlice* pinnable_val) {
  assert(pinnable_val != nullptr);
  PERF_CPU_TIMER_GUARD(get_cpu_nanos, env_);
  StopWatch sw(env_, stats_, DB_GET);
  PERF_TIMER_GUARD(get_snapshot_time);

  auto cfh = static_cast<ColumnFamilyHandleImpl*>(column_family);
  ColumnFamilyData* cfd = cfh->cfd();
  if (tracer_) {
    InstrumentedMutexLock lock(&trace_mutex_);
    if (tracer_) {
      tracer_->Get(column_family, key);
    }
  }
  // Acquire SuperVersion
  SuperVersion* super_version = GetAndRefSuperVersion(cfd);
  SequenceNumber snapshot = versions_->LastSequence();
  MergeContext merge_context;
  SequenceNumber max_covering_tombstone_seq = 0;
  Status s;
  LookupKey lkey(key, snapshot);
  PERF_TIMER_STOP(get_snapshot_time);

  bool done = false;
  if (super_version->mem->Get(lkey, pinnable_val->GetSelf(),
                              /*timestamp=*/nullptr, &s, &merge_context,
                              &max_covering_tombstone_seq, read_options)) {
    done = true;
    pinnable_val->PinSelf();
    RecordTick(stats_, MEMTABLE_HIT);
  } else if ((s.ok() || s.IsMergeInProgress()) &&
             super_version->imm->Get(
                 lkey, pinnable_val->GetSelf(), /*timestamp=*/nullptr, &s,
                 &merge_context, &max_covering_tombstone_seq, read_options)) {
    done = true;
    pinnable_val->PinSelf();
    RecordTick(stats_, MEMTABLE_HIT);
  }
  if (!done && !s.ok() && !s.IsMergeInProgress()) {
    ReturnAndCleanupSuperVersion(cfd, super_version);
    return s;
  }
  if (!done) {
    PERF_TIMER_GUARD(get_from_output_files_time);
    super_version->current->Get(read_options, lkey, pinnable_val,
                                /*timestamp=*/nullptr, &s, &merge_context,
                                &max_covering_tombstone_seq);
    RecordTick(stats_, MEMTABLE_MISS);
  }
  {
    PERF_TIMER_GUARD(get_post_process_time);
    ReturnAndCleanupSuperVersion(cfd, super_version);
    RecordTick(stats_, NUMBER_KEYS_READ);
    size_t size = pinnable_val->size();
    RecordTick(stats_, BYTES_READ, size);
    RecordTimeToHistogram(stats_, BYTES_PER_READ, size);
    PERF_COUNTER_ADD(get_read_bytes, size);
  }
  return s;
}

Iterator* DBImplSecondary::NewIterator(const ReadOptions& read_options,
                                       ColumnFamilyHandle* column_family) {
  if (read_options.managed) {
    return NewErrorIterator(
        Status::NotSupported("Managed iterator is not supported anymore."));
  }
  if (read_options.read_tier == kPersistedTier) {
    return NewErrorIterator(Status::NotSupported(
        "ReadTier::kPersistedData is not yet supported in iterators."));
  }
  Iterator* result = nullptr;
  auto cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
  auto cfd = cfh->cfd();
  ReadCallback* read_callback = nullptr;  // No read callback provided.
  if (read_options.tailing) {
    return NewErrorIterator(Status::NotSupported(
        "tailing iterator not supported in secondary mode"));
  } else if (read_options.snapshot != nullptr) {
    // TODO (yanqin) support snapshot.
    return NewErrorIterator(
        Status::NotSupported("snapshot not supported in secondary mode"));
  } else {
    auto snapshot = versions_->LastSequence();
    result = NewIteratorImpl(read_options, cfd, snapshot, read_callback);
  }
  return result;
}

ArenaWrappedDBIter* DBImplSecondary::NewIteratorImpl(
    const ReadOptions& read_options, ColumnFamilyData* cfd,
    SequenceNumber snapshot, ReadCallback* read_callback) {
  assert(nullptr != cfd);
  SuperVersion* super_version = cfd->GetReferencedSuperVersion(this);
  auto db_iter = NewArenaWrappedDbIterator(
      env_, read_options, *cfd->ioptions(), super_version->mutable_cf_options,
      snapshot,
      super_version->mutable_cf_options.max_sequential_skip_in_iterations,
      super_version->version_number, read_callback);
  auto internal_iter =
      NewInternalIterator(read_options, cfd, super_version, db_iter->GetArena(),
                          db_iter->GetRangeDelAggregator(), snapshot);
  db_iter->SetIterUnderDBIter(internal_iter);
  return db_iter;
}

Status DBImplSecondary::NewIterators(
    const ReadOptions& read_options,
    const std::vector<ColumnFamilyHandle*>& column_families,
    std::vector<Iterator*>* iterators) {
  if (read_options.managed) {
    return Status::NotSupported("Managed iterator is not supported anymore.");
  }
  if (read_options.read_tier == kPersistedTier) {
    return Status::NotSupported(
        "ReadTier::kPersistedData is not yet supported in iterators.");
  }
  ReadCallback* read_callback = nullptr;  // No read callback provided.
  if (iterators == nullptr) {
    return Status::InvalidArgument("iterators not allowed to be nullptr");
  }
  iterators->clear();
  iterators->reserve(column_families.size());
  if (read_options.tailing) {
    return Status::NotSupported(
        "tailing iterator not supported in secondary mode");
  } else if (read_options.snapshot != nullptr) {
    // TODO (yanqin) support snapshot.
    return Status::NotSupported("snapshot not supported in secondary mode");
  } else {
    SequenceNumber read_seq = versions_->LastSequence();
    for (auto cfh : column_families) {
      ColumnFamilyData* cfd = static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd();
      iterators->push_back(
          NewIteratorImpl(read_options, cfd, read_seq, read_callback));
    }
  }
  return Status::OK();
}

Status DBImplSecondary::CheckConsistency() {
  mutex_.AssertHeld();
  Status s = DBImpl::CheckConsistency();
  // If DBImpl::CheckConsistency() which is stricter returns success, then we
  // do not need to give a second chance.
  if (s.ok()) {
    return s;
  }
  // It's possible that DBImpl::CheckConssitency() can fail because the primary
  // may have removed certain files, causing the GetFileSize(name) call to
  // fail and returning a PathNotFound. In this case, we take a best-effort
  // approach and just proceed.
  TEST_SYNC_POINT_CALLBACK(
      "DBImplSecondary::CheckConsistency:AfterFirstAttempt", &s);

  if (immutable_db_options_.skip_checking_sst_file_sizes_on_db_open) {
    return Status::OK();
  }

  std::vector<LiveFileMetaData> metadata;
  versions_->GetLiveFilesMetaData(&metadata);

  std::string corruption_messages;
  for (const auto& md : metadata) {
    // md.name has a leading "/".
    std::string file_path = md.db_path + md.name;

    uint64_t fsize = 0;
    s = env_->GetFileSize(file_path, &fsize);
    if (!s.ok() &&
        (env_->GetFileSize(Rocks2LevelTableFileName(file_path), &fsize).ok() ||
         s.IsPathNotFound())) {
      s = Status::OK();
    }
    if (!s.ok()) {
      corruption_messages +=
          "Can't access " + md.name + ": " + s.ToString() + "\n";
    }
  }
  return corruption_messages.empty() ? Status::OK()
                                     : Status::Corruption(corruption_messages);
}

Status DBImplSecondary::TryCatchUpWithPrimary() {
  assert(versions_.get() != nullptr);
  assert(manifest_reader_.get() != nullptr);
  Status s;
  // read the manifest and apply new changes to the secondary instance
  std::unordered_set<ColumnFamilyData*> cfds_changed;
  JobContext job_context(0, true /*create_superversion*/);
  {
    InstrumentedMutexLock lock_guard(&mutex_);
    s = static_cast_with_check<ReactiveVersionSet>(versions_.get())
            ->ReadAndApply(&mutex_, &manifest_reader_, &cfds_changed);

    ROCKS_LOG_INFO(immutable_db_options_.info_log, "Last sequence is %" PRIu64,
                   static_cast<uint64_t>(versions_->LastSequence()));
    for (ColumnFamilyData* cfd : cfds_changed) {
      if (cfd->IsDropped()) {
        ROCKS_LOG_DEBUG(immutable_db_options_.info_log, "[%s] is dropped\n",
                        cfd->GetName().c_str());
        continue;
      }
      VersionStorageInfo::LevelSummaryStorage tmp;
      ROCKS_LOG_DEBUG(immutable_db_options_.info_log,
                      "[%s] Level summary: %s\n", cfd->GetName().c_str(),
                      cfd->current()->storage_info()->LevelSummary(&tmp));
    }

    // discover new WALs and apply new changes to the secondary
    // instance
    if (s.ok()) {
      s = FindAndRecoverLogFiles(&cfds_changed, &job_context);
    }
    if (s.IsPathNotFound()) {
      ROCKS_LOG_INFO(
          immutable_db_options_.info_log,
          "Secondary tries to read WAL, but WAL file(s) have already "
          "been purged by primary.");
      s = Status::OK();
    }
    if (s.ok()) {
      for (auto cfd : cfds_changed) {
        cfd->imm()->RemoveOldMemTables(cfd->GetLogNumber(),
                                       &job_context.memtables_to_free);
        auto& sv_context = job_context.superversion_contexts.back();
        cfd->InstallSuperVersion(&sv_context, &mutex_);
        sv_context.NewSuperVersion();
      }
    }
  }
  job_context.Clean();

  // Cleanup unused, obsolete files.
  JobContext purge_files_job_context(0);
  {
    InstrumentedMutexLock lock_guard(&mutex_);
    // Currently, secondary instance does not own the database files, thus it
    // is unnecessary for the secondary to force full scan.
    FindObsoleteFiles(&purge_files_job_context, /*force=*/false);
  }
  if (purge_files_job_context.HaveSomethingToDelete()) {
    PurgeObsoleteFiles(purge_files_job_context);
  }
  purge_files_job_context.Clean();
  return s;
}

Status DB::OpenAsSecondary(const Options& options, const std::string& dbname,
                           const std::string& secondary_path, DB** dbptr) {
  *dbptr = nullptr;

  DBOptions db_options(options);
  ColumnFamilyOptions cf_options(options);
  std::vector<ColumnFamilyDescriptor> column_families;
  column_families.emplace_back(kDefaultColumnFamilyName, cf_options);
  std::vector<ColumnFamilyHandle*> handles;

  Status s = DB::OpenAsSecondary(db_options, dbname, secondary_path,
                                 column_families, &handles, dbptr);
  if (s.ok()) {
    assert(handles.size() == 1);
    delete handles[0];
  }
  return s;
}

Status DB::OpenAsSecondary(
    const DBOptions& db_options, const std::string& dbname,
    const std::string& secondary_path,
    const std::vector<ColumnFamilyDescriptor>& column_families,
    std::vector<ColumnFamilyHandle*>* handles, DB** dbptr) {
  *dbptr = nullptr;
  if (db_options.max_open_files != -1) {
    // TODO (yanqin) maybe support max_open_files != -1 by creating hard links
    // on SST files so that db secondary can still have access to old SSTs
    // while primary instance may delete original.
    return Status::InvalidArgument("require max_open_files to be -1");
  }

  DBOptions tmp_opts(db_options);
  Status s;
  if (nullptr == tmp_opts.info_log) {
    s = CreateLoggerFromOptions(secondary_path, tmp_opts, &tmp_opts.info_log);
    if (!s.ok()) {
      tmp_opts.info_log = nullptr;
    }
  }

  handles->clear();
  DBImplSecondary* impl = new DBImplSecondary(tmp_opts, dbname);
  impl->versions_.reset(new ReactiveVersionSet(
      dbname, &impl->immutable_db_options_, impl->file_options_,
      impl->table_cache_.get(), impl->write_buffer_manager_,
      &impl->write_controller_));
  impl->column_family_memtables_.reset(
      new ColumnFamilyMemTablesImpl(impl->versions_->GetColumnFamilySet()));

  impl->mutex_.Lock();
  s = impl->Recover(column_families, true, false, false);
  if (s.ok()) {
    for (auto cf : column_families) {
      auto cfd =
          impl->versions_->GetColumnFamilySet()->GetColumnFamily(cf.name);
      if (nullptr == cfd) {
        s = Status::InvalidArgument("Column family not found: ", cf.name);
        break;
      }
      handles->push_back(new ColumnFamilyHandleImpl(cfd, impl, &impl->mutex_));
    }
  }
  SuperVersionContext sv_context(true /* create_superversion */);
  if (s.ok()) {
    for (auto cfd : *impl->versions_->GetColumnFamilySet()) {
      sv_context.NewSuperVersion();
      cfd->InstallSuperVersion(&sv_context, &impl->mutex_);
    }
  }
  impl->mutex_.Unlock();
  sv_context.Clean();
  if (s.ok()) {
    *dbptr = impl;
    for (auto h : *handles) {
      impl->NewThreadStatusCfInfo(
          reinterpret_cast<ColumnFamilyHandleImpl*>(h)->cfd());
    }
  } else {
    for (auto h : *handles) {
      delete h;
    }
    handles->clear();
    delete impl;
  }
  return s;
}
#else   // !ROCKSDB_LITE

Status DB::OpenAsSecondary(const Options& /*options*/,
                           const std::string& /*name*/,
                           const std::string& /*secondary_path*/,
                           DB** /*dbptr*/) {
  return Status::NotSupported("Not supported in ROCKSDB_LITE.");
}

Status DB::OpenAsSecondary(
    const DBOptions& /*db_options*/, const std::string& /*dbname*/,
    const std::string& /*secondary_path*/,
    const std::vector<ColumnFamilyDescriptor>& /*column_families*/,
    std::vector<ColumnFamilyHandle*>* /*handles*/, DB** /*dbptr*/) {
  return Status::NotSupported("Not supported in ROCKSDB_LITE.");
}
#endif  // !ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE
