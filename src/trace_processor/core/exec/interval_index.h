/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INDEX_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INDEX_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// A set of intervals, each in a lane named by a key, arranged for answering
// runs of probes whose starts never decrease within a lane.
//
// An interval is [start, end), or the point `start` when the two are equal.
// Within a lane the intervals are held sorted by start, then by the order
// they were added in, in one flat array shared by every lane.
//
// The property which picks how a lane is searched is computed once when the
// index is built: whether the lane is disjoint, in which case no interval
// starts before the one ahead of it ends, and the intervals containing a time
// are a run which needs no tree to find.
class IntervalIndex {
 private:
  // One lane's range of the flat arrays, and what is known about it.
  struct Lane {
    uint32_t begin;
    uint32_t end;
    bool disjoint;
  };

 public:
  // Adds an interval to the lane of `key`, which is `key_size` values. All
  // intervals are added before Build(); `row` is handed back on a match.
  void Add(const int64_t* key,
           size_t key_size,
           int64_t start,
           int64_t end,
           uint32_t row);
  void Build();
  // Drops every interval and lane, ready for intervals to be added again.
  void Clear();

  // The lane of `key`, or nothing when no interval has that key.
  std::optional<uint32_t> FindLane(const int64_t* key, size_t key_size) const;

  int64_t start(uint32_t i) const { return starts_[i]; }
  int64_t end(uint32_t i) const { return ends_[i]; }
  uint32_t row(uint32_t i) const { return rows_[i]; }
  // Whether interval `i` contains the point `p`.
  bool Contains(uint32_t i, int64_t p) const {
    return starts_[i] == ends_[i] ? starts_[i] == p
                                  : starts_[i] <= p && p < ends_[i];
  }

  // Walks one lane alongside probes whose starts never decrease.
  //
  // Start() puts the sweep at the beginning of a lane. After Advance(s),
  // active() holds every interval containing s, in lane order, and next() is
  // the first interval of the lane starting after s; limit() is the end of
  // the lane. The intervals starting in [s, e) for some e are then next()
  // onwards while start(i) < e. One sweep serves lane after lane, keeping
  // the storage of its active set.
  //
  // Advancing costs what lies between the probes: a few intervals are
  // stepped over, many are skipped by galloping to the first which starts
  // after `s` and taking those containing `s` from the tree, or from the run
  // ending there when the lane is disjoint.
  class Sweep {
   public:
    explicit Sweep(const IntervalIndex& index) : index_(index) {}

    void Start(uint32_t lane);
    // Returns false, changing nothing, if `s` is before the previous probe.
    bool Advance(int64_t s);
    Span<const uint32_t> active() const {
      return {active_.data(), active_.data() + active_.size()};
    }
    uint32_t next() const { return next_; }
    uint32_t limit() const { return lane_->end; }

   private:
    const IntervalIndex& index_;
    const Lane* lane_ = nullptr;
    // Nothing has been advanced to yet.
    bool fresh_ = true;
    int64_t at_ = 0;
    uint32_t next_ = 0;
    std::vector<uint32_t> active_;
  };

 private:
  struct Entry {
    uint32_t lane;
    int64_t start;
    int64_t end;
    uint32_t row;
  };

  uint32_t LaneOf(const int64_t* key, size_t key_size, bool insert);
  int64_t BuildMaxEnd(uint32_t lo, uint32_t hi);
  // Calls `fn(i)` in order for each i in [from, limit) of the tree over
  // [lo, hi) whose end is at or after `min_end`.
  template <typename Fn>
  void Visit(uint32_t lo,
             uint32_t hi,
             uint32_t from,
             uint32_t limit,
             int64_t min_end,
             Fn& fn) const;

  std::vector<Entry> entries_;
  // Which lane a key names. `lane_by_key` serves a key of one value, which
  // is the common case; `lane_by_keys` holds the bytes of a wider one.
  base::FlatHashMap<int64_t, uint32_t> lane_by_key_;
  base::FlatHashMap<std::string, uint32_t> lane_by_keys_;
  size_t key_size_ = 0;

  std::vector<Lane> lanes_;
  std::vector<int64_t> starts_;
  std::vector<int64_t> ends_;
  std::vector<uint32_t> rows_;
  // For each interval of a lane which is not disjoint, the latest end among
  // the intervals of the balanced binary tree rooted there over the lane's
  // range: the root of [lo, hi) is its middle. A search for intervals ending
  // after some time skips every subtree whose latest end is too early.
  std::vector<int64_t> subtree_max_end_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INDEX_H_
