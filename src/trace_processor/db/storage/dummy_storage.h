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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_DUMMY_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_DUMMY_STORAGE_H_

#include <variant>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/storage.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

// Dummy storage. Used for columns that are not supposed to have operations done
// on them.
class DummyStorage final : public Storage {
 public:
  DummyStorage() = default;

  RangeOrBitVector Search(FilterOp, SqlValue, RowMap::Range) const override;

  RangeOrBitVector IndexSearch(FilterOp,
                               SqlValue,
                               uint32_t*,
                               uint32_t,
                               bool) const override;

  void StableSort(uint32_t*, uint32_t) const override;

  void Sort(uint32_t*, uint32_t) const override;

  uint32_t size() const override;
};

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_DUMMY_STORAGE_H_
