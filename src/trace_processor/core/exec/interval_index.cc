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

#include "src/trace_processor/core/exec/interval_index.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::string KeyBytes(const int64_t* key, size_t key_size) {
  return {reinterpret_cast<const char*>(key), key_size * sizeof(int64_t)};
}

}  // namespace

uint32_t IntervalIndex::LaneOf(const int64_t* key,
                               size_t key_size,
                               bool insert) {
  auto next = static_cast<uint32_t>(lanes_.size());
  const uint32_t* lane;
  if (key_size == 0) {
    return 0;
  }
  if (key_size == 1) {
    lane = insert ? lane_by_key_.Insert(key[0], next).first
                  : lane_by_key_.Find(key[0]);
  } else {
    lane = insert ? lane_by_keys_.Insert(KeyBytes(key, key_size), next).first
                  : lane_by_keys_.Find(KeyBytes(key, key_size));
  }
  if (!lane) {
    return std::numeric_limits<uint32_t>::max();
  }
  if (*lane == next) {
    lanes_.push_back({});
  }
  return *lane;
}

void IntervalIndex::Add(const int64_t* key,
                        size_t key_size,
                        int64_t start,
                        int64_t end,
                        uint32_t row) {
  PERFETTO_DCHECK(lanes_.empty() || key_size == key_size_);
  key_size_ = key_size;
  if (key_size == 0 && lanes_.empty()) {
    lanes_.push_back({});
  }
  entries_.push_back({LaneOf(key, key_size, true), start, end, row});
}

std::optional<uint32_t> IntervalIndex::FindLane(const int64_t* key,
                                                size_t key_size) const {
  if (key_size == 0) {
    return lanes_.empty() ? std::nullopt : std::make_optional(0u);
  }
  const uint32_t* lane = key_size == 1
                             ? lane_by_key_.Find(key[0])
                             : lane_by_keys_.Find(KeyBytes(key, key_size));
  return lane ? std::make_optional(*lane) : std::nullopt;
}

void IntervalIndex::Build() {
  // Group by lane with a counting sort, then order each lane on its own: the
  // lanes are many and small, so sorting them one at a time is far cheaper
  // than one sort over everything with the lane as the leading key.
  auto count = static_cast<uint32_t>(entries_.size());
  for (Lane& lane : lanes_) {
    lane = {0, 0, true};
  }
  for (const Entry& entry : entries_) {
    ++lanes_[entry.lane].end;
  }
  uint32_t offset = 0;
  for (Lane& lane : lanes_) {
    lane.begin = offset;
    offset += lane.end;
    lane.end = lane.begin;
  }
  std::vector<Entry> grouped(count);
  for (const Entry& entry : entries_) {
    grouped[lanes_[entry.lane].end++] = entry;
  }
  entries_.clear();
  entries_.shrink_to_fit();
  starts_.resize(count);
  ends_.resize(count);
  rows_.resize(count);
  for (const Lane& lane : lanes_) {
    std::sort(grouped.begin() + lane.begin, grouped.begin() + lane.end,
              [](const Entry& a, const Entry& b) {
                return a.start != b.start ? a.start < b.start : a.row < b.row;
              });
  }
  for (uint32_t i = 0; i < count; ++i) {
    starts_[i] = grouped[i].start;
    ends_[i] = grouped[i].end;
    rows_[i] = grouped[i].row;
  }

  bool need_tree = false;
  for (Lane& lane : lanes_) {
    for (uint32_t i = lane.begin + 1; lane.disjoint && i < lane.end; ++i) {
      lane.disjoint = ends_[i - 1] <= starts_[i];
    }
    need_tree = need_tree || !lane.disjoint;
  }
  if (need_tree) {
    subtree_max_end_.resize(count);
    for (const Lane& lane : lanes_) {
      if (!lane.disjoint) {
        BuildMaxEnd(lane.begin, lane.end);
      }
    }
  }
}

void IntervalIndex::Clear() {
  entries_.clear();
  lane_by_key_.Clear();
  lane_by_keys_.Clear();
  key_size_ = 0;
  lanes_.clear();
  starts_.clear();
  ends_.clear();
  rows_.clear();
  subtree_max_end_.clear();
}

int64_t IntervalIndex::BuildMaxEnd(uint32_t lo, uint32_t hi) {
  if (lo >= hi) {
    return std::numeric_limits<int64_t>::min();
  }
  uint32_t mid = lo + (hi - lo) / 2;
  int64_t max_end =
      std::max({ends_[mid], BuildMaxEnd(lo, mid), BuildMaxEnd(mid + 1, hi)});
  subtree_max_end_[mid] = max_end;
  return max_end;
}

template <typename Fn>
void IntervalIndex::Visit(uint32_t lo,
                          uint32_t hi,
                          uint32_t from,
                          uint32_t limit,
                          int64_t min_end,
                          Fn& fn) const {
  if (lo >= hi || lo >= limit || hi <= from) {
    return;
  }
  uint32_t mid = lo + (hi - lo) / 2;
  if (subtree_max_end_[mid] < min_end) {
    return;
  }
  Visit(lo, mid, from, limit, min_end, fn);
  if (mid < limit) {
    if (mid >= from && ends_[mid] >= min_end) {
      fn(mid);
    }
    Visit(mid + 1, hi, from, limit, min_end, fn);
  }
}

void IntervalIndex::Sweep::Start(uint32_t lane) {
  lane_ = &index_.lanes_[lane];
  fresh_ = true;
  next_ = lane_->begin;
  active_.clear();
}

bool IntervalIndex::Sweep::Advance(int64_t s) {
  if (!fresh_ && s < at_) {
    return false;
  }
  const std::vector<int64_t>& starts = index_.starts_;
  uint32_t from = next_;
  if (fresh_) {
    fresh_ = false;
  } else {
    // What was active and no longer contains `s` has ended, since `s` only
    // ever grows.
    uint32_t kept = 0;
    for (uint32_t i : active_) {
      if (index_.Contains(i, s)) {
        active_[kept++] = i;
      }
    }
    active_.resize(kept);
  }
  at_ = s;

  // The first interval starting after `s`, reached by galloping from `from`:
  // probes are usually close together, and a far one costs the log of the
  // distance rather than of the lane.
  uint32_t end = lane_->end;
  uint32_t step = 1;
  uint32_t lo = from;
  while (lo + step < end && starts[lo + step] <= s) {
    lo += step;
    step *= 2;
  }
  uint32_t hi = std::min(lo + step, end);
  next_ = static_cast<uint32_t>(
      std::upper_bound(starts.begin() + lo, starts.begin() + hi, s) -
      starts.begin());

  // The intervals in [from, next_) start at or before `s`; those containing
  // it become active. A few are checked one by one. Of many, a disjoint lane
  // holds the containing ones in a run ending at `next_`, and a tree finds
  // them in any other lane.
  constexpr uint32_t kStepOver = 16;
  if (next_ - from <= kStepOver) {
    for (uint32_t i = from; i < next_; ++i) {
      if (index_.Contains(i, s)) {
        active_.push_back(i);
      }
    }
    return true;
  }
  if (lane_->disjoint) {
    uint32_t i = next_;
    while (i > from && index_.Contains(i - 1, s)) {
      --i;
    }
    for (; i < next_; ++i) {
      active_.push_back(i);
    }
    return true;
  }
  auto visit = [&](uint32_t i) {
    if (index_.Contains(i, s)) {
      active_.push_back(i);
    }
  };
  index_.Visit(lane_->begin, end, from, next_, s, visit);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
