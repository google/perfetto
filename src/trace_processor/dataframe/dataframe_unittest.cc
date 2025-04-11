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
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/util/regex.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {

inline std::string TrimSpacePerLine(const std::string& s) {
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

template <typename... Args>
std::vector<impl::Column> MakeColumnVector(Args&&... args) {
  std::vector<impl::Column> container;
  container.reserve(sizeof...(Args));
  ((container.emplace_back(std::forward<Args>(args))), ...);
  return container;
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

  void RunBytecodeTest(std::vector<impl::Column>& cols,
                       std::vector<FilterSpec>& filters,
                       const std::vector<DistinctSpec>& distinct_specs,
                       const std::vector<SortSpec>& sort_specs,
                       LimitSpec limit_spec,
                       const std::string& expected_bytecode,
                       uint64_t cols_used = 0xFFFFFFFF) {
    // Sanitize cols_used to ensure it only references valid columns.
    PERFETTO_CHECK(cols.size() < 64);
    uint64_t sanitized_cols_used = cols_used & ((1ull << cols.size()) - 1ull);
    Dataframe df(std::move(cols), 0, &string_pool_);
    ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                         df.PlanQuery(filters, distinct_specs, sort_specs,
                                      limit_spec, sanitized_cols_used));
    EXPECT_THAT(FormatBytecode(plan),
                EqualsIgnoringWhitespace(expected_bytecode));
  }

  StringPool string_pool_;
};

// Simple test case with no filters
TEST_F(DataframeBytecodeTest, NoFilters) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col1", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"col2", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}});
  std::vector<FilterSpec> filters;
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
  )");
}

// Test case with a single filter
TEST_F(DataframeBytecodeTest, SingleFilter) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col1", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"col2", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col1", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"col2", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"col3", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}});

  // Direct initialization of filter specs
  std::vector<FilterSpec> filters = {
      {0, 0, Eq{}, std::nullopt},  // Filter on column 0
      {1, 1, Eq{}, std::nullopt}   // Filter on column 1
  };
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      "col1", impl::Storage::Uint32{}, impl::Overlay::NoOverlay{}, Sorted{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, NumericSortedInEq) {
  {
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        "col1", impl::Storage::Uint32{}, impl::Overlay::NoOverlay{}, Sorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Lt{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(2)]
      SortedFilter<Uint32, LowerBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(2)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
    )");
  }
  {
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        "col1", impl::Storage::Uint32{}, impl::Overlay::NoOverlay{}, Sorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Le{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(3)]
      SortedFilter<Uint32, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(2)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
    )");
  }
  {
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        "col1", impl::Storage::Uint32{}, impl::Overlay::NoOverlay{}, Sorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Gt{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
      SortedFilter<Uint32, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
    )");
  }
  {
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        "col1", impl::Storage::Uint32{}, impl::Overlay::NoOverlay{}, Sorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Ge{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(5)]
      SortedFilter<Uint32, LowerBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
    )");
  }
}

TEST_F(DataframeBytecodeTest, Numeric) {
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col1", impl::Storage::Uint32{},
                                      impl::Overlay::NoOverlay{}, Unsorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Eq{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
      NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
    )");
  }
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col1", impl::Storage::Uint32{},
                                      impl::Overlay::NoOverlay{}, Unsorted{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Ge{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(5)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      Iota: [source_register=Register(0), update_register=Register(3)]
      NonStringFilter<Uint32, Ge>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
  }
}

TEST_F(DataframeBytecodeTest, SortingOfFilters) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"id", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"col1", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Sorted{}},
                       impl::Column{"col2", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col3", impl::Storage::String{},
                                    impl::Overlay::NoOverlay{}, Sorted{}},
                       impl::Column{"col4", impl::Storage::String{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters = {
      {0, 0, Le{}, std::nullopt}, {1, 0, Eq{}, std::nullopt},
      {0, 0, Eq{}, std::nullopt}, {4, 0, Le{}, std::nullopt},
      {2, 0, Eq{}, std::nullopt}, {3, 0, Le{}, std::nullopt},
      {3, 0, Eq{}, std::nullopt}, {1, 0, Le{}, std::nullopt},
  };
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  if constexpr (!regex::IsRegexSupported()) {
    GTEST_SKIP() << "Regex is not supported";
  }
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      "col1", impl::Storage::String{}, impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters = {
      {0, 0, Regex{}, std::nullopt},
  };
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(7)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    StringFilter<Regex>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, StringFilterGlob) {
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      "col1", impl::Storage::String{}, impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters = {
      {0, 0, Glob{}, std::nullopt},
  };
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(6)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    StringFilter<Glob>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, SparseNullFilters) {
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_sparse", impl::Storage::Uint32{},
                                      impl::Overlay::SparseNull{}, Unsorted{}});
    std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnull, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNull>: [col=0, update_register=Register(2)]
  )",
                    /*cols_used=*/0);
  }

  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_sparse", impl::Storage::Uint32{},
                                      impl::Overlay::SparseNull{}, Unsorted{}});
    std::vector<FilterSpec> filters_isnotnull = {
        {0, 0, IsNotNull{}, std::nullopt},
    };
    RunBytecodeTest(cols, filters_isnotnull, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(2)]
  )",
                    /*cols_used=*/0);
  }
}

TEST_F(DataframeBytecodeTest, DenseNullFilters) {
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_dense", impl::Storage::Uint32{},
                                      impl::Overlay::DenseNull{}, Unsorted{}});

    // Test IsNull
    std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnull, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNull>: [col=0, update_register=Register(2)]
  )",
                    /*cols_used=*/0);
  }
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_dense", impl::Storage::Uint32{},
                                      impl::Overlay::DenseNull{}, Unsorted{}});

    // Test IsNotNull
    std::vector<FilterSpec> filters_isnotnull = {
        {0, 0, IsNotNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnotnull, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullFilter<IsNotNull>: [col=0, update_register=Register(2)]
  )",
                    /*cols_used=*/0);
  }
}

TEST_F(DataframeBytecodeTest, NonNullFilters) {
  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_nonnull", impl::Storage::Uint32{},
                                      impl::Overlay::NoOverlay{}, Unsorted{}});

    // Test IsNull: Should result in an empty result set as the column is
    // NonNull
    std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnull, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    )");
  }

  {
    std::vector<impl::Column> cols =
        MakeColumnVector(impl::Column{"col_nonnull", impl::Storage::Uint32{},
                                      impl::Overlay::NoOverlay{}, Unsorted{}});

    // Test IsNotNull: Should have no effect as the column is already NonNull
    std::vector<FilterSpec> filters_isnotnull = {
        {0, 0, IsNotNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnotnull, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
      Iota: [source_register=Register(0), update_register=Register(2)]
    )");
  }
}

TEST_F(DataframeBytecodeTest, StandardFilterOnSparseNull) {
  // Test a standard filter (Eq) on a SparseNull column.
  // Expect bytecode to handle nulls first, then apply the filter.
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_sparse", impl::Storage::Uint32{},
                                    impl::Overlay::SparseNull{}, Unsorted{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_dense", impl::Storage::Uint32{},
                                    impl::Overlay::DenseNull{}, Unsorted{}});

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_nonnull", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_sparse", impl::Storage::Int64{},
                                    impl::Overlay::SparseNull{}, Unsorted{}});

  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b10 means use column at index 1 (col_sparse)
  uint64_t cols_used = 0b10;

  // Since we request a nullable column (col_sparse at index 1), the output
  // needs two slots per row:
  // Slot 0: Original index (copied by StrideCopy)
  // Slot 1: Translated index for col_sparse (or UINT32_MAX for null)
  // Therefore, stride = 2.
  // col_sparse (index 1) maps to offset 1 in the output row.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_nonnull", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_dense", impl::Storage::Int64{},
                                    impl::Overlay::DenseNull{}, Unsorted{}});

  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b10 means use column at index 1 (col_dense)
  uint64_t cols_used = 0b10;

  // Since we request a nullable column (col_dense at index 1), the output
  // needs two slots per row:
  // Slot 0: Original index (copied by StrideCopy)
  // Slot 1: Original index if non-null, else UINT32_MAX (copied by
  // StrideCopyDenseNullIndices) Therefore, stride = 2. col_dense (index 1)
  // maps to offset 1 in the output row.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_nonnull", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_sparse", impl::Storage::Int64{},
                                    impl::Overlay::SparseNull{}, Unsorted{}},
                       impl::Column{"col_dense", impl::Storage::Double{},
                                    impl::Overlay::DenseNull{}, Unsorted{}});
  std::vector<FilterSpec> filters;  // No filters

  // cols_used_bitmap: 0b110 means use columns at index 1 (sparse) and 2
  // (dense)
  uint64_t cols_used = 0b110;

  // Output needs 3 slots per row:
  // Slot 0: Original index (StrideCopy)
  // Slot 1: Translated index for col_sparse (index 1)
  // Slot 2: Copied index for col_dense (index 2)
  // Stride = 3.
  // col_sparse (index 1) maps to offset 1.
  // col_dense (index 2) maps to offset 2.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
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
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, SetIdSorted{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};

  // Expect the specialized Uint32SetIdSortedEq bytecode for this combination
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    Uint32SetIdSortedEq: [col=0, val_register=Register(1), update_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}
// Test sorting by a single Uint32 column, ascending.
TEST_F(DataframeBytecodeTest, SortSingleUint32Asc) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_A", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  // Expect direction=SortDirection(0) and StableSortIndices
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    StableSortIndices<Uint32>: [col=0, direction=SortDirection(0), update_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

// Test sorting by a single String column, descending.
TEST_F(DataframeBytecodeTest, SortSingleStringDesc) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_S", impl::Storage::String{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  // Expect direction=SortDirection(1) and StableSortIndices
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    StableSortIndices<String>: [col=0, direction=SortDirection(1), update_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

// Test multi-column sorting (Stable Sort).
TEST_F(DataframeBytecodeTest, SortMultiColumnStable) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_I", impl::Storage::Int64{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_D", impl::Storage::Double{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters;
  // Sort specs: Primary Int64 DESC, Secondary Double ASC
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending},
                                 {1, SortDirection::kAscending}};
  // Expect direction=SortDirection(...) and StableSortIndices
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    StableSortIndices<Double>: [col=1, direction=SortDirection(0), update_register=Register(2)]
    StableSortIndices<Int64>: [col=0, direction=SortDirection(1), update_register=Register(2)]
  )",
                  /*cols_used=*/3);
}

// Test sorting combined with filtering.
TEST_F(DataframeBytecodeTest, SortWithFilter) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"id_col", impl::Storage::Id{},
                                    impl::Overlay::NoOverlay{}, IdSorted{}},
                       impl::Column{"val_col", impl::Storage::Double{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters = {{0, 0, Gt{}, std::nullopt}};
  std::vector<SortSpec> sorts = {{1, SortDirection::kAscending}};
  // Expect direction=SortDirection(0) and StableSortIndices
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
    SortedFilter<Id, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    StableSortIndices<Double>: [col=1, direction=SortDirection(0), update_register=Register(3)]
  )",
                  /*cols_used=*/3);
}

// Test planning sort on a nullable column.
TEST_F(DataframeBytecodeTest, SortNullableColumn) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"nullable_int", impl::Storage::Int32{},
                                    impl::Overlay::SparseNull{}, Unsorted{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  // Expect direction=SortDirection(1) and StableSortIndices
  // Also check the output bytecode which was generated when cols_used=1
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(1), partition_register=Register(2), dest_non_null_register=Register(3)]
    PrefixPopcount: [col=0, dest_register=Register(4)]
    TranslateSparseNullIndices: [col=0, popcount_register=Register(4), source_register=Register(3), update_register=Register(3)]
    StableSortIndices<Int32>: [col=0, direction=SortDirection(1), update_register=Register(3)]
    AllocateIndices: [size=0, dest_slab_register=Register(5), dest_span_register=Register(6)]
    StrideCopy: [source_register=Register(2), update_register=Register(6), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(4), update_register=Register(6), offset=1, stride=2]
  )",
                  /*cols_used=*/1);  // cols_used=1 requires output generation
                                     // for the nullable column
}

TEST_F(DataframeBytecodeTest, PlanQuery_DistinctTwoNonNullCols) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_int", impl::Storage::Int32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_str", impl::Storage::String{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});

  std::vector<FilterSpec> filters;
  std::vector<DistinctSpec> distinct_specs = {{0}, {1}};
  uint64_t cols_used = 0b11;

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = int_size + str_id_size;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = int_size;

  const std::string expected_bytecode =
      base::StackString<2048>(
          R"(
            InitRange: [size=0, dest_register=Register(0)]
            AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
            Iota: [source_register=Register(0), update_register=Register(2)]
            AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
            CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            CopyToRowLayoutNonNull: [col=1, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(3), total_row_stride=%u, indices_register=Register(2)]
          )",
          col0_offset, stride, int_size, col1_offset, stride, str_id_size,
          static_cast<uint32_t>(stride))
          .ToStdString();

  RunBytecodeTest(cols, filters, distinct_specs, {}, {}, expected_bytecode,
                  cols_used);
}

TEST_F(DataframeBytecodeTest, LimitOffsetPlacement) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_filter", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}},
                       impl::Column{"col_sparse", impl::Storage::Int64{},
                                    impl::Overlay::SparseNull{}, Unsorted{}});

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  LimitSpec spec;
  spec.offset = 2;
  spec.limit = 10;

  // cols_used=2 requests the sparse null column (index 1)
  RunBytecodeTest(cols, filters, {}, {}, spec, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
    LimitOffsetIndices: [offset_value=2, limit_value=10, update_register=Register(3)]
    AllocateIndices: [size=0, dest_slab_register=Register(4), dest_span_register=Register(5)]
    StrideCopy: [source_register=Register(3), update_register=Register(5), stride=2]
    PrefixPopcount: [col=1, dest_register=Register(6)]
    StrideTranslateAndCopySparseNullIndices: [col=1, popcount_register=Register(6), update_register=Register(5), offset=1, stride=2]
  )",
                  /*cols_used=*/2);  // Request col_sparse output
}

TEST_F(DataframeBytecodeTest, PlanQuery_MinOptimizationApplied) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{"col_A", impl::Storage::Uint32{},
                                    impl::Overlay::NoOverlay{}, Unsorted{}});
  std::vector<FilterSpec> filters;
  std::vector<DistinctSpec> distinct_specs;
  std::vector<SortSpec> sort_specs = {{0, SortDirection::kAscending}};
  LimitSpec limit_spec;
  limit_spec.limit = 1;

  std::string expected_bytecode = R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    FindMinMaxIndex<Uint32, MinOp>: [col=0, update_register=Register(2)]
  )";

  RunBytecodeTest(cols, filters, distinct_specs, sort_specs, limit_spec,
                  expected_bytecode, /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, PlanQuery_MinOptimizationNotAppliedNullable) {
  auto bv = impl::BitVector::CreateWithSize(0);
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      "col_A", impl::Storage::Uint32{},
      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  std::vector<FilterSpec> filters;
  std::vector<DistinctSpec> distinct_specs;
  std::vector<SortSpec> sort_specs = {{0, SortDirection::kAscending}};
  LimitSpec limit_spec;
  limit_spec.limit = 1;

  std::string expected_bytecode = R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(0), partition_register=Register(2), dest_non_null_register=Register(3)]
    PrefixPopcount: [col=0, dest_register=Register(4)]
    TranslateSparseNullIndices: [col=0, popcount_register=Register(4), source_register=Register(3), update_register=Register(3)]
    StableSortIndices<Uint32>: [col=0, direction=SortDirection(0), update_register=Register(3)]
    LimitOffsetIndices: [offset_value=0, limit_value=1, update_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(5), dest_span_register=Register(6)]
    StrideCopy: [source_register=Register(2), update_register=Register(6), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(4), update_register=Register(6), offset=1, stride=2]
  )";

  RunBytecodeTest(cols, filters, distinct_specs, sort_specs, limit_spec,
                  expected_bytecode, /*cols_used=*/1);
}

}  // namespace perfetto::trace_processor::dataframe
