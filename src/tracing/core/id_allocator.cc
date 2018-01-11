/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/core/id_allocator.h"

#include "perfetto/base/logging.h"

namespace perfetto {

IdAllocator::IdAllocator(IdType end) : max_id_(end - 1) {
  PERFETTO_DCHECK(end > 1);
}

IdAllocator::~IdAllocator() = default;

IdAllocator::IdType IdAllocator::Allocate() {
  for (IdType ignored = 1; ignored <= max_id_; ignored++) {
    last_id_ = last_id_ < max_id_ ? last_id_ + 1 : 1;
    const auto id = last_id_;

    // 0 is never a valid ID. So if we are looking for |id| == N and there are
    // N or less elements in the vector, they must necessarily be all < N.
    // e.g. if |id| == 4 and size() == 4, the vector will contain IDs 0,1,2,3.
    if (id >= ids_.size()) {
      ids_.resize(id + 1);
      ids_[id] = true;
      return id;
    }

    if (!ids_[id]) {
      ids_[id] = true;
      return id;
    }
  }
  return 0;
}

void IdAllocator::Free(IdType id) {
  if (id == 0 || id >= ids_.size() || !ids_[id]) {
    PERFETTO_DCHECK(false);
    return;
  }
  ids_[id] = false;
}

// TODO(primiano): Remove after we can run twice.
void IdAllocator::Reset() {
  last_id_ = 0;
}

}  // namespace perfetto
