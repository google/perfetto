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
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe_test_utils.h"
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/types.h"
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
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (uint32_t i = 0; i < cols.size(); ++i) {
      col_names.emplace_back("col" + std::to_string(i));
    }
    auto df = MakeDatafame(col_names, std::move(cols));
    RunBytecodeTest(*df, filters, distinct_specs, sort_specs, limit_spec,
                    expected_bytecode, cols_used);
  }

  static void RunBytecodeTest(const Dataframe& df,
                              std::vector<FilterSpec>& filters,
                              const std::vector<DistinctSpec>& distinct_specs,
                              const std::vector<SortSpec>& sort_specs,
                              LimitSpec limit_spec,
                              const std::string& expected_bytecode,
                              uint64_t cols_used = 0xFFFFFFFF) {
    // Sanitize cols_used to ensure it only references valid columns.
    PERFETTO_CHECK(df.column_names().size() < 64);
    uint64_t sanitized_cols_used =
        cols_used & ((1ull << df.column_names().size()) - 1ull);

    ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                         df.PlanQuery(filters, distinct_specs, sort_specs,
                                      limit_spec, sanitized_cols_used));
    EXPECT_THAT(FormatBytecode(plan),
                EqualsIgnoringWhitespace(expected_bytecode));
  }

  std::unique_ptr<Dataframe> MakeDatafame(std::vector<std::string> col_names,
                                          std::vector<impl::Column> cols) {
    std::vector<std::shared_ptr<impl::Column>> col_fixed_vec;
    col_fixed_vec.reserve(cols.size());
    for (auto& col : cols) {
      col_fixed_vec.emplace_back(
          std::make_shared<impl::Column>(std::move(col)));
    }
    return std::unique_ptr<Dataframe>(new Dataframe(false, std::move(col_names),
                                                    std::move(col_fixed_vec), 0,
                                                    &string_pool_));
  }

  StringPool string_pool_;
};

// Simple test case with no filters
TEST_F(DataframeBytecodeTest, NoFilters) {
  std::vector<impl::Column> cols =
      MakeColumnVector(impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}});

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
      impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Sorted{}});
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
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Sorted{}});
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
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Sorted{}});
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
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Sorted{}});
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
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Sorted{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Uint32{},
                                    impl::NullStorage::NonNull{}, Sorted{}},
                       impl::Column{impl::Storage::Uint32{},
                                    impl::NullStorage::NonNull{}, Unsorted{}},
                       impl::Column{impl::Storage::String{},
                                    impl::NullStorage::NonNull{}, Sorted{}},
                       impl::Column{impl::Storage::String{},
                                    impl::NullStorage::NonNull{}, Unsorted{}});
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
      impl::Storage::String{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
      impl::Storage::String{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::SparseNull{}, Unsorted{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::SparseNull{}, Unsorted{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::DenseNull{}, Unsorted{}});

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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::DenseNull{}, Unsorted{}});

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
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});

    // Test IsNull: Should result in an empty result set as the column is
    // NonNull
    std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnull, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    )");
  }

  {
    std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
        impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::SparseNull{}, Unsorted{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::DenseNull{}, Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::DenseNull{},
                   Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}},
      impl::Column{impl::Storage::Double{}, impl::NullStorage::DenseNull{},
                   Unsorted{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, SetIdSorted{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::String{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Int64{},
                                    impl::NullStorage::NonNull{}, Unsorted{}},
                       impl::Column{impl::Storage::Double{},
                                    impl::NullStorage::NonNull{}, Unsorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Id{},
                                    impl::NullStorage::NonNull{}, IdSorted{}},
                       impl::Column{impl::Storage::Double{},
                                    impl::NullStorage::NonNull{}, Unsorted{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Int32{}, impl::NullStorage::SparseNull{}, Unsorted{}});
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
      MakeColumnVector(impl::Column{impl::Storage::Int32{},
                                    impl::NullStorage::NonNull{}, Unsorted{}},
                       impl::Column{impl::Storage::String{},
                                    impl::NullStorage::NonNull{}, Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::NonNull{}, Unsorted{}});
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
      impl::Storage::Uint32{},
      impl::NullStorage{impl::NullStorage::SparseNull{std::move(bv), {}}},
      Unsorted{}});

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

TEST_F(DataframeBytecodeTest, PlanQuery_SingleColIndex_EqFilter_NonNullInt) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col1"}, CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}));
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  for (uint32_t i = 0; i < 100; ++i) {
    df.InsertUnchecked(kSpec, i);
  }
  df.MarkFinalized();

  std::vector<uint32_t> p_vec(100);
  std::iota(p_vec.begin(), p_vec.end(), 0);
  df.AddIndex(
      Index({0}, std::make_shared<std::vector<uint32_t>>(std::move(p_vec))));

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  std::string expected_bytecode = R"(
    InitRange: [size=100, dest_register=Register(0)]
    IndexPermutationVectorToSpan: [index=0, write_register=Register(1)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(2), op=NonNullOp(0)]
    IndexedFilterEq<Uint32, NonNull>: [col=0, filter_value_reg=Register(2), popcount_register=Register(3), update_register=Register(1)]
    AllocateIndices: [size=100, dest_slab_register=Register(4), dest_span_register=Register(5)]
    CopySpanIntersectingRange: [source_register=Register(1), source_range_register=Register(0), update_register=Register(5)]
  )";
  RunBytecodeTest(df, filters, {}, {}, {}, expected_bytecode,
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest,
       PlanQuery_SingleColIndex_EqFilter_NullableString) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col_str_nullable"},
      CreateTypedColumnSpec(String(), SparseNull(), Unsorted()));

  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  df.InsertUnchecked(kSpec,
                     std::make_optional(string_pool_.InternString("apple")));
  df.InsertUnchecked(kSpec, std::nullopt);
  df.InsertUnchecked(kSpec,
                     std::make_optional(string_pool_.InternString("banana")));
  df.InsertUnchecked(kSpec,
                     std::make_optional(string_pool_.InternString("apple")));
  df.MarkFinalized();
  df.AddIndex(Index({0}, std::make_shared<std::vector<uint32_t>>(
                             std::vector<uint32_t>{1, 0, 3, 2})));

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  std::string expected_bytecode = R"(
    InitRange: [size=4, dest_register=Register(0)]
    IndexPermutationVectorToSpan: [index=0, write_register=Register(1)]
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(2), op=NonNullOp(0)]
    PrefixPopcount: [col=0, dest_register=Register(3)]
    IndexedFilterEq<String, SparseNull>: [col=0, filter_value_reg=Register(2), popcount_register=Register(3), update_register=Register(1)]
    AllocateIndices: [size=4, dest_slab_register=Register(4), dest_span_register=Register(5)]
    CopySpanIntersectingRange: [source_register=Register(1), source_range_register=Register(0), update_register=Register(5)]
    AllocateIndices: [size=8, dest_slab_register=Register(6), dest_span_register=Register(7)]
    StrideCopy: [source_register=Register(5), update_register=Register(7), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(3), update_register=Register(7), offset=1, stride=2]
  )";
  RunBytecodeTest(df, filters, {}, {}, {}, expected_bytecode);
}

TEST_F(DataframeBytecodeTest, PlanQuery_MultiColIndex_PrefixEqFilters) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col0_uint32", "col1_uint32"},
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()));

  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  df.InsertUnchecked(kSpec, 10u, 100u);
  df.InsertUnchecked(kSpec, 10u, 200u);
  df.InsertUnchecked(kSpec, 20u, 100u);
  df.InsertUnchecked(kSpec, 10u, 100u);
  df.MarkFinalized();

  std::vector<uint32_t> p_vec(4);
  std::iota(p_vec.begin(), p_vec.end(), 0);
  df.AddIndex(
      Index({0, 1}, std::make_shared<std::vector<uint32_t>>(std::move(p_vec))));

  std::vector<FilterSpec> filters = {
      {0, 0, Eq{}, std::nullopt},
      {1, 1, Eq{}, std::nullopt},
  };
  std::string expected_bytecode = R"(
    InitRange: [size=4, dest_register=Register(0)]
    IndexPermutationVectorToSpan: [index=0, write_register=Register(1)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(2), op=NonNullOp(0)]
    IndexedFilterEq<Uint32, NonNull>: [col=0, filter_value_reg=Register(2), popcount_register=Register(3), update_register=Register(1)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(1), write_register=Register(4), op=NonNullOp(0)]
    IndexedFilterEq<Uint32, NonNull>: [col=1, filter_value_reg=Register(4), popcount_register=Register(5), update_register=Register(1)]
    AllocateIndices: [size=4, dest_slab_register=Register(6), dest_span_register=Register(7)]
    CopySpanIntersectingRange: [source_register=Register(1), source_range_register=Register(0), update_register=Register(7)]
  )";
  RunBytecodeTest(df, filters, {}, {}, {}, expected_bytecode);
}

TEST(DataframeTest, Insert) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNull(), Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  VerifyData(
      df, 0b1111,
      Rows(Row(0u, 10u, int64_t(0l), "foo"), Row(1u, 20u, nullptr, nullptr)));
}

TEST(DataframeTest, GetCellAndSetCell) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullSupportingCellGetAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), 0u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), 10u);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 0), 0l);
  ASSERT_EQ(df.GetCellUnchecked<3>(kSpec, 0), pool.InternString("foo"));

  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), 1u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1), 20u);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<3>(kSpec, 1), std::nullopt);

  df.SetCellUnchecked<1>(kSpec, 0, 9u);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), 9u);

  df.SetCellUnchecked<2>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<2>(kSpec, 0), std::nullopt);
}

TEST(DataframeTest, TypedCursor) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullSupportingCellGetAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  auto cursor =
      df.CreateTypedCursorUnchecked(kSpec, {FilterSpec{0, 0, Eq{}, {}}}, {});
  {
    cursor.SetFilterValues(0l);
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(), 0u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 10u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(), 0l);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(), pool.InternString("foo"));
    cursor.Next();
    ASSERT_TRUE(cursor.Eof());
  }
  {
    cursor.SetFilterValues(1l);
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(), 1u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(), std::nullopt);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(), std::nullopt);
    cursor.Next();
    ASSERT_TRUE(cursor.Eof());
  }
}

TEST(DataframeTest, TypedCursorSetMultipleTimes) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullSupportingCellGetAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);
  {
    auto cursor = df.CreateTypedCursorUnchecked(kSpec, {}, {});
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 10u);
    cursor.SetCellUnchecked<1>(20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 20u);
  }
  {
    auto cursor =
        df.CreateTypedCursorUnchecked(kSpec, {FilterSpec{1, 0, Eq{}, {}}}, {});
    cursor.SetFilterValues(int64_t(20));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 20u);
    cursor.SetCellUnchecked<1>(20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(), 20u);
  }
}

}  // namespace perfetto::trace_processor::dataframe
