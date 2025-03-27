/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_QUERY_PLAN_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_QUERY_PLAN_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe::impl {

static constexpr uint32_t kMaxFilters = 16;

// A QueryPlan encapsulates all the information needed to execute a query,
// including the bytecode instructions and interpreter configuration.
struct QueryPlan {
  // Specification for the bytecode interpreter.
  // Contains parameters needed to execute the bytecode instructions.
  struct InterpreterSpec {
    // Number of filter values used by this query.
    uint32_t filter_value_count = 0;

    // Register holding the final filtered indices.
    bytecode::reg::ReadHandle<Slab<uint32_t>> output_register;

    // Maps column indices to output offsets.
    std::array<uint32_t, kMaxFilters> col_to_output_offset;

    // Number of output indices per row.
    uint32_t output_per_row = 0;
  };
  static_assert(std::is_trivially_copyable_v<InterpreterSpec>);
  static_assert(std::is_trivially_destructible_v<InterpreterSpec>);

  // Serializes the query plan to a Base64-encoded string.
  // This allows plans to be stored or transmitted between processes.
  std::string Serialize() const {
    size_t size = sizeof(size_t) +
                  (bytecode.size() * sizeof(bytecode::Bytecode)) +
                  sizeof(interpreter_spec);
    std::string res(size, '\0');
    char* p = res.data();
    {
      size_t bytecode_size = bytecode.size();
      memcpy(p, &bytecode_size, sizeof(bytecode_size));
      p += sizeof(bytecode_size);
    }
    {
      memcpy(p, bytecode.data(), bytecode.size() * sizeof(bytecode::Bytecode));
      p += bytecode.size() * sizeof(bytecode::Bytecode);
    }
    {
      memcpy(p, &interpreter_spec, sizeof(interpreter_spec));
      p += sizeof(interpreter_spec);
    }
    PERFETTO_CHECK(p == res.data() + res.size());
    return base::Base64Encode(base::StringView(res));
  }

  // Deserializes a query plan from a Base64-encoded string.
  // Returns the reconstructed QueryPlan.
  static QueryPlan Deserialize(std::string_view serialized) {
    QueryPlan res;
    std::optional<std::string> raw_data = base::Base64Decode(
        base::StringView(serialized.data(), serialized.size()));
    PERFETTO_CHECK(raw_data);
    const char* p = raw_data->data();
    size_t bytecode_size;
    {
      memcpy(&bytecode_size, p, sizeof(bytecode_size));
      p += sizeof(bytecode_size);
    }
    {
      for (uint32_t i = 0; i < bytecode_size; ++i) {
        res.bytecode.emplace_back();
      }
      memcpy(res.bytecode.data(), p,
             bytecode_size * sizeof(bytecode::Bytecode));
      p += bytecode_size * sizeof(bytecode::Bytecode);
    }
    {
      memcpy(&res.interpreter_spec, p, sizeof(res.interpreter_spec));
      p += sizeof(res.interpreter_spec);
    }
    PERFETTO_CHECK(p == raw_data->data() + raw_data->size());
    return res;
  }

  // Configuration for the bytecode interpreter.
  InterpreterSpec interpreter_spec;

  // Vector of bytecode instructions to execute.
  bytecode::BytecodeVector bytecode;
};

// Builder class for creating query plans.
//
// QueryPlans contain the bytecode instructions and interpreter configuration
// needed to execute a query.
class QueryPlanBuilder {
 public:
  static QueryPlan Build(uint32_t row_count,
                         const std::vector<Column>& columns,
                         std::vector<FilterSpec>& specs,
                         uint64_t cols_used) {
    QueryPlanBuilder builder(row_count, columns);
    builder.Filter(specs);
    builder.Output(cols_used);
    return std::move(builder).Build();
  }

 private:
  // Represents register types for holding indices.
  using IndicesReg = std::variant<bytecode::reg::RwHandle<Range>,
                                  bytecode::reg::RwHandle<Slab<uint32_t>>>;

  // State information for a column during query planning.
  struct ColumnState {};

  // Constructs a builder for the given number of rows and columns.
  QueryPlanBuilder(uint32_t row_count, const std::vector<Column>& columns);

  // Adds filter operations to the query plan based on filter specifications.
  // Optimizes the order of filters for efficiency.
  void Filter(std::vector<FilterSpec>& specs);

  // Configures output handling for the filtered rows.
  // |cols_used_bitmap| is a bitmap with bits set for columns that will be
  // accessed.
  void Output(uint64_t cols_used_bitmap);

  // Finalizes and returns the built query plan.
  QueryPlan Build() &&;

  // Processes non-string filter constraints.
  void NonStringConstraint(
      const FilterSpec& c,
      const NonStringContent& type,
      const NonStringOp& op,
      const bytecode::reg::ReadHandle<CastFilterValueResult>& result);

  // Attempts to apply optimized filtering on sorted data.
  // Returns true if the optimization was applied.
  bool TrySortedConstraint(
      const FilterSpec& fs,
      const Content& type,
      const NonNullOp& op,
      const bytecode::reg::RwHandle<CastFilterValueResult>& result);

  // Adds overlay translation for handling special column properties like
  // nullability.
  bytecode::reg::RwHandle<Slab<uint32_t>> MaybeAddOverlayTranslation(
      const FilterSpec& c);

  // Ensures indices are stored in a Slab, converting from Range if necessary.
  PERFETTO_NO_INLINE bytecode::reg::RwHandle<Slab<uint32_t>>
  EnsureIndicesAreInSlab();

  // Adds a new bytecode instruction of type T to the plan.
  template <typename T>
  T& AddOpcode() {
    return AddOpcode<T>(bytecode::Index<T>());
  }

  // Adds a new bytecode instruction of type T with the given option value.
  template <typename T>
  T& AddOpcode(uint32_t option);

  // Sets the result to an empty set. Use when a filter guarantees no matches.
  void SetGuaranteedToBeEmpty();

  // Maximum number of rows in the query result.
  uint32_t max_row_count_ = 0;

  // Reference to the columns being queried.
  const std::vector<Column>& columns_;

  // The query plan being built.
  QueryPlan plan_;

  // State information for each column during planning.
  std::vector<ColumnState> column_states_;

  // Number of registers allocated so far.
  uint32_t register_count_ = 0;

  // Current register holding the set of matching indices.
  IndicesReg indices_reg_;
};

}  // namespace perfetto::trace_processor::dataframe::impl

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_QUERY_PLAN_H_
