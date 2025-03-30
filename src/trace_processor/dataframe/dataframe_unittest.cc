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

#include "src/trace_processor/dataframe/dataframe.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/specs.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {
namespace {

std::string TrimSpacePerLine(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  bool at_line_start = true;
  for (char c : s) {
    if (c == '\n') {
      at_line_start = true;
      result += c;
    } else if (at_line_start && std::isspace(c)) {
      // Skip whitespace at line start
      continue;
    } else {
      at_line_start = false;
      result += c;
    }
  }
  return result;
}

// Custom matcher that compares strings ignoring all whitespace
MATCHER_P(EqualsIgnoringWhitespace,
          expected_str,
          "equals (ignoring all whitespace)") {
  std::string stripped_expected =
      TrimSpacePerLine(base::TrimWhitespace(expected_str));
  std::string stripped_actual = TrimSpacePerLine(base::TrimWhitespace(arg));
  if (stripped_actual == stripped_expected) {
    return true;
  }
  *result_listener << "after removing all whitespace:\nExpected:\n"
                   << stripped_expected << "\nActual:\n"
                   << stripped_actual;
  return false;
}

// Test fixture for diff-based testing of bytecode generation
class DataframeBytecodeTest : public ::testing::Test {
 protected:
  // Formats the bytecode for comparison
  static std::string FormatBytecode(const Dataframe::QueryPlan& plan) {
    std::string result;
    for (const auto& bc : plan.GetImplForTesting().bytecode) {
      result += impl::bytecode::ToString(bc) + "\n";
    }
    return result;
  }

  void RunBytecodeTest(const std::vector<ColumnSpec>& col_specs,
                       std::vector<FilterSpec>& filters,
                       const std::string& expected_bytecode,
                       uint64_t cols_used = 0xFFFFFFFF) {
    // Sanitize cols_used to ensure it only references valid columns.
    PERFETTO_CHECK(col_specs.size() < 64);
    uint64_t sanitized_cols_used =
        cols_used & ((1ull << col_specs.size()) - 1ull);
    Dataframe df(col_specs, &string_pool_);
    ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                         df.PlanQuery(filters, sanitized_cols_used));
    EXPECT_THAT(FormatBytecode(plan),
                EqualsIgnoringWhitespace(expected_bytecode));
  }

  StringPool string_pool_;
};

// Simple test case with no filters
TEST_F(DataframeBytecodeTest, NoFilters) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Id{}, IdSorted{}, NonNull{}},
      {"col2", Id{}, IdSorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters;
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
  )");
}

// Test case with a single filter
TEST_F(DataframeBytecodeTest, SingleFilter) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Id{}, IdSorted{}, NonNull{}},
      {"col2", Id{}, IdSorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Id, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

// Test case with multiple filters
TEST_F(DataframeBytecodeTest, MultipleFilters) {
  // Direct initialization of column specs
  std::vector<ColumnSpec> col_specs = {{"col1", Id{}, IdSorted{}, NonNull{}},
                                       {"col2", Id{}, IdSorted{}, NonNull{}},
                                       {"col3", Id{}, IdSorted{}, NonNull{}}};

  // Direct initialization of filter specs
  std::vector<FilterSpec> filters = {
      {0, 0, Eq{}, std::nullopt},  // Filter on column 0
      {1, 1, Eq{}, std::nullopt}   // Filter on column 1
  };
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Id, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(1), write_register=Register(2), op=NonNullOp(0)]
    SortedFilter<Id, EqualRange>: [col=1, val_register=Register(2), update_register=Register(0), write_result_to=BoundModifier(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(3), dest_span_register=Register(4)]
    Iota: [source_register=Register(0), update_register=Register(4)]
  )");
}

TEST_F(DataframeBytecodeTest, NumericSortedEq) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Uint32{}, Sorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, NumericEq) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Uint32{}, Unsorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
