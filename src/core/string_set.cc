// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "core/string_set.h"

#include "absl/flags/flag.h"
#include "core/compact_object.h"
#include "core/page_usage_stats.h"
#include "core/sds_utils.h"

extern "C" {
#include "redis/sds.h"
#include "redis/zmalloc.h"
}

#include "base/logging.h"

ABSL_FLAG(bool, legacy_saddex_keepttl, false,
          "If true SADDEX does not update TTL for existing fields");

using namespace std;

namespace dfly {

namespace {

inline bool MayHaveTtl(sds s) {
  char* alloc_ptr = (char*)sdsAllocPtr(s);
  return sdslen(s) + 1 + 4 <= zmalloc_usable_size(alloc_ptr);
}

sds AllocImmutableWithTtl(uint32_t len, uint32_t at) {
  sds res = AllocSdsWithSpace(len, sizeof(at));
  absl::little_endian::Store32(res + len + 1, at);  // Save TTL

  return res;
}

}  // namespace

StringSet::~StringSet() {
  Clear();
}

bool StringSet::Add(string_view src, uint32_t ttl_sec) {
  uint64_t hash = Hash(&src, 1);
  void* prev = FindInternal(&src, hash, 1);
  if (prev != nullptr) {
    return false;
  }

  sds newsds = MakeSetSds(src, ttl_sec);
  bool has_ttl = ttl_sec != UINT32_MAX;
  AddUnique(newsds, has_ttl, hash);
  return true;
}

unsigned StringSet::AddMany(absl::Span<std::string_view> span, uint32_t ttl_sec, bool keepttl) {
  std::string_view views[kMaxBatchLen];
  unsigned res = 0;
  if (BucketCount() < span.size()) {
    Reserve(span.size());
  }

  while (span.size() >= kMaxBatchLen) {
    for (size_t i = 0; i < kMaxBatchLen; i++)
      views[i] = span[i];

    span.remove_prefix(kMaxBatchLen);
    res += AddBatch(absl::MakeSpan(views), ttl_sec, keepttl);
  }

  if (span.size()) {
    for (size_t i = 0; i < span.size(); i++)
      views[i] = span[i];

    res += AddBatch(absl::MakeSpan(views, span.size()), ttl_sec, keepttl);
  }
  return res;
}

unsigned StringSet::AddBatch(absl::Span<std::string_view> span, uint32_t ttl_sec, bool keepttl) {
  uint64_t hash[kMaxBatchLen];
  bool has_ttl = ttl_sec != UINT32_MAX;
  unsigned count = span.size();
  unsigned res = 0;

  DCHECK_LE(count, kMaxBatchLen);

  for (size_t i = 0; i < count; i++) {
    hash[i] = CompactObj::HashCode(span[i]);
    Prefetch(hash[i]);
  }

  // update ttl if legacy_saddex_keepttl is off (which is default). This variable is intended for
  // SADDEX, but this method is called from SADD as well, where ttl is set to UINT32_MAX value,
  // which results in has_ttl being false. This means that ObjUpdateExpireTime is never called from
  // SADD code path even when update_ttl is true.
  const thread_local bool update_ttl = !absl::GetFlag(FLAGS_legacy_saddex_keepttl);

  for (unsigned i = 0; i < count; ++i) {
    void* prev = FindInternal(&span[i], hash[i], 1);
    if (prev == nullptr) {
      ++res;
      sds field = MakeSetSds(span[i], ttl_sec);
      AddUnique(field, has_ttl, hash[i]);
    } else if (update_ttl && has_ttl && !keepttl) {
      ObjUpdateExpireTime(prev, ttl_sec);
    }
  }

  return res;
}

StringSet::iterator StringSet::GetRandomMember() {
  return iterator{DenseSet::GetRandomIterator()};
}

std::optional<std::string> StringSet::Pop() {
  sds str = (sds)PopInternal();

  if (str == nullptr) {
    return std::nullopt;
  }

  std::string ret{str, sdslen(str)};
  sdsfree(str);

  return ret;
}

uint32_t StringSet::Scan(uint32_t cursor, const std::function<void(const sds)>& func) const {
  return DenseSet::Scan(cursor, [func](const void* ptr) { func((sds)ptr); });
}

uint64_t StringSet::Hash(const void* ptr, uint32_t cookie) const {
  DCHECK_LT(cookie, 2u);

  if (cookie == 0) {
    sds s = (sds)ptr;
    return CompactObj::HashCode(string_view{s, sdslen(s)});
  }

  const string_view* sv = (const string_view*)ptr;
  return CompactObj::HashCode(*sv);
}

bool StringSet::ObjEqual(const void* left, const void* right, uint32_t right_cookie) const {
  DCHECK_LT(right_cookie, 2u);

  sds s1 = (sds)left;

  if (right_cookie == 0) {
    sds s2 = (sds)right;

    if (sdslen(s1) != sdslen(s2)) {
      return false;
    }

    return sdslen(s1) == 0 || memcmp(s1, s2, sdslen(s1)) == 0;
  }

  const string_view* right_sv = (const string_view*)right;
  string_view left_sv{s1, sdslen(s1)};
  return left_sv == (*right_sv);
}

size_t StringSet::ObjectAllocSize(const void* s1) const {
  return zmalloc_usable_size(sdsAllocPtr((sds)s1));
}

uint32_t StringSet::ObjExpireTime(const void* str) const {
  sds s = (sds)str;
  DCHECK(MayHaveTtl(s));

  char* ttlptr = s + sdslen(s) + 1;
  return absl::little_endian::Load32(ttlptr);
}

void StringSet::ObjUpdateExpireTime(const void* obj, uint32_t ttl_sec) {
  return SdsUpdateExpireTime(obj, time_now() + ttl_sec, 0);
}

void StringSet::ObjDelete(void* obj, bool has_ttl) const {
  sdsfree((sds)obj);
}

void* StringSet::ObjectClone(const void* obj, bool has_ttl, bool add_ttl) const {
  sds src = (sds)obj;
  string_view sv{src, sdslen(src)};
  uint32_t ttl_sec = add_ttl ? 0 : (has_ttl ? ObjExpireTime(obj) : UINT32_MAX);
  return (void*)MakeSetSds(sv, ttl_sec);
}

sds StringSet::MakeSetSds(string_view src, uint32_t ttl_sec) const {
  if (ttl_sec != UINT32_MAX) {
    uint32_t at = time_now() + ttl_sec;

    sds newsds = AllocImmutableWithTtl(src.size(), at);
    if (!src.empty())
      memcpy(newsds, src.data(), src.size());
    return newsds;
  }

  return sdsnewlen(src.data(), src.size());
}

// Does not release obj. Callers must deallocate with sdsfree explicitly
pair<sds, bool> StringSet::DuplicateEntryIfFragmented(void* obj, PageUsage* page_usage) {
  sds key = (sds)obj;

  if (!page_usage->IsPageForObjectUnderUtilized(key))
    return {key, false};

  size_t key_len = sdslen(key);
  bool has_ttl = MayHaveTtl(key);

  if (has_ttl) {
    sds res = AllocSdsWithSpace(key_len, sizeof(uint32_t));
    std::memcpy(res, key, key_len + sizeof(uint32_t));
    return {res, true};
  }

  return {sdsnewlen(key, key_len), true};
}

bool StringSet::iterator::ReallocIfNeeded(PageUsage* page_usage) {
  auto* ptr = curr_entry_;
  if (ptr->IsLink()) {
    ptr = ptr->AsLink();
  }

  DCHECK(!ptr->IsEmpty());
  DCHECK(ptr->IsObject());

  auto* obj = ptr->GetObject();
  auto [new_obj, realloced] =
      static_cast<StringSet*>(owner_)->DuplicateEntryIfFragmented(obj, page_usage);

  if (realloced) {
    ptr->SetObject(new_obj);
    sdsfree((sds)obj);
  }

  return realloced;
}

}  // namespace dfly
