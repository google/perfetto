/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/android_bugreport/android_battery_stats_history_string_tracker.h"

namespace perfetto::trace_processor {

AndroidBatteryStatsHistoryStringTracker::
    ~AndroidBatteryStatsHistoryStringTracker() = default;

base::Status AndroidBatteryStatsHistoryStringTracker::SetStringPoolItem(
    uint64_t index,
    int32_t uid,
    const std::string& str) {
  const HistoryStringPoolItem item{uid, str};

  if (PERFETTO_LIKELY(index == hsp_items_.size())) {
    hsp_items_.push_back(item);
    return base::OkStatus();
  } else if (index > hsp_items_.size()) {
    hsp_items_.resize(index + 1, {-1, ""});
  }
  hsp_items_[index] = item;
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
