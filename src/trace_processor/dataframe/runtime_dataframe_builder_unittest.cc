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

#include "src/trace_processor/dataframe/runtime_dataframe_builder.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/cursor.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {

inline void PrintTo(const Dataframe::ColumnSpec& spec, std::ostream* os) {
  *os << "\n  ColumnSpec{\n"
      << "    name: \"" << spec.name << "\",\n"
      << "    type: " << spec.type.ToString() << ",\n"
      << "    nullability: " << spec.nullability.ToString() << ",\n"
      << "    sort_state: " << spec.sort_state.ToString() << "\n"
      << "  }";
}

inline bool operator==(const Dataframe::ColumnSpec& lhs,
                       const Dataframe::ColumnSpec& rhs) {
  return lhs.name == rhs.name && lhs.type == rhs.type &&
         lhs.nullability == rhs.nullability && lhs.sort_state == rhs.sort_state;
}

namespace {

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::SizeIs;

struct TestRowFetcher : ValueFetcher {
  using Value = std::variant<std::nullopt_t, int64_t, double, const char*>;
  enum Type : uint8_t {
    kNull,
    kInt64,
    kDouble,
    kString,
  };

  void SetRow(const std::vector<Value>& row_data) { current_row_ = &row_data; }

  Type GetValueType(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    const auto& var = (*current_row_)[index];
    if (std::holds_alternative<std::nullopt_t>(var))
      return Type::kNull;
    if (std::holds_alternative<int64_t>(var))
      return Type::kInt64;
    if (std::holds_alternative<double>(var))
      return Type::kDouble;
    if (std::holds_alternative<const char*>(var))
      return Type::kString;
    PERFETTO_FATAL("Invalid variant state");
  }

  int64_t GetInt64Value(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<int64_t>((*current_row_)[index]);
  }

  double GetDoubleValue(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<double>((*current_row_)[index]);
  }

  const char* GetStringValue(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<const char*>((*current_row_)[index]);
  }

 private:
  const std::vector<Value>* current_row_ = nullptr;
};

// Callback for verifying cell values from the cursor.
// Stores visited values in variants for later checking.
struct ValueVerifier : CellCallback {
  using ValueVariant = std::variant<uint32_t,
                                    int32_t,
                                    int64_t,
                                    double,
                                    NullTermStringView,
                                    nullptr_t>;

  void Fetch(Cursor<TestRowFetcher>* cursor, uint32_t col_count) {
    for (uint32_t i = 0; i < col_count; ++i) {
      cursor->Cell(i, *this);
    }
  }
  void OnCell(int64_t value) { values.emplace_back(value); }
  void OnCell(double value) { values.emplace_back(value); }
  void OnCell(NullTermStringView value) { values.emplace_back(value); }
  void OnCell(nullptr_t) { values.emplace_back(nullptr); }
  void OnCell(int32_t value) { values.emplace_back(value); }
  void OnCell(uint32_t value) { values.emplace_back(value); }

  std::vector<ValueVariant> values;
};

class DataframeBuilderTest : public ::testing::Test {
 protected:
  base::StatusOr<Dataframe> BuildDf(
      const std::vector<std::string>& names,
      const std::vector<std::vector<TestRowFetcher::Value>>& rows) {
    RuntimeDataframeBuilder builder(names, &pool_);
    TestRowFetcher fetcher;
    for (const auto& row_data : rows) {
      fetcher.SetRow(row_data);
      if (!builder.AddRow(&fetcher)) {
        return builder.status();
      }
    }
    return std::move(builder).Build();
  }

  static void VerifyData(
      Dataframe& df,
      uint64_t cols_bitmap,
      const std::vector<std::vector<ValueVerifier::ValueVariant>>& expected) {
    std::vector<FilterSpec> filter_specs;
    auto num_cols_selected =
        static_cast<uint32_t>(PERFETTO_POPCOUNT(cols_bitmap));
    ASSERT_OK_AND_ASSIGN(auto plan, df.PlanQuery(filter_specs, cols_bitmap));

    TestRowFetcher execute_fetcher;
    std::optional<Cursor<TestRowFetcher>> cursor;
    df.PrepareCursor(std::move(plan), cursor);
    cursor->Execute(execute_fetcher);

    size_t row_index = 0;
    for (const auto& row : expected) {
      ValueVerifier verifier;
      ASSERT_FALSE(cursor->Eof())
          << "Cursor finished early at row " << row_index;
      verifier.Fetch(&cursor.value(), num_cols_selected);
      EXPECT_THAT(verifier.values, ElementsAreArray(row))
          << "Mismatch in data for row " << row_index;
      cursor->Next();
      row_index++;
    }
    ASSERT_TRUE(cursor->Eof())
        << "Cursor has more rows than expected. Expected " << expected.size()
        << " rows.";
    ASSERT_EQ(row_index, expected.size())
        << "Mismatch in number of rows processed.";
  }

  StringPool pool_;
};

template <typename... Args>
std::vector<ValueVerifier::ValueVariant> Row(Args&&... args) {
  return std::vector<ValueVerifier::ValueVariant>{
      ValueVerifier::ValueVariant(std::forward<Args>(args))...};
}

template <typename... Args>
std::vector<std::vector<ValueVerifier::ValueVariant>> Rows(Args&&... rows) {
  return std::vector<std::vector<ValueVerifier::ValueVariant>>{
      std::forward<Args>(rows)...};
}

// Test building an empty dataframe with no columns and no rows.
TEST_F(DataframeBuilderTest, BuildEmpty) {
  RuntimeDataframeBuilder builder({}, &pool_);
  base::StatusOr<Dataframe> df = std::move(builder).Build();
  ASSERT_OK(df.status());

  std::vector<Dataframe::ColumnSpec> specs = df->CreateColumnSpecs();
  ASSERT_THAT(specs, IsEmpty());
}

TEST_F(DataframeBuilderTest, AddSingleRowSimple) {
  base::StatusOr<Dataframe> df = BuildDf({"int_col", "double_col", "str_col"},
                                         {{int64_t{123}, 45.6, "hello"}});
  ASSERT_OK(df.status());
  ASSERT_THAT(
      df->CreateColumnSpecs(),
      ElementsAre(
          Dataframe::ColumnSpec{"int_col", Uint32{}, NonNull{}, Sorted{}},
          Dataframe::ColumnSpec{"double_col", Double{}, NonNull{}, Sorted{}},
          Dataframe::ColumnSpec{"str_col", String{}, NonNull{}, Sorted{}}));
  VerifyData(*df, 7,
             Rows(Row(uint32_t{123}, 45.6, NullTermStringView{"hello"})));
}

TEST_F(DataframeBuilderTest, AddMultipleRowsConsistentTypes) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"a", "b"}, {{int64_t{10}, "A"}, {int64_t{20}, "B"}, {int64_t{5}, "C"}});
  ASSERT_OK(df.status());
  ASSERT_THAT(
      df->CreateColumnSpecs(),
      ElementsAre(Dataframe::ColumnSpec{"a", Uint32{}, NonNull{}, Unsorted{}},
                  Dataframe::ColumnSpec{"b", String{}, NonNull{}, Sorted{}}));
  VerifyData(*df, 3,
             Rows(Row(uint32_t{10}, NullTermStringView{"A"}),
                  Row(uint32_t{20}, NullTermStringView{"B"}),
                  Row(uint32_t{5}, NullTermStringView{"C"})));
}

TEST_F(DataframeBuilderTest, AddRowsWithNulls) {
  base::StatusOr<Dataframe> df =
      BuildDf({"nullable_int", "nullable_str", "non_null"},
              {{int64_t{1}, std::nullopt, 1.1},
               {std::nullopt, "A", 2.2},
               {int64_t{3}, "B", 3.3},
               {int64_t{2}, std::nullopt, 4.4}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"nullable_int", Uint32{},
                                                SparseNull{}, Unsorted{}},
                          Dataframe::ColumnSpec{"nullable_str", String{},
                                                SparseNull{}, Unsorted{}},
                          Dataframe::ColumnSpec{"non_null", Double{}, NonNull{},
                                                Sorted{}}));
  VerifyData(*df, 7,
             Rows(Row(uint32_t{1}, nullptr, 1.1),
                  Row(nullptr, NullTermStringView{"A"}, 2.2),
                  Row(uint32_t{3}, NullTermStringView{"B"}, 3.3),
                  Row(uint32_t{2}, nullptr, 4.4)));
}

TEST_F(DataframeBuilderTest, AddRowTypeMismatch) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - infers type as Int64
  std::vector<TestRowFetcher::Value> row1 = {{{int64_t{100}}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - attempt to add a double to the int64 column
  std::vector<TestRowFetcher::Value> row2 = {{{200.5}}};
  fetcher.SetRow(row2);
  ASSERT_FALSE(builder.AddRow(&fetcher));
  ASSERT_FALSE(builder.status().ok());

  // Optional: Check error message content
  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("Type mismatch in column 'col_a'"));
  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("Existing type != fetched type"));

  // Attempting to build after an error should also fail
  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_FALSE(df_status.ok());
  EXPECT_THAT(df_status.status().message(),
              testing::HasSubstr("Type mismatch in column 'col_a'"));
}

TEST_F(DataframeBuilderTest, BuildIntegerDowncasting) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"should_be_uint32", "should_be_int32", "should_be_int64"},
      {{int64_t{0}, int64_t{-10}, int64_t{3'000'000'000LL}},
       {int64_t{100}, int64_t{0}, int64_t{4'000'000'000LL}},
       {int64_t{4'000'000'000LL}, int64_t{1'000'000'000}, int64_t{10LL}},
       {int64_t{50}, int64_t{-2'000'000'000}, int64_t{-5'000'000'000LL}}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"should_be_uint32", Uint32{},
                                                NonNull{}, Unsorted{}},
                          Dataframe::ColumnSpec{"should_be_int32", Int32{},
                                                NonNull{}, Unsorted{}},
                          Dataframe::ColumnSpec{"should_be_int64", Int64{},
                                                NonNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, BuildIdColumn) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"id_col"}, {{int64_t{0}}, {int64_t{1}}, {int64_t{2}}, {int64_t{3}}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"id_col", Id{}, NonNull{},
                                                IdSorted{}}));
  VerifyData(*df, 1,
             Rows(Row(uint32_t{0}), Row(uint32_t{1}), Row(uint32_t{2}),
                  Row(uint32_t{3})));
}

TEST_F(DataframeBuilderTest, BuildSetIdSortedColumn) {
  base::StatusOr<Dataframe> df = BuildDf({"setid_col"}, {{int64_t{0}},
                                                         {int64_t{0}},
                                                         {int64_t{2}},
                                                         {int64_t{2}},
                                                         {int64_t{2}},
                                                         {int64_t{5}},
                                                         {int64_t{5}},
                                                         {int64_t{7}}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"setid_col", Uint32{},
                                                NonNull{}, SetIdSorted{}}));
}

TEST_F(DataframeBuilderTest, BuildSetIdSortedViolated) {
  base::StatusOr<Dataframe> df =
      BuildDf({"not_setid_col"},
              {{int64_t{0}}, {int64_t{0}}, {int64_t{2}}, {int64_t{1}}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"not_setid_col", Uint32{},
                                                NonNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, InferTypeAfterNull) {
  base::StatusOr<Dataframe> df =
      BuildDf({"int_col", "str_col"}, {{std::nullopt, std::nullopt},
                                       {int64_t{999}, "world"},
                                       {int64_t{888}, std::nullopt}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"int_col", Uint32{},
                                                SparseNull{}, Unsorted{}},
                          Dataframe::ColumnSpec{"str_col", String{},
                                                SparseNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, BuildIntegerNoDowncast) {
  int64_t int32_max = std::numeric_limits<int32_t>::max();
  int64_t int32_min = std::numeric_limits<int32_t>::min();
  int64_t uint32_max = std::numeric_limits<uint32_t>::max();

  base::StatusOr<Dataframe> df =
      BuildDf({"col_a", "col_b"}, {{int64_t{int32_max + 1}, std::nullopt},
                                   {int64_t{uint32_max + 1}, std::nullopt},
                                   {int64_t{int32_min - 1}, std::nullopt}});
  ASSERT_OK(df.status());
  ASSERT_THAT(
      df->CreateColumnSpecs(),
      ElementsAre(
          Dataframe::ColumnSpec{"col_a", Int64{}, NonNull{}, Unsorted{}},
          Dataframe::ColumnSpec{"col_b", Uint32{}, SparseNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, BuildAllNullColumn) {
  base::StatusOr<Dataframe> df =
      BuildDf({"non_null_col", "all_null_col"}, {{int64_t{0}, std::nullopt},
                                                 {int64_t{1}, std::nullopt},
                                                 {int64_t{2}, std::nullopt}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"non_null_col", Id{}, NonNull{},
                                                IdSorted{}},
                          Dataframe::ColumnSpec{"all_null_col", Uint32{},
                                                SparseNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, BuildSortStateUnsortedAfterNull) {
  base::StatusOr<Dataframe> df =
      BuildDf({"sorted_then_null"},
              {{int64_t{10}}, {int64_t{20}}, {std::nullopt}, {int64_t{30}}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"sorted_then_null", Uint32{},
                                                SparseNull{}, Unsorted{}}));
}

TEST_F(DataframeBuilderTest, BuildEmptyColumn) {
  base::StatusOr<Dataframe> df =
      BuildDf({"populated_col", "empty_col"}, {{int64_t{10}, std::nullopt},
                                               {int64_t{20}, std::nullopt},
                                               {int64_t{30}, std::nullopt}});
  ASSERT_OK(df.status());
  ASSERT_THAT(df->CreateColumnSpecs(),
              ElementsAre(Dataframe::ColumnSpec{"populated_col", Uint32{},
                                                NonNull{}, Sorted{}},
                          Dataframe::ColumnSpec{"empty_col", Uint32{},
                                                SparseNull{}, Unsorted{}}));
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
