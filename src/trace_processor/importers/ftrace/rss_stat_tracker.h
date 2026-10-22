/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_RSS_STAT_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_RSS_STAT_TRACKER_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class RssStatTracker {
 public:
  using ConstBytes = protozero::ConstBytes;

  explicit RssStatTracker(TraceProcessorContext*);

  void ParseRssStat(int64_t ts,
                    uint32_t field_id,
                    uint32_t pid,
                    ConstBytes blob);
  void ParseRssStat(int64_t ts,
                    uint32_t pid,
                    int64_t size,
                    uint32_t member,
                    std::optional<bool> curr,
                    std::optional<int64_t> mm_id,
                    bool throttled);

 private:
  std::optional<UniqueTid> FindUtidForMmId(int64_t mm_id,
                                           bool is_curr,
                                           uint32_t pid,
                                           bool throttled);

  bool IsRacyCurrEvent(int64_t mm_id, UniqueTid owner_utid, UniqueTid utid);

  base::FlatHashMap<int64_t, UniqueTid> mm_id_to_utid_;
  // Indexed by utid: the mm most recently reported by the thread with
  // curr=true or kNoMmId if none. mm ids are uint32 in the trace so can never
  // be negative.
  static constexpr int64_t kNoMmId = -1;
  std::vector<int64_t> utid_to_curr_mm_id_;
  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_RSS_STAT_TRACKER_H_
