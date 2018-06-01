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

#include "src/trace_processor/trace_storage.h"

#include <string.h>

namespace perfetto {
namespace trace_processor {

void TraceStorage::AddSliceForCpu(uint32_t cpu,
                                  uint64_t start_timestamp,
                                  uint64_t duration,
                                  const char* thread_name) {
  if (cpu_events_.size() <= cpu)
    cpu_events_.resize(cpu + 1);

  SlicesPerCpu* slices = &cpu_events_[cpu];
  slices->cpu_ = cpu;
  slices->start_timestamps.emplace_back(start_timestamp);
  slices->durations.emplace_back(duration);

  uint32_t hash = 0;
  for (size_t i = 0; i < strlen(thread_name); ++i) {
    hash = static_cast<uint32_t>(thread_name[i]) + (hash * 31);
  }
  auto id_it = string_pool_.find(hash);
  if (id_it == string_pool_.end()) {
    strings_.emplace_back(thread_name);
    string_pool_.emplace(hash, strings_.size() - 1);
    slices->thread_names.emplace_back(strings_.size() - 1);
  } else {
    slices->thread_names.emplace_back(id_it->second);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
