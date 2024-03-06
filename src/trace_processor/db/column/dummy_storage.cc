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

#include "src/trace_processor/db/column/dummy_storage.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

SingleSearchResult DummyStorage::ChainImpl::SingleSearch(FilterOp,
                                                         SqlValue,
                                                         uint32_t) const {
  PERFETTO_FATAL("Shouldn't be called");
}

SearchValidationResult DummyStorage::ChainImpl::ValidateSearchConstraints(
    FilterOp,
    SqlValue) const {
  PERFETTO_FATAL("Shouldn't be called");
}

RangeOrBitVector DummyStorage::ChainImpl::SearchValidated(FilterOp,
                                                          SqlValue,
                                                          Range) const {
  PERFETTO_FATAL("Shouldn't be called");
}

RangeOrBitVector DummyStorage::ChainImpl::IndexSearchValidated(FilterOp,
                                                               SqlValue,
                                                               Indices) const {
  PERFETTO_FATAL("Shouldn't be called");
}

Range DummyStorage::ChainImpl::OrderedIndexSearchValidated(FilterOp,
                                                           SqlValue,
                                                           Indices) const {
  PERFETTO_FATAL("Shouldn't be called");
}

void DummyStorage::ChainImpl::StableSort(SortToken*,
                                         SortToken*,
                                         SortDirection) const {
  PERFETTO_FATAL("Shouldn't be called");
}

uint32_t DummyStorage::ChainImpl::size() const {
  return 0;
}

void DummyStorage::ChainImpl::Serialize(StorageProto*) const {
  PERFETTO_FATAL("Shouldn't be called");
}

}  // namespace perfetto::trace_processor::column
