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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/iterator.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using testing::ElementsAre;

using CellValue = std::variant<std::nullptr_t, int64_t, double, std::string>;

struct CollectCell {
  void OnCell(int64_t v) { value = v; }
  void OnCell(uint32_t v) { value = static_cast<int64_t>(v); }
  void OnCell(int32_t v) { value = static_cast<int64_t>(v); }
  void OnCell(double v) { value = v; }
  void OnCell(NullTermStringView v) { value = std::string(v.data(), v.size()); }
  void OnCell(std::nullptr_t) { value = nullptr; }

  CellValue value = nullptr;
};

struct TestFetcher : core::ValueFetcher {
  using Type = int;
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  int64_t GetInt64Value(uint32_t i) const { return std::get<int64_t>(vals[i]); }
  double GetDoubleValue(uint32_t i) const { return std::get<double>(vals[i]); }
  const char* GetStringValue(uint32_t i) const {
    return std::get<std::string>(vals[i]).c_str();
  }
  Type GetValueType(uint32_t i) const {
    if (std::holds_alternative<int64_t>(vals[i]))
      return kInt64;
    if (std::holds_alternative<double>(vals[i]))
      return kDouble;
    if (std::holds_alternative<std::string>(vals[i]))
      return kString;
    return kNull;
  }
  bool IteratorInit(uint32_t) { return false; }
  bool IteratorNext(uint32_t) { return false; }

  std::vector<CellValue> vals;
};

class PipelineApiTest : public ::testing::Test {
 protected:
  dataframe::Dataframe MakeSampleDataframe() {
    dataframe::AdhocDataframeBuilder::Options opts;
    opts.emit_auto_id = true;
    dataframe::AdhocDataframeBuilder b({"k", "v", "name", "opt"}, &pool_, opts);
    b.PushNonNull(0, int64_t{10});
    b.PushNonNull(1, 1.5);
    b.PushNonNull(2, pool_.InternString("alpha"));
    b.PushNull(3);

    b.PushNonNull(0, int64_t{20});
    b.PushNonNull(1, 2.5);
    b.PushNonNull(2, pool_.InternString("beta"));
    b.PushNonNull(3, int64_t{100});

    b.PushNonNull(0, int64_t{10});
    b.PushNonNull(1, 3.5);
    b.PushNonNull(2, pool_.InternString("alpha_2"));
    b.PushNonNull(3, int64_t{200});

    b.PushNonNull(0, int64_t{30});
    b.PushNonNull(1, 4.5);
    b.PushNonNull(2, pool_.InternString("gamma"));
    b.PushNull(3);

    auto df = std::move(b).Build();
    PERFETTO_CHECK(df.ok());
    return std::move(*df);
  }

  std::vector<std::vector<CellValue>> DrainRows(RowIterator& it) {
    std::vector<std::vector<CellValue>> out;
    for (bool ok = it.Open(); ok; ok = it.Next()) {
      std::vector<CellValue> row;
      for (uint32_t c = 0; c < it.column_count(); ++c) {
        CollectCell cell;
        it.Cell(c, cell);
        row.push_back(std::move(cell.value));
      }
      out.push_back(std::move(row));
    }
    EXPECT_TRUE(it.status().ok()) << it.status().message();
    return out;
  }

  std::vector<std::vector<CellValue>> DrainBatches(BatchIterator& it) {
    std::vector<std::vector<CellValue>> out;
    while (std::optional<Batch> batch = it.Next()) {
      for (uint32_t r = 0; r < batch->row_count(); ++r) {
        std::vector<CellValue> row;
        for (uint32_t c = 0; c < batch->column_count(); ++c) {
          CollectCell cell;
          batch->Cell(c, r, cell);
          row.push_back(std::move(cell.value));
        }
        out.push_back(std::move(row));
      }
    }
    EXPECT_TRUE(it.status().ok()) << it.status().message();
    return out;
  }

  StringPool pool_;
};

TEST_F(PipelineApiTest, BuilderExecuteBatchesAndRowsMatch) {
  dataframe::Dataframe df = MakeSampleDataframe();
  Builder b;
  auto [rel, cols] = b.Scan(df);
  rel = b.Where(rel, {{cols[0].id, core::Ge{}, {Literal::Int64(10)}},
                      {cols[3].id, core::IsNotNull{}, {}}});
  rel = b.OrderBy(rel, {{cols[3].id, /*descending=*/true}});
  rel = b.Limit(rel, 10, 0);
  std::vector<ColumnId> outputs = {cols[0].id, cols[2].id, cols[3].id};
  PreparedStatement stmt = b.Finish(rel, outputs);

  EXPECT_EQ(stmt.column_count(), 3u);
  EXPECT_EQ(stmt.column_name(0), "k");
  EXPECT_EQ(stmt.column_name(1), "name");
  EXPECT_EQ(stmt.column_name(2), "opt");

  const dataframe::Dataframe* arg = &df;
  ExecutionContext ctx{&pool_, nullptr, nullptr, {&arg, &arg + 1}};

  auto batches_or = stmt.Execute(ctx);
  ASSERT_TRUE(batches_or.ok()) << batches_or.status().message();
  auto batch_rows = DrainBatches(*batches_or);

  // Rewind batch iterator and drain again.
  batches_or->Open();
  auto batch_rows_again = DrainBatches(*batches_or);
  EXPECT_EQ(batch_rows, batch_rows_again);

  auto rows_or = stmt.ExecuteRows(ctx);
  ASSERT_TRUE(rows_or.ok()) << rows_or.status().message();
  auto row_rows = DrainRows(*rows_or);

  EXPECT_EQ(batch_rows, row_rows);
  ASSERT_EQ(row_rows.size(), 2u);
  EXPECT_EQ(row_rows[0],
            (std::vector<CellValue>{int64_t{10}, std::string("alpha_2"),
                                    int64_t{200}}));
  EXPECT_EQ(row_rows[1], (std::vector<CellValue>{
                             int64_t{20}, std::string("beta"), int64_t{100}}));
}

TEST_F(PipelineApiTest, UnstampedSerializationRoundTripAndParams) {
  dataframe::Dataframe df = MakeSampleDataframe();
  uint32_t idx_col = 0;
  auto idx_or = df.BuildIndex(&idx_col, &idx_col + 1);
  ASSERT_TRUE(idx_or.ok()) << idx_or.status().message();
  df = df.AddIndex(std::move(*idx_or));

  Builder b;
  std::vector<uint32_t> scan_cols = {0, 2, 3, 4};  // includes hidden _auto_id
  auto [rel, cols] = b.Scan(df, scan_cols);
  rel = b.Where(rel, {{cols[0].id, core::Eq{}, {Literal::Bound(0)}},
                      {cols[1].id, core::Glob{}, {Literal::External(1)}}});
  std::vector<ColumnId> out_cols = {cols[3].id, cols[1].id, cols[2].id};
  PreparedStatement original = b.Finish(rel, out_cols);

  std::string bytes = original.Serialize();
  PreparedStatement restored = PreparedStatement::Deserialize(bytes);

  const dataframe::Dataframe* arg = &df;
  ExecutionContext ctx{&pool_, nullptr, nullptr, {&arg, &arg + 1}};

  auto rows_or = restored.ExecuteRows(ctx);
  ASSERT_TRUE(rows_or.ok()) << rows_or.status().message();
  auto bindings = rows_or->MakeBindings<TestFetcher>();

  TestFetcher fetcher;
  fetcher.vals = {int64_t{10}, std::string("alpha*")};
  bindings.SetExternalLiterals(fetcher);

  std::vector<std::vector<CellValue>> got;
  for (bool ok = rows_or->Open(bindings.Bind(fetcher)); ok;
       ok = rows_or->Next()) {
    std::vector<CellValue> row;
    for (uint32_t c = 0; c < rows_or->column_count(); ++c) {
      CollectCell cell;
      rows_or->Cell(c, cell);
      row.push_back(std::move(cell.value));
    }
    got.push_back(std::move(row));
  }
  ASSERT_TRUE(rows_or->status().ok());
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], (std::vector<CellValue>{int64_t{0}, std::string("alpha"),
                                            nullptr}));
  EXPECT_EQ(got[1], (std::vector<CellValue>{int64_t{2}, std::string("alpha_2"),
                                            int64_t{200}}));

  // Re-run with a different bound param (no match for "alpha*").
  fetcher.vals[0] = int64_t{20};
  EXPECT_FALSE(rows_or->Open(bindings.Bind(fetcher)));
  EXPECT_TRUE(rows_or->status().ok());

  // Passing a null dataframe argument (empty relation) with hidden columns
  // succeeds and produces zero rows.
  const dataframe::Dataframe* null_arg = nullptr;
  auto empty_rows = restored.ExecuteRows(
      ExecutionContext{&pool_, nullptr, nullptr, {&null_arg, &null_arg + 1}});
  ASSERT_TRUE(empty_rows.ok()) << empty_rows.status().message();
  auto empty_bindings = empty_rows->MakeBindings<TestFetcher>();
  empty_bindings.SetExternalLiterals(fetcher);
  EXPECT_FALSE(empty_rows->Open(empty_bindings.Bind(fetcher)));
  EXPECT_TRUE(empty_rows->status().ok());
}

TEST_F(PipelineApiTest, StampedSerializationRequiresMatchingPlanKey) {
  dataframe::Dataframe df = MakeSampleDataframe();
  Builder b;
  auto [rel, cols] = b.Scan(df);
  std::vector<ColumnId> out_cols = {cols[0].id};
  PreparedStatement stmt = b.Finish(rel, out_cols);

  PlanKey key(42, 99);
  std::string stamped = stmt.Serialize(key);
  EXPECT_FALSE(PreparedStatement::Deserialize(stamped, PlanKey(42, 100)).ok());

  auto restored = PreparedStatement::Deserialize(stamped, key);
  ASSERT_TRUE(restored.ok()) << restored.status().message();

  const dataframe::Dataframe* arg = &df;
  // Executing a stamped statement without passing plan_key must fail.
  EXPECT_FALSE(restored
                   ->Execute(ExecutionContext{
                       &pool_, nullptr, nullptr, {&arg, &arg + 1}})
                   .ok());
  auto ok_it = restored->Execute(
      ExecutionContext{&pool_, nullptr, &key, {&arg, &arg + 1}});
  ASSERT_TRUE(ok_it.ok()) << ok_it.status().message();
  EXPECT_EQ(DrainBatches(*ok_it).size(), 4u);
}

TEST_F(PipelineApiTest, DistinctAggregateAndJoinViaBuilder) {
  dataframe::Dataframe df = MakeSampleDataframe();
  const dataframe::Dataframe* arg = &df;
  ExecutionContext ctx{&pool_, nullptr, nullptr, {&arg, &arg + 1}};

  {
    Builder b;
    auto [rel, cols] = b.Scan(df);
    std::vector<ColumnId> keys = {cols[0].id};
    rel = b.Distinct(rel, keys);
    rel = b.OrderBy(rel, {{cols[0].id, false}});
    PreparedStatement stmt = b.Finish(rel, keys);

    auto it = stmt.Execute(ctx);
    ASSERT_TRUE(it.ok()) << it.status().message();
    EXPECT_THAT(DrainBatches(*it),
                ElementsAre(std::vector<CellValue>{int64_t{10}},
                            std::vector<CellValue>{int64_t{20}},
                            std::vector<CellValue>{int64_t{30}}));
  }

  {
    Builder b;
    auto [rel, cols] = b.Scan(df);
    std::vector<ColumnId> keys = {cols[0].id};
    std::vector<Builder::AggregateItem> aggs = {
        {b.InternName("cnt"), core::exec::AggregateCall::Function::kCountStar,
         0}};
    auto agg = b.Aggregate(rel, keys, aggs);
    auto sorted = b.OrderBy(agg.relation, {{cols[0].id, false}});
    std::vector<ColumnId> out_cols = {cols[0].id, agg.columns[0]};
    PreparedStatement stmt = b.Finish(sorted, out_cols);

    auto it = stmt.Execute(ctx);
    ASSERT_TRUE(it.ok()) << it.status().message();
    EXPECT_THAT(DrainBatches(*it),
                ElementsAre(std::vector<CellValue>{int64_t{10}, int64_t{2}},
                            std::vector<CellValue>{int64_t{20}, int64_t{1}},
                            std::vector<CellValue>{int64_t{30}, int64_t{1}}));
  }

  {
    dataframe::AdhocDataframeBuilder::Options opts;
    opts.emit_auto_id = false;
    dataframe::AdhocDataframeBuilder right_b({"id", "label"}, &pool_, opts);
    right_b.PushNonNull(0, int64_t{0});
    right_b.PushNonNull(1, pool_.InternString("zero"));
    right_b.PushNonNull(0, int64_t{1});
    right_b.PushNonNull(1, pool_.InternString("one"));
    auto right_df = std::move(right_b).Build();
    ASSERT_TRUE(right_df.ok());

    Builder b;
    std::vector<uint32_t> left_cols = {4, 2};  // _auto_id, name
    auto [left, lcols] = b.Scan(df, left_cols);
    auto [right, rcols] = b.Scan(*right_df);
    std::vector<ColumnId> right_outs = {rcols[1].id};
    auto joined = b.Join(left, right, core::exec::JoinSpec::Kind::kLeft,
                         lcols[0].id, rcols[0].id, right_outs);
    std::vector<ColumnId> out_cols = {lcols[0].id, lcols[1].id, rcols[1].id};
    PreparedStatement stmt = b.Finish(joined, out_cols);

    const dataframe::Dataframe* args[] = {&df, &*right_df};
    auto it = std::move(stmt).ExecuteRows(
        ExecutionContext{&pool_, nullptr, nullptr, args});
    ASSERT_TRUE(it.ok()) << it.status().message();
    EXPECT_THAT(
        DrainRows(*it),
        ElementsAre(
            std::vector<CellValue>{int64_t{0}, std::string("alpha"),
                                   std::string("zero")},
            std::vector<CellValue>{int64_t{1}, std::string("beta"),
                                   std::string("one")},
            std::vector<CellValue>{int64_t{2}, std::string("alpha_2"), nullptr},
            std::vector<CellValue>{int64_t{3}, std::string("gamma"), nullptr}));
  }
}

TEST_F(PipelineApiTest, ScanSqlRoundTripsUnstampedSerialization) {
  Builder b;
  std::vector<Builder::SqlColumn> cols = {{b.InternName("x"), core::Int64{}}};
  auto scanned = b.ScanSql("SELECT 1 AS x", cols);
  PreparedStatement stmt = b.Finish(scanned.relation, scanned.columns);

  std::string bytes = stmt.Serialize();
  PreparedStatement restored = PreparedStatement::Deserialize(bytes);
  EXPECT_EQ(restored.column_count(), 1u);
  EXPECT_EQ(restored.column_name(0), "x");
  auto sql = std::move(restored).ToSqliteSql(PlanKey(1, 2));
  ASSERT_TRUE(sql.ok()) << sql.status().message();
}

TEST_F(PipelineApiTest, IteratorsAreSafelyMovableMidStream) {
  dataframe::Dataframe df = MakeSampleDataframe();
  Builder b;
  auto [rel, cols] = b.Scan(df);
  std::vector<ColumnId> out_cols = {cols[0].id};
  PreparedStatement stmt = b.Finish(rel, out_cols);

  const dataframe::Dataframe* arg = &df;
  ExecutionContext ctx{&pool_, nullptr, nullptr, {&arg, &arg + 1}};

  auto rows_a = stmt.ExecuteRows(ctx);
  ASSERT_TRUE(rows_a.ok());
  ASSERT_TRUE(rows_a->Open());

  // Move-construct mid-stream and keep iterating.
  RowIterator rows_b(std::move(*rows_a));
  std::vector<int64_t> values;
  do {
    CollectCell cell;
    rows_b.Cell(0, cell);
    values.push_back(std::get<int64_t>(cell.value));
  } while (rows_b.Next());
  EXPECT_TRUE(rows_b.status().ok());
  EXPECT_THAT(values, ElementsAre(10, 20, 10, 30));

  // Move-assign over an active iterator.
  auto rows_c = stmt.ExecuteRows(ctx);
  ASSERT_TRUE(rows_c.ok());
  ASSERT_TRUE(rows_c->Open());
  rows_b = std::move(*rows_c);
  EXPECT_FALSE(rows_b.eof());
  EXPECT_TRUE(rows_b.Next());
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
