/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/active_chrome_processes_tracker.h"

namespace perfetto {
namespace trace_processor {

std::vector<ProcessWithDataLoss>
ActiveChromeProcessesTracker::GetProcessesWithDataLoss() const {
  std::vector<ProcessWithDataLoss> processes_with_data_loss;
  for (auto it = process_data_.GetIterator(); it; ++it) {
    UniquePid upid = it.key();
    const auto& process_data = it.value();
    base::Optional<int64_t> last_loss_moment;
    base::Optional<int64_t> next_no_loss_moment;
    for (int64_t metadata_ts : process_data.metadata_timestamps) {
      // Looks for a matching process descriptor in the [t - 0.2s, t + 0.2s]
      // window. The window size is somewhat arbitrary, and can be changed in
      // the future. It should be smaller than the incremental state reset
      // interval, which is 5s for Chromium traces.
      constexpr int64_t kMaxTimestampDiff = 200 * 1000 * 1000;
      auto descriptor_it = process_data.descriptor_timestamps.lower_bound(
          metadata_ts - kMaxTimestampDiff);
      if (descriptor_it != process_data.descriptor_timestamps.end()) {
        if (*descriptor_it > metadata_ts + kMaxTimestampDiff) {
          // There's no matching descriptor, but there's a descriptor at some
          // point in the future.
          last_loss_moment = metadata_ts;
          next_no_loss_moment = *descriptor_it;
        }
      } else {
        // There's no matching descriptor, and there're no descriptors in the
        // future.
        last_loss_moment = metadata_ts;
        next_no_loss_moment = base::nullopt;
        break;
      }
    }
    if (last_loss_moment) {
      processes_with_data_loss.push_back({upid, next_no_loss_moment});
    }
  }
  return processes_with_data_loss;
}

void ActiveChromeProcessesTracker::NotifyEndOfFile() {
  const auto processes = GetProcessesWithDataLoss();
  for (const auto& p : processes) {
    tables::ExperimentalMissingChromeProcessesTable::Row row;
    row.upid = p.upid;
    row.reliable_from = p.reliable_from;
    context_->storage->mutable_experimental_missing_chrome_processes_table()
        ->Insert(row);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
