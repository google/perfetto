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

#include "src/trace_processor/core/tree/tree_transformer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::core::tree {
namespace {

struct IdCallback : core::dataframe::CellCallback {
  void OnCell(int64_t id) {
    id_value = id;
    type_ok = true;
  }
  void OnCell(double) { type_ok = false; }
  void OnCell(NullTermStringView) { type_ok = false; }
  void OnCell(std::nullptr_t) {
    id_value = std::nullopt;
    type_ok = true;
  }
  void OnCell(uint32_t id) {
    id_value = id;
    type_ok = true;
  }
  void OnCell(int32_t id) {
    id_value = id;
    type_ok = true;
  }
  std::optional<int64_t> id_value;
  bool type_ok = false;
};

}  // namespace

TreeTransformer::TreeTransformer(dataframe::Dataframe df, StringPool* pool)
    : df_(std::move(df)), pool_(pool) {}

base::StatusOr<dataframe::Dataframe> TreeTransformer::ToDataframe() && {
  base::FlatHashMap<int64_t, uint32_t> id_to_row;
  IdCallback id_cb;
  for (uint32_t row = 0; row < df_.row_count(); ++row) {
    df_.GetCell(row, 0, id_cb);
    if (PERFETTO_UNLIKELY(!id_cb.type_ok)) {
      return base::ErrStatus("ID column has non-integer values");
    }
    if (PERFETTO_UNLIKELY(!id_cb.id_value.has_value())) {
      return base::ErrStatus("ID column has null values");
    }
    id_to_row[*id_cb.id_value] = row;
  }
  dataframe::AdhocDataframeBuilder builder(
      {"_tree_id", "_tree_parent_id"}, pool_,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull});
  for (uint32_t row = 0; row < df_.row_count(); ++row) {
    df_.GetCell(row, 1, id_cb);
    if (PERFETTO_UNLIKELY(!id_cb.type_ok)) {
      return base::ErrStatus("Parent ID column has non-integer values");
    }
    builder.PushNonNull(0, row);
    if (id_cb.id_value) {
      auto* parent_row = id_to_row.Find(id_cb.id_value.value());
      if (!parent_row) {
        return base::ErrStatus("Parent ID not found in ID column");
      }
      builder.PushNonNull(1, *parent_row);
    } else {
      builder.PushNull(1);
    }
  }
  ASSIGN_OR_RETURN(auto build_result, std::move(builder).Build());
  return dataframe::Dataframe::HorizontalConcat(std::move(build_result),
                                                std::move(df_));
}

}  // namespace perfetto::trace_processor::core::tree
