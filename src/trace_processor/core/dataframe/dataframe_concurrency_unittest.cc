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

// A finalized Dataframe must be safe for concurrent reads via independent
// cursors. Each thread plans, prepares and iterates its own query over one
// shared dataframe; under ThreadSanitizer this must be race-free and match a
// single-threaded run. Planning happens per thread too, so any shared state on
// the planning path is exercised as well.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/dataframe_test_utils.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/util/concurrency_stress_test_util.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

struct GoldenRow {
  int64_t int_val;
  double double_val;
  std::string str_val;

  bool operator==(const GoldenRow& o) const {
    // The doubles are exact half-integers; comparing bits avoids -Wfloat-equal.
    return int_val == o.int_val && str_val == o.str_val &&
           memcmp(&double_val, &o.double_val, sizeof(double_val)) == 0;
  }
};

// Column indices in the dataframe built below.
constexpr uint32_t kIntCol = 0;
constexpr uint32_t kDoubleCol = 1;
constexpr uint32_t kStrCol = 2;

constexpr uint32_t kRowCount = 4000;

// The range query keeps rows with int_col > kFilterValue.
constexpr int64_t kFilterValue = 1000;

// The Eq query matches int_col == kEqValue, one row since values are unique.
constexpr int64_t kEqValue = 2000;

// Leaves out the _auto_id column the builder appends.
constexpr uint64_t kColsBitmap =
    (1ull << kIntCol) | (1ull << kDoubleCol) | (1ull << kStrCol);

// A permutation of [0, kRowCount): 7919 is prime and coprime with kRowCount.
// The column must be unsorted, otherwise it is classified as an Id column and
// an Eq filter binary-searches it instead of reading the attached index.
int64_t IntForRow(uint32_t row) {
  return static_cast<int64_t>((static_cast<uint64_t>(row) * 7919) % kRowCount);
}

// A few repeated words, so the string sort has duplicates to deal with.
const char* StrForRow(uint32_t row) {
  static const char* const kWords[] = {"apple", "banana", "cherry", "date",
                                       "elder", "fig",    "grape",  "honey"};
  return kWords[row % 8];
}

class DataframeConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    AdhocDataframeBuilder builder({"int_col", "double_col", "str_col"}, &pool_);
    for (uint32_t i = 0; i < kRowCount; ++i) {
      PERFETTO_CHECK(builder.PushNonNull(kIntCol, IntForRow(i)));
      PERFETTO_CHECK(
          builder.PushNonNull(kDoubleCol, static_cast<double>(i) * 0.5));
      PERFETTO_CHECK(
          builder.PushNonNull(kStrCol, pool_.InternString(StrForRow(i))));
    }
    base::StatusOr<Dataframe> df_or = std::move(builder).Build();
    ASSERT_OK(df_or.status());
    Dataframe df = std::move(df_or.value());
    df.Finalize();

    // AddIndex returns a new dataframe; the indexed one is the shared instance.
    uint32_t index_cols[] = {kIntCol};
    base::StatusOr<Index> index_or = df.BuildIndex(index_cols, index_cols + 1);
    ASSERT_OK(index_or.status());
    df_ = std::make_unique<Dataframe>(df.AddIndex(std::move(index_or.value())));
    PERFETTO_CHECK(df_->finalized());
  }

  // Plans and runs one query with a fresh plan and cursor, returning every row.
  std::vector<GoldenRow> RunQueryWithSpecs(const Dataframe& df,
                                           std::vector<FilterSpec> filters,
                                           std::vector<SortSpec> sorts,
                                           int64_t filter_value) {
    base::StatusOr<Dataframe::QueryPlan> plan_or =
        df.PlanQuery(filters, {}, sorts, {}, kColsBitmap);
    PERFETTO_CHECK(plan_or.ok());

    auto cursor = std::make_unique<Cursor<TestRowFetcher>>();
    df.PrepareCursor(plan_or.value(), *cursor);

    std::vector<TestRowFetcher::Value> filter_row = {filter_value};
    TestRowFetcher fetcher;
    fetcher.SetRow(filter_row);
    cursor->Execute(fetcher);

    std::vector<GoldenRow> rows;
    for (; !cursor->Eof(); cursor->Next()) {
      ValueVerifier verifier;
      verifier.Fetch(&*cursor, /*col_count=*/3);
      PERFETTO_CHECK(verifier.values.size() == 3);

      // The builder downcasts the int column to uint32_t; widen it back.
      GoldenRow row;
      row.int_val = std::visit(
          [](auto v) -> int64_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, uint32_t> ||
                          std::is_same_v<T, int32_t> ||
                          std::is_same_v<T, int64_t>) {
              return static_cast<int64_t>(v);
            } else {
              PERFETTO_FATAL("Unexpected integer cell type");
            }
          },
          verifier.values[kIntCol]);
      row.double_val = std::get<double>(verifier.values[kDoubleCol]);
      row.str_val =
          std::get<NullTermStringView>(verifier.values[kStrCol]).ToStdString();
      rows.push_back(std::move(row));
    }
    return rows;
  }

  // Filter int_col > kFilterValue, sort by (str_col, int_col). The planner only
  // uses an index for Eq/In, so this covers the scan, sort and string paths.
  std::vector<GoldenRow> RunQuery(const Dataframe& df) {
    return RunQueryWithSpecs(df, {FilterSpec{kIntCol, 0, Gt{}, std::nullopt}},
                             {SortSpec{kStrCol, SortDirection::kAscending},
                              SortSpec{kIntCol, SortDirection::kAscending}},
                             kFilterValue);
  }

  // Filter int_col == kEqValue, which the planner serves from the index.
  std::vector<GoldenRow> RunIndexedQuery(const Dataframe& df) {
    return RunQueryWithSpecs(df, {FilterSpec{kIntCol, 0, Eq{}, std::nullopt}},
                             {}, kEqValue);
  }

  // Guards against the Eq query silently falling back to a scan, which would
  // leave the index read path untested.
  void AssertIndexedQueryUsesIndex(const Dataframe& df) {
    std::vector<FilterSpec> filters = {
        FilterSpec{kIntCol, 0, Eq{}, std::nullopt}};
    base::StatusOr<Dataframe::QueryPlan> plan_or =
        df.PlanQuery(filters, {}, {}, {}, kColsBitmap);
    ASSERT_OK(plan_or.status());
    bool uses_index = false;
    std::string all;
    for (const std::string& line : plan_or.value().BytecodeToString()) {
      all += line + "\n";
      if (line.find("Indexed") != std::string::npos)
        uses_index = true;
    }
    ASSERT_TRUE(uses_index) << "Eq query did not engage the index:\n" << all;
  }

  StringPool pool_;
  std::unique_ptr<Dataframe> df_;
};

TEST_F(DataframeConcurrencyTest, IndependentCursorsAgreeWithGolden) {
  std::vector<GoldenRow> golden = RunQuery(*df_);
  ASSERT_EQ(golden.size(), static_cast<size_t>(kRowCount - kFilterValue - 1));

  // Check the golden itself, otherwise the threads could all agree on a wrong
  // answer.
  for (const GoldenRow& row : golden) {
    ASSERT_GT(row.int_val, kFilterValue);
  }
  ASSERT_TRUE(std::is_sorted(
      golden.begin(), golden.end(), [](const GoldenRow& a, const GoldenRow& b) {
        return std::tie(a.str_val, a.int_val) < std::tie(b.str_val, b.int_val);
      }));

  std::vector<GoldenRow> indexed_golden = RunIndexedQuery(*df_);
  ASSERT_EQ(indexed_golden.size(), 1u);
  ASSERT_EQ(indexed_golden[0].int_val, kEqValue);
  ASSERT_NO_FATAL_FAILURE(AssertIndexedQueryUsesIndex(*df_));

  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kIterations = 16;
  const Dataframe& shared_df = *df_;
  ConcurrentlyRun(kThreads, kIterations, [&](uint32_t) {
    // EXPECT_* is thread-safe in gtest.
    EXPECT_EQ(RunQuery(shared_df), golden);
    EXPECT_EQ(RunIndexedQuery(shared_df), indexed_golden);
  });
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
