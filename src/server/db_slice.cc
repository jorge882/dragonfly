// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/db_slice.h"

extern "C" {
#include "redis/hyperloglog.h"
}

#include <absl/cleanup/cleanup.h>

#include "base/flags.h"
#include "base/logging.h"
#include "core/top_keys.h"
#include "search/doc_index.h"
#include "server/channel_store.h"
#include "server/cluster/slot_set.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/journal/journal.h"
#include "server/server_state.h"
#include "server/tiered_storage.h"
#include "strings/human_readable.h"
#include "util/fibers/fibers.h"
#include "util/fibers/stacktrace.h"

ABSL_FLAG(uint32_t, max_eviction_per_heartbeat, 100,
          "The maximum number of key-value pairs that will be deleted in each eviction "
          "when heartbeat based eviction is triggered under memory pressure.");

ABSL_FLAG(uint32_t, max_segment_to_consider, 4,
          "The maximum number of dashtable segments to scan in each eviction "
          "when heartbeat based eviction is triggered under memory pressure.");

ABSL_FLAG(double, table_growth_margin, 0.4,
          "Prevents table from growing if number of free slots x average object size x this ratio "
          "is larger than memory budget.");

ABSL_FLAG(std::string, notify_keyspace_events, "",
          "notify-keyspace-events. Only Ex is supported for now");

ABSL_FLAG(bool, cluster_flush_decommit_memory, false, "Decommit memory after flushing slots");

namespace dfly {

using namespace std;
using namespace util;
using absl::GetFlag;
using namespace facade;
using Payload = journal::Entry::Payload;

namespace {

constexpr auto kPrimeSegmentSize = PrimeTable::kSegBytes;
constexpr auto kExpireSegmentSize = ExpireTable::kSegBytes;
constexpr auto kExpireRegularSize = ExpireTable::kSegRegularBytes;
// mi_malloc good size is 32768. i.e. we have malloc waste of 1.5%.
static_assert(kPrimeSegmentSize <= 32304);

// 20480 is the next goodsize so we are loosing ~300 bytes or 1.5%.
// 24576
static_assert(kExpireSegmentSize <= 23544);
static_assert(double(kExpireRegularSize) / kExpireSegmentSize > 0.9);

void AccountObjectMemory(string_view key, unsigned type, int64_t size, DbTable* db) {
  DCHECK_NE(db, nullptr);
  if (size == 0)
    return;

  DbTableStats& stats = db->stats;
  DCHECK_GE(static_cast<int64_t>(stats.obj_memory_usage) + size, 0)
      << "Can't decrease " << size << " from " << stats.obj_memory_usage;

  stats.AddTypeMemoryUsage(type, size);

  if (db->slots_stats) {
    db->slots_stats[KeySlot(key)].memory_bytes += size;
  }
}

class PrimeEvictionPolicy {
 public:
  static constexpr bool can_evict = true;  // we implement eviction functionality.
  static constexpr bool can_gc = true;

  // mem_offset - memory_offset that we should account for in addition to DbSlice::memory_budget.
  // May be negative.
  PrimeEvictionPolicy(const DbContext& cntx, bool can_evict, ssize_t mem_offset, ssize_t soft_limit,
                      DbSlice* db_slice, bool apply_memory_limit)
      : db_slice_(db_slice),
        mem_offset_(mem_offset),
        soft_limit_(soft_limit),
        cntx_(cntx),
        can_evict_(can_evict),
        apply_memory_limit_(apply_memory_limit) {
  }

  // A hook function that is called every time a segment is full and requires splitting.
  void RecordSplit(PrimeTable::Segment_t* segment) {
    DVLOG(2) << "split: " << segment->SlowSize() << "/" << segment->capacity();
  }
  void OnMove(PrimeTable::Cursor source, PrimeTable::Cursor dest) {
    moved_items_.push_back(std::make_pair(source, dest));
  }

  bool CanGrow(const PrimeTable& tbl) const;

  unsigned GarbageCollect(const PrimeTable::HotBuckets& eb, PrimeTable* me);
  unsigned Evict(const PrimeTable::HotBuckets& eb, PrimeTable* me);

  unsigned evicted() const {
    return evicted_;
  }

  unsigned checked() const {
    return checked_;
  }
  const DbSlice::MovedItemsVec& moved_items() {
    return moved_items_;
  }

 private:
  DbSlice::MovedItemsVec moved_items_;
  DbSlice* db_slice_;
  ssize_t mem_offset_;
  ssize_t soft_limit_ = 0;
  const DbContext cntx_;

  unsigned evicted_ = 0;
  unsigned checked_ = 0;

  // unlike static constexpr can_evict, this parameter tells whether we can evict
  // items in runtime.
  const bool can_evict_;
  const bool apply_memory_limit_;
};

bool PrimeEvictionPolicy::CanGrow(const PrimeTable& tbl) const {
  ssize_t mem_available = db_slice_->memory_budget() + mem_offset_;
  if (!apply_memory_limit_ || mem_available > soft_limit_)
    return true;

  DCHECK_LE(tbl.size(), tbl.capacity());

  // We take a conservative stance here -
  // we estimate how much memory we will take with the current capacity
  // even though we may currently use less memory.
  // see https://github.com/dragonflydb/dragonfly/issues/256#issuecomment-1227095503
  size_t table_free_items = ((tbl.capacity() - tbl.size()) + PrimeTable::kSegCapacity) *
                            GetFlag(FLAGS_table_growth_margin);

  size_t obj_bytes_estimation = db_slice_->bytes_per_object() * table_free_items;
  bool res = mem_available > int64_t(PrimeTable::kSegBytes + obj_bytes_estimation);
  if (res) {
    VLOG(1) << "free_items: " << table_free_items
            << ", obj_bytes: " << db_slice_->bytes_per_object() << " "
            << " mem_available: " << mem_available;
  } else {
    LOG_EVERY_T(INFO, 1) << "Can't grow, free_items " << table_free_items
                         << ", obj_bytes: " << db_slice_->bytes_per_object() << " "
                         << " mem_available: " << mem_available;
  }

  return res;
}

unsigned PrimeEvictionPolicy::GarbageCollect(const PrimeTable::HotBuckets& eb, PrimeTable* me) {
  unsigned res = 0;

  if (db_slice_->WillBlockOnJournalWrite()) {
    return res;
  }

  // Disable flush journal changes to prevent preemtion in GarbageCollect.
  journal::JournalFlushGuard journal_flush_guard(db_slice_->shard_owner()->journal());

  // bool should_print = (eb.key_hash % 128) == 0;

  // based on tests - it's more efficient to pass regular buckets to gc.
  // stash buckets are filled last so much smaller change they have expired items.
  string scratch;
  unsigned num_buckets =
      std::min<unsigned>(PrimeTable::HotBuckets::kRegularBuckets, eb.num_buckets);
  for (unsigned i = 0; i < num_buckets; ++i) {
    auto bucket_it = eb.at(i);
    for (; !bucket_it.is_done(); ++bucket_it) {
      if (bucket_it->second.HasExpire()) {
        string_view key = bucket_it->first.GetSlice(&scratch);
        ++checked_;
        auto [prime_it, exp_it] = db_slice_->ExpireIfNeeded(
            cntx_, DbSlice::Iterator(bucket_it, StringOrView::FromView(key)));
        if (prime_it.is_done())
          ++res;
      }
    }
  }

  return res;
}

unsigned PrimeEvictionPolicy::Evict(const PrimeTable::HotBuckets& eb, PrimeTable* me) {
  if (!can_evict_ || db_slice_->WillBlockOnJournalWrite())
    return 0;

  // Disable flush journal changes to prevent preemtion in evict.
  journal::JournalFlushGuard journal_flush_guard(db_slice_->shard_owner()->journal());

  constexpr size_t kNumStashBuckets = ABSL_ARRAYSIZE(eb.probes.by_type.stash_buckets);

  // choose "randomly" a stash bucket to evict an item.
  auto bucket_it = eb.probes.by_type.stash_buckets[eb.key_hash % kNumStashBuckets];
  auto last_slot_it = bucket_it;
  last_slot_it += (PrimeTable::kSlotNum - 1);
  if (!last_slot_it.is_done()) {
    // don't evict sticky items
    if (last_slot_it->first.IsSticky()) {
      return 0;
    }

    DbTable* table = db_slice_->GetDBTable(cntx_.db_index);
    auto& lt = table->trans_locks;
    string scratch;
    string_view key = last_slot_it->first.GetSlice(&scratch);
    // do not evict locked keys
    if (lt.Find(LockTag(key)).has_value())
      return 0;

    // log the evicted keys to journal.
    if (auto journal = db_slice_->shard_owner()->journal(); journal) {
      RecordExpiryBlocking(cntx_.db_index, key);
    }
    db_slice_->PerformDeletion(DbSlice::Iterator(last_slot_it, StringOrView::FromView(key)), table);

    ++evicted_;
  }
  me->ShiftRight(bucket_it);

  return 1;
}

class AsyncDeleter {
 public:
  static void EnqueDeletion(uint32_t next, DenseSet* ds);
  static void Shutdown();

 private:
  static constexpr uint32_t kClearStepSize = 1024;
  struct ClearNode {
    DenseSet* ds;
    uint32_t cursor;
    ClearNode* next;

    ClearNode(DenseSet* d, uint32_t c, ClearNode* n) : ds(d), cursor(c), next(n) {
    }
  };

  // Asynchronously deletes entries during the cpu-idle time.
  static int32_t IdleCb();

  // We add async deletion requests to a linked list and process them asynchronously
  // in each thread.
  static __thread ClearNode* head_;
};

__thread AsyncDeleter::ClearNode* AsyncDeleter::head_ = nullptr;

void AsyncDeleter::EnqueDeletion(uint32_t next, DenseSet* ds) {
  bool launch_task = (head_ == nullptr);

  // register ds
  head_ = new ClearNode{ds, next, head_};
  ProactorBase* pb = ProactorBase::me();
  DCHECK(pb);
  DVLOG(2) << "Adding async deletion task, thread " << pb->GetPoolIndex() << " " << launch_task;
  if (launch_task) {
    pb->AddOnIdleTask(&IdleCb);
  }
}

void AsyncDeleter::Shutdown() {
  // we do not bother with deleting objects scheduled for asynchronous deletion
  // during the shutdown. this should work well because we destroy mimalloc heap anyways.
  while (head_) {
    auto* next = head_->next;
    delete head_;
    head_ = next;
  }
}

int32_t AsyncDeleter::IdleCb() {
  if (head_ == nullptr)
    return -1;  // unregister itself.

  auto* current = head_;

  DVLOG(2) << "IdleCb " << current->cursor;
  uint32_t next = current->ds->ClearStep(current->cursor, kClearStepSize);
  if (next == current->ds->BucketCount()) {  // reached the end.
    CompactObj::DeleteMR<DenseSet>(current->ds);
    head_ = current->next;
    delete current;
  } else {
    current->cursor = next;
  }
  return ProactorBase::kOnIdleMaxLevel;
};

inline void TouchTopKeysIfNeeded(string_view key, TopKeys* top_keys) {
  if (top_keys) {
    top_keys->Touch(key);
  }
}

inline void TouchHllIfNeeded(string_view key, uint8_t* hll) {
  if (hll) {
    HllBufferPtr hll_buf;
    hll_buf.size = getDenseHllSize();
    hll_buf.hll = hll;
    pfadd_dense(hll_buf, reinterpret_cast<const uint8_t*>(key.data()), key.size());
  }
}

}  // namespace

#define ADD(x) (x) += o.x

DbStats& DbStats::operator+=(const DbStats& o) {
  constexpr size_t kDbSz = sizeof(DbStats) - sizeof(DbTableStats);
  static_assert(kDbSz == 40);

  DbTableStats::operator+=(o);

  ADD(key_count);
  ADD(expire_count);
  ADD(prime_capacity);
  ADD(expire_capacity);
  ADD(table_mem_usage);

  return *this;
}

SliceEvents& SliceEvents::operator+=(const SliceEvents& o) {
  static_assert(sizeof(SliceEvents) == 136, "You should update this function with new fields");

  ADD(evicted_keys);
  ADD(hard_evictions);
  ADD(expired_keys);
  ADD(garbage_collected);
  ADD(stash_unloaded);
  ADD(bumpups);
  ADD(garbage_checked);
  ADD(hits);
  ADD(misses);
  ADD(mutations);
  ADD(insertion_rejections);
  ADD(update);
  ADD(ram_hits);
  ADD(ram_cool_hits);
  ADD(ram_misses);
  ADD(huff_encode_total);
  ADD(huff_encode_success);
  return *this;
}

#undef ADD

class DbSlice::PrimeBumpPolicy {
 public:
  bool CanBump(const CompactObj& obj) const {
    return !obj.IsSticky();
  }
  void OnMove(PrimeTable::Cursor source, PrimeTable::Cursor dest) {
    moved_items_.push_back(std::make_pair(source, dest));
  }

  const DbSlice::MovedItemsVec& moved_items() {
    return moved_items_;
  }

 private:
  DbSlice::MovedItemsVec moved_items_;
};

DbSlice::DbSlice(uint32_t index, bool cache_mode, EngineShard* owner)
    : shard_id_(index),
      cache_mode_(cache_mode),
      owner_(owner),
      client_tracking_map_(owner->memory_resource()) {
  db_arr_.emplace_back();
  CreateDb(0);
  expire_base_[0] = expire_base_[1] = 0;

  std::string keyspace_events = GetFlag(FLAGS_notify_keyspace_events);
  if (!keyspace_events.empty() && keyspace_events != "Ex") {
    LOG(ERROR) << "Only Ex is currently supported";
    exit(0);
  }
  expired_keys_events_recording_ = !keyspace_events.empty();
}

DbSlice::~DbSlice() {
  // we do not need this code but it's easier to debug in case we encounter
  // memory allocation bugs during delete operations.

  for (auto& db : db_arr_) {
    if (!db)
      continue;
    db.reset();
  }

  AsyncDeleter::Shutdown();
}

auto DbSlice::GetStats() const -> Stats {
  Stats s;
  s.events = events_;
  s.db_stats.resize(db_arr_.size());

  for (size_t i = 0; i < db_arr_.size(); ++i) {
    if (!db_arr_[i])
      continue;
    const auto& db_wrap = *db_arr_[i];
    DbStats& stats = s.db_stats[i];
    stats = db_wrap.stats;
    stats.key_count = db_wrap.prime.size();
    stats.prime_capacity = db_wrap.prime.capacity();
    stats.expire_capacity = db_wrap.expire.capacity();
    stats.expire_count = db_wrap.expire.size();
    stats.table_mem_usage = db_wrap.table_memory();
  }
  auto co_stats = CompactObj::GetStatsThreadLocal();
  s.small_string_bytes = co_stats.small_string_bytes;
  s.events.huff_encode_total = co_stats.huff_encode_total;
  s.events.huff_encode_success = co_stats.huff_encode_success;

  return s;
}

SlotStats DbSlice::GetSlotStats(SlotId sid) const {
  CHECK(db_arr_[0]);
  return db_arr_[0]->slots_stats[sid];
}

void DbSlice::Reserve(DbIndex db_ind, size_t key_size) {
  ActivateDb(db_ind);

  auto& db = db_arr_[db_ind];
  DCHECK(db);

  db->prime.Reserve(key_size);
}

DbSlice::AutoUpdater::AutoUpdater() {
}

DbSlice::AutoUpdater::AutoUpdater(AutoUpdater&& o) noexcept {
  *this = std::move(o);
}

DbSlice::AutoUpdater& DbSlice::AutoUpdater::operator=(AutoUpdater&& o) noexcept {
  Run();
  fields_ = o.fields_;
  o.Cancel();
  return *this;
}

DbSlice::AutoUpdater::~AutoUpdater() {
  Run();
}

void DbSlice::AutoUpdater::ReduceHeapUsage() {
  AccountObjectMemory(fields_.key, fields_.it->second.ObjType(), -fields_.orig_heap_size,
                      fields_.db_slice->GetDBTable(fields_.db_ind));
  fields_.orig_heap_size = 0;  // Reset to avoid double accounting.
}

void DbSlice::AutoUpdater::Run() {
  if (fields_.db_slice == nullptr) {
    return;
  }

  // Check that AutoUpdater does not run after a key was removed.
  // If this CHECK() failed for you, it probably means that you deleted a key while having an auto
  // updater in scope. You'll probably want to call Run() (or Cancel() - but be careful).
  DCHECK(IsValid(fields_.db_slice->db_arr_[fields_.db_ind]->prime.Find(fields_.key)));

  CHECK_NE(fields_.db_slice, nullptr);

  ssize_t delta = static_cast<int64_t>(fields_.it->second.MallocUsed()) -
                  static_cast<int64_t>(fields_.orig_heap_size);
  AccountObjectMemory(fields_.key, fields_.it->second.ObjType(), delta,
                      fields_.db_slice->GetDBTable(fields_.db_ind));
  fields_.db_slice->PostUpdate(fields_.db_ind, fields_.key);
  Cancel();  // Reset to not run again
}

void DbSlice::AutoUpdater::Cancel() {
  this->fields_ = {};
}

DbSlice::AutoUpdater::AutoUpdater(DbIndex db_ind, std::string_view key, const Iterator& it,
                                  DbSlice* db_slice)
    : fields_{.db_slice = db_slice,
              .db_ind = db_ind,
              .it = it,
              .key = key,
              .orig_heap_size = it->second.MallocUsed()} {
  DCHECK(IsValid(it));
}

DbSlice::ItAndUpdater DbSlice::FindMutable(const Context& cntx, string_view key) {
  return std::move(FindMutableInternal(cntx, key, std::nullopt).value());
}

OpResult<DbSlice::ItAndUpdater> DbSlice::FindMutable(const Context& cntx, string_view key,
                                                     unsigned req_obj_type) {
  return FindMutableInternal(cntx, key, req_obj_type);
}

OpResult<DbSlice::ItAndUpdater> DbSlice::FindMutableInternal(const Context& cntx, string_view key,
                                                             std::optional<unsigned> req_obj_type) {
  auto res = FindInternal(cntx, key, req_obj_type, UpdateStatsMode::kMutableStats);
  if (!res.ok()) {
    return res.status();
  }

  auto it = Iterator(res->it, StringOrView::FromView(key));
  auto exp_it = ExpIterator(res->exp_it, StringOrView::FromView(key));
  PreUpdateBlocking(cntx.db_index, it);
  // PreUpdate() might have caused a deletion of `it`
  if (res->it.IsOccupied()) {
    DCHECK_GE(db_arr_[cntx.db_index]->stats.obj_memory_usage, res->it->second.MallocUsed());

    return {{it, exp_it, AutoUpdater{cntx.db_index, key, it, this}}};
  } else {
    return OpStatus::KEY_NOTFOUND;
  }
}

DbSlice::ItAndExpConst DbSlice::FindReadOnly(const Context& cntx, std::string_view key) const {
  auto res = FindInternal(cntx, key, std::nullopt, UpdateStatsMode::kReadStats);
  return {ConstIterator(res->it, StringOrView::FromView(key)),
          ExpConstIterator(res->exp_it, StringOrView::FromView(key))};
}

OpResult<DbSlice::ConstIterator> DbSlice::FindReadOnly(const Context& cntx, string_view key,
                                                       unsigned req_obj_type) const {
  auto res = FindInternal(cntx, key, req_obj_type, UpdateStatsMode::kReadStats);
  if (res.ok()) {
    return ConstIterator(res->it, StringOrView::FromView(key));
  }
  return res.status();
}

auto DbSlice::FindInternal(const Context& cntx, string_view key, optional<unsigned> req_obj_type,
                           UpdateStatsMode stats_mode) const -> OpResult<PrimeItAndExp> {
  if (!IsDbValid(cntx.db_index)) {  // Can it even happen?
    LOG(DFATAL) << "Invalid db index " << cntx.db_index;
    return OpStatus::KEY_NOTFOUND;
  }

  auto& db = *db_arr_[cntx.db_index];
  PrimeItAndExp res;
  res.it = db.prime.Find(key);
  int miss_weight = (stats_mode == UpdateStatsMode::kReadStats);

  if (!IsValid(res.it)) {
    events_.misses += miss_weight;
    return OpStatus::KEY_NOTFOUND;
  }

  TouchTopKeysIfNeeded(key, db.top_keys);
  TouchHllIfNeeded(key, db.dense_hll);

  if (req_obj_type.has_value() && res.it->second.ObjType() != req_obj_type.value()) {
    events_.misses += miss_weight;
    return OpStatus::WRONG_TYPE;
  }

  if (res.it->second.HasExpire()) {  // check expiry state
    res = ExpireIfNeeded(cntx, res.it);
    if (!IsValid(res.it)) {
      events_.misses += miss_weight;
      return OpStatus::KEY_NOTFOUND;
    }
  }

  DCHECK(IsValid(res.it));

  if (IsCacheMode()) {
    fetched_items_.insert({res.it->first.HashCode(), cntx.db_index});
  }

  switch (stats_mode) {
    case UpdateStatsMode::kMutableStats:
      events_.mutations++;
      break;
    case UpdateStatsMode::kReadStats:
      events_.hits++;
      if (db.slots_stats) {
        db.slots_stats[KeySlot(key)].total_reads++;
      }
      if (res.it->second.IsExternal()) {
        if (res.it->second.IsCool())
          events_.ram_cool_hits++;
        else
          events_.ram_misses++;
      } else {
        events_.ram_hits++;
      }
      break;
  }

  auto& pv = res.it->second;

  // Cancel any pending stashes of looked up values
  // Rationale: we either look it up for reads - and then it's hot, or alternatively,
  // we follow up with modifications, so the pending stash becomes outdated.
  if (pv.HasStashPending()) {
    owner_->tiered_storage()->CancelStash(cntx.db_index, key, &pv);
  }

  // Fetch back cool items
  if (pv.IsExternal() && pv.IsCool()) {
    pv = owner_->tiered_storage()->Warmup(cntx.db_index, pv.GetCool());
  }

  // Mark this entry as being looked up. We use key (first) deliberately to preserve the hotness
  // attribute of the entry in case of value overrides.
  res.it->first.SetTouched(true);

  return res;
}

OpResult<DbSlice::ItAndUpdater> DbSlice::AddOrFind(const Context& cntx, string_view key,
                                                   std::optional<unsigned> req_obj_type) {
  return AddOrFindInternal(cntx, key, req_obj_type);
}

OpResult<DbSlice::ItAndUpdater> DbSlice::AddOrFindInternal(const Context& cntx, string_view key,
                                                           std::optional<unsigned> req_obj_type) {
  DCHECK(IsDbValid(cntx.db_index));

  DbTable& db = *db_arr_[cntx.db_index];
  auto res = FindInternal(cntx, key, req_obj_type, UpdateStatsMode::kMutableStats);

  if (res.ok()) {
    Iterator it(res->it, StringOrView::FromView(key));
    ExpIterator exp_it(res->exp_it, StringOrView::FromView(key));
    PreUpdateBlocking(cntx.db_index, it);

    // PreUpdate() might have caused a deletion of `it`
    if (res->it.IsOccupied()) {
      return ItAndUpdater{
          .it = it, .exp_it = exp_it, .post_updater{cntx.db_index, key, it, this}, .is_new = false};
    } else {
      res = OpStatus::KEY_NOTFOUND;
    }
  } else if (res == OpStatus::WRONG_TYPE) {
    return OpStatus::WRONG_TYPE;
  }

  auto status = res.status();
  CHECK(status == OpStatus::KEY_NOTFOUND || status == OpStatus::OUT_OF_MEMORY) << status;

  // It's a new entry.
  CallChangeCallbacks(cntx.db_index, ChangeReq{key});

  ssize_t memory_offset = -key.size();
  size_t reclaimed = 0;
  // If we are low on memory due to cold storage, free some memory.
  if (owner_->tiered_storage()) {
    // At least 40KB bytes to cover potential segment split.
    ssize_t red_line = std::max<size_t>(key.size() * 2, 40_KB);
    if (memory_budget_ < red_line) {
      size_t goal = red_line - memory_budget_;
      reclaimed = owner_->tiered_storage()->ReclaimMemory(goal);
      memory_budget_ += reclaimed;
    }

    // CoolMemoryUsage is the memory that we can always reclaim, like in the block above,
    // therefore we include it for PrimeEvictionPolicy considerations.
    memory_offset += owner_->tiered_storage()->CoolMemoryUsage();
  }

  // In case we are loading from rdb file or replicating we want to disable conservative memory
  // checks (inside PrimeEvictionPolicy::CanGrow) and reject insertions only after we pass max
  // memory limit. When loading a snapshot created by the same server configuration (memory and
  // number of shards) we will create a different dash table segment directory tree, because the
  // tree shape is related to the order of entries insertion. Therefore when loading data from
  // snapshot or from replication the conservative memory checks might fail as the new tree might
  // have more segments. Because we dont want to fail loading a snapshot from the same server
  // configuration we disable this checks on loading and replication.
  bool apply_memory_limit =
      !owner_->IsReplica() && !(ServerState::tlocal()->gstate() == GlobalState::LOADING);

  // If we are over limit in non-cache scenario, just be conservative and throw.
  if (apply_memory_limit && !IsCacheMode() && memory_budget_ + memory_offset < 0) {
    LOG_EVERY_T(WARNING, 1) << "AddOrFind: over limit, budget: " << memory_budget_
                            << " reclaimed: " << reclaimed << " offset: " << memory_offset;
    events_.insertion_rejections++;
    return OpStatus::OUT_OF_MEMORY;
  }

  ssize_t soft_budget_limit =
      (0.3 * max_memory_limit.load(memory_order_relaxed)) / shard_set->size();
  PrimeEvictionPolicy evp{cntx,          (IsCacheMode() && !owner_->IsReplica()),
                          memory_offset, soft_budget_limit,
                          this,          apply_memory_limit};

  // Fast-path if change_cb_ is empty so we Find or Add using
  // the insert operation: twice more efficient.
  CompactObj co_key{key};
  PrimeIterator it;

  ssize_t table_before = db.prime.mem_usage();

  try {
    it = db.prime.InsertNew(std::move(co_key), PrimeValue{}, evp);
  } catch (bad_alloc& e) {
    LOG_EVERY_T(WARNING, 1) << "AddOrFind: InsertNew failed, budget: " << memory_budget_
                            << " reclaimed: " << reclaimed << " offset: " << memory_offset;
    events_.insertion_rejections++;
    return OpStatus::OUT_OF_MEMORY;
  }
  CallMovedCallbacks(cntx.db_index, evp.moved_items());

  events_.mutations++;
  ssize_t table_increase = db.prime.mem_usage() - table_before;
  memory_budget_ -= table_increase;

  if (memory_budget_ < 0 && apply_memory_limit) {
    // We may reach the state when our memory usage is below the limit even if we
    // do not add new segments. For example, we have half full segments
    // and we add new objects or update the existing ones and our memory usage grows.
    // We do not require for a single operation to unload the whole negative debt.
    // Instead, we create a positive, converging force that should help with freeing enough memory.
    // Free at least K bytes or 3% of the total debt.
    // TODO: to reenable and optimize this - this call significantly slows down server
    // when evictions are running.
#if 0
    size_t evict_goal = std::max<size_t>(512, (-evp.mem_budget()) / 32);
    auto [items, bytes] = FreeMemWithEvictionStep(cntx.db_index, it.segment_id(), evict_goal);
    events_.hard_evictions += items;
#endif
  }

  table_memory_ += table_increase;
  entries_count_++;

  if (it->first.IsInline()) {
    ++db.stats.inline_keys;
  } else {
    AccountObjectMemory(key, it->first.ObjType(), it->first.MallocUsed(), &db);  // Account for key
  }

  DCHECK_EQ(it->second.MallocUsed(), 0UL);  // Make sure accounting is no-op
  it.SetVersion(NextVersion());

  TouchTopKeysIfNeeded(key, db.top_keys);
  TouchHllIfNeeded(key, db.dense_hll);

  events_.garbage_collected = db.prime.garbage_collected();
  events_.stash_unloaded = db.prime.stash_unloaded();
  events_.evicted_keys += evp.evicted();
  events_.garbage_checked += evp.checked();
  if (db.slots_stats) {
    SlotId sid = KeySlot(key);
    db.slots_stats[sid].key_count += 1;
  }

  return ItAndUpdater{
      .it = Iterator(it, StringOrView::FromView(key)),
      .exp_it = ExpIterator{},
      .post_updater{cntx.db_index, key, Iterator(it, StringOrView::FromView(key)), this},
      .is_new = true};
}

void DbSlice::ActivateDb(DbIndex db_ind) {
  if (db_arr_.size() <= db_ind)
    db_arr_.resize(db_ind + 1);
  CreateDb(db_ind);
}

void DbSlice::Del(Context cntx, Iterator it) {
  CHECK(IsValid(it));

  auto& db = db_arr_[cntx.db_index];
  auto obj_type = it->second.ObjType();

  if (doc_del_cb_ && (obj_type == OBJ_JSON || obj_type == OBJ_HASH)) {
    string tmp;
    string_view key = it->first.GetSlice(&tmp);
    doc_del_cb_(key, cntx, it->second);
  }
  PerformDeletion(it, db.get());
}

void DbSlice::FlushSlotsFb(const cluster::SlotSet& slot_ids) {
  VLOG(1) << "Start FlushSlotsFb";
  // Slot deletion can take time as it traverses all the database, hence it runs in fiber.
  // We want to flush all the data of a slot that was added till the time the call to FlushSlotsFb
  // was made. Therefore we delete slots entries with version < next_version
  uint64_t next_version = 0;
  uint64_t del_count = 0;

  // Explicitly copy table smart pointer to keep reference count up (flushall drops it)
  boost::intrusive_ptr<DbTable> table = db_arr_.front();
  size_t memory_before = table->table_memory() + table->stats.obj_memory_usage;

  std::string tmp;
  auto iterate_bucket = [&](PrimeTable::bucket_iterator it) {
    it.AdvanceIfNotOccupied();
    while (!it.is_done()) {
      std::string_view key = it->first.GetSlice(&tmp);
      SlotId sid = KeySlot(key);
      if (slot_ids.Contains(sid) && it.GetVersion() < next_version) {
        PerformDeletion(Iterator::FromPrime(it), table.get());
        ++del_count;
      }
      ++it;
    }
  };

  auto on_change = [&](DbIndex db_index, const ChangeReq& req) {
    FiberAtomicGuard fg;
    PrimeTable* table = GetTables(db_index).first;

    if (const PrimeTable::bucket_iterator* bit = req.update()) {
      if (!bit->is_done() && bit->GetVersion() < next_version) {
        iterate_bucket(*bit);
      }
    } else {
      string_view key = get<string_view>(req.change);
      table->CVCUponInsert(next_version, key,
                           [next_version, iterate_bucket](PrimeTable::bucket_iterator it) {
                             DCHECK_LT(it.GetVersion(), next_version);
                             iterate_bucket(it);
                           });
    }
  };
  next_version = RegisterOnChange(std::move(on_change));

  ServerState& etl = *ServerState::tlocal();
  PrimeTable* pt = &table->prime;
  PrimeTable::Cursor cursor;

  do {
    PrimeTable::Cursor next = pt->TraverseBuckets(cursor, iterate_bucket);
    cursor = next;
    ThisFiber::Yield();
  } while (cursor && etl.gstate() != GlobalState::SHUTTING_DOWN);

  VLOG(1) << "FlushSlotsFb del count is: " << del_count;
  UnregisterOnChange(next_version);

  if (absl::GetFlag(FLAGS_cluster_flush_decommit_memory)) {
    int64_t start = absl::GetCurrentTimeNanos();
    etl.DecommitMemory(ServerState::kDataHeap);
    int64_t took = absl::GetCurrentTimeNanos() - start;
    size_t memory_after = table->table_memory() + table->stats.obj_memory_usage;

    LOG(INFO) << "Memory decommit took " << took << "ns, deleted " << del_count << ", memory delta "
              << (memory_before - memory_after);
  }
}

void DbSlice::FlushSlots(const cluster::SlotRanges& slot_ranges) {
  cluster::SlotSet slot_set(slot_ranges);
  InvalidateSlotWatches(slot_set);
  fb2::Fiber("flush_slots", [this, slot_set = std::move(slot_set)]() mutable {
    FlushSlotsFb(slot_set);
  }).Detach();
}

void DbSlice::FlushDbIndexes(const std::vector<DbIndex>& indexes) {
  bool clear_tiered = owner_->tiered_storage() != nullptr;

  if (clear_tiered)
    ClearOffloadedEntries(indexes, db_arr_);

  DbTableArray flush_db_arr(db_arr_.size());

  for (DbIndex index : indexes) {
    if (!index) {
      owner_->search_indices()->DropAllIndices();
    }

    table_memory_ -= db_arr_[index]->table_memory();
    entries_count_ -= db_arr_[index]->prime.size();

    InvalidateDbWatches(index);
    flush_db_arr[index] = std::move(db_arr_[index]);

    CreateDb(index);
    std::swap(db_arr_[index]->trans_locks, flush_db_arr[index]->trans_locks);
  }

  LOG_IF(DFATAL, !fetched_items_.empty())
      << "Some operation might bumped up items outside of a transaction";

  auto cb = [indexes, flush_db_arr = std::move(flush_db_arr)]() mutable {
    flush_db_arr.clear();
    ServerState::tlocal()->DecommitMemory(ServerState::kDataHeap | ServerState::kBackingHeap |
                                          ServerState::kGlibcmalloc);
  };

  fb2::Fiber("flush_dbs", std::move(cb)).Detach();
}

void DbSlice::FlushDb(DbIndex db_ind) {
  DVLOG(1) << "Flushing db " << db_ind;

  // clear client tracking map.
  client_tracking_map_.clear();

  if (db_ind != kDbAll) {
    // Flush a single database if a specific index is provided
    FlushDbIndexes({db_ind});
    return;
  }

  std::vector<DbIndex> indexes;
  indexes.reserve(db_arr_.size());
  for (DbIndex i = 0; i < db_arr_.size(); ++i) {
    if (db_arr_[i]) {
      indexes.push_back(i);
    }
  }

  FlushDbIndexes(indexes);
}

void DbSlice::AddExpire(DbIndex db_ind, Iterator main_it, uint64_t at) {
  uint64_t delta = at - expire_base_[0];  // TODO: employ multigen expire updates.
  auto& db = *db_arr_[db_ind];
  size_t table_before = db.expire.mem_usage();
  CHECK(db.expire.Insert(main_it->first.AsRef(), ExpirePeriod(delta)).second);
  table_memory_ += (db.expire.mem_usage() - table_before);
  main_it->second.SetExpire(true);
}

bool DbSlice::RemoveExpire(DbIndex db_ind, Iterator main_it) {
  if (main_it->second.HasExpire()) {
    auto& db = *db_arr_[db_ind];
    size_t table_before = db.expire.mem_usage();
    CHECK_EQ(1u, db.expire.Erase(main_it->first));
    main_it->second.SetExpire(false);
    table_memory_ += (db.expire.mem_usage() - table_before);
    return true;
  }
  return false;
}

// Returns true if a state has changed, false otherwise.
bool DbSlice::UpdateExpire(DbIndex db_ind, Iterator it, uint64_t at) {
  if (at == 0) {
    return RemoveExpire(db_ind, it);
  }

  if (!it->second.HasExpire() && at) {
    AddExpire(db_ind, it, at);
    return true;
  }

  return false;
}

void DbSlice::SetMCFlag(DbIndex db_ind, PrimeKey key, uint32_t flag) {
  auto& db = *db_arr_[db_ind];
  if (flag == 0) {
    db.mcflag.Erase(key);
  } else {
    auto [it, _] = db.mcflag.Insert(std::move(key), flag);
    it->second = flag;
  }
}

uint32_t DbSlice::GetMCFlag(DbIndex db_ind, const PrimeKey& key) const {
  auto& db = *db_arr_[db_ind];
  auto it = db.mcflag.Find(key);
  if (it.is_done()) {
    LOG(DFATAL) << "Internal error, inconsistent state, mcflag should be present but not found "
                << key.ToString();
    return 0;
  }
  return it->second;
}

OpResult<DbSlice::ItAndUpdater> DbSlice::AddNew(const Context& cntx, string_view key,
                                                PrimeValue obj, uint64_t expire_at_ms) {
  auto op_result = AddOrUpdateInternal(cntx, key, std::move(obj), expire_at_ms, false);
  RETURN_ON_BAD_STATUS(op_result);
  auto& res = *op_result;
  CHECK(res.is_new);

  return DbSlice::ItAndUpdater{
      .it = res.it, .exp_it = res.exp_it, .post_updater = std::move(res.post_updater)};
}

int64_t DbSlice::ExpireParams::Cap(int64_t value, TimeUnit unit) {
  return unit == TimeUnit::SEC ? min(value, kMaxExpireDeadlineSec)
                               : min(value, kMaxExpireDeadlineMs);
}

pair<int64_t, int64_t> DbSlice::ExpireParams::Calculate(uint64_t now_ms, bool cap) const {
  if (persist)
    return {0, 0};

  // return a negative absolute time if we overflow.
  if (unit == TimeUnit::SEC && value > INT64_MAX / 1000) {
    return {0, -1};
  }

  int64_t msec = (unit == TimeUnit::SEC) ? value * 1000 : value;
  int64_t rel_msec = absolute ? msec - now_ms : msec;
  if (cap)
    rel_msec = Cap(rel_msec, TimeUnit::MSEC);
  return make_pair(rel_msec, now_ms + rel_msec);
}

OpResult<int64_t> DbSlice::UpdateExpire(const Context& cntx, Iterator prime_it,
                                        ExpIterator expire_it, const ExpireParams& params) {
  constexpr uint64_t kPersistValue = 0;
  DCHECK(params.IsDefined());
  DCHECK(IsValid(prime_it));
  // If this need to persist, then only set persist value and return
  if (params.persist) {
    RemoveExpire(cntx.db_index, prime_it);
    return kPersistValue;
  }

  auto [rel_msec, abs_msec] = params.Calculate(cntx.time_now_ms, false);
  if (abs_msec < 0 || rel_msec > kMaxExpireDeadlineMs) {
    return OpStatus::OUT_OF_RANGE;
  }

  if (rel_msec <= 0) {  // implicit - don't persist
    Del(cntx, prime_it);
    return -1;
  } else if (IsValid(expire_it) && !params.persist) {
    auto current = ExpireTime(expire_it);
    if (params.expire_options & ExpireFlags::EXPIRE_NX) {
      return OpStatus::SKIPPED;
    }
    if ((params.expire_options & ExpireFlags::EXPIRE_LT) && current <= abs_msec) {
      return OpStatus::SKIPPED;
    } else if ((params.expire_options & ExpireFlags::EXPIRE_GT) && current >= abs_msec) {
      return OpStatus::SKIPPED;
    }

    expire_it->second = FromAbsoluteTime(abs_msec);
    return abs_msec;
  } else {
    if (params.expire_options & ExpireFlags::EXPIRE_XX) {
      return OpStatus::SKIPPED;
    }
    AddExpire(cntx.db_index, prime_it, abs_msec);
    return abs_msec;
  }
}

OpResult<DbSlice::ItAndUpdater> DbSlice::AddOrUpdateInternal(const Context& cntx,
                                                             std::string_view key, PrimeValue obj,
                                                             uint64_t expire_at_ms,
                                                             bool force_update) {
  DCHECK(!obj.IsRef());

  auto op_result = AddOrFind(cntx, key, std::nullopt);
  RETURN_ON_BAD_STATUS(op_result);

  auto& res = *op_result;
  if (!res.is_new && !force_update)  // have not inserted.
    return op_result;

  auto& db = *db_arr_[cntx.db_index];
  auto& it = res.it;

  it->second = std::move(obj);

  if (expire_at_ms) {
    it->second.SetExpire(true);
    uint64_t delta = expire_at_ms - expire_base_[0];
    if (IsValid(res.exp_it) && force_update) {
      res.exp_it->second = ExpirePeriod(delta);
    } else {
      size_t table_before = db.expire.mem_usage();
      auto exp_it = db.expire.InsertNew(it->first.AsRef(), ExpirePeriod(delta));
      res.exp_it = ExpIterator(exp_it, StringOrView::FromView(key));
      table_memory_ += (db.expire.mem_usage() - table_before);
    }
  }

  return op_result;
}

OpResult<DbSlice::ItAndUpdater> DbSlice::AddOrUpdate(const Context& cntx, string_view key,
                                                     PrimeValue obj, uint64_t expire_at_ms) {
  return AddOrUpdateInternal(cntx, key, std::move(obj), expire_at_ms, true);
}

size_t DbSlice::DbSize(DbIndex db_ind) const {
  DCHECK_LT(db_ind, db_array_size());

  if (IsDbValid(db_ind)) {
    return db_arr_[db_ind]->prime.size();
  }
  return 0;
}

bool DbSlice::Acquire(IntentLock::Mode mode, const KeyLockArgs& lock_args) {
  if (lock_args.fps.empty()) {  // Can be empty for NO_KEY_TRANSACTIONAL commands.
    return true;
  }
  DCHECK_LT(lock_args.db_index, db_array_size());

  auto& lt = db_arr_[lock_args.db_index]->trans_locks;
  bool lock_acquired = true;

  if (lock_args.fps.size() == 1) {
    lock_acquired = lt.Acquire(lock_args.fps.front(), mode);
    uniq_fps_ = {lock_args.fps.front()};  // needed only for tests.
  } else {
    uniq_fps_.clear();

    for (LockFp fp : lock_args.fps) {
      if (uniq_fps_.insert(fp).second) {
        lock_acquired &= lt.Acquire(fp, mode);
      }
    }
  }

  DVLOG(2) << "Acquire " << IntentLock::ModeName(mode) << " for " << lock_args.fps[0]
           << " has_acquired: " << lock_acquired;

  return lock_acquired;
}

void DbSlice::Release(IntentLock::Mode mode, const KeyLockArgs& lock_args) {
  if (lock_args.fps.empty()) {  // Can be empty for NO_KEY_TRANSACTIONAL commands.
    return;
  }

  DVLOG(2) << "Release " << IntentLock::ModeName(mode) << " for " << lock_args.fps[0];
  auto& lt = db_arr_[lock_args.db_index]->trans_locks;
  if (lock_args.fps.size() == 1) {
    uint64_t fp = lock_args.fps.front();
    lt.Release(fp, mode);
  } else {
    uniq_fps_.clear();
    for (LockFp fp : lock_args.fps) {
      if (uniq_fps_.insert(fp).second) {
        lt.Release(fp, mode);
      }
    }
  }
  uniq_fps_.clear();
}

bool DbSlice::CheckLock(IntentLock::Mode mode, DbIndex dbid, uint64_t fp) const {
  const auto& lt = db_arr_[dbid]->trans_locks;
  auto lock = lt.Find(fp);
  if (lock) {
    return lock->Check(mode);
  }
  return true;
}

void DbSlice::PreUpdateBlocking(DbIndex db_ind, Iterator it) {
  CallChangeCallbacks(db_ind, ChangeReq{it.GetInnerIt()});
  it.GetInnerIt().SetVersion(NextVersion());
}

void DbSlice::PostUpdate(DbIndex db_ind, std::string_view key) {
  auto& db = *db_arr_[db_ind];
  auto& watched_keys = db.watched_keys;
  if (!watched_keys.empty()) {
    // Check if the key is watched.
    if (auto wit = watched_keys.find(key); wit != watched_keys.end()) {
      for (auto conn_ptr : wit->second) {
        conn_ptr->watched_dirty.store(true, memory_order_relaxed);
      }
      // No connections need to watch it anymore.
      watched_keys.erase(wit);
    }
  }

  ++events_.update;

  if (db.slots_stats) {
    db.slots_stats[KeySlot(key)].total_writes += 1;
  }

  if (!client_tracking_map_.empty()) {
    QueueInvalidationTrackingMessageAtomic(key);
  }
}

DbSlice::ItAndExp DbSlice::ExpireIfNeeded(const Context& cntx, Iterator it) const {
  auto res = ExpireIfNeeded(cntx, it.GetInnerIt());
  return {.it = Iterator::FromPrime(res.it), .exp_it = ExpIterator::FromPrime(res.exp_it)};
}

DbSlice::PrimeItAndExp DbSlice::ExpireIfNeeded(const Context& cntx, PrimeIterator it) const {
  if (!it->second.HasExpire()) {
    LOG(DFATAL) << "Invalid call to ExpireIfNeeded";
    return {it, ExpireIterator{}};
  }

  auto& db = db_arr_[cntx.db_index];
  auto expire_it = db->expire.Find(it->first);

  if (!IsValid(expire_it)) {
    LOG(DFATAL) << "Internal error, entry " << it->first.ToString()
                << " not found in expire table, db_index: " << cntx.db_index
                << ", expire table size: " << db->expire.size()
                << ", prime table size: " << db->prime.size() << util::fb2::GetStacktrace();
    return {it, ExpireIterator{}};
  }

  // TODO: to employ multi-generation update of expire-base and the underlying values.
  time_t expire_time = ExpireTime(expire_it);

  // Never do expiration on replica or if expiration is disabled or global lock was taken.
  if (time_t(cntx.time_now_ms) < expire_time || owner_->IsReplica() || !expire_allowed_ ||
      !shard_owner()->shard_lock()->Check(IntentLock::Mode::EXCLUSIVE)) {
    return {it, expire_it};
  }

  string scratch;
  string_view key = it->first.GetSlice(&scratch);

  // Replicate expiry
  if (auto journal = owner_->journal(); journal) {
    RecordExpiryBlocking(cntx.db_index, key);
  }

  if (expired_keys_events_recording_)
    db->expired_keys_events_.emplace_back(key);

  auto obj_type = it->second.ObjType();
  if (doc_del_cb_ && (obj_type == OBJ_JSON || obj_type == OBJ_HASH)) {
    doc_del_cb_(key, cntx, it->second);
  }

  const_cast<DbSlice*>(this)->PerformDeletionAtomic(
      Iterator(it, StringOrView::FromView(key)),
      ExpIterator(expire_it, StringOrView::FromView(key)), db.get());

  ++events_.expired_keys;

  return {PrimeIterator{}, ExpireIterator{}};
}

void DbSlice::ExpireAllIfNeeded() {
  // We hold no locks to any of the keys so we should Wait() here such that
  // we don't preempt in ExpireIfNeeded
  serialization_latch_.Wait();
  // Disable flush journal changes to prevent preemtion in traverse.
  journal::JournalFlushGuard journal_flush_guard(owner_->journal());

  for (DbIndex db_index = 0; db_index < db_arr_.size(); db_index++) {
    if (!db_arr_[db_index])
      continue;
    auto& db = *db_arr_[db_index];

    auto cb = [&](ExpireTable::iterator exp_it) {
      auto prime_it = db.prime.Find(exp_it->first);
      if (!IsValid(prime_it)) {
        LOG(DFATAL) << "Expire entry " << exp_it->first.ToString() << " not found in prime table";
        return;
      }
      ExpireIfNeeded(Context{nullptr, db_index, GetCurrentTimeMs()}, prime_it);
    };

    ExpireTable::Cursor cursor;
    do {
      cursor = db.expire.Traverse(cursor, cb);
    } while (cursor);
  }
}

uint64_t DbSlice::RegisterOnChange(ChangeCallback cb) {
  return change_cb_.emplace_back(NextVersion(), std::move(cb)).first;
}

uint64_t DbSlice::RegisterOnMove(MovedCallback cb) {
  ++next_moved_id_;
  moved_cb_.emplace_back(next_moved_id_, cb);
  return next_moved_id_;
}

void DbSlice::FlushChangeToEarlierCallbacks(DbIndex db_ind, Iterator it, uint64_t upper_bound) {
  unique_lock<LocalLatch> lk(serialization_latch_);

  uint64_t bucket_version = it.GetVersion();
  // change_cb_ is ordered by version.
  DVLOG(2) << "Running callbacks in dbid " << db_ind << " with bucket_version=" << bucket_version
           << ", upper_bound=" << upper_bound;

  const size_t limit = change_cb_.size();
  auto ccb = change_cb_.begin();
  for (size_t i = 0; i < limit; ++i) {
    uint64_t cb_version = ccb->first;
    DCHECK_LE(cb_version, upper_bound);
    if (cb_version == upper_bound) {
      return;
    }
    if (bucket_version < cb_version) {
      ccb->second(db_ind, ChangeReq{it.GetInnerIt()});
    }
    ++ccb;
  }
}

//! Unregisters the callback.
void DbSlice::UnregisterOnChange(uint64_t id) {
  serialization_latch_.Wait();
  auto it = find_if(change_cb_.begin(), change_cb_.end(),
                    [id](const auto& cb) { return cb.first == id; });
  CHECK(it != change_cb_.end());
  change_cb_.erase(it);
}

void DbSlice::UnregisterOnMoved(uint64_t id) {
  serialization_latch_.Wait();
  auto it =
      find_if(moved_cb_.begin(), moved_cb_.end(), [id](const auto& cb) { return cb.first == id; });
  CHECK(it != moved_cb_.end());
  moved_cb_.erase(it);
}

auto DbSlice::DeleteExpiredStep(const Context& cntx, unsigned count) -> DeleteExpiredStats {
  auto& db = *db_arr_[cntx.db_index];
  DeleteExpiredStats result;

  std::string stash;

  auto cb = [&](ExpireIterator it) {
    string_view key = it->first.GetSlice(&stash);
    if (!CheckLock(IntentLock::EXCLUSIVE, cntx.db_index, key))
      return;

    result.traversed++;
    time_t ttl = ExpireTime(it) - cntx.time_now_ms;
    if (ttl <= 0) {
      auto prime_it = db.prime.Find(it->first);
      if (prime_it.is_done()) {  // A workaround for the case our tables are inconsistent.
        LOG(DFATAL) << "Expired key " << key
                    << " not found in prime table, expire_done: " << it.is_done();
        if (!it.is_done()) {
          db.expire.Erase(it->first);
        }
      } else {
        result.deleted_bytes += prime_it->first.MallocUsed() + prime_it->second.MallocUsed();
        ExpireIfNeeded(cntx, prime_it);
      }
      ++result.deleted;
    } else {
      result.survivor_ttl_sum += ttl;
    }
  };

  unsigned i = 0;
  for (; i < count / 3; ++i) {
    db.expire_cursor = db.expire.Traverse(db.expire_cursor, cb);
  }

  // continue traversing only if we had strong deletion rate based on the first sample.
  if (result.deleted * 4 > result.traversed) {
    for (; i < count; ++i) {
      db.expire_cursor = db.expire.Traverse(db.expire_cursor, cb);
    }
  }

  // Send and clear accumulated expired key events
  if (auto& events = db_arr_[cntx.db_index]->expired_keys_events_; !events.empty()) {
    ChannelStore* store = ServerState::tlocal()->channel_store();
    store->SendMessages(absl::StrCat("__keyevent@", cntx.db_index, "__:expired"), events);
    events.clear();
  }

  return result;
}

int32_t DbSlice::GetNextSegmentForEviction(int32_t segment_id, DbIndex db_ind) const {
  // wraps around if we reached the end
  return db_arr_[db_ind]->prime.NextSeg((size_t)segment_id) %
         db_arr_[db_ind]->prime.GetSegmentCount();
}

pair<uint64_t, size_t> DbSlice::FreeMemWithEvictionStepAtomic(DbIndex db_ind,
                                                              size_t starting_segment_id,
                                                              size_t increase_goal_bytes) {
  // Disable flush journal changes to prevent preemtion
  journal::JournalFlushGuard journal_flush_guard(shard_owner()->journal());
  FiberAtomicGuard guard;
  DCHECK(!owner_->IsReplica());

  size_t evicted_items = 0, evicted_bytes = 0;

  if (owner_->tiered_storage()) {
    evicted_bytes = owner_->tiered_storage()->ReclaimMemory(increase_goal_bytes);
    if (evicted_bytes >= increase_goal_bytes)
      return {0, evicted_bytes};
  }

  if ((!IsCacheMode()) || !expire_allowed_)
    return {0, 0};

  auto max_eviction_per_hb = GetFlag(FLAGS_max_eviction_per_heartbeat);
  auto max_segment_to_consider = GetFlag(FLAGS_max_segment_to_consider);

  auto time_start = absl::GetCurrentTimeNanos();
  auto& db_table = db_arr_[db_ind];
  constexpr int32_t num_slots = PrimeTable::Segment_t::kSlotNum;

  string tmp;

  bool record_keys = owner_->journal() != nullptr || expired_keys_events_recording_;
  vector<string> keys_to_journal;

  for (int32_t slot_id = num_slots - 1; slot_id >= 0; --slot_id) {
    for (int32_t bucket_id = PrimeTable::LargestBucketId(); bucket_id >= 0; --bucket_id) {
      // pick a random segment to start with in each eviction,
      // as segment_id does not imply any recency, and random selection should be fair enough
      int32_t segment_id = starting_segment_id;
      for (size_t num_seg_visited = 0; num_seg_visited < max_segment_to_consider;
           ++num_seg_visited, segment_id = GetNextSegmentForEviction(segment_id, db_ind)) {
        const auto& segment = db_table->prime.GetSegment(segment_id);
        if (unsigned(bucket_id) >= segment->num_buckets())
          bucket_id = segment->num_buckets() - 1;
        const auto& bucket = segment->GetBucket(bucket_id);
        if (bucket.IsEmpty() || !bucket.IsBusy(slot_id))
          continue;

        auto evict_it = db_table->prime.GetIterator(segment_id, bucket_id, slot_id);
        // TODO: consider evicting inline entries as well

        bool has_allocated = evict_it->second.HasAllocated() || evict_it->first.HasAllocated();
        if (evict_it->first.IsSticky() || !has_allocated)
          continue;

        // check if the key is locked by looking up transaction table.
        const auto& lt = db_table->trans_locks;
        string_view key = evict_it->first.GetSlice(&tmp);
        if (lt.Find(LockTag(key)).has_value())
          continue;

        if (record_keys)
          keys_to_journal.emplace_back(key);

        evicted_bytes += evict_it->first.MallocUsed() + evict_it->second.MallocUsed();
        ++evicted_items;
        PerformDeletion(Iterator(evict_it, StringOrView::FromView(key)), db_table.get());

        // returns when whichever condition is met first
        if ((evicted_items == max_eviction_per_hb) || (evicted_bytes >= increase_goal_bytes))
          goto finish;
      }
    }
  }

finish:
  // send the deletion to the replicas.
  for (string_view key : keys_to_journal) {
    if (auto journal = owner_->journal(); journal)
      // Won't block because we disabled journal flushing. See first line of this function.
      RecordExpiryBlocking(db_ind, key);

    if (expired_keys_events_recording_)
      db_table->expired_keys_events_.emplace_back(key);
  }

  // This might not always be atomic on exceptional cases -- see comments on the function
  // declaration.
  SendQueuedInvalidationMessagesAsync();
  auto time_finish = absl::GetCurrentTimeNanos();
  events_.evicted_keys += evicted_items;
  DVLOG(2) << "Eviction time (us): " << (time_finish - time_start) / 1000;
  return pair<uint64_t, size_t>{evicted_items, evicted_bytes};
}

void DbSlice::CreateDb(DbIndex db_ind) {
  auto& db = db_arr_[db_ind];
  if (!db) {
    db.reset(new DbTable{owner_->memory_resource(), db_ind});
    table_memory_ += db->table_memory();
  }
}

void DbSlice::RegisterWatchedKey(DbIndex db_indx, std::string_view key,
                                 ConnectionState::ExecInfo* exec_info) {
  // Because we might insert while another fiber is preempted
  db_arr_[db_indx]->watched_keys[key].push_back(exec_info);
}

void DbSlice::UnregisterConnectionWatches(const ConnectionState::ExecInfo* exec_info) {
  for (const auto& [db_indx, key] : exec_info->watched_keys) {
    auto& watched_keys = db_arr_[db_indx]->watched_keys;
    if (auto it = watched_keys.find(key); it != watched_keys.end()) {
      it->second.erase(std::remove(it->second.begin(), it->second.end(), exec_info),
                       it->second.end());
      if (it->second.empty())
        watched_keys.erase(it);
    }
  }
}

void DbSlice::InvalidateDbWatches(DbIndex db_indx) {
  for (const auto& [key, conn_list] : db_arr_[db_indx]->watched_keys) {
    for (auto conn_ptr : conn_list) {
      conn_ptr->watched_dirty.store(true, memory_order_relaxed);
    }
  }
}

void DbSlice::InvalidateSlotWatches(const cluster::SlotSet& slot_ids) {
  for (const auto& [key, conn_list] : db_arr_[0]->watched_keys) {
    SlotId sid = KeySlot(key);
    if (!slot_ids.Contains(sid)) {
      continue;
    }
    for (auto conn_ptr : conn_list) {
      conn_ptr->watched_dirty.store(true, memory_order_relaxed);
    }
  }
}

void DbSlice::ClearOffloadedEntries(absl::Span<const DbIndex> indices, const DbTableArray& db_arr) {
  // Currently being used only for tiered storage.
  TieredStorage* tiered_storage = shard_owner()->tiered_storage();
  string scratch;
  for (DbIndex index : indices) {
    const auto& db_ptr = db_arr[index];
    if (!db_ptr)
      continue;

    // Delete all tiered entries
    PrimeTable::Cursor cursor;
    do {
      cursor = db_ptr->prime.Traverse(cursor, [&](PrimeIterator it) {
        if (it->second.IsExternal()) {
          tiered_storage->Delete(index, &it->second);
        } else if (it->second.HasStashPending()) {
          tiered_storage->CancelStash(index, it->first.GetSlice(&scratch), &it->second);
        }
      });
    } while (cursor);

    // While tiered_storage may delete some of its entries asynchronously, it updates
    // stats.tiered_entries immediately during the Delete call, therefore tiered_entries
    // should be zero by this point.
    CHECK_EQ(db_ptr->stats.tiered_entries, 0u);
  }
}

void DbSlice::SetDocDeletionCallback(DocDeletionCallback ddcb) {
  doc_del_cb_ = std::move(ddcb);
}

void DbSlice::ResetUpdateEvents() {
  events_.update = 0;
}

void DbSlice::ResetEvents() {
  events_ = {};
}

void DbSlice::SetNotifyKeyspaceEvents(std::string_view notify_keyspace_events) {
  expired_keys_events_recording_ = !notify_keyspace_events.empty();
}

void DbSlice::QueueInvalidationTrackingMessageAtomic(std::string_view key) {
  FiberAtomicGuard guard;
  auto it = client_tracking_map_.find(key);
  if (it == client_tracking_map_.end()) {
    return;
  }

  ConnectionHashSet moved_set = std::move(it->second);
  client_tracking_map_.erase(it);

  auto [pend_it, inserted] = pending_send_map_.emplace(key, std::move(moved_set));
  if (!inserted) {
    ConnectionHashSet& client_set = pend_it->second;
    for (auto& weak_ref : moved_set) {
      client_set.insert(weak_ref);
    }
  }
}

void DbSlice::SendQueuedInvalidationMessagesCb(const TrackingMap& track_map,
                                               unsigned calling_thread_id) const {
  for (auto& [key, client_list] : track_map) {
    for (auto& weak_ref : client_list) {
      if (weak_ref.IsExpired() || (weak_ref.LastKnownThreadId() != calling_thread_id)) {
        continue;  // Expired or migrated.
      }
      auto* conn = weak_ref.Get();
      auto* cntx = static_cast<ConnectionContext*>(conn->cntx());
      if (cntx && cntx->conn_state.tracking_info_.IsTrackingOn()) {
        conn->SendInvalidationMessageAsync({key});
      }
    }
  }
}

void DbSlice::SendQueuedInvalidationMessages() {
  // We run while loop because when we block below, we might have new items added to
  // pending_send_map_.
  while (!pending_send_map_.empty()) {
    // Notify all the clients. this function is not efficient,
    // because it broadcasts to all threads unrelated to the subscribers for the key.
    auto local_map = std::move(pending_send_map_);
    pending_send_map_ = {};
    auto cb = [&](unsigned thread_id, util::ProactorBase*) {
      SendQueuedInvalidationMessagesCb(local_map, thread_id);
    };

    shard_set->pool()->AwaitBrief(std::move(cb));
  }
}

// This function might preempt if the task queue within DispatchBrief is full and we can't
// enqueue the callback. Although a rare case, this code might not be atomic.
void DbSlice::SendQueuedInvalidationMessagesAsync() {
  if (pending_send_map_.empty()) {
    return;
  }
  // DispatchBrief will copy local_map
  auto cb = [lm = std::move(pending_send_map_), this](unsigned idx, util::ProactorBase*) {
    SendQueuedInvalidationMessagesCb(lm, idx);
  };

  shard_set->pool()->DispatchBrief(std::move(cb));
}

void DbSlice::StartSampleTopK(DbIndex db_ind, uint32_t min_freq) {
  auto& db = *db_arr_[db_ind];
  if (db.top_keys) {
    LOG(INFO) << "Sampling already started for db " << db_ind;
    return;
  }

  TopKeys::Options opts;
  opts.min_key_count_to_record = min_freq;
  db.top_keys = new TopKeys(opts);
}

auto DbSlice::StopSampleTopK(DbIndex db_ind) -> SamplingResult {
  auto& db = *db_arr_[db_ind];

  if (!db.top_keys) {
    LOG(WARNING) << "Sampling not started for db " << db_ind;
    return {};
  }
  auto fmap = db.top_keys->GetTopKeys();
  delete db.top_keys;
  db.top_keys = nullptr;
  SamplingResult result;
  result.top_keys.reserve(fmap.size());
  for (auto& [key, count] : fmap) {
    result.top_keys.emplace_back(std::move(key), count);
  }
  return result;
}

void DbSlice::StartSampleKeys(DbIndex db_ind) {
  auto& db = *db_arr_[db_ind];
  if (db.dense_hll) {
    LOG(INFO) << "Sampling already started for db " << db_ind;
    return;
  }

  HllBufferPtr hll_buf;
  hll_buf.size = getDenseHllSize();
  db.dense_hll = new uint8_t[hll_buf.size];
  hll_buf.hll = db.dense_hll;
  CHECK_EQ(0, createDenseHll(hll_buf));
}

// Returns number of unique keys sampled.
size_t DbSlice::StopSampleKeys(DbIndex db_ind) {
  auto& db = *db_arr_[db_ind];
  if (!db.dense_hll) {
    LOG(INFO) << "Keys sampling not started for db " << db_ind;
    return 0;
  }
  HllBufferPtr hll_buf;
  hll_buf.hll = db.dense_hll;
  hll_buf.size = getDenseHllSize();
  int64_t count = pfcountSingle(hll_buf);

  delete[] db.dense_hll;
  db.dense_hll = nullptr;

  return count;
}

void DbSlice::PerformDeletionAtomic(Iterator del_it, ExpIterator exp_it, DbTable* table) {
  FiberAtomicGuard guard;
  size_t table_before = table->table_memory();
  if (!exp_it.is_done()) {
    table->expire.Erase(exp_it.GetInnerIt());
  }

  if (del_it->second.HasFlag()) {
    if (table->mcflag.Erase(del_it->first) == 0) {
      LOG(DFATAL) << "Internal error, inconsistent state, mcflag should be present but not found "
                  << del_it->first.ToString();
    }
  }

  DbTableStats& stats = table->stats;
  PrimeValue& pv = del_it->second;

  if (pv.HasStashPending()) {
    string scratch;
    string_view key = del_it->first.GetSlice(&scratch);
    shard_owner()->tiered_storage()->CancelStash(table->index, key, &pv);
  } else if (pv.IsExternal()) {
    shard_owner()->tiered_storage()->Delete(table->index, &del_it->second);
  }

  ssize_t value_heap_size = pv.MallocUsed(), key_size_used = del_it->first.MallocUsed();
  if (del_it->first.IsInline()) {
    --stats.inline_keys;
  } else {
    AccountObjectMemory(del_it.key(), del_it->first.ObjType(), -key_size_used,
                        table);  // Key
  }
  AccountObjectMemory(del_it.key(), pv.ObjType(), -value_heap_size, table);  // Value

  if (del_it->first.IsAsyncDelete() && pv.ObjType() == OBJ_SET &&
      pv.Encoding() == kEncodingStrMap2) {
    DenseSet* ds = (DenseSet*)pv.RObjPtr();
    pv.SetRObjPtr(nullptr);
    const size_t kClearStepSize = 512;

    uint32_t next = ds->ClearStep(0, kClearStepSize);
    if (next < ds->BucketCount()) {
      AsyncDeleter::EnqueDeletion(next, ds);
    } else {
      CompactObj::DeleteMR<DenseSet>(ds);
    }
  }  // del_it->first.IsAsyncDelete()

  if (table->slots_stats) {
    SlotId sid = KeySlot(del_it.key());
    table->slots_stats[sid].key_count -= 1;
  }

  table->prime.Erase(del_it.GetInnerIt());

  // Note, currently we do not shrink our tables upon deletion.
  // This DCHECK ensures that if we decide to do so, we will have to update table_memory_
  // accordingly.
  DCHECK_EQ(table->table_memory(), table_before);

  --entries_count_;
  memory_budget_ += (value_heap_size + key_size_used);

  if (!client_tracking_map_.empty()) {
    QueueInvalidationTrackingMessageAtomic(del_it.key());
  }
}

void DbSlice::PerformDeletion(Iterator del_it, DbTable* table) {
  ExpIterator exp_it;
  if (del_it->second.HasExpire()) {
    exp_it = ExpIterator::FromPrime(table->expire.Find(del_it->first));
    DCHECK(!exp_it.is_done());
  }

  PerformDeletionAtomic(del_it, exp_it, table);
}

void DbSlice::OnCbFinishBlocking() {
  if (IsCacheMode()) {
    // move fetched items to local variable
    auto fetched_items = std::move(fetched_items_);
    fetched_items_ = {};
    for (const auto& [key_hash, db_index] : fetched_items) {
      auto& db = *db_arr_[db_index];

      // We intentionally don't do extra key checking on this callback to speedup
      // fetching. Probability of having hash collision is quite low and for bumpup
      // purposes it should be fine if different key (with same hash) is returned.
      auto predicate = [](const PrimeKey&) { return true; };

      PrimeIterator it = db.prime.FindFirst(key_hash, predicate);

      if (!IsValid(it)) {
        continue;
      }

      if (!change_cb_.empty()) {
        auto bump_cb = [&](PrimeTable::bucket_iterator bit) {
          CallChangeCallbacks(db_index, ChangeReq{bit});
        };
        db.prime.CVCUponBump(change_cb_.back().first, it, bump_cb);
      }

      // We must not change the bucket's internal order during serialization
      serialization_latch_.Wait();
      PrimeBumpPolicy policy;
      auto bump_it = db.prime.BumpUp(it, policy);
      if (bump_it != it) {  // the item was bumped
        ++events_.bumpups;
      }
      CallMovedCallbacks(db_index, policy.moved_items());
    }
  }

  // Sends only if !pending_send_map_.empty()
  SendQueuedInvalidationMessages();
}

void DbSlice::CallChangeCallbacks(DbIndex id, const ChangeReq& cr) const {
  if (change_cb_.empty())
    return;

  // does not preempt, just increments the counter.
  unique_lock<LocalLatch> lk(serialization_latch_);

  const size_t limit = change_cb_.size();
  auto ccb = change_cb_.begin();
  for (size_t i = 0; i < limit; ++i) {
    CHECK(ccb->second);
    ccb->second(id, cr);
    ++ccb;
  }
}

void DbSlice::CallMovedCallbacks(
    DbIndex id, const std::vector<std::pair<PrimeTable::Cursor, PrimeTable::Cursor>>& moved_items) {
  if (moved_cb_.empty())
    return;

  // does not preempt, just increments the counter.
  unique_lock<LocalLatch> lk(serialization_latch_);

  const size_t limit = moved_cb_.size();
  auto ccb = moved_cb_.begin();
  for (size_t i = 0; i < limit; ++i) {
    CHECK(ccb->second);
    ccb->second(id, moved_items);
    ++ccb;
  }
}

}  // namespace dfly
