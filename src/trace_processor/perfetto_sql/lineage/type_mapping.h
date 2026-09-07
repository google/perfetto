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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_TYPE_MAPPING_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_TYPE_MAPPING_H_

#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::lineage {

namespace analysis = ::perfetto::perfetto_sql::analysis;

// Maps between dataframe storage types and the analysis column type
// vocabulary. Semantic analysis cannot depend on trace processor, so only
// this side can know both sets. The vocabularies mirror each other position
// by position, pinned here, which makes each mapping an identity on the
// index.
static_assert(analysis::ColumnType::kSize == core::StorageType::kSize,
              "The analysis vocabulary must mirror StorageType");
static_assert(analysis::ColumnType::GetTypeIndex<analysis::Id>() ==
              core::StorageType::GetTypeIndex<core::Id>());
static_assert(analysis::ColumnType::GetTypeIndex<analysis::Uint32>() ==
              core::StorageType::GetTypeIndex<core::Uint32>());
static_assert(analysis::ColumnType::GetTypeIndex<analysis::Int32>() ==
              core::StorageType::GetTypeIndex<core::Int32>());
static_assert(analysis::ColumnType::GetTypeIndex<analysis::Int64>() ==
              core::StorageType::GetTypeIndex<core::Int64>());
static_assert(analysis::ColumnType::GetTypeIndex<analysis::Double>() ==
              core::StorageType::GetTypeIndex<core::Double>());
static_assert(analysis::ColumnType::GetTypeIndex<analysis::String>() ==
              core::StorageType::GetTypeIndex<core::String>());

inline analysis::ColumnType ToAnalysisType(core::StorageType type) {
  return type.MapByIndex<analysis::ColumnType>();
}

inline core::StorageType ToStorageType(analysis::ColumnType type) {
  return type.MapByIndex<core::StorageType>();
}

}  // namespace perfetto::trace_processor::lineage

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_TYPE_MAPPING_H_
