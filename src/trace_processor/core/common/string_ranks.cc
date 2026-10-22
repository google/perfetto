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

#include "src/trace_processor/core/common/string_ranks.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/util/sort.h"

namespace perfetto::trace_processor::core {
namespace {

struct StringSortToken {
  std::string_view str_view;
  StringPool::Id id;
};

struct StringSortKey {
  std::string_view operator()(const StringSortToken& token) const {
    return token.str_view;
  }
};

}  // namespace

void StringRanks::Rank(const StringPool& pool) {
  // Initially do *not* default initialize the array for performance.
  std::unique_ptr<StringSortToken[]> ids_to_sort(
      new StringSortToken[ranks_.size()]);
  std::unique_ptr<StringSortToken[]> scratch(
      new StringSortToken[ranks_.size()]);
  uint32_t i = 0;
  for (auto it = ranks_.GetIterator(); it; ++it) {
    base::StringView str_view = pool.Get(it.key());
    ids_to_sort[i++] = StringSortToken{
        std::string_view(str_view.data(), str_view.size()),
        it.key(),
    };
  }
  auto* sorted =
      core::MsdRadixSort(ids_to_sort.get(), ids_to_sort.get() + ranks_.size(),
                         scratch.get(), StringSortKey{});
  for (uint32_t rank = 0; rank < ranks_.size(); ++rank) {
    uint32_t* it = ranks_.Find(sorted[rank].id);
    PERFETTO_DCHECK(it);
    *it = rank;
  }
}

}  // namespace perfetto::trace_processor::core
