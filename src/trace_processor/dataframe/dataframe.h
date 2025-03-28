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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bytecode_interpreter.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe {

// Dataframe is a columnar data structure for efficient querying and filtering
// of tabular data. It provides:
//
// - Type-specialized storage and filtering optimized for common trace data
// patterns
// - Efficient query execution with optimized bytecode generation
// - Support for serializable query plans that separate planning from execution
// - Memory-efficient storage with support for specialized column types
class Dataframe {
 public:
  // QueryPlan encapsulates an executable, serializable representation of a
  // dataframe query operation. It contains the bytecode instructions and
  // metadata needed to execute a query.
  class QueryPlan {
   public:
    QueryPlan() = default;

    // Serializes the query plan to a string.
    std::string Serialize() const { return plan_.Serialize(); }

    // Deserializes a query plan from a string previously produced by
    // `Serialize()`.
    static QueryPlan Deserialize(std::string_view serialized) {
      return QueryPlan(impl::QueryPlan::Deserialize(serialized));
    }

    // Returns the underlying implementation for testing purposes.
    const impl::QueryPlan& GetImplForTesting() const { return plan_; }

   private:
    friend class Dataframe;
    explicit QueryPlan(impl::QueryPlan plan) : plan_(std::move(plan)) {}
    impl::QueryPlan plan_;
  };

  class Cursor {
   public:
    struct Visitor {
      void Column(int64_t);
      void Column(double);
      void Column(NullTermStringView);
      void Column(nullptr_t);
      void Column(uint32_t value);
      void Column(int32_t value);
    };

    PERFETTO_ALWAYS_INLINE void Execute() {
      using S = impl::Span<uint32_t>;
      interpeter_.Execute();

      const auto& span =
          *interpeter_.GetRegisterValue<S>(params_.output_register);
      pos_ = span.b;
      end_ = span.e;
    }

    PERFETTO_ALWAYS_INLINE void Next() { pos_ += params_.output_per_row; }

    PERFETTO_ALWAYS_INLINE bool Eof() const { return pos_ == end_; }

    template <typename V>
    PERFETTO_ALWAYS_INLINE void Column(V& visitor, uint32_t col) {
      const impl::Column& c = columns_[col];
      uint32_t idx = pos_[params_.col_to_output_offset[col]];
      if (idx == std::numeric_limits<uint32_t>::max()) {
        visitor.Column(nullptr);
        return;
      }
      using C = Content;
      switch (c.spec.content.index()) {
        case C::GetTypeIndex<Id>():
          visitor.Column(idx);
          break;
        default:
          PERFETTO_FATAL("Invalid storage spec");
      }
    }

    PERFETTO_ALWAYS_INLINE FilterSpec::Value* filter_values() {
      return values_.get();
    }
    PERFETTO_ALWAYS_INLINE size_t filter_value_size() const {
      return params_.filter_value_count;
    }

   private:
    friend class Dataframe;

    explicit Cursor(impl::QueryPlan plan,
                    impl::Column* columns,
                    StringPool* pool)
        : values_(std::make_unique<FilterSpec::Value[]>(
              plan.params.filter_value_count)),
          interpeter_(std::move(plan.bytecode), values_.get(), columns, pool),
          params_(plan.params),
          columns_(columns) {}

    std::unique_ptr<FilterSpec::Value[]> values_;
    impl::bytecode::Interpreter interpeter_;
    impl::QueryPlan::ExecutionParams params_;
    const impl::Column* columns_;
    uint32_t* pos_;
    uint32_t* end_;
  };

  // Creates a dataframe.
  //
  // StringPool is passed here to allow for implicit lookup of the value of
  // string columns.
  Dataframe(const std::vector<ColumnSpec>&, const StringPool* string_pool);

  // Non-copyable
  Dataframe(const Dataframe&) = delete;
  Dataframe& operator=(const Dataframe&) = delete;

  // Movable
  Dataframe(Dataframe&&) = default;
  Dataframe& operator=(Dataframe&&) = default;

  // Creates an execution plan for querying the dataframe with specified filters
  // and column selection.
  //
  // Parameters:
  //   specs:            Filter predicates to apply to the data.
  //   cols_used_bitmap: Bitmap where each bit corresponds to a column that may
  //                     be requested. Only columns with set bits can be
  //                     fetched.
  base::StatusOr<QueryPlan> PlanQuery(std::vector<FilterSpec>& specs,
                                      uint64_t cols_used_bitmap);

  void SetupCursor(QueryPlan plan, Cursor* cursor);

 private:
  // Internal storage for columns
  std::vector<impl::Column> columns_;

  // Number of rows in the dataframe
  uint32_t row_count_ = 0;

  // String pool for efficient string storage
  const StringPool* string_pool_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
