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

#include "src/trace_processor/trace_summary/summarizer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/perfetto_sql/structured_query.pbzero.h"
#include "protos/perfetto/trace_summary/file.pbzero.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::summary {
namespace {

using ::testing::HasSubstr;
// Import public types from parent namespace.
using ::perfetto::trace_processor::Summarizer;
using ::perfetto::trace_processor::SummarizerQueryResult;
using ::perfetto::trace_processor::SummarizerUpdateSpecResult;
using QuerySyncInfo = SummarizerUpdateSpecResult::QuerySyncInfo;

class SummarizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tp_ = TraceProcessor::CreateInstance(Config{});
    // NotifyEndOfFile() without loading a trace is intentional - it initializes
    // TP's internal state (tables, functions) so we can execute queries against
    // it. The Summarizer doesn't need trace data, just a working SQL engine.
    tp_->NotifyEndOfFile();
    summarizer_ = std::make_unique<SummarizerImpl>(tp_.get(), nullptr);
  }

  // Helper to create a TraceSummarySpec with queries.
  std::vector<uint8_t> CreateSpec(
      const std::vector<std::pair<std::string, std::string>>& queries) {
    protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
    for (const auto& [id, sql] : queries) {
      auto* query = spec->add_query();
      query->set_id(id);
      auto* sql_source = query->set_sql();
      sql_source->set_sql(sql);
      sql_source->add_column_names("value");
    }
    return spec.SerializeAsArray();
  }

  // Helper to create a spec for chained queries A -> B -> C.
  std::vector<uint8_t> CreateChainedSpec(const std::string& sql_a) {
    protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;

    // Query A: standalone query.
    {
      auto* query = spec->add_query();
      query->set_id("A");
      auto* sql_source = query->set_sql();
      sql_source->set_sql(sql_a);
      sql_source->add_column_names("value");
    }

    // Query B: depends on A via inner_query_id.
    {
      auto* query = spec->add_query();
      query->set_id("B");
      query->set_inner_query_id("A");
      // Apply a filter to make it different from A.
      auto* filter = query->add_filters();
      filter->set_column_name("value");
      filter->set_op(
          protos::pbzero::PerfettoSqlStructuredQuery::Filter::GREATER_THAN);
      filter->add_int64_rhs(0);
    }

    // Query C: depends on B via inner_query_id.
    {
      auto* query = spec->add_query();
      query->set_id("C");
      query->set_inner_query_id("B");
    }

    return spec.SerializeAsArray();
  }

  std::unique_ptr<TraceProcessor> tp_;
  std::unique_ptr<SummarizerImpl> summarizer_;
};

TEST_F(SummarizerTest, BasicMaterialization) {
  auto spec_data = CreateSpec({{"test_query", "SELECT 42 as value"}});

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  // Should have one query result.
  ASSERT_EQ(result.queries.size(), 1u);
  EXPECT_EQ(result.queries[0].query_id, "test_query");
  EXPECT_TRUE(result.queries[0].was_updated);
  EXPECT_FALSE(result.queries[0].was_dropped);
  EXPECT_FALSE(result.queries[0].error.has_value());

  // Should be able to fetch the result.
  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("test_query", &info));
  EXPECT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_EQ(info.row_count, 1);
  ASSERT_EQ(info.columns.size(), 1u);
  EXPECT_EQ(info.columns[0], "value");
}

TEST_F(SummarizerTest, UnchangedQueryNotRematerialized) {
  auto spec_data = CreateSpec({{"test_query", "SELECT 42 as value"}});

  // First update - should materialize.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 1u);
  EXPECT_TRUE(result1.queries[0].was_updated);

  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("test_query", &info1));
  std::string first_table_name = info1.table_name;

  // Second update with same spec - should NOT rematerialize.
  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 1u);
  EXPECT_FALSE(result2.queries[0].was_updated);

  SummarizerQueryResult info2;
  ASSERT_OK(summarizer_->Query("test_query", &info2));
  EXPECT_EQ(info2.table_name, first_table_name);
}

TEST_F(SummarizerTest, ChangedQueryRematerialized) {
  auto spec_data1 = CreateSpec({{"test_query", "SELECT 42 as value"}});

  // First update.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 1u);
  EXPECT_TRUE(result1.queries[0].was_updated);

  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("test_query", &info1));
  std::string first_table_name = info1.table_name;

  // Second update with changed SQL.
  auto spec_data2 = CreateSpec({{"test_query", "SELECT 100 as value"}});

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 1u);
  EXPECT_TRUE(result2.queries[0].was_updated);

  SummarizerQueryResult info2;
  ASSERT_OK(summarizer_->Query("test_query", &info2));
  // Table name should be different (new materialization).
  EXPECT_NE(info2.table_name, first_table_name);
}

TEST_F(SummarizerTest, AutoDropWhenQueryRemoved) {
  auto spec_data1 = CreateSpec(
      {{"query_a", "SELECT 1 as value"}, {"query_b", "SELECT 2 as value"}});

  // First update with both queries.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 2u);

  // Second update with only query_a.
  auto spec_data2 = CreateSpec({{"query_a", "SELECT 1 as value"}});

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));

  // Should have 2 entries: query_a (unchanged) and query_b (dropped).
  ASSERT_EQ(result2.queries.size(), 2u);

  bool found_query_a = false;
  bool found_query_b_dropped = false;
  for (const auto& info : result2.queries) {
    if (info.query_id == "query_a") {
      found_query_a = true;
      EXPECT_FALSE(info.was_dropped);
    }
    if (info.query_id == "query_b") {
      found_query_b_dropped = true;
      EXPECT_TRUE(info.was_dropped);
    }
  }
  EXPECT_TRUE(found_query_a);
  EXPECT_TRUE(found_query_b_dropped);

  // query_b should no longer exist.
  SummarizerQueryResult info_b;
  ASSERT_OK(summarizer_->Query("query_b", &info_b));
  EXPECT_FALSE(info_b.exists);
}

TEST_F(SummarizerTest, ChainedQueriesWithOptimization) {
  // Create a chain: A -> B -> C.
  // A is a standalone query.
  // B references A via inner_query_id and adds a filter.
  // C references B via inner_query_id.
  auto spec_data =
      CreateChainedSpec("SELECT 1 as value UNION ALL SELECT 2 as value");

  // First update - all should be materialized.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 3u);

  for (const auto& info : result1.queries) {
    EXPECT_TRUE(info.was_updated)
        << "Query " << info.query_id << " should be materialized on first run";
  }

  // Get the table names.
  SummarizerQueryResult info_a1;
  ASSERT_OK(summarizer_->Query("A", &info_a1));
  SummarizerQueryResult info_b1;
  ASSERT_OK(summarizer_->Query("B", &info_b1));
  SummarizerQueryResult info_c1;
  ASSERT_OK(summarizer_->Query("C", &info_c1));
  ASSERT_TRUE(info_a1.exists);
  ASSERT_TRUE(info_b1.exists);
  ASSERT_TRUE(info_c1.exists);

  // Second update with same spec - none should be rematerialized.
  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 3u);

  for (const auto& info : result2.queries) {
    EXPECT_FALSE(info.was_updated)
        << "Query " << info.query_id
        << " should NOT be rematerialized on second run";
  }

  // Table names should be the same.
  SummarizerQueryResult info_a2;
  ASSERT_OK(summarizer_->Query("A", &info_a2));
  SummarizerQueryResult info_b2;
  ASSERT_OK(summarizer_->Query("B", &info_b2));
  SummarizerQueryResult info_c2;
  ASSERT_OK(summarizer_->Query("C", &info_c2));
  EXPECT_EQ(info_a2.table_name, info_a1.table_name);
  EXPECT_EQ(info_b2.table_name, info_b1.table_name);
  EXPECT_EQ(info_c2.table_name, info_c1.table_name);
}

TEST_F(SummarizerTest, ChainedQueriesPartialRematerialization) {
  // Create initial chain: A -> B -> C.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec1_proto;

  // Query A: SELECT 1, 2, 3.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("A");
    auto* sql_source = query->set_sql();
    sql_source->set_sql(
        "SELECT 1 as value UNION ALL SELECT 2 UNION ALL SELECT 3");
    sql_source->add_column_names("value");
  }

  // Query B: filter A to value > 1.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("B");
    query->set_inner_query_id("A");
    auto* filter = query->add_filters();
    filter->set_column_name("value");
    filter->set_op(
        protos::pbzero::PerfettoSqlStructuredQuery::Filter::GREATER_THAN);
    filter->add_int64_rhs(1);
  }

  // Query C: references B.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("C");
    query->set_inner_query_id("B");
  }

  auto spec1_data = spec1_proto.SerializeAsArray();

  // First update.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec1_data.data(), spec1_data.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 3u);

  SummarizerQueryResult info_a1;
  ASSERT_OK(summarizer_->Query("A", &info_a1));
  SummarizerQueryResult info_b1;
  ASSERT_OK(summarizer_->Query("B", &info_b1));
  SummarizerQueryResult info_c1;
  ASSERT_OK(summarizer_->Query("C", &info_c1));
  ASSERT_TRUE(info_a1.exists);
  ASSERT_TRUE(info_b1.exists);
  ASSERT_TRUE(info_c1.exists);

  // Modify B's filter (change > 1 to > 2) and re-sync.
  // A should stay the same, B and C should be rematerialized.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec2_proto;

  // Query A: unchanged.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("A");
    auto* sql_source = query->set_sql();
    sql_source->set_sql(
        "SELECT 1 as value UNION ALL SELECT 2 UNION ALL SELECT 3");
    sql_source->add_column_names("value");
  }

  // Query B: filter changed to value > 2.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("B");
    query->set_inner_query_id("A");
    auto* filter = query->add_filters();
    filter->set_column_name("value");
    filter->set_op(
        protos::pbzero::PerfettoSqlStructuredQuery::Filter::GREATER_THAN);
    filter->add_int64_rhs(2);  // Changed from 1 to 2.
  }

  // Query C: references B.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("C");
    query->set_inner_query_id("B");
  }

  auto spec2_data = spec2_proto.SerializeAsArray();

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec2_data.data(), spec2_data.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 3u);

  // Check what was updated.
  bool a_updated = false, b_updated = false, c_updated = false;
  for (const auto& info : result2.queries) {
    if (info.query_id == "A")
      a_updated = info.was_updated;
    if (info.query_id == "B")
      b_updated = info.was_updated;
    if (info.query_id == "C")
      c_updated = info.was_updated;
  }

  // A should NOT be updated (optimization: table-source query used).
  EXPECT_FALSE(a_updated) << "A should NOT be rematerialized";

  // B should be updated (its filter changed).
  EXPECT_TRUE(b_updated) << "B should be rematerialized (filter changed)";

  // C should be updated (its dependency B changed).
  EXPECT_TRUE(c_updated) << "C should be rematerialized (B changed)";

  // Verify A's table name is preserved.
  SummarizerQueryResult info_a2;
  ASSERT_OK(summarizer_->Query("A", &info_a2));
  EXPECT_EQ(info_a2.table_name, info_a1.table_name);

  // B and C should have new table names.
  SummarizerQueryResult info_b2;
  ASSERT_OK(summarizer_->Query("B", &info_b2));
  SummarizerQueryResult info_c2;
  ASSERT_OK(summarizer_->Query("C", &info_c2));
  EXPECT_NE(info_b2.table_name, info_b1.table_name);
  EXPECT_NE(info_c2.table_name, info_c1.table_name);

  // Verify the row counts are correct.
  // B now has filter > 2, so only value 3 should remain (1 row).
  EXPECT_EQ(info_b2.row_count, 1);
  EXPECT_EQ(info_c2.row_count, 1);
}

TEST_F(SummarizerTest, OldTableAccessibleUntilNewMaterialization) {
  // This test verifies that the old materialized table is NOT dropped
  // immediately when UpdateSpec() detects a change. The old table should remain
  // accessible until Query() creates the new table. This prevents race
  // conditions where in-flight queries fail with "no such table" errors.

  auto spec_data1 = CreateSpec({{"test_query", "SELECT 42 as value"}});

  // First update and materialize.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));
  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("test_query", &info1));
  ASSERT_TRUE(info1.exists);
  std::string old_table_name = info1.table_name;

  // Verify old table is accessible.
  {
    auto check1 = tp_->ExecuteQuery("SELECT COUNT(*) FROM " + old_table_name);
    ASSERT_TRUE(check1.Next());
    EXPECT_EQ(check1.Get(0).AsLong(), 1);
    while (check1.Next()) {
    }  // Fully consume iterator.
  }

  // Update with changed SQL - this marks the query for re-materialization
  // but should NOT drop the old table yet.
  auto spec_data2 = CreateSpec({{"test_query", "SELECT 100 as value"}});

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 1u);
  EXPECT_TRUE(
      result2.queries[0].was_updated);  // Marked for re-materialization.

  // CRITICAL: Old table should still be accessible after UpdateSpec().
  // This is the key behavior that prevents race conditions.
  {
    auto check2 = tp_->ExecuteQuery("SELECT COUNT(*) FROM " + old_table_name);
    ASSERT_TRUE(check2.Next())
        << "Old table should still exist after UpdateSpec()";
    EXPECT_EQ(check2.Get(0).AsLong(), 1);
  }  // Iterator goes out of scope, releasing any locks.

  // Now fetch the result, which triggers materialization of the new table.
  SummarizerQueryResult info2;
  ASSERT_OK(summarizer_->Query("test_query", &info2));
  ASSERT_TRUE(info2.exists);
  EXPECT_NE(info2.table_name, old_table_name);  // New table created.

  // After Query(), old table should be dropped automatically.
  auto check3 = tp_->ExecuteQuery("SELECT COUNT(*) FROM " + old_table_name);
  check3.Next();  // Execute the query.
  // Query should fail because table was dropped.
  EXPECT_FALSE(check3.Status().ok())
      << "Old table should be dropped after new materialization: "
      << check3.Status().message();
}

// Tests dependency propagation for queries with nested embedded queries.
// This simulates the FilterDuring scenario where the main query embeds
// references to other queries via inner_query fields.
//
// The test verifies that when A changes, C is marked for re-materialization
// even though C's inner_query_id reference to A is nested inside an embedded
// query (not at the top level of C's proto).
TEST_F(SummarizerTest, NestedEmbeddedQueryDependencyPropagation) {
  // Create three queries:
  // A: Source data
  // B: Another source
  // C: Has an embedded inner_query that references A via inner_query_id
  //    (simulating how FilterDuring, Join, etc. embed references)

  // First spec: A and B are simple queries, C embeds a reference to A.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec1_proto;

  // Query A: Simple SELECT.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("A");
    auto* sql_source = query->set_sql();
    sql_source->set_sql("SELECT 1 as value UNION ALL SELECT 2 as value");
    sql_source->add_column_names("value");
  }

  // Query B: Simple SELECT.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("B");
    auto* sql_source = query->set_sql();
    sql_source->set_sql("SELECT 10 as other");
    sql_source->add_column_names("other");
  }

  // Query C: Uses inner_query with an embedded query that has inner_query_id.
  // This creates a nested dependency: C -> (embedded) -> A.
  {
    auto* query = spec1_proto->add_query();
    query->set_id("C");
    // Embed a query that references A via inner_query_id.
    auto* inner = query->set_inner_query();
    inner->set_id("C_inner");
    inner->set_inner_query_id("A");
    // Apply a trivial filter to make it different from A.
    auto* filter = inner->add_filters();
    filter->set_column_name("value");
    filter->set_op(
        protos::pbzero::PerfettoSqlStructuredQuery::Filter::GREATER_THAN);
    filter->add_int64_rhs(0);
  }

  auto spec1_data = spec1_proto.SerializeAsArray();

  // First update.
  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec1_data.data(), spec1_data.size(), &result1));
  ASSERT_EQ(result1.queries.size(), 3u);

  // Materialize all queries.
  SummarizerQueryResult info_a1;
  ASSERT_OK(summarizer_->Query("A", &info_a1));
  SummarizerQueryResult info_b1;
  ASSERT_OK(summarizer_->Query("B", &info_b1));
  SummarizerQueryResult info_c1;
  ASSERT_OK(summarizer_->Query("C", &info_c1));
  ASSERT_TRUE(info_a1.exists);
  ASSERT_TRUE(info_b1.exists);
  ASSERT_TRUE(info_c1.exists);
  EXPECT_EQ(info_a1.row_count, 2);  // 1 and 2.
  EXPECT_EQ(info_c1.row_count, 2);  // Both > 0.

  // Second spec: Modify A (the nested dependency of C).
  // C should be marked for rematerialization even though its direct proto
  // hasn't changed - the change was in a nested inner_query_id reference.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec2_proto;

  // Query A: CHANGED - now returns only 1 row.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("A");
    auto* sql_source = query->set_sql();
    sql_source->set_sql("SELECT 1 as value");  // Only 1 row now.
    sql_source->add_column_names("value");
  }

  // Query B: unchanged.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("B");
    auto* sql_source = query->set_sql();
    sql_source->set_sql("SELECT 10 as other");
    sql_source->add_column_names("other");
  }

  // Query C: unchanged proto, but depends on A which changed.
  {
    auto* query = spec2_proto->add_query();
    query->set_id("C");
    auto* inner = query->set_inner_query();
    inner->set_id("C_inner");
    inner->set_inner_query_id("A");
    auto* filter = inner->add_filters();
    filter->set_column_name("value");
    filter->set_op(
        protos::pbzero::PerfettoSqlStructuredQuery::Filter::GREATER_THAN);
    filter->add_int64_rhs(0);
  }

  auto spec2_data = spec2_proto.SerializeAsArray();

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec2_data.data(), spec2_data.size(), &result2));
  ASSERT_EQ(result2.queries.size(), 3u);

  // Check what was updated.
  bool a_updated = false, b_updated = false, c_updated = false;
  for (const auto& info : result2.queries) {
    if (info.query_id == "A")
      a_updated = info.was_updated;
    if (info.query_id == "B")
      b_updated = info.was_updated;
    if (info.query_id == "C")
      c_updated = info.was_updated;
  }

  // A should be updated (its SQL changed).
  EXPECT_TRUE(a_updated) << "A should be rematerialized (SQL changed)";

  // B should NOT be updated (unchanged).
  EXPECT_FALSE(b_updated) << "B should NOT be rematerialized";

  // CRITICAL: C should be updated because its nested dependency A changed.
  // This is the key test - before the fix, C would NOT be updated because
  // the dependency propagation only checked top-level inner_query_id.
  EXPECT_TRUE(c_updated)
      << "C should be rematerialized (nested dependency A changed)";

  // Verify the results reflect the change.
  SummarizerQueryResult info_a2;
  ASSERT_OK(summarizer_->Query("A", &info_a2));
  SummarizerQueryResult info_c2;
  ASSERT_OK(summarizer_->Query("C", &info_c2));

  // A now has 1 row.
  EXPECT_EQ(info_a2.row_count, 1);

  // C should also reflect A's change (1 row now, since only value=1 > 0).
  EXPECT_EQ(info_c2.row_count, 1);
}

}  // namespace
}  // namespace perfetto::trace_processor::summary
