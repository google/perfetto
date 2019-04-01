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

#include "src/trace_processor/storage_columns.h"

namespace perfetto {
namespace trace_processor {

StorageColumn::StorageColumn(std::string col_name, bool hidden)
    : col_name_(col_name), hidden_(hidden) {}
StorageColumn::~StorageColumn() = default;

TsEndAccessor::TsEndAccessor(const std::deque<int64_t>* ts,
                             const std::deque<int64_t>* dur)
    : ts_(ts), dur_(dur) {}
TsEndAccessor::~TsEndAccessor() = default;

RowIdAccessor::RowIdAccessor(TableId table_id) : table_id_(table_id) {}
RowIdAccessor::~RowIdAccessor() = default;

RowAccessor::RowAccessor() = default;
RowAccessor::~RowAccessor() = default;

}  // namespace trace_processor
}  // namespace perfetto
