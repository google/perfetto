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

TraceStorage::TraceStorage() {
  // Upid/utid 0 is reserved for invalid processes/threads.
  unique_processes_.emplace_back(0);
  unique_threads_.emplace_back(0);

  // Reserve string ID 0 for the empty string.
  InternString("", 0);
}

TraceStorage::~TraceStorage() {}

void TraceStorage::AddSliceToCpu(uint32_t cpu,
                                 uint64_t start_ns,
                                 uint64_t duration_ns,
                                 UniqueTid utid) {
  cpu_events_[cpu].AddSlice(start_ns, duration_ns, utid);
};

StringId TraceStorage::InternString(const char* data, size_t length) {
  uint32_t hash = 0x811c9dc5;  // FNV-1a-32 offset basis.
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<decltype(hash)>(data[i]);
    hash *= 16777619;  // FNV-1a-32 prime.
  }
  auto id_it = string_index_.find(hash);
  if (id_it != string_index_.end()) {
    // TODO(lalitm): check if this DCHECK happens and if so, then change hash
    // to 64bit.
    PERFETTO_DCHECK(
        strncmp(string_pool_[id_it->second].c_str(), data, length) == 0);
    return id_it->second;
  }
  string_pool_.emplace_back(data, length);
  StringId string_id = string_pool_.size() - 1;
  string_index_.emplace(hash, string_id);
  return string_id;
}

void TraceStorage::ResetStorage() {
  *this = TraceStorage();
}

}  // namespace trace_processor
}  // namespace perfetto
