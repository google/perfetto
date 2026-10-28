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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_INTERVAL_GAPS_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_INTERVAL_GAPS_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core {

// Finds the parts of each lane's background that the lane's covering
// intervals leave uncovered. A lane's covering intervals arrive sorted by
// start, so each gap is found as soon as an interval starts beyond all
// earlier ones.
class IntervalGaps {
 public:
  // An uncovered part of background interval `id`, in lane `lane`.
  struct Gap {
    Ts start;
    Ts end;
    uint32_t id;
    uint32_t lane;
  };

  // Starts lane `lane`, whose background is sorted by start, with no two
  // intervals of width overlapping. Ends the current lane first.
  void StartLane(uint32_t lane, Span<const Interval> background) {
    EndLane();
    in_lane_ = true;
    lane_ = lane;
    covered_ = 0;
    next_ = background.b;
    end_ = background.e;
  }

  // Covers [start, end) in the current lane. Starts must not decrease.
  void Cover(Ts start, Ts end) {
    PERFETTO_DCHECK(in_lane_);
    if (start == end) {
      return;
    }
    FillTo(start);
    covered_ = std::max(covered_, end);
  }

  // Ends the current lane, if any. The rest of its background is a gap.
  void EndLane() {
    if (in_lane_) {
      FillTo(std::numeric_limits<Ts>::max());
      in_lane_ = false;
    }
  }

  // Changes the number the current lane's later gaps are given.
  void set_lane(uint32_t lane) { lane_ = lane; }

  void Clear() {
    in_lane_ = false;
    gaps_.clear();
  }

  // Clear(), letting go of the gaps' storage.
  void EndRun() {
    in_lane_ = false;
    std::vector<Gap>().swap(gaps_);
  }

  bool in_lane() const { return in_lane_; }

  // The gaps found, in the order found. The caller removes those it uses.
  std::vector<Gap>& gaps() { return gaps_; }

 private:
  // Adds the gaps before `to`. Everything between `covered_` and `to` is
  // uncovered, since covering intervals arrive sorted by start.
  void FillTo(Ts to) {
    for (; next_ != end_; ++next_) {
      if (next_->start >= to) {
        return;
      }
      Ts start = std::max(next_->start, covered_);
      Ts end = std::min(next_->end, to);
      if (start < end) {
        gaps_.push_back({start, end, next_->id, lane_});
      }
      if (next_->end > to) {
        return;
      }
    }
  }

  bool in_lane_ = false;
  uint32_t lane_ = 0;
  // The end of the current lane's covering intervals so far.
  Ts covered_ = 0;
  // The current lane's background not yet passed.
  const Interval* next_ = nullptr;
  const Interval* end_ = nullptr;
  std::vector<Gap> gaps_;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_INTERVAL_GAPS_H_
