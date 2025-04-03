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

TEST_F(DataframeBytecodeTest, NumericSortedInEq) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Uint32{}, Sorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters;
  filters = {{0, 0, Lt{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(2)]
    SortedFilter<Uint32, LowerBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
  filters = {{0, 0, Le{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(3)]
    SortedFilter<Uint32, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
  filters = {{0, 0, Gt{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
    SortedFilter<Uint32, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
  filters = {{0, 0, Ge{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(5)]
    SortedFilter<Uint32, LowerBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, Numeric) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", Uint32{}, Unsorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters;
  filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
  filters = {{0, 0, Ge{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(5)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NonStringFilter<Uint32, Ge>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, SortingOfFilters) {
  std::vector<ColumnSpec> col_specs = {
      {"id", Id{}, IdSorted{}, NonNull{}},
      {"col1", Uint32{}, Sorted{}, NonNull{}},
      {"col2", Uint32{}, Unsorted{}, NonNull{}},
      {"col3", String{}, Sorted{}, NonNull{}},
      {"col4", String{}, Unsorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters = {
      {0, 0, Le{}, std::nullopt}, {1, 0, Eq{}, std::nullopt},
      {0, 0, Eq{}, std::nullopt}, {4, 0, Le{}, std::nullopt},
      {2, 0, Eq{}, std::nullopt}, {3, 0, Le{}, std::nullopt},
      {3, 0, Eq{}, std::nullopt}, {1, 0, Le{}, std::nullopt},
  };
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Id, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(1), write_register=Register(2), op=NonNullOp(3)]
    SortedFilter<Id, UpperBound>: [col=0, val_register=Register(2), update_register=Register(0), write_result_to=BoundModifier(2)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(2), write_register=Register(3), op=NonNullOp(0)]
    SortedFilter<Uint32, EqualRange>: [col=1, val_register=Register(3), update_register=Register(0), write_result_to=BoundModifier(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(3), write_register=Register(4), op=NonNullOp(3)]
    SortedFilter<Uint32, UpperBound>: [col=1, val_register=Register(4), update_register=Register(0), write_result_to=BoundModifier(2)]
    CastFilterValue<String>: [fval_handle=FilterValue(4), write_register=Register(5), op=NonNullOp(0)]
    SortedFilter<String, EqualRange>: [col=3, val_register=Register(5), update_register=Register(0), write_result_to=BoundModifier(0)]
    CastFilterValue<String>: [fval_handle=FilterValue(5), write_register=Register(6), op=NonNullOp(3)]
    SortedFilter<String, UpperBound>: [col=3, val_register=Register(6), update_register=Register(0), write_result_to=BoundModifier(2)]
    CastFilterValue<String>: [fval_handle=FilterValue(6), write_register=Register(7), op=NonNullOp(3)]
    AllocateIndices: [size=0, dest_slab_register=Register(8), dest_span_register=Register(9)]
    Iota: [source_register=Register(0), update_register=Register(9)]
    StringFilter<Le>: [col=4, val_register=Register(7), source_register=Register(9), update_register=Register(9)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(7), write_register=Register(10), op=NonNullOp(0)]
    NonStringFilter<Uint32, Eq>: [col=2, val_register=Register(10), source_register=Register(9), update_register=Register(9)]
  )");
}

TEST_F(DataframeBytecodeTest, StringFilter) {
  std::vector<ColumnSpec> col_specs = {
      {"col1", String{}, Unsorted{}, NonNull{}},
  };
  std::vector<FilterSpec> filters = {
      {0, 0, Regex{}, std::nullopt},
      {0, 0, Glob{}, std::nullopt},
  };
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(7)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    StringFilter<Regex>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
    CastFilterValue<String>: [fval_handle=FilterValue(1), write_register=Register(4), op=NonNullOp(6)]
    StringFilter<Glob>: [col=0, val_register=Register(4), source_register=Register(3), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, SparseNullFilters) {
  std::vector<ColumnSpec> col_specs = {
      {"col_sparse", Uint32{}, Unsorted{}, SparseNull{}},
  };

  std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters_isnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNull>: [col=0, update_register=Register(2)]
  )",
                  /*cols_used=*/0);

  std::vector<FilterSpec> filters_isnotnull = {
      {0, 0, IsNotNull{}, std::nullopt},
  };
  RunBytecodeTest(col_specs, filters_isnotnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(2)]
  )",
                  /*cols_used=*/0);
}

TEST_F(DataframeBytecodeTest, DenseNullFilters) {
  // Test IsNull and IsNotNull filters on a DenseNull column
  std::vector<ColumnSpec> col_specs = {
      {"col_dense", Uint32{}, Unsorted{}, DenseNull{}},
  };

  // Test IsNull
  std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters_isnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNull>: [col=0, update_register=Register(2)]
  )",
                  /*cols_used=*/0);

  // Test IsNotNull
  std::vector<FilterSpec> filters_isnotnull = {
      {0, 0, IsNotNull{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters_isnotnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(2)]
  )",
                  /*cols_used=*/0);
}

TEST_F(DataframeBytecodeTest, NonNullFilters) {
  // Test IsNull and IsNotNull filters on a NonNull column
  std::vector<ColumnSpec> col_specs = {
      {"col_nonnull", Uint32{}, Unsorted{}, NonNull{}},
  };

  // Test IsNull: Should result in an empty result set as the column is NonNull
  std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters_isnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
  )");

  // Test IsNotNull: Should have no effect as the column is already NonNull
  std::vector<FilterSpec> filters_isnotnull = {
      {0, 0, IsNotNull{}, std::nullopt}};
  RunBytecodeTest(col_specs, filters_isnotnull, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
  )");
}

TEST_F(DataframeBytecodeTest, StandardFilterOnSparseNull) {
  // Test a standard filter (Eq) on a SparseNull column.
  // Expect bytecode to handle nulls first, then apply the filter.
  std::vector<ColumnSpec> col_specs = {
      {"col_sparse", Uint32{}, Unsorted{}, SparseNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(3)]
    AllocateIndices: [size=0, dest_slab_register=Register(4), dest_span_register=Register(5)]
    PrefixPopcount: [col=0, dest_register=Register(6)]
    TranslateSparseNullIndices: [col=0, popcount_register=Register(6), source_register=Register(3), update_register=Register(5)]
    NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(5), update_register=Register(3)]
  )",
                  /*cols_used=*/0);
}

TEST_F(DataframeBytecodeTest, StandardFilterOnDenseNull) {
  // Test a standard filter (Eq) on a DenseNull column.
  // Expect bytecode to handle nulls first, then apply the filter directly.
  std::vector<ColumnSpec> col_specs = {
      {"col_dense", Uint32{}, Unsorted{}, DenseNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(3)]
    NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )",
                  /*cols_used=*/0);
}

TEST_F(DataframeBytecodeTest, OutputSparseNullColumn) {
  // Test requesting a SparseNull column in the output
  std::vector<ColumnSpec> col_specs = {
      {"col_nonnull", Uint32{}, Unsorted{}, NonNull{}},
      {"col_sparse", Int64{}, Unsorted{}, SparseNull{}},
  };
  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b10 means use column at index 1 (col_sparse)
  uint64_t cols_used = 0b10;

  // Since we request a nullable column (col_sparse at index 1), the output
  // needs two slots per row:
  // Slot 0: Original index (copied by StrideCopy)
  // Slot 1: Translated index for col_sparse (or UINT32_MAX for null)
  // Therefore, stride = 2.
  // col_sparse (index 1) maps to offset 1 in the output row.
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(3), dest_span_register=Register(4)]
    StrideCopy: [source_register=Register(2), update_register=Register(4), stride=2]
    PrefixPopcount: [col=1, dest_register=Register(5)]
    StrideTranslateAndCopySparseNullIndices: [col=1, popcount_register=Register(5), update_register=Register(4), offset=1, stride=2]
  )",
                  cols_used);
}

TEST_F(DataframeBytecodeTest, OutputDenseNullColumn) {
  // Test requesting a DenseNull column in the output
  std::vector<ColumnSpec> col_specs = {
      {"col_nonnull", Uint32{}, Unsorted{}, NonNull{}},
      {"col_dense", Int64{}, Unsorted{}, DenseNull{}},  // The column we request
  };
  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b10 means use column at index 1 (col_dense)
  uint64_t cols_used = 0b10;

  // Since we request a nullable column (col_dense at index 1), the output
  // needs two slots per row:
  // Slot 0: Original index (copied by StrideCopy)
  // Slot 1: Original index if non-null, else UINT32_MAX (copied by
  // StrideCopyDenseNullIndices) Therefore, stride = 2. col_dense (index 1) maps
  // to offset 1 in the output row.
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(3), dest_span_register=Register(4)]
    StrideCopy: [source_register=Register(2), update_register=Register(4), stride=2]
    StrideCopyDenseNullIndices: [col=1, update_register=Register(4), offset=1, stride=2]
  )",
                  cols_used);
}

TEST_F(DataframeBytecodeTest, OutputMultipleNullableColumns) {
  // Test requesting both a SparseNull and a DenseNull column
  std::vector<ColumnSpec> col_specs = {
      {"col_nonnull", Uint32{}, Unsorted{}, NonNull{}},
      {"col_sparse", Int64{}, Unsorted{}, SparseNull{}},  // Requested (index 1)
      {"col_dense", Double{}, Unsorted{}, DenseNull{}},   // Requested (index 2)
  };
  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b110 means use columns at index 1 (sparse) and 2 (dense)
  uint64_t cols_used = 0b110;

  // Output needs 3 slots per row:
  // Slot 0: Original index (StrideCopy)
  // Slot 1: Translated index for col_sparse (index 1)
  // Slot 2: Copied index for col_dense (index 2)
  // Stride = 3.
  // col_sparse (index 1) maps to offset 1.
  // col_dense (index 2) maps to offset 2.
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(3), dest_span_register=Register(4)]
    StrideCopy: [source_register=Register(2), update_register=Register(4), stride=3]
    PrefixPopcount: [col=1, dest_register=Register(5)]
    StrideTranslateAndCopySparseNullIndices: [col=1, popcount_register=Register(5), update_register=Register(4), offset=1, stride=3]
    StrideCopyDenseNullIndices: [col=2, update_register=Register(4), offset=2, stride=3]
  )",
                  cols_used);
}

TEST_F(DataframeBytecodeTest, Uint32SetIdSortedEqGeneration) {
  std::vector<ColumnSpec> col_specs = {
      {"col", Uint32(), SetIdSorted(), NonNull{}},
  };
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  // Expect the specialized Uint32SetIdSortedEq bytecode for this combination
  RunBytecodeTest(col_specs, filters, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    Uint32SetIdSortedEq: [col=0, val_register=Register(1), update_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
