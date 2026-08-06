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

#include "src/trace_processor/core/tree/tree_column_ops.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::tree_ops {
namespace {

template <typename T>
void UpdateTypedRowHashes(Span<const T> values,
                          const BitVector* non_null,
                          Span<base::MurmurHashCombiner> hashes) {
  PERFETTO_DCHECK(values.size() == hashes.size());
  PERFETTO_DCHECK(!non_null || non_null->size() == values.size());
  for (uint32_t row = 0; row < values.size(); ++row) {
    if (non_null && !non_null->is_set(row)) {
      hashes[row].Combine(false, T{});
    } else {
      hashes[row].Combine(true, values[row]);
    }
  }
}

template <typename T>
Tree::Column GatherTyped(const Tree::Column& input, Span<const uint32_t> rows) {
  Tree::Column output =
      Tree::Column::Create<T>(static_cast<uint32_t>(rows.size()));
  if (input.null_bv.size() == 0) {
    ops::GatherRows(input.unchecked_span<T>(), output.unchecked_span<T>(),
                    rows);
    return output;
  }
  output.null_bv =
      BitVector::CreateWithSize(static_cast<uint32_t>(rows.size()), false);
  ops::GatherNullableRows(input.unchecked_span<T>(), input.null_bv,
                          output.unchecked_span<T>(), &output.null_bv, rows);
  return output;
}

template <typename T>
void GatherInto(const Tree::Column& input,
                Span<const uint32_t> rows,
                uint32_t offset,
                Tree::Column* output) {
  Span<T> destination =
      output->unchecked_span<T>().subspan(offset, rows.size());
  Span<const T> source = input.unchecked_span<T>();
  for (uint32_t row = 0; row < rows.size(); ++row) {
    const uint32_t source_row = rows[row];
    if (input.null_bv.size() > 0 && !input.null_bv.is_set(source_row)) {
      continue;
    }
    destination[row] = source[source_row];
    output->null_bv.set(offset + row);
  }
}

template <typename T>
Tree::Column GatherConcatTyped(const Tree::Column& first,
                               Span<const uint32_t> first_rows,
                               const Tree::Column& second,
                               Span<const uint32_t> second_rows) {
  const uint32_t first_size = static_cast<uint32_t>(first_rows.size());
  const uint32_t second_size = static_cast<uint32_t>(second_rows.size());
  Tree::Column output = Tree::Column::Create<T>(first_size + second_size);
  if (first.null_bv.size() == 0 && second.null_bv.size() == 0) {
    Span<T> destination = output.unchecked_span<T>();
    ops::GatherRows(first.unchecked_span<T>(),
                    destination.subspan(0, first_size), first_rows);
    ops::GatherRows(second.unchecked_span<T>(),
                    destination.subspan(first_size, second_size), second_rows);
    return output;
  }
  output.null_bv = BitVector::CreateWithSize(first_size + second_size, false);
  GatherInto<T>(first, first_rows, 0, &output);
  GatherInto<T>(second, second_rows, first_size, &output);
  return output;
}

}  // namespace

void UpdateRowHashes(const Tree::Column& column,
                     Span<base::MurmurHashCombiner> hashes) {
  const BitVector* non_null =
      column.null_bv.size() > 0 ? &column.null_bv : nullptr;
  switch (column.type.index()) {
    case Tree::Column::Type::GetTypeIndex<Int64>():
      UpdateTypedRowHashes(column.unchecked_span<int64_t>(), non_null, hashes);
      return;
    case Tree::Column::Type::GetTypeIndex<Double>():
      UpdateTypedRowHashes(column.unchecked_span<double>(), non_null, hashes);
      return;
    case Tree::Column::Type::GetTypeIndex<String>():
      UpdateTypedRowHashes(column.unchecked_span<StringPool::Id>(), non_null,
                           hashes);
      return;
    default:
      PERFETTO_FATAL("Unsupported tree column type");
  }
}

Tree::Column Gather(const Tree::Column& input, Span<const uint32_t> rows) {
  switch (input.type.index()) {
    case Tree::Column::Type::GetTypeIndex<Int64>():
      return GatherTyped<int64_t>(input, rows);
    case Tree::Column::Type::GetTypeIndex<Double>():
      return GatherTyped<double>(input, rows);
    case Tree::Column::Type::GetTypeIndex<String>():
      return GatherTyped<StringPool::Id>(input, rows);
    default:
      PERFETTO_FATAL("Unsupported tree column type");
  }
}

Tree::Column GatherConcat(const Tree::Column& first,
                          Span<const uint32_t> first_rows,
                          const Tree::Column& second,
                          Span<const uint32_t> second_rows) {
  PERFETTO_DCHECK(first.type.index() == second.type.index());
  switch (first.type.index()) {
    case Tree::Column::Type::GetTypeIndex<Int64>():
      return GatherConcatTyped<int64_t>(first, first_rows, second, second_rows);
    case Tree::Column::Type::GetTypeIndex<Double>():
      return GatherConcatTyped<double>(first, first_rows, second, second_rows);
    case Tree::Column::Type::GetTypeIndex<String>():
      return GatherConcatTyped<StringPool::Id>(first, first_rows, second,
                                               second_rows);
    default:
      PERFETTO_FATAL("Unsupported tree column type");
  }
}

}  // namespace perfetto::trace_processor::core::tree_ops
