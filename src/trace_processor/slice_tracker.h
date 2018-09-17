/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SLICE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_SLICE_TRACKER_H_

#include <stdint.h>

#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class SliceTracker {
 public:
  explicit SliceTracker(TraceProcessorContext*);
  ~SliceTracker();

  void Begin(uint64_t timestamp, UniqueTid utid, StringId cat, StringId name);

  void Scoped(uint64_t timestamp,
              UniqueTid utid,
              StringId cat,
              StringId name,
              uint64_t duration);

  void End(uint64_t timestamp,
           UniqueTid utid,
           StringId opt_cat = {},
           StringId opt_name = {});

 private:
  struct Slice {
    StringId cat_id;
    StringId name_id;
    uint64_t start_ts;
    uint64_t end_ts;  // Only for complete events (scoped TRACE_EVENT macros).
  };
  using SlicesStack = std::vector<Slice>;

  static inline void MaybeCloseStack(uint64_t end_ts, SlicesStack&);
  static inline std::tuple<uint64_t, uint64_t> GetStackHashes(
      const SlicesStack&);
  void CompleteSlice(UniqueTid tid);

  TraceProcessorContext* const context_;
  std::unordered_map<UniqueTid, SlicesStack> threads_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SLICE_TRACKER_H_
