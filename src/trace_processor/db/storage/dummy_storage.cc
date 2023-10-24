/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/storage/dummy_storage.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

RangeOrBitVector DummyStorage::Search(FilterOp, SqlValue, RowMap::Range) const {
  PERFETTO_FATAL("Shouldn't be called");
}

RangeOrBitVector DummyStorage::IndexSearch(FilterOp,
                                           SqlValue,
                                           uint32_t*,
                                           uint32_t,
                                           bool) const {
  PERFETTO_FATAL("Shouldn't be called");
}

void DummyStorage::StableSort(uint32_t*, uint32_t) const {
  PERFETTO_FATAL("Shouldn't be called");
}

void DummyStorage::Sort(uint32_t*, uint32_t) const {
  PERFETTO_FATAL("Shouldn't be called");
}

uint32_t DummyStorage::size() const {
  return 0;
}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
