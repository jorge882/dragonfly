// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <absl/functional/function_ref.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/bptree_set.h"
#include "core/score_map.h"

extern "C" {

/* Struct to hold an inclusive/exclusive range spec by score comparison. */
typedef struct {
  double min, max;
  int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
  sds min, max;     /* May be set to shared.(minstring|maxstring) */
  int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

}  // extern "C"

/* Input flags. */
#define ZADD_IN_NONE 0
#define ZADD_IN_INCR (1 << 0) /* Increment the score instead of setting it. */
#define ZADD_IN_NX (1 << 1)   /* Don't touch elements already existing. */
#define ZADD_IN_XX (1 << 2)   /* Only touch elements already existing. */
#define ZADD_IN_GT (1 << 3)   /* Only update existing when new scores are higher. */
#define ZADD_IN_LT (1 << 4)   /* Only update existing when new scores are lower. */

/* Output flags. */
#define ZADD_OUT_NOP (1 << 0)     /* Operation not performed because of conditionals.*/
#define ZADD_OUT_NAN (1 << 1)     /* Only touch elements already existing. */
#define ZADD_OUT_ADDED (1 << 2)   /* The element was new and was added. */
#define ZADD_OUT_UPDATED (1 << 3) /* The element already existed, score updated. */

namespace dfly {

class PageUsage;

// Copied from zset.h
extern sds cmaxstring;
extern sds cminstring;

namespace detail {

/**
 * @brief SortedMap is a sorted map implementation based on zset.h. It holds unique strings that
 * are ordered by score and lexicographically. The score is a double value and has higher priority.
 * The map is implemented as a skip list and a hash table. For more details see
 * zset.h and t_zset.c files in Redis.
 */
class SortedMap {
 public:
  using ScoredMember = std::pair<std::string, double>;
  using ScoredArray = std::vector<ScoredMember>;
  using ScoreSds = void*;
  using RankAndScore = std::pair<unsigned, double>;

  SortedMap(PMR_NS::memory_resource* res);
  ~SortedMap();

  SortedMap(const SortedMap&) = delete;
  SortedMap& operator=(const SortedMap&) = delete;

  bool Reserve(size_t sz);
  int AddElem(double score, std::string_view ele, int in_flags, int* out_flags, double* newscore);

  // Inserts a new element. Returns false if the element already exists.
  // No score update is performed in this case.
  bool InsertNew(double score, std::string_view member);

  bool Delete(std::string_view ele) const;

  // Upper bound size of the set.
  // Note: Currently we do not allow member expiry in sorted sets, therefore it's exact
  // But if we decide to add expire, this method will provide an approximation from above.
  size_t Size() const {
    return score_map->UpperBoundSize();
  }

  size_t MallocSize() const;

  size_t DeleteRangeByRank(unsigned start, unsigned end);
  size_t DeleteRangeByScore(const zrangespec& range);
  size_t DeleteRangeByLex(const zlexrangespec& range);

  ScoredArray PopTopScores(unsigned count, bool reverse);

  std::optional<double> GetScore(std::string_view ele) const;
  std::optional<unsigned> GetRank(std::string_view ele, bool reverse) const;
  std::optional<RankAndScore> GetRankAndScore(std::string_view ele, bool reverse) const;
  ScoredArray GetRange(const zrangespec& r, unsigned offs, unsigned len, bool rev) const;
  ScoredArray GetLexRange(const zlexrangespec& r, unsigned o, unsigned l, bool rev) const;

  size_t Count(const zrangespec& range) const;
  size_t LexCount(const zlexrangespec& range) const;

  // Runs cb for each element in the range [start_rank, start_rank + len).
  // Stops iteration if cb returns false. Returns false in this case.
  bool Iterate(unsigned start_rank, unsigned len, bool reverse,
               std::function<bool(sds, double)> cb) const;

  uint64_t Scan(uint64_t cursor, absl::FunctionRef<void(std::string_view, double)> cb) const;

  uint8_t* ToListPack() const;
  static SortedMap* FromListPack(PMR_NS::memory_resource* res, const uint8_t* lp);

  bool DefragIfNeeded(PageUsage* page_usage);

 private:
  struct Query {
    ScoreSds item;
    bool ignore_score;
    bool str_is_infinite;

    Query(ScoreSds key, bool ign_score = false, int is_inf = 0)
        : item(key), ignore_score(ign_score), str_is_infinite(is_inf != 0) {
    }
  };

  struct ScoreSdsPolicy {
    using KeyT = ScoreSds;

    struct KeyCompareTo {
      int operator()(Query q, ScoreSds key) const;
    };
  };

  using ScoreTree = BPTree<ScoreSds, ScoreSdsPolicy>;

  // hash map from fields to scores.
  ScoreMap* score_map = nullptr;

  // sorted tree of (score,field) items.
  ScoreTree* score_tree = nullptr;
};

// Used by CompactObject.
unsigned char* ZzlInsert(unsigned char* zl, std::string_view ele, double score);
unsigned char* ZzlFind(unsigned char* lp, std::string_view ele, double* score);

// Used by SortedMap and ZsetFamily.
double ZzlGetScore(const uint8_t* sptr);
void ZzlNext(const uint8_t* zl, uint8_t** eptr, uint8_t** sptr);
void ZzlPrev(const uint8_t* zl, uint8_t** eptr, uint8_t** sptr);
void ZslFreeLexRange(const zlexrangespec* spec);
uint8_t* ZzlLastInRange(uint8_t* zl, const zrangespec* range);
uint8_t* ZzlFirstInRange(uint8_t* zl, const zrangespec* range);

uint8_t* ZzlFirstInLexRange(uint8_t* zl, const zlexrangespec* range);
uint8_t* ZzlLastInLexRange(uint8_t* zl, const zlexrangespec* range);

int ZzlLexValueGteMin(uint8_t* p, const zlexrangespec* spec);
int ZzlLexValueLteMax(uint8_t* p, const zlexrangespec* spec);

uint8_t* ZzlDeleteRangeByLex(uint8_t* zl, const zlexrangespec* range, unsigned long* deleted);
uint8_t* ZzlDeleteRangeByScore(uint8_t* zl, const zrangespec* range, unsigned long* deleted);

inline int ZslValueGteMin(double value, const zrangespec* spec) {
  return spec->minex ? (value > spec->min) : (value >= spec->min);
}

inline int ZslValueLteMax(double value, const zrangespec* spec) {
  return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

}  // namespace detail
}  // namespace dfly
