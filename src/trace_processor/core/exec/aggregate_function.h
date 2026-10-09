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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_FUNCTION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_FUNCTION_H_

#include <cstdint>
#include <memory>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// One aggregate an operator computes, as its spec names it.
struct AggregateCall {
  enum class Function : uint8_t { kCountStar, kSum, kCount, kMin, kMax };
  Function function = Function::kCountStar;
  // The column aggregated; unused by COUNT(*).
  uint32_t column = 0;
};

// What an aggregate reads of one batch: a value a row, contiguous, and
// which rows hold one.
struct AggregateInput {
  const int64_t* values = nullptr;
  // Bit r set if row r holds a value; null if every row does.
  const uint64_t* valid = nullptr;
  uint32_t rows = 0;
};

// Loads a batch's column at the rows it keeps into the shape AggregateInput
// describes, pointing at the column's own values when they are the first
// rows and copying them otherwise.
class AggregateInputLoader {
 public:
  // Loads `count` of the rows `selection` keeps: the first, or if `rows` is
  // given those (by position among the rows kept). Returns false if the
  // column can't be aggregated.
  bool Load(const ColumnView& column,
            const Selection& selection,
            const uint32_t* rows,
            uint32_t count,
            AggregateInput* out);

 private:
  FlexVector<int64_t> values_;
  FlexVector<uint64_t> valid_;
};

// Where each group's state for one aggregate is: group g's words start at
// `words + g * stride`. For a function which skips nulls, `seen[g]` is 1 once
// group g has added a value, or `seen` is null if every group has (see
// SeenBytes).
struct GroupStates {
  int64_t* words;
  uint64_t stride;
  uint8_t* seen = nullptr;
};

// Keeps GroupStates::seen for one aggregate. A group exists only because a
// row of it arrived, so while no input row has been null every group has seen
// a value and nothing is kept. From the first null, a byte a group is: a byte
// rather than a bit, so neighbouring groups' updates don't wait on each other.
class SeenBytes {
 public:
  // Returns what to pass as GroupStates::seen for a batch with `groups`
  // groups in all, whose input holds nulls if `nulls`.
  uint8_t* For(uint32_t groups, bool nulls);
  void Clear();

 private:
  FlexVector<uint8_t> bytes_;
  bool tracking_ = false;
  // How many groups the last batch had.
  uint32_t groups_ = 0;
};

// A merge of one group's state into another's.
struct GroupMerge {
  uint32_t into;
  uint32_t from;
};

// An aggregate function, e.g. SUM. Each operator keeps a state for each of
// its groups, starting zeroed, and the function says how many words it needs
// and how to update, merge and read it. Every method takes a whole batch, so
// a call costs once a batch rather than once a row.
class AggregateFunction {
 public:
  virtual ~AggregateFunction();

  // How many words of state each group keeps.
  virtual uint32_t state_words() const = 0;
  // Whether Update reads an input column (COUNT(*) doesn't).
  virtual bool reads_input() const = 0;
  // Whether the function needs GroupStates::seen.
  virtual bool tracks_seen() const = 0;
  // Adds each row of `in` into the state of its group `groups[row]`.
  virtual void Update(const AggregateInput& in,
                      const uint32_t* groups,
                      GroupStates states,
                      bool* overflow) const = 0;
  // In order, for each of the `count` merges, adds the state of group
  // `from` into that of group `into`.
  virtual void Combine(const GroupMerge* merges,
                       uint32_t count,
                       GroupStates states,
                       bool* overflow) const = 0;
  // Writes the result of each of `groups` to `values`, and to `valid`
  // whether it holds one.
  virtual void Finalize(GroupStates states,
                        const uint32_t* groups,
                        uint32_t count,
                        int64_t* values,
                        BitVector* valid) const = 0;
};

std::unique_ptr<AggregateFunction> MakeAggregateFunction(
    AggregateCall::Function);

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_AGGREGATE_FUNCTION_H_
