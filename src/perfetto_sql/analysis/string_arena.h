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

#ifndef SRC_PERFETTO_SQL_ANALYSIS_STRING_ARENA_H_
#define SRC_PERFETTO_SQL_ANALYSIS_STRING_ARENA_H_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

#include "perfetto/base/logging.h"

namespace perfetto::perfetto_sql::analysis::internal {

// Dense monotonic string storage with no hashing or interning. Views returned
// by Append remain valid for the lifetime of the arena.
class StringArena {
 public:
  explicit StringArena(size_t capacity)
      : data_(capacity ? std::make_unique<char[]>(capacity) : nullptr),
        capacity_(capacity) {}

  std::string_view Append(std::string_view value) {
    if (value.empty()) {
      return {};
    }
    PERFETTO_DCHECK(size_ + value.size() <= capacity_);
    char* begin = data_.get() + size_;
    std::copy(value.begin(), value.end(), begin);
    size_ += value.size();
    return {begin, value.size()};
  }

 private:
  std::unique_ptr<char[]> data_;
  size_t capacity_ = 0;
  size_t size_ = 0;
};

}  // namespace perfetto::perfetto_sql::analysis::internal

#endif  // SRC_PERFETTO_SQL_ANALYSIS_STRING_ARENA_H_
