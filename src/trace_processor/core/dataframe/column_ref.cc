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

#include "src/trace_processor/core/dataframe/column_ref.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/transient_column.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

const void* StorageData(const Storage& storage) {
  switch (storage.type().index()) {
    case StorageType::GetTypeIndex<Id>():
      return nullptr;
    case StorageType::GetTypeIndex<Uint32>():
      return Storage::CastDataPtr<Uint32>(storage.data());
    case StorageType::GetTypeIndex<Int32>():
      return Storage::CastDataPtr<Int32>(storage.data());
    case StorageType::GetTypeIndex<Int64>():
      return Storage::CastDataPtr<Int64>(storage.data());
    case StorageType::GetTypeIndex<Double>():
      return Storage::CastDataPtr<Double>(storage.data());
    case StorageType::GetTypeIndex<String>():
      return Storage::CastDataPtr<String>(storage.data());
    default:
      PERFETTO_FATAL("Unknown storage type");
  }
}

}  // namespace

exec::TransientColumn BorrowColumn(const struct Column& c) {
  // Only the columns filters read enter a batch, and the lowering accepts
  // NonNull filters only, so a sparse column never reaches here.
  PERFETTO_CHECK(c.null_storage.nullability().Is<NonNull>());
  return exec::TransientColumn::Reference(c.storage.type(),
                                          StorageData(c.storage), nullptr);
}

}  // namespace perfetto::trace_processor::core::dataframe
