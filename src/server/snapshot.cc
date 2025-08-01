// Copyright 2024, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/snapshot.h"

#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>

#include "base/cycle_clock.h"
#include "base/flags.h"
#include "base/logging.h"
#include "core/heap_size.h"
#include "server/db_slice.h"
#include "server/engine_shard_set.h"
#include "server/journal/journal.h"
#include "server/rdb_extensions.h"
#include "server/rdb_save.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "util/fibers/synchronization.h"

ABSL_FLAG(bool, point_in_time_snapshot, true, "If true replication uses point in time snapshoting");

namespace dfly {

using namespace std;
using namespace util;
using namespace chrono_literals;

using facade::operator""_KB;

namespace {
thread_local absl::flat_hash_set<SliceSnapshot*> tl_slice_snapshots;

// Controls the chunks size for pushing serialized data. The larger the chunk the more CPU
// it may require (especially with compression), and less responsive the server may be.
constexpr size_t kMinBlobSize = 8_KB;

}  // namespace

SliceSnapshot::SliceSnapshot(CompressionMode compression_mode, DbSlice* slice,
                             SnapshotDataConsumerInterface* consumer, ExecutionState* cntx)
    : db_slice_(slice),
      db_array_(slice->databases()),
      compression_mode_(compression_mode),
      consumer_(consumer),
      cntx_(cntx) {
  tl_slice_snapshots.insert(this);
}

SliceSnapshot::~SliceSnapshot() {
  DCHECK(db_slice_->shard_owner()->IsMyThread());
  tl_slice_snapshots.erase(this);
}

size_t SliceSnapshot::GetThreadLocalMemoryUsage() {
  size_t mem = 0;
  for (SliceSnapshot* snapshot : tl_slice_snapshots) {
    mem += snapshot->GetBufferCapacity();
  }
  return mem;
}

bool SliceSnapshot::IsSnaphotInProgress() {
  return !tl_slice_snapshots.empty();
}

void SliceSnapshot::Start(bool stream_journal, SnapshotFlush allow_flush) {
  DCHECK(!snapshot_fb_.IsJoinable());

  auto db_cb = [this](DbIndex db_index, const DbSlice::ChangeReq& req) {
    OnDbChange(db_index, req);
  };

  snapshot_version_ = db_slice_->RegisterOnChange(std::move(db_cb));

  if (stream_journal) {
    use_snapshot_version_ = absl::GetFlag(FLAGS_point_in_time_snapshot);
    auto* journal = db_slice_->shard_owner()->journal();
    DCHECK(journal);
    journal_cb_id_ = journal->RegisterOnChange(this);
    if (!use_snapshot_version_) {
      auto moved_cb = [this](DbIndex db_index, const DbSlice::MovedItemsVec& items) {
        OnMoved(db_index, items);
      };
      moved_cb_id_ = db_slice_->RegisterOnMove(std::move(moved_cb));
    }
  }

  const auto flush_threshold = ServerState::tlocal()->serialization_max_chunk_size;
  std::function<void(size_t, RdbSerializer::FlushState)> flush_fun;
  if (flush_threshold != 0 && allow_flush == SnapshotFlush::kAllow) {
    flush_fun = [this, flush_threshold](size_t bytes_serialized,
                                        RdbSerializer::FlushState flush_state) {
      if (bytes_serialized > flush_threshold) {
        size_t serialized = FlushSerialized(flush_state);
        VLOG(2) << "FlushSerialized " << serialized << " bytes";
        auto& stats = ServerState::tlocal()->stats;
        ++stats.big_value_preemptions;
      }
    };
  }
  serializer_ = std::make_unique<RdbSerializer>(compression_mode_, flush_fun);

  VLOG(1) << "DbSaver::Start - saving entries with version less than " << snapshot_version_;

  string fb_name = absl::StrCat("SliceSnapshot-", ProactorBase::me()->GetPoolIndex());
  snapshot_fb_ = fb2::Fiber(fb_name, [this, stream_journal] {
    this->IterateBucketsFb(stream_journal);
    db_slice_->UnregisterOnChange(snapshot_version_);
    if (!use_snapshot_version_) {
      db_slice_->UnregisterOnMoved(moved_cb_id_);
    }
    consumer_->Finalize();
    VLOG(1) << "Serialization peak bytes: " << serializer_->GetSerializationPeakBytes();
  });
}

void SliceSnapshot::StartIncremental(LSN start_lsn) {
  VLOG(1) << "StartIncremental: " << start_lsn;
  serializer_ = std::make_unique<RdbSerializer>(compression_mode_);

  snapshot_fb_ = fb2::Fiber("incremental_snapshot",
                            [start_lsn, this] { this->SwitchIncrementalFb(start_lsn); });
}

// Called only for replication use-case.
void SliceSnapshot::FinalizeJournalStream(bool cancel) {
  VLOG(1) << "FinalizeJournalStream";
  DCHECK(db_slice_->shard_owner()->IsMyThread());
  if (!journal_cb_id_) {  // Finalize only once.
    // In case of incremental snapshotting in StartIncremental, if an error is encountered,
    // journal_cb_id_ may not be set, but the snapshot fiber is still running.
    snapshot_fb_.JoinIfNeeded();
    return;
  }
  uint32_t cb_id = journal_cb_id_;
  journal_cb_id_ = 0;

  // Wait for serialization to finish in any case.
  snapshot_fb_.JoinIfNeeded();

  auto* journal = db_slice_->shard_owner()->journal();

  journal->UnregisterOnChange(cb_id);
  if (!cancel) {
    // always succeeds because serializer_ flushes to string.
    VLOG(1) << "FinalizeJournalStream lsn: " << journal->GetLsn();
    std::ignore = serializer_->SendJournalOffset(journal->GetLsn());
    PushSerialized(true);
  }
}

// The algorithm is to go over all the buckets and serialize those with
// version < snapshot_version_. In order to serialize each physical bucket exactly once we update
// bucket version to snapshot_version_ once it has been serialized.
// We handle serialization at physical bucket granularity.
// To further complicate things, Table::Traverse covers a logical bucket that may comprise of
// several physical buckets in dash table. For example, items belonging to logical bucket 0
// can reside in buckets 0,1 and stash buckets 56-59.
// PrimeTable::Traverse guarantees an atomic traversal of a single logical bucket,
// it also guarantees 100% coverage of all items that exists when the traversal started
// and survived until it finished.

// Serializes all the entries with version less than snapshot_version_.
void SliceSnapshot::IterateBucketsFb(bool send_full_sync_cut) {
  for (DbIndex db_indx = 0; db_indx < db_array_.size(); ++db_indx) {
    stats_.keys_total += db_slice_->DbSize(db_indx);
  }

  const uint64_t kCyclesPerJiffy = base::CycleClock::Frequency() >> 16;  // ~15usec.

  for (DbIndex snapshot_db_index_ = 0; snapshot_db_index_ < db_array_.size();
       ++snapshot_db_index_) {
    if (!cntx_->IsRunning())
      return;

    if (!db_array_[snapshot_db_index_])
      continue;

    PrimeTable* pt = &db_array_[snapshot_db_index_]->prime;
    VLOG(1) << "Start traversing " << pt->size() << " items for index " << snapshot_db_index_;

    do {
      if (!cntx_->IsRunning()) {
        return;
      }

      PrimeTable::Cursor next = pt->TraverseBuckets(
          snapshot_cursor_,
          [this, &snapshot_db_index_](auto it) { return BucketSaveCb(snapshot_db_index_, it); });
      snapshot_cursor_ = next;

      // If we do not flush the data, and have not preempted,
      // we may need to yield to other fibers to avoid grabbing CPU for too long.
      if (!PushSerialized(false)) {
        if (ThisFiber::GetRunningTimeCycles() > kCyclesPerJiffy) {
          ThisFiber::Yield();
        }
      }
    } while (snapshot_cursor_);

    DVLOG(2) << "after loop " << ThisFiber::GetName();
    PushSerialized(true);
  }  // for (dbindex)

  CHECK(!serialize_bucket_running_);
  if (send_full_sync_cut) {
    CHECK(!serializer_->SendFullSyncCut());
    PushSerialized(true);
  }

  // serialized + side_saved must be equal to the total saved.
  VLOG(1) << "Exit SnapshotSerializer loop_serialized: " << stats_.loop_serialized
          << ", side_saved " << stats_.side_saved << ", cbcalls " << stats_.savecb_calls
          << ", journal_saved " << stats_.jounal_changes << ", moved_saved " << stats_.moved_saved;
}

void SliceSnapshot::SwitchIncrementalFb(LSN lsn) {
  auto* journal = db_slice_->shard_owner()->journal();
  DCHECK(journal);
  DCHECK_LE(lsn, journal->GetLsn()) << "The replica tried to sync from the future.";

  VLOG(1) << "Starting incremental snapshot from lsn=" << lsn;

  // The replica sends the LSN of the next entry is wants to receive.
  while (cntx_->IsRunning() && journal->IsLSNInBuffer(lsn)) {
    std::ignore = serializer_->WriteJournalEntry(journal->GetEntry(lsn));
    PushSerialized(false);
    lsn++;
  }

  VLOG(1) << "Last LSN sent in incremental snapshot was " << (lsn - 1);

  // This check is safe, but it is not trivially safe.
  // We rely here on the fact that JournalSlice::AddLogRecord can
  // only preempt while holding the callback lock.
  // That guarantees that if we have processed the last LSN the callback
  // will only be added after JournalSlice::AddLogRecord has finished
  // iterating its callbacks and we won't process the record twice.
  // We have to make sure we don't preempt ourselves before registering the callback!

  // GetLsn() is always the next lsn that we expect to create.
  if (journal->GetLsn() == lsn) {
    std::ignore = serializer_->SendFullSyncCut();

    journal_cb_id_ = journal->RegisterOnChange(this);
    PushSerialized(true);
  } else {
    // We stopped but we didn't manage to send the whole stream.
    cntx_->ReportError(
        std::make_error_code(errc::state_not_recoverable),
        absl::StrCat("Partial sync was unsuccessful because entry #", lsn,
                     " was dropped from the buffer. Current lsn=", journal->GetLsn()));
  }
}

bool SliceSnapshot::BucketSaveCb(DbIndex db_index, PrimeTable::bucket_iterator it) {
  std::lock_guard guard(big_value_mu_);

  ++stats_.savecb_calls;

  if (use_snapshot_version_) {
    if (it.GetVersion() >= snapshot_version_) {
      // either has been already serialized or added after snapshotting started.
      DVLOG(3) << "Skipped " << it.segment_id() << ":" << it.bucket_id() << " at "
               << it.GetVersion();
      ++stats_.skipped;
      return false;
    }

    db_slice_->FlushChangeToEarlierCallbacks(db_index, DbSlice::Iterator::FromPrime(it),
                                             snapshot_version_);
  }

  auto* latch = db_slice_->GetLatch();

  // Locking this never preempts. We merely just increment the underline counter such that
  // if SerializeBucket preempts, Heartbeat() won't run because the blocking counter is not
  // zero.
  std::lock_guard latch_guard(*latch);

  stats_.loop_serialized += SerializeBucket(db_index, it);

  return false;
}

unsigned SliceSnapshot::SerializeBucket(DbIndex db_index, PrimeTable::bucket_iterator it) {
  if (use_snapshot_version_) {
    DCHECK_LT(it.GetVersion(), snapshot_version_);
    it.SetVersion(snapshot_version_);
  }

  // traverse physical bucket and write it into string file.
  serialize_bucket_running_ = true;

  unsigned result = 0;

  for (it.AdvanceIfNotOccupied(); !it.is_done(); ++it) {
    ++result;
    // might preempt due to big value serialization.
    SerializeEntry(db_index, it->first, it->second);
  }
  serialize_bucket_running_ = false;
  return result;
}

void SliceSnapshot::SerializeEntry(DbIndex db_indx, const PrimeKey& pk, const PrimeValue& pv) {
  if (pv.IsExternal() && pv.IsCool())
    return SerializeEntry(db_indx, pk, pv.GetCool().record->value);

  time_t expire_time = 0;
  if (pv.HasExpire()) {
    auto eit = db_array_[db_indx]->expire.Find(pk);
    expire_time = db_slice_->ExpireTime(eit);
  }
  uint32_t mc_flags = pv.HasFlag() ? db_slice_->GetMCFlag(db_indx, pk) : 0;

  if (pv.IsExternal()) {
    // TODO: we loose the stickiness attribute by cloning like this PrimeKey.
    SerializeExternal(db_indx, PrimeKey{pk.ToString()}, pv, expire_time, mc_flags);
  } else {
    io::Result<uint8_t> res = serializer_->SaveEntry(pk, pv, expire_time, mc_flags, db_indx);
    CHECK(res);
    ++type_freq_map_[*res];
  }
}

size_t SliceSnapshot::FlushSerialized(SerializerBase::FlushState flush_state) {
  io::StringFile sfile;
  error_code ec = serializer_->FlushToSink(&sfile, flush_state);
  CHECK(!ec);  // always succeeds

  size_t serialized = sfile.val.size();
  if (serialized == 0)
    return 0;

  uint64_t id = rec_id_++;
  DVLOG(2) << "Pushing " << id;

  uint64_t running_cycles = ThisFiber::GetRunningTimeCycles();

  fb2::NoOpLock lk;
  // We create a critical section here that ensures that records are pushed in sequential order.
  // As a result, it is not possible for two fiber producers to push concurrently.
  // If A.id = 5, and then B.id = 6, and both are blocked here, it means that last_pushed_id_ < 4.
  // Once last_pushed_id_ = 4, A will be unblocked, while B will wait until A finishes pushing and
  // update last_pushed_id_ to 5.
  seq_cond_.wait(lk, [&] { return id == this->last_pushed_id_ + 1; });

  // Blocking point.
  consumer_->ConsumeData(std::move(sfile.val), cntx_);

  DCHECK_EQ(last_pushed_id_ + 1, id);
  last_pushed_id_ = id;
  seq_cond_.notify_all();

  VLOG(2) << "Pushed with Serialize() " << serialized;

  // FlushToSink can be quite slow for large values or due compression, therefore
  // we counter-balance CPU over-usage by forcing sleep.
  // We measure running_cycles before the preemption points, because they reset the counter.
  uint64_t sleep_usec = (running_cycles * 1000'000 / base::CycleClock::Frequency()) / 2;
  ThisFiber::SleepFor(chrono::microseconds(std::min<uint64_t>(sleep_usec, 2000ul)));

  return serialized;
}

bool SliceSnapshot::PushSerialized(bool force) {
  if (!force && serializer_->SerializedLen() < kMinBlobSize && delayed_entries_.size() < 32)
    return false;

  // Flush any of the leftovers to avoid interleavings
  size_t serialized = FlushSerialized(FlushState::kFlushEndEntry);

  if (!delayed_entries_.empty()) {
    // Async bucket serialization might have accumulated some delayed values.
    // Because we can finally block in this function, we'll await and serialize them
    do {
      // We may call PushSerialized from multiple fibers concurrently, so we need to
      // ensure that we are not serializing the same entry concurrently.
      DelayedEntry entry = std::move(delayed_entries_.back());
      delayed_entries_.pop_back();

      // TODO: https://github.com/dragonflydb/dragonfly/issues/4654
      // there are a few problems with how we serialize external values.
      // 1. We may block here too frequently, slowing down the process.
      // 2. For small bin values, we issue multiple reads for the same page, creating
      //    read factor amplification that can reach factor of ~60.
      PrimeValue pv{*entry.value.Get()};  // Might block until the future resolves.

      // TODO: to introduce RdbSerializer::SaveString that can accept a string value directly.
      serializer_->SaveEntry(entry.key, pv, entry.expire, entry.mc_flags, entry.dbid);
    } while (!delayed_entries_.empty());

    // blocking point.
    serialized += FlushSerialized(FlushState::kFlushEndEntry);
  }
  return serialized > 0;
}

void SliceSnapshot::SerializeExternal(DbIndex db_index, PrimeKey key, const PrimeValue& pv,
                                      time_t expire_time, uint32_t mc_flags) {
  // We prefer avoid blocking, so we just schedule a tiered read and append
  // it to the delayed entries.
  auto future = EngineShard::tlocal()->tiered_storage()->Read(db_index, key.ToString(), pv);
  delayed_entries_.push_back({db_index, std::move(key), std::move(future), expire_time, mc_flags});
  ++type_freq_map_[RDB_TYPE_STRING];
}

void SliceSnapshot::OnDbChange(DbIndex db_index, const DbSlice::ChangeReq& req) {
  std::lock_guard guard(big_value_mu_);
  // Only when creating point in time snapshot we need to serialize the bucket before we change the
  // db entry. When creating no point in time snapshot we need to call OnDbChange which will take
  // the big_value_mu_ to make sure we do not mutate the bucket while serializing it.
  if (use_snapshot_version_) {
    PrimeTable* table = db_slice_->GetTables(db_index).first;
    const PrimeTable::bucket_iterator* bit = req.update();

    if (bit) {
      if (!bit->is_done() && bit->GetVersion() < snapshot_version_) {
        stats_.side_saved += SerializeBucket(db_index, *bit);
      }
    } else {
      string_view key = get<string_view>(req.change);
      table->CVCUponInsert(snapshot_version_, key,
                           [this, db_index](PrimeTable::bucket_iterator it) {
                             DCHECK_LT(it.GetVersion(), snapshot_version_);
                             stats_.side_saved += SerializeBucket(db_index, it);
                           });
    }
  }
}

bool SliceSnapshot::IsPositionSerialized(DbIndex id, PrimeTable::Cursor cursor) {
  uint8_t depth = db_slice_->GetTables(id).first->depth();

  return id < snapshot_db_index_ ||
         (id == snapshot_db_index_ &&
          (cursor.bucket_id() < snapshot_cursor_.bucket_id() ||
           (cursor.bucket_id() == snapshot_cursor_.bucket_id() &&
            cursor.segment_id(depth) < snapshot_cursor_.segment_id(depth))));
}

void SliceSnapshot::OnMoved(DbIndex id, const DbSlice::MovedItemsVec& items) {
  std::lock_guard barrier(big_value_mu_);
  DCHECK(!use_snapshot_version_);
  for (const auto& item_cursors : items) {
    // If item was moved from a bucket that was serialized to a bucket that was not serialized
    // serialize the moved item.
    const PrimeTable::Cursor& dest = item_cursors.second;
    const PrimeTable::Cursor& source = item_cursors.first;
    if (IsPositionSerialized(id, dest) && !IsPositionSerialized(id, source)) {
      PrimeTable::bucket_iterator bit = db_slice_->GetTables(id).first->CursorToBucketIt(dest);
      ++stats_.moved_saved;
      SerializeBucket(id, bit);
    }
  }
}

// For any key any journal entry must arrive at the replica strictly after its first original rdb
// value. This is guaranteed by the fact that OnJournalEntry runs always after OnDbChange, and
// no database switch can be performed between those two calls, because they are part of one
// transaction.
void SliceSnapshot::ConsumeJournalChange(const journal::JournalItem& item) {
  // We grab the lock in case we are in the middle of serializing a bucket, so it serves as a
  // barrier here for atomic serialization.
  std::lock_guard barrier(big_value_mu_);
  std::ignore = serializer_->WriteJournalEntry(item.data);
  ++stats_.jounal_changes;
}

void SliceSnapshot::ThrottleIfNeeded() {
  PushSerialized(false);
}

size_t SliceSnapshot::GetBufferCapacity() const {
  if (serializer_ == nullptr) {
    return 0;
  }

  return serializer_->GetBufferCapacity();
}

size_t SliceSnapshot::GetTempBuffersSize() const {
  if (serializer_ == nullptr) {
    return 0;
  }

  return serializer_->GetTempBufferSize();
}

RdbSaver::SnapshotStats SliceSnapshot::GetCurrentSnapshotProgress() const {
  return {stats_.loop_serialized + stats_.side_saved, stats_.keys_total};
}

}  // namespace dfly
