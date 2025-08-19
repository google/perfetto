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
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/dataframe_test_utils.h"
#include "src/trace_processor/dataframe/specs.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {

inline void PrintTo(const ColumnSpec& spec, std::ostream* os) {
  *os << "\n  ColumnSpec{\n"
      << "    type: " << spec.type.ToString() << ",\n"
      << "    nullability: " << spec.nullability.ToString() << ",\n"
      << "    sort_state: " << spec.sort_state.ToString() << ",\n"
      << "    duplicate_state: " << spec.duplicate_state.ToString() << "\n"
      << "  }";
}

inline bool operator==(const ColumnSpec& lhs, const ColumnSpec& rhs) {
  return lhs.type == rhs.type && lhs.nullability == rhs.nullability &&
         lhs.sort_state == rhs.sort_state &&
         lhs.duplicate_state == rhs.duplicate_state;
}

namespace {

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::SizeIs;

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

  StringPool pool_;
};

// Test building an empty dataframe with no columns and no rows.
TEST_F(DataframeBuilderTest, BuildEmpty) {
  RuntimeDataframeBuilder builder({}, &pool_);
  base::StatusOr<Dataframe> df = std::move(builder).Build();
  ASSERT_OK(df.status());

  auto spec = df->CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
  ASSERT_THAT(spec.column_names, ElementsAre("_auto_id"));
}

TEST_F(DataframeBuilderTest, AddSingleRowSimple) {
  base::StatusOr<Dataframe> df = BuildDf({"int_col", "double_col", "str_col"},
                                         {{int64_t{123}, 45.6, "hello"}});
  ASSERT_OK(df.status());

  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names,
              ElementsAre("int_col", "double_col", "str_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Double{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{String{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
  VerifyData(*df, 7,
             Rows(Row(uint32_t{123}, 45.6, NullTermStringView{"hello"})));
}

TEST_F(DataframeBuilderTest, AddMultipleRowsConsistentTypes) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"a", "b"}, {{int64_t{10}, "A"}, {int64_t{20}, "B"}, {int64_t{5}, "C"}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("a", "b", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Unsorted{}, NoDuplicates{}},
                  ColumnSpec{String{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
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
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("nullable_int", "nullable_str",
                                             "non_null", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Double{}, NonNull{}, Sorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
  VerifyData(*df, 7,
             Rows(Row(uint32_t{1}, nullptr, 1.1),
                  Row(nullptr, NullTermStringView{"A"}, 2.2),
                  Row(uint32_t{3}, NullTermStringView{"B"}, 3.3),
                  Row(uint32_t{2}, nullptr, 4.4)));
}

TEST_F(DataframeBuilderTest,
       AddRowErrorWhenExistingIntNotRepresentableAsDouble) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - an int64 that cannot be perfectly represented as double.
  constexpr int64_t kNonRepresentableInt = (1LL << 53) + 1;
  std::vector<TestRowFetcher::Value> row1 = {{{kNonRepresentableInt}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - attempt to add a double. This should fail because the
  // existing int64 in the column cannot be converted to double without loss.
  std::vector<TestRowFetcher::Value> row2 = {{{200.5}}};
  fetcher.SetRow(row2);
  ASSERT_FALSE(builder.AddRow(&fetcher));
  ASSERT_FALSE(builder.status().ok());

  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("Unable to represent"));
  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("in column 'col_a'"));
  EXPECT_THAT(builder.status().message(), testing::HasSubstr("as a double"));

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_FALSE(df_status.ok());
  EXPECT_THAT(df_status.status().message(),
              testing::HasSubstr("Unable to represent"));
}

TEST_F(DataframeBuilderTest, AddRowErrorWhenAddingTooLargeIntToDoubleColumn) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - a double.
  std::vector<TestRowFetcher::Value> row1 = {{{100.5}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - an int64 that cannot be perfectly represented as double.
  constexpr int64_t kNonRepresentableInt = (1LL << 53) + 1;
  std::vector<TestRowFetcher::Value> row2 = {{{kNonRepresentableInt}}};
  fetcher.SetRow(row2);
  ASSERT_FALSE(builder.AddRow(&fetcher));
  ASSERT_FALSE(builder.status().ok());

  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("Inserting a too-large integer"));
  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("in column 'col_a'"));
  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("Column currently holds doubles"));

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_FALSE(df_status.ok());
  EXPECT_THAT(df_status.status().message(),
              testing::HasSubstr("Inserting a too-large integer"));
}

TEST_F(DataframeBuilderTest, AddRowErrorStringToInt) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - infers type as Int64
  std::vector<TestRowFetcher::Value> row1 = {{{int64_t{100}}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - attempt to add a string to the int64 column
  std::vector<TestRowFetcher::Value> row2 = {{{"string_val"}}};
  fetcher.SetRow(row2);
  ASSERT_FALSE(builder.AddRow(&fetcher));
  ASSERT_FALSE(builder.status().ok());

  EXPECT_THAT(builder.status().message(),
              testing::HasSubstr("column 'col_a' was inferred to be LONG, but "
                                 "later received a value of type STRING"));

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_FALSE(df_status.ok());
  EXPECT_THAT(df_status.status().message(),
              testing::HasSubstr("column 'col_a' was inferred to be LONG, but "
                                 "later received a value of type STRING"));
}

TEST_F(DataframeBuilderTest, AddRowPromoteIntColumnToDouble) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - an int64 that IS representable as double.
  std::vector<TestRowFetcher::Value> row1 = {{{int64_t{100}}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - a double.
  std::vector<TestRowFetcher::Value> row2 = {{{200.5}}};
  fetcher.SetRow(row2);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  base::StatusOr<Dataframe> df = std::move(builder).Build();
  ASSERT_OK(df.status());

  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("col_a", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Double{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
  VerifyData(*df, 1, Rows(Row(100.0), Row(200.5)));
}

TEST_F(DataframeBuilderTest, AddRowConvertNewIntToDoubleInDoubleColumn) {
  RuntimeDataframeBuilder builder({"col_a"}, &pool_);
  TestRowFetcher fetcher;

  // Add first row - a double.
  std::vector<TestRowFetcher::Value> row1 = {{{100.5}}};
  fetcher.SetRow(row1);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  // Add second row - an int64 that IS representable as double.
  std::vector<TestRowFetcher::Value> row2 = {{{int64_t{200}}}};
  fetcher.SetRow(row2);
  ASSERT_TRUE(builder.AddRow(&fetcher));
  ASSERT_TRUE(builder.status().ok());

  base::StatusOr<Dataframe> df = std::move(builder).Build();
  ASSERT_OK(df.status());

  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("col_a", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Double{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
  VerifyData(*df, 1, Rows(Row(100.5), Row(200.0)));
}

TEST_F(DataframeBuilderTest, BuildIntegerDowncasting) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"should_be_uint32", "should_be_int32", "should_be_int64"},
      {{int64_t{0}, int64_t{-10}, int64_t{3'000'000'000LL}},
       {int64_t{100}, int64_t{0}, int64_t{4'000'000'000LL}},
       {int64_t{4'000'000'000LL}, int64_t{1'000'000'000}, int64_t{10LL}},
       {int64_t{50}, int64_t{-2'000'000'000}, int64_t{-5'000'000'000LL}}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names,
              ElementsAre("should_be_uint32", "should_be_int32",
                          "should_be_int64", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Int32{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Int64{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, BuildIdColumn) {
  base::StatusOr<Dataframe> df = BuildDf(
      {"id_col"}, {{int64_t{0}}, {int64_t{1}}, {int64_t{2}}, {int64_t{3}}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("id_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
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
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("setid_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, NonNull{}, SetIdSorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, BuildSetIdSortedViolated) {
  base::StatusOr<Dataframe> df =
      BuildDf({"not_setid_col"},
              {{int64_t{0}}, {int64_t{0}}, {int64_t{2}}, {int64_t{1}}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("not_setid_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, InferTypeAfterNull) {
  base::StatusOr<Dataframe> df =
      BuildDf({"int_col", "str_col"}, {{std::nullopt, std::nullopt},
                                       {int64_t{999}, "world"},
                                       {int64_t{888}, std::nullopt}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("int_col", "str_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
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
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("col_a", "col_b", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Int64{}, NonNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, BuildAllNullColumn) {
  base::StatusOr<Dataframe> df =
      BuildDf({"non_null_col", "all_null_col"}, {{int64_t{0}, std::nullopt},
                                                 {int64_t{1}, std::nullopt},
                                                 {int64_t{2}, std::nullopt}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names,
              ElementsAre("non_null_col", "all_null_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}},
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, BuildSortStateUnsortedAfterNull) {
  base::StatusOr<Dataframe> df =
      BuildDf({"sorted_then_null"},
              {{int64_t{10}}, {int64_t{20}}, {std::nullopt}, {int64_t{30}}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("sorted_then_null", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, BuildEmptyColumn) {
  base::StatusOr<Dataframe> df =
      BuildDf({"populated_col", "empty_col"}, {{int64_t{10}, std::nullopt},
                                               {int64_t{20}, std::nullopt},
                                               {int64_t{30}, std::nullopt}});
  ASSERT_OK(df.status());
  auto spec = df->CreateSpec();
  ASSERT_THAT(spec.column_names,
              ElementsAre("populated_col", "empty_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, NonNull{}, Sorted{}, NoDuplicates{}},
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Int_NoDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_int"}, {{int64_t{10}}, {int64_t{20}}, {int64_t{30}}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Sorted{}, NoDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Int_HasDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_int"}, {{int64_t{10}}, {int64_t{20}}, {int64_t{10}}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Uint32{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Int_NegativeConsideredDuplicate) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_int"}, {{int64_t{-10}}, {int64_t{-20}}, {int64_t{-10}}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Int32{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Int_NullableBecomesHasDuplicates) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_int"}, {{int64_t{10}}, {std::nullopt}, {int64_t{30}}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Int_HasDuplicates_Nullable) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_int"}, {{int64_t{10}}, {std::nullopt}, {int64_t{10}}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Uint32{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Double_NoDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_double"}, {{10.0}, {20.0}, {30.0}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Double{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Double_HasDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_double"}, {{10.0}, {20.0}, {10.0}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{Double{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest,
       DuplicateState_Double_NullableBecomesHasDuplicates) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_double"}, {{10.0}, {std::nullopt}, {30.0}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Double{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_Double_HasDuplicates_Nullable) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_double"}, {{10.0}, {std::nullopt}, {10.0}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{Double{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_String_NoDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_str"}, {{"apple"}, {"banana"}, {"cherry"}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{String{}, NonNull{}, Sorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_String_HasDuplicates_NonNull) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_str"}, {{"apple"}, {"banana"}, {"apple"}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(ColumnSpec{String{}, NonNull{}, Unsorted{}, HasDuplicates{}},
                  ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest,
       DuplicateState_String_NullableBecomesHasDuplicates) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_str"}, {{"apple"}, {std::nullopt}, {"cherry"}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

TEST_F(DataframeBuilderTest, DuplicateState_String_HasDuplicates_Nullable) {
  base::StatusOr<Dataframe> df_status =
      BuildDf({"col_str"}, {{"apple"}, {std::nullopt}, {"apple"}});
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());
  auto spec = df.CreateSpec();
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
