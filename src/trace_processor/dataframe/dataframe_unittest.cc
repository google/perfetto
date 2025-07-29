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
#include <set>
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
#include "src/trace_processor/dataframe/typed_cursor.h"
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}});
  std::vector<FilterSpec> filters;
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
  )");
}

// Test case with a single filter
TEST_F(DataframeBytecodeTest, SingleFilter) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, InFilter) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, In{}, std::nullopt}};
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValueList<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    In<Uint32>: [col=0, value_list_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )");
}

TEST_F(DataframeBytecodeTest, NumericSortedInEq) {
  {
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Sorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Sorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Sorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Sorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Unsorted{}, HasDuplicates{}});
    std::vector<FilterSpec> filters;
    filters = {{0, 0, Eq{}, std::nullopt}};
    RunBytecodeTest(cols, filters, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
      LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(1), popcount_register=Register(4294967295), source_register=Register(0), update_register=Register(3)]
    )");
  }
  {
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Unsorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::SparseNull{},
                     Unsorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::SparseNull{},
                     Unsorted{}, HasDuplicates{}});
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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::DenseNull{},
                     Unsorted{}, HasDuplicates{}});

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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::DenseNull{},
                     Unsorted{}, HasDuplicates{}});

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
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Unsorted{}, HasDuplicates{}});

    // Test IsNull: Should result in an empty result set as the column is
    // NonNull
    std::vector<FilterSpec> filters_isnull = {{0, 0, IsNull{}, std::nullopt}};
    RunBytecodeTest(cols, filters_isnull, {}, {}, {}, R"(
      InitRange: [size=0, dest_register=Register(0)]
      AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    )");
  }

  {
    std::vector<impl::Column> cols = MakeColumnVector(
        impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                     Unsorted{}, HasDuplicates{}});

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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::SparseNull{},
                   Unsorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::DenseNull{},
                   Unsorted{}, HasDuplicates{}});

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
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}, HasDuplicates{}});

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
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::DenseNull{},
                   Unsorted{}, HasDuplicates{}});

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
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Double{}, impl::NullStorage::DenseNull{},
                   Unsorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   SetIdSorted{}, HasDuplicates{}});
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
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

// Test sorting by a single String column, descending.
TEST_F(DataframeBytecodeTest, SortSingleStringDesc) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    InitRankMap: [dest_register=Register(3)]
    CollectIdIntoRankMap: [col=0, source_register=Register(2), rank_map_register=Register(3)]
    FinalizeRanksInMap: [update_register=Register(3)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(4)]
    CopyToRowLayout<String, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(4), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(3)]
    SortRowLayout: [buffer_register=Register(4), total_row_stride=4, indices_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

// Test multi-column sorting (Stable Sort).
TEST_F(DataframeBytecodeTest, SortMultiColumnStable) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Double{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  // Sort specs: Primary Int64 DESC, Secondary Double ASC
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending},
                                 {1, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    CopyToRowLayout<Int64, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=16, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<Double, NonNull>: [col=1, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=8, row_layout_stride=16, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=16, indices_register=Register(2)]
  )",
                  /*cols_used=*/3);
}

// Test sorting combined with filtering.
TEST_F(DataframeBytecodeTest, SortWithFilter) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{}, NoDuplicates{}},
      impl::Column{impl::Storage::Double{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Gt{}, std::nullopt}};
  std::vector<SortSpec> sorts = {{1, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
    SortedFilter<Id, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(4)]
    CopyToRowLayout<Double, NonNull>: [col=1, source_indices_register=Register(3), dest_buffer_register=Register(4), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(4), total_row_stride=8, indices_register=Register(3)]
  )",
                  /*cols_used=*/3);
}

// Test planning sort on a nullable column.
TEST_F(DataframeBytecodeTest, SortNullableColumn) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Int32{}, impl::NullStorage::SparseNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    PrefixPopcount: [col=0, dest_register=Register(4)]
    CopyToRowLayout<Int32, SparseNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=5, invert_copied_bits=1, popcount_register=Register(4), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=5, indices_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(5), dest_span_register=Register(6)]
    StrideCopy: [source_register=Register(2), update_register=Register(6), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(4), update_register=Register(6), offset=1, stride=2]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, PlanQuery_DistinctTwoNonNullCols) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Int32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});

  std::vector<FilterSpec> filters;
  std::vector<DistinctSpec> distinct_specs = {{0}, {1}};
  uint64_t cols_used = 0b11;

  const std::string expected_bytecode = R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    CopyToRowLayout<Int32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, NonNull>: [col=1, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=4, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(3), total_row_stride=8, indices_register=Register(2)]
  )";

  RunBytecodeTest(cols, filters, distinct_specs, {}, {}, expected_bytecode,
                  cols_used);
}

TEST_F(DataframeBytecodeTest, LimitOffsetPlacement) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Int64{}, impl::NullStorage::SparseNull{},
                   Unsorted{}, HasDuplicates{}});

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  LimitSpec spec;
  spec.offset = 2;
  spec.limit = 10;

  RunBytecodeTest(cols, filters, {}, {}, spec, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(1), popcount_register=Register(4294967295), source_register=Register(0), update_register=Register(3)]
    LimitOffsetIndices: [offset_value=2, limit_value=10, update_register=Register(3)]
    AllocateIndices: [size=0, dest_slab_register=Register(4), dest_span_register=Register(5)]
    StrideCopy: [source_register=Register(3), update_register=Register(5), stride=2]
    PrefixPopcount: [col=1, dest_register=Register(6)]
    StrideTranslateAndCopySparseNullIndices: [col=1, popcount_register=Register(6), update_register=Register(5), offset=1, stride=2]
  )",
                  /*cols_used=*/2);
}

TEST_F(DataframeBytecodeTest, PlanQuery_MinOptimizationApplied) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
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

TEST_F(DataframeBytecodeTest, SortOptimizationApplied_SingleAscNonNullSorted) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, SortOptimizationNotApplied_MultipleSpecs) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}},
      impl::Column{impl::Storage::Int32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending},
                                 {1, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<Int32, NonNull>: [col=1, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=4, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=8, indices_register=Register(2)]
  )",
                  /*cols_used=*/3);  // 0b11
}

TEST_F(DataframeBytecodeTest, SortOptimization_Reverse) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Sorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kDescending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    Reverse: [update_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, SortOptimizationNotApplied_NullableColumn) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::SparseNull{},
                   Sorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    PrefixPopcount: [col=0, dest_register=Register(4)]
    CopyToRowLayout<Uint32, SparseNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=5, invert_copied_bits=0, popcount_register=Register(4), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=5, indices_register=Register(2)]
    AllocateIndices: [size=0, dest_slab_register=Register(5), dest_span_register=Register(6)]
    StrideCopy: [source_register=Register(2), update_register=Register(6), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(4), update_register=Register(6), offset=1, stride=2]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, SortOptimizationNotApplied_UnsortedColumn) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters;
  std::vector<SortSpec> sorts = {{0, SortDirection::kAscending}};
  RunBytecodeTest(cols, filters, {}, sorts, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=4, indices_register=Register(2)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, PlanQuery_MinOptimizationNotAppliedNullable) {
  auto bv = impl::BitVector::CreateWithSize(0);
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{},
      impl::NullStorage{impl::NullStorage::SparseNull{std::move(bv), {}}},
      Unsorted{}, HasDuplicates{}});

  std::vector<FilterSpec> filters;
  std::vector<DistinctSpec> distinct_specs;
  std::vector<SortSpec> sort_specs = {{0, SortDirection::kAscending}};
  LimitSpec limit_spec;
  limit_spec.limit = 1;

  std::string expected_bytecode = R"(
    InitRange: [size=0, dest_register=Register(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(1), dest_span_register=Register(2)]
    Iota: [source_register=Register(0), update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(3)]
    PrefixPopcount: [col=0, dest_register=Register(4)]
    CopyToRowLayout<Uint32, SparseNull>: [col=0, source_indices_register=Register(2), dest_buffer_register=Register(3), row_layout_offset=0, row_layout_stride=5, invert_copied_bits=0, popcount_register=Register(4), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(3), total_row_stride=5, indices_register=Register(2)]
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
  df.Finalize();

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
  df.Finalize();
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

TEST_F(DataframeBytecodeTest, PlanQuery_SingleColIndex_EqFilter_DenseNullInt) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col_dense_nullable"},
      CreateTypedColumnSpec(Uint32(), DenseNull(), Unsorted()));

  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &string_pool_);
  df.InsertUnchecked(kSpec, std::make_optional(10u));
  df.InsertUnchecked(kSpec, std::nullopt);
  df.InsertUnchecked(kSpec, std::make_optional(20u));
  df.InsertUnchecked(kSpec, std::make_optional(10u));
  df.Finalize();
  df.AddIndex(Index({0}, std::make_shared<std::vector<uint32_t>>(
                             std::vector<uint32_t>{1, 0, 3, 2})));

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  std::string expected_bytecode = R"(
    InitRange: [size=4, dest_register=Register(0)]
    IndexPermutationVectorToSpan: [index=0, write_register=Register(1)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(2), op=NonNullOp(0)]
    IndexedFilterEq<Uint32, DenseNull>: [col=0, filter_value_reg=Register(2), popcount_register=Register(3), update_register=Register(1)]
    AllocateIndices: [size=4, dest_slab_register=Register(4), dest_span_register=Register(5)]
    CopySpanIntersectingRange: [source_register=Register(1), source_range_register=Register(0), update_register=Register(5)]
    AllocateIndices: [size=8, dest_slab_register=Register(6), dest_span_register=Register(7)]
    StrideCopy: [source_register=Register(5), update_register=Register(7), stride=2]
    StrideCopyDenseNullIndices: [col=0, update_register=Register(7), offset=1, stride=2]
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
  df.Finalize();

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

TEST_F(DataframeBytecodeTest, PlanQuery_LinearFilterEq_NonNullUint32) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  // Expect LinearFilterEq because:
  // 1. Input is a Range (initially).
  // 2. Operation is Eq.
  // 3. Column is NonNull.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(1), popcount_register=Register(4294967295), source_register=Register(0), update_register=Register(3)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, PlanQuery_LinearFilterEq_NonNullString) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::String{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<String>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    LinearFilterEq<String>: [col=0, filter_value_reg=Register(1), popcount_register=Register(4294967295), source_register=Register(0), update_register=Register(3)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest,
       PlanQuery_NoLinearFilterEq_IfInputNotRangeAfterSortedFilter) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                   IdSorted{},
                   NoDuplicates{}},  // col0, sorted, used to make input a Span
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}}  // col1, target for filter
  );
  std::vector<FilterSpec> filters = {
      {0, 0, Gt{}, std::nullopt},  // This filter makes indices_reg_ a Span
      {1, 1, Eq{}, std::nullopt}   // This should use NonStringFilter
  };
  // After the Gt filter on col0, indices_reg_ will be a Span (materialized by
  // Iota). So, the Eq filter on col1 should use NonStringFilter, not
  // LinearFilterEq.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Id>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
    SortedFilter<Id, UpperBound>: [col=0, val_register=Register(1), update_register=Register(0), write_result_to=BoundModifier(1)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(1), write_register=Register(2), op=NonNullOp(0)]
    AllocateIndices: [size=0, dest_slab_register=Register(3), dest_span_register=Register(4)]
    LinearFilterEq<Uint32>: [col=1, filter_value_reg=Register(2), popcount_register=Register(4294967295), source_register=Register(0), update_register=Register(4)]
  )",
                  /*cols_used=*/3);  // 0b11
}

TEST_F(DataframeBytecodeTest, PlanQuery_NoLinearFilterEq_IfNotEqOperator) {
  std::vector<impl::Column> cols = MakeColumnVector(
      impl::Column{impl::Storage::Uint32{}, impl::NullStorage::NonNull{},
                   Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Gt{}, std::nullopt}};  // Not Eq
  // Should use NonStringFilter because op is Gt, not Eq.
  RunBytecodeTest(cols, filters, {}, {}, {}, R"(
    InitRange: [size=0, dest_register=Register(0)]
    CastFilterValue<Uint32>: [fval_handle=FilterValue(0), write_register=Register(1), op=NonNullOp(4)]
    AllocateIndices: [size=0, dest_slab_register=Register(2), dest_span_register=Register(3)]
    Iota: [source_register=Register(0), update_register=Register(3)]
    NonStringFilter<Uint32, Gt>: [col=0, val_register=Register(1), source_register=Register(3), update_register=Register(3)]
  )",
                  /*cols_used=*/1);
}

TEST_F(DataframeBytecodeTest, PlanQuery_NoLinearFilterEq_IfNullableColumn) {
  std::vector<impl::Column> cols = MakeColumnVector(impl::Column{
      impl::Storage::Uint32{}, impl::NullStorage::SparseNull{},  // Nullable
      Unsorted{}, HasDuplicates{}});
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, std::nullopt}};
  // Should use NonStringFilter because column is nullable.
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
    AllocateIndices: [size=0, dest_slab_register=Register(7), dest_span_register=Register(8)]
    StrideCopy: [source_register=Register(3), update_register=Register(8), stride=2]
    StrideTranslateAndCopySparseNullIndices: [col=0, popcount_register=Register(6), update_register=Register(8), offset=1, stride=2]
  )",
                  /*cols_used=*/1);
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
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
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

TEST(DataframeTest, SetCellUncheckedInternal_SparseNullWithPopcount) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"col_sparse_uint32", "col_sparse_str"},
      CreateTypedColumnSpec(Uint32(), SparseNullWithPopcountAlways(),
                            Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountUntilFinalization(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  // Initial values:
  // Row 0: {100, "aa"}
  // Row 1: {null, null}
  // Row 2: {200, "bb"}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  df.InsertUnchecked(kSpec, std::make_optional(100u),
                     std::make_optional(pool.InternString("aa")));
  df.InsertUnchecked(kSpec, std::nullopt, std::nullopt);
  df.InsertUnchecked(kSpec, std::make_optional(200u),
                     std::make_optional(pool.InternString("bb")));
  df.InsertUnchecked(kSpec, std::nullopt,
                     std::make_optional(pool.InternString("cc")));
  df.InsertUnchecked(kSpec, std::make_optional(300u), std::nullopt);

  // Verify initial state
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(100u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 2), std::make_optional(200u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 2),
            std::make_optional(pool.InternString("bb")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 3), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4), std::nullopt);

  // Test 1: Set existing non-null to new non-null
  // Row 0: {100, "aa"} -> {101, "new_aa"}
  df.SetCellUnchecked<0>(kSpec, 0, std::make_optional(101u));
  df.SetCellUnchecked<1>(kSpec, 0,
                         std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));

  // Test 2: Set existing non-null to null (triggers memmove for deletion)
  // Row 2: {200, "bb"} -> {null, null}
  // Expected data after:
  // Row 0: {101, "new_aa"}
  // Row 1: {null, null}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [101, 300]
  // Sparse String data: ["new_aa", "cc"]
  df.SetCellUnchecked<0>(kSpec, 2, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 2, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 2), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 2), std::nullopt);
  // Check surrounding values are not affected
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 3: Set existing null to non-null (triggers memmove for insertion)
  // Row 1: {null, null} -> {150, "dd"}
  // Expected data after:
  // Row 0: {101, "new_aa"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [101, 150, 300]
  // Sparse String data: ["new_aa", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 1, std::make_optional(150u));
  df.SetCellUnchecked<1>(kSpec, 1, std::make_optional(pool.InternString("dd")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::make_optional(150u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1),
            std::make_optional(pool.InternString("dd")));
  // Check surrounding values
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(101u));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(300u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("new_aa")));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 4: Set at the beginning - null to non-null
  // Row 0 was {101, "new_aa"}. Set to {50, "start"}
  // This is effectively an update, not an insertion in sparse terms if it was
  // already non-null. Let's make Row 0 null first to test insertion at
  // beginning. Row 0: {101, "new_aa"} -> {null, null} Sparse Uint32 data: [150,
  // 300] Sparse String data: ["dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0), std::nullopt);

  // Now set Row 0 from null to non-null: {null, null} -> {50, "start"}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {300, null}
  // Sparse Uint32 data: [50, 150, 300]
  // Sparse String data: ["start", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 0, std::make_optional(50u));
  df.SetCellUnchecked<1>(kSpec, 0,
                         std::make_optional(pool.InternString("start")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 0), std::make_optional(50u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 0),
            std::make_optional(pool.InternString("start")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1),
            std::make_optional(150u));  // Check next element
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 1),
            std::make_optional(pool.InternString("dd")));

  // Test 5: Set at the end - null to non-null
  // Row 4 was {300, null}. Let's make it {null, null} first.
  // Sparse Uint32 data: [50, 150]
  // Sparse String data: ["start", "dd", "cc"] (Row 4 string was already null)
  df.SetCellUnchecked<0>(kSpec, 4, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::nullopt);

  // Now set Row 4 from {null, null} -> {400, "end"}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {400, "end"}
  // Sparse Uint32 data: [50, 150, 400]
  // Sparse String data: ["start", "dd", "cc", "end"]
  df.SetCellUnchecked<0>(kSpec, 4, std::make_optional(400u));
  df.SetCellUnchecked<1>(kSpec, 4,
                         std::make_optional(pool.InternString("end")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::make_optional(400u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4),
            std::make_optional(pool.InternString("end")));
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1),
            std::make_optional(150u));  // Check previous element
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 6: Set at the end - non-null to null
  // Row 4: {400, "end"} -> {null, null}
  // Expected data after:
  // Row 0: {50, "start"}
  // Row 1: {150, "dd"}
  // Row 2: {null, null}
  // Row 3: {null, "cc"}
  // Row 4: {null, null}
  // Sparse Uint32 data: [50, 150]
  // Sparse String data: ["start", "dd", "cc"]
  df.SetCellUnchecked<0>(kSpec, 4, std::nullopt);
  df.SetCellUnchecked<1>(kSpec, 4, std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 4), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 4), std::nullopt);
  ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, 1), std::make_optional(150u));
  ASSERT_EQ(df.GetCellUnchecked<1>(kSpec, 3),
            std::make_optional(pool.InternString("cc")));

  // Test 7: Operations on an empty column (implicitly tested by starting with
  // empty and inserting) Create a new dataframe for this.
  Dataframe df_empty = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df_empty.InsertUnchecked(kSpec, std::nullopt,
                           std::nullopt);  // Row 0: {null, null}
  df_empty.SetCellUnchecked<0>(kSpec, 0, std::make_optional(1u));
  ASSERT_EQ(df_empty.GetCellUnchecked<0>(kSpec, 0), std::make_optional(1u));
  df_empty.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df_empty.GetCellUnchecked<0>(kSpec, 0), std::nullopt);

  // Test 8: Full column (all non-null), then set to null
  Dataframe df_full = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df_full.InsertUnchecked(kSpec, std::make_optional(10u),
                          std::make_optional(pool.InternString("f1")));
  df_full.InsertUnchecked(kSpec, std::make_optional(20u),
                          std::make_optional(pool.InternString("f2")));
  df_full.SetCellUnchecked<0>(kSpec, 0, std::nullopt);
  ASSERT_EQ(df_full.GetCellUnchecked<0>(kSpec, 0), std::nullopt);
  ASSERT_EQ(df_full.GetCellUnchecked<0>(kSpec, 1), std::make_optional(20u));
  ASSERT_EQ(
      df_full.GetCellUnchecked<1>(kSpec, 0),
      std::make_optional(pool.InternString("f1")));  // String col unaffected
}

TEST(DataframeTest, InsertAndSet_WordBoundaryStress) {
  StringPool pool;
  constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sparse_col_uint32"},
      CreateTypedColumnSpec(Uint32(), SparseNullWithPopcountAlways(),
                            Unsorted()));

  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  std::vector<std::optional<uint32_t>> current_state;

  const uint32_t kMaxRows = 130;  // Crosses two 64-bit boundaries

  uint32_t critical_indices_arr[] = {0,   1,   62,  63,  64,          65,
                                     126, 127, 128, 129, kMaxRows - 1};
  std::set<uint32_t> critical_indices(
      critical_indices_arr,
      critical_indices_arr + (sizeof(critical_indices_arr) / sizeof(uint32_t)));

  auto verify_state = [&]() {
    ASSERT_EQ(df.row_count(), current_state.size()) << "Row count mismatch.";
    for (uint32_t i = 0; i < current_state.size(); ++i) {
      ASSERT_EQ(df.GetCellUnchecked<0>(kSpec, i), current_state[i])
          << "Mismatch at index " << i;
    }
  };

  // Phase 1: Insertions (stressing InsertUncheckedInternal)
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    std::optional<uint32_t> val_to_insert;
    if (i % 3 == 0) {  // Introduce some nulls
      val_to_insert = std::nullopt;
    } else {
      val_to_insert = std::make_optional(i * 10);
    }

    df.InsertUnchecked(kSpec, val_to_insert);
    current_state.push_back(val_to_insert);

    if (critical_indices.count(i)) {
      verify_state();
    }
  }
  verify_state();  // Final verification after all insertions

  // Phase 2: Set Operations (stressing SetCellUncheckedInternal)

  // Test 2.1: Non-null -> Null at critical indices
  // Setup: Ensure all elements are non-null to robustly test the transition.
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    if (!current_state[i].has_value()) {
      current_state[i] =
          std::make_optional((i * 10) + 77);  // Arbitrary non-null
      df.SetCellUnchecked<0>(kSpec, i, current_state[i]);
    }
  }
  verify_state();  // Verify setup

  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is non-null due to setup.
    current_state[idx] = std::nullopt;
    df.SetCellUnchecked<0>(kSpec, idx, std::nullopt);
    verify_state();
  }

  // Test 2.2: Null -> Non-null at critical indices
  // Setup: Ensure all elements are null.
  for (uint32_t i = 0; i < kMaxRows; ++i) {
    if (current_state[i].has_value()) {
      current_state[i] = std::nullopt;
      df.SetCellUnchecked<0>(kSpec, i, std::nullopt);
    }
  }
  verify_state();  // Verify setup

  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is null.
    uint32_t new_val = (idx * 100) + 1;
    current_state[idx] = std::make_optional(new_val);
    df.SetCellUnchecked<0>(kSpec, idx, std::make_optional(new_val));
    verify_state();
  }

  // Test 2.3: Non-null -> Different Non-null at critical indices
  // Setup: Ensure all elements are non-null (they are from the previous step).
  for (uint32_t idx : critical_indices) {
    // Precondition: current_state[idx] is non-null.
    uint32_t new_val = current_state[idx].value() + 55;
    current_state[idx] = std::make_optional(new_val);
    df.SetCellUnchecked<0>(kSpec, idx, std::make_optional(new_val));
    verify_state();
  }

  // Test 2.4: Sequence of operations crossing a boundary (e.g., 60-70)
  uint32_t seq_start = 60;
  uint32_t seq_end = 70;

  // Sequence 2.4.1: Make the range [seq_start, seq_end] all non-null
  for (uint32_t i = seq_start; i <= seq_end; ++i) {
    uint32_t val = (i * 300) + 3;
    current_state[i] = std::make_optional(val);
    df.SetCellUnchecked<0>(kSpec, i, std::make_optional(val));
    verify_state();
  }

  // Sequence 2.4.2: Make the range [seq_start, seq_end] all null
  for (uint32_t i = seq_start; i <= seq_end; ++i) {
    current_state[i] = std::nullopt;
    df.SetCellUnchecked<0>(kSpec, i, std::nullopt);
    verify_state();
  }

  // Test 2.5: A mix of operations in a specific boundary region (e.g. 63, 64,
  // 65) Setup: 63=non-null, 64=null, 65=non-null
  current_state[63] = std::make_optional(6300u);
  df.SetCellUnchecked<0>(kSpec, 63, current_state[63]);
  current_state[64] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 64, current_state[64]);
  current_state[65] = std::make_optional(6500u);
  df.SetCellUnchecked<0>(kSpec, 65, current_state[65]);
  verify_state();

  // Test: Flip them - 63=null
  current_state[63] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 63, current_state[63]);
  verify_state();

  // Test: Flip them - 64=non-null
  current_state[64] = std::make_optional(6401u);
  df.SetCellUnchecked<0>(kSpec, 64, current_state[64]);
  verify_state();

  // Test: Flip them - 65=null
  current_state[65] = std::nullopt;
  df.SetCellUnchecked<0>(kSpec, 65, current_state[65]);
  verify_state();
}

TEST(DataframeTest, TypedCursor) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"id", "col2", "col3", "col4"},
      CreateTypedColumnSpec(Id(), NonNull(), IdSorted()),
      CreateTypedColumnSpec(Uint32(), NonNull(), Unsorted()),
      CreateTypedColumnSpec(Int64(), DenseNull(), Unsorted()),
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);

  TypedCursor cursor(&df, {FilterSpec{0, 0, Eq{}, {}}}, {});
  {
    cursor.SetFilterValueUnchecked(0, int64_t(0l));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(kSpec), 0u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 10u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(kSpec), 0l);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(kSpec), pool.InternString("foo"));
    cursor.Next();
    ASSERT_TRUE(cursor.Eof());
  }
  {
    cursor.SetFilterValueUnchecked(0, int64_t(1l));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<0>(kSpec), 1u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<2>(kSpec), std::nullopt);
    ASSERT_EQ(cursor.GetCellUnchecked<3>(kSpec), std::nullopt);
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
      CreateTypedColumnSpec(String(), SparseNullWithPopcountAlways(),
                            Unsorted()));
  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.InsertUnchecked(kSpec, std::monostate(), 10u, std::make_optional(0l),
                     std::make_optional(pool.InternString("foo")));
  df.InsertUnchecked(kSpec, std::monostate(), 20u, std::nullopt, std::nullopt);
  {
    TypedCursor cursor(&df, {}, {});
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 10u);
    cursor.SetCellUnchecked<1>(kSpec, 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  }
  {
    TypedCursor cursor(&df, {FilterSpec{1, 0, Eq{}, {}}}, {});
    cursor.SetFilterValueUnchecked(0, int64_t(20));
    cursor.ExecuteUnchecked();
    ASSERT_FALSE(cursor.Eof());
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
    cursor.SetCellUnchecked<1>(kSpec, 20u);
    ASSERT_EQ(cursor.GetCellUnchecked<1>(kSpec), 20u);
  }
}

TEST(DataframeTest,
     QueryPlanEqualityFilterOnNoDuplicatesColumnEstimatesOneRow) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"unique_int_col", "other_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(),
                            NoDuplicates{}),  // Target column with NoDuplicates
      CreateTypedColumnSpec(Int64(), NonNull(), Unsorted(),
                            HasDuplicates{})  // Other column
  );

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  // Insert unique, non-null data into the first column
  df.InsertUnchecked(kSpec, int64_t{10}, int64_t{100});
  df.InsertUnchecked(kSpec, int64_t{20}, int64_t{200});
  df.InsertUnchecked(kSpec, int64_t{30}, int64_t{300});
  df.Finalize();

  // Plan a query with an equality filter on the "unique_int_col".
  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  LimitSpec limit_spec;

  ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                       df.PlanQuery(filters, {}, {}, limit_spec, 1u));

  // Assert that the estimated_row_count and max_row_count are 1.
  EXPECT_EQ(plan.GetImplForTesting().params.estimated_row_count, 1u);
  EXPECT_EQ(plan.GetImplForTesting().params.max_row_count, 1u);
}

TEST(DataframeTest, SortedFilterWithDuplicatesAndRowCountOfOne) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);

  df.InsertUnchecked(kSpec, int64_t{10});
  df.InsertUnchecked(kSpec, int64_t{20});
  df.InsertUnchecked(kSpec, int64_t{20});
  df.Finalize();

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                       df.PlanQuery(filters, {}, {}, {}, 1u));
  EXPECT_EQ(plan.GetImplForTesting().params.estimated_row_count, 1u);
}

TEST(DataframeTest, SortedFilterWithDuplicatesAndRowCountOfZero) {
  static constexpr auto kSpec = CreateTypedDataframeSpec(
      {"sorted_col"},
      CreateTypedColumnSpec(Int64(), NonNull(), Sorted{}, HasDuplicates{}));

  StringPool pool;
  Dataframe df = Dataframe::CreateFromTypedSpec(kSpec, &pool);
  df.Finalize();

  std::vector<FilterSpec> filters = {{0, 0, Eq{}, int64_t{20}}};
  ASSERT_OK_AND_ASSIGN(Dataframe::QueryPlan plan,
                       df.PlanQuery(filters, {}, {}, {}, 1u));
  EXPECT_EQ(plan.GetImplForTesting().params.estimated_row_count, 0u);
}

}  // namespace perfetto::trace_processor::dataframe
