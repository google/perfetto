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
    summarizer_ =
        std::make_unique<SummarizerImpl>(tp_.get(), nullptr, "test_id");
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

TEST_F(SummarizerTest, MultipleSummarizersCoexist) {
  // Two summarizers created via the public API should be able to materialize
  // the same query without table name collisions. The ids are auto-generated
  // internally by TraceProcessor.
  std::unique_ptr<Summarizer> s1;
  std::unique_ptr<Summarizer> s2;
  ASSERT_OK(tp_->CreateSummarizer(&s1));
  ASSERT_OK(tp_->CreateSummarizer(&s2));

  auto spec_data = CreateSpec({{"q", "SELECT 1 as value"}});

  SummarizerUpdateSpecResult r1;
  ASSERT_OK(s1->UpdateSpec(spec_data.data(), spec_data.size(), &r1));
  ASSERT_EQ(r1.queries.size(), 1u);

  SummarizerUpdateSpecResult r2;
  ASSERT_OK(s2->UpdateSpec(spec_data.data(), spec_data.size(), &r2));
  ASSERT_EQ(r2.queries.size(), 1u);

  // Trigger actual materialization for both — if table names collided, the
  // CREATE TABLE would fail with a "table already exists" error.
  SummarizerQueryResult info1;
  ASSERT_OK(s1->Query("q", &info1));
  ASSERT_TRUE(info1.exists);

  SummarizerQueryResult info2;
  ASSERT_OK(s2->Query("q", &info2));
  ASSERT_TRUE(info2.exists);

  // Table names must differ (namespaced by auto-generated summarizer id).
  EXPECT_NE(info1.table_name, info2.table_name);
}

TEST_F(SummarizerTest, TableNameContainsSummarizerId) {
  // Verify that the materialized table name includes the summarizer id.
  // The test fixture creates a SummarizerImpl with id "test_id".
  auto spec_data = CreateSpec({{"q", "SELECT 1 as value"}});

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("q", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_THAT(info.table_name, HasSubstr("test_id"));
}

TEST_F(SummarizerTest, DestructorCleansUpOldTableName) {
  // Verify that destroying a summarizer between UpdateSpec (which sets
  // old_table_name) and Query (which would normally clean it up) still
  // drops the old table.
  auto spec_data1 = CreateSpec({{"q", "SELECT 42 as value"}});

  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));

  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("q", &info1));
  ASSERT_TRUE(info1.exists);
  std::string old_table = info1.table_name;

  // Update with changed SQL — sets old_table_name but does NOT call Query().
  auto spec_data2 = CreateSpec({{"q", "SELECT 100 as value"}});
  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));

  // Verify old table still exists before destruction.
  {
    auto check = tp_->ExecuteQuery("SELECT COUNT(*) FROM " + old_table);
    ASSERT_TRUE(check.Next());
    while (check.Next()) {
    }
  }

  // Destroy the summarizer WITHOUT calling Query().
  summarizer_.reset();

  // Old table should be dropped (no new table was created since Query() was
  // never called after the second UpdateSpec).
  auto check = tp_->ExecuteQuery("SELECT COUNT(*) FROM " + old_table);
  check.Next();  // Execute the query.
  EXPECT_FALSE(check.Status().ok())
      << "old_table_name should be dropped on destruction";
}

TEST_F(SummarizerTest, SimpleTableSourceCreatesView) {
  // A query with a simple table source (no aggregation, no joins) should be
  // created as a VIEW instead of a TABLE, preserving source table indexes.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("table_query");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
    table->add_column_names("dur");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("table_query", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_TRUE(info.is_view);
}

TEST_F(SummarizerTest, SqlSourceMaterializesAsTable) {
  // A query with SQL source should always be materialized as a TABLE.
  auto spec_data = CreateSpec({{"sql_query", "SELECT 42 as value"}});

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("sql_query", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_FALSE(info.is_view);
}

TEST_F(SummarizerTest, TableSourceWithGroupByMaterializesAsTable) {
  // A query with GROUP BY should always be materialized, even if source is
  // a simple table.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("agg_query");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
    auto* group_by = query->set_group_by();
    group_by->add_column_names("ts");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("agg_query", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_FALSE(info.is_view);
}

TEST_F(SummarizerTest, ViewQueryableWithPagination) {
  // Verify that a view can be queried with LIMIT/OFFSET (as the UI does for
  // DataGrid pagination).
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("view_query");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
    table->add_column_names("dur");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("view_query", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_TRUE(info.is_view);

  // Query the view with LIMIT/OFFSET (simulates DataGrid pagination).
  auto paginated = tp_->ExecuteQuery("SELECT * FROM " + info.table_name +
                                     " LIMIT 10 OFFSET 0");
  // Verify the query succeeds and returns columns.
  EXPECT_GT(paginated.ColumnCount(), 0u);
  while (paginated.Next()) {
  }
  EXPECT_TRUE(paginated.Status().ok());
}

TEST_F(SummarizerTest, ViewRematerializedOnChange) {
  // Verify that a view is properly dropped and recreated when its source
  // table changes.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec1;
  {
    auto* query = spec1->add_query();
    query->set_id("v");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
  }
  auto spec_data1 = spec1.SerializeAsArray();

  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));
  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("v", &info1));
  ASSERT_TRUE(info1.exists);
  EXPECT_TRUE(info1.is_view);
  std::string old_view = info1.table_name;

  // Change the query (add a column).
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec2;
  {
    auto* query = spec2->add_query();
    query->set_id("v");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
    table->add_column_names("dur");
  }
  auto spec_data2 = spec2.SerializeAsArray();

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));
  SummarizerQueryResult info2;
  ASSERT_OK(summarizer_->Query("v", &info2));
  ASSERT_TRUE(info2.exists);

  // Should have a new name.
  EXPECT_NE(info2.table_name, old_view);

  // Old view should be dropped.
  auto check = tp_->ExecuteQuery("SELECT * FROM " + old_view + " LIMIT 0");
  check.Next();
  EXPECT_FALSE(check.Status().ok())
      << "Old view should be dropped after rematerialization";
}

TEST_F(SummarizerTest, DestructorCleansUpViews) {
  // Verify that destroying a summarizer cleans up views (not just tables).
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("v");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));
  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("v", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_TRUE(info.is_view);
  std::string view_name = info.table_name;

  // Verify view exists before destruction.
  {
    auto check = tp_->ExecuteQuery("SELECT * FROM " + view_name + " LIMIT 0");
    while (check.Next()) {
    }
    EXPECT_TRUE(check.Status().ok());
  }

  // Destroy the summarizer.
  summarizer_.reset();

  // View should be dropped.
  auto check = tp_->ExecuteQuery("SELECT * FROM " + view_name + " LIMIT 0");
  check.Next();
  EXPECT_FALSE(check.Status().ok()) << "View should be dropped on destruction";
}

TEST_F(SummarizerTest, ChainedViewsQueryCorrectly) {
  // Test a chain where B and C are views referencing A (a table).
  // CreateChainedSpec builds: A = SQL source, B = inner_query_id("A") with
  // a "value > 0" filter, C = inner_query_id("B") with no filter.
  // Verify the whole chain produces correct results when C is queried.
  auto spec_data =
      CreateChainedSpec("SELECT 1 as value UNION ALL SELECT 2 as value");

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  // Query all three to verify types.
  SummarizerQueryResult info_a;
  ASSERT_OK(summarizer_->Query("A", &info_a));
  ASSERT_TRUE(info_a.exists);
  EXPECT_FALSE(info_a.is_view) << "A has SQL source, should be a table";

  SummarizerQueryResult info_b;
  ASSERT_OK(summarizer_->Query("B", &info_b));
  ASSERT_TRUE(info_b.exists);
  EXPECT_TRUE(info_b.is_view) << "B is inner_query_id, should be a view";

  SummarizerQueryResult info_c;
  ASSERT_OK(summarizer_->Query("C", &info_c));
  ASSERT_TRUE(info_c.exists);
  EXPECT_TRUE(info_c.is_view) << "C is inner_query_id, should be a view";

  // B filters value > 0, so both rows (1 and 2) should pass through.
  // C has no additional filter, so it should have the same rows as B.
  EXPECT_EQ(info_c.row_count, 2);

  // Verify C's view resolves to the correct values.
  auto query_c = tp_->ExecuteQuery("SELECT value FROM " + info_c.table_name +
                                   " ORDER BY value");
  ASSERT_TRUE(query_c.Next());
  EXPECT_EQ(query_c.Get(0).AsLong(), 1);
  ASSERT_TRUE(query_c.Next());
  EXPECT_EQ(query_c.Get(0).AsLong(), 2);
  EXPECT_FALSE(query_c.Next());
  ASSERT_OK(query_c.Status());
}

TEST_F(SummarizerTest, QuerySwitchesFromTableToView) {
  // A query that initially has GROUP BY (→ table) then changes to not have
  // GROUP BY (→ view). The old TABLE should be dropped even though the new
  // one is a VIEW.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec1;
  {
    auto* query = spec1->add_query();
    query->set_id("q");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("ts");
    auto* group_by = query->set_group_by();
    group_by->add_column_names("ts");
  }
  auto spec_data1 = spec1.SerializeAsArray();

  SummarizerUpdateSpecResult result1;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data1.data(), spec_data1.size(), &result1));
  SummarizerQueryResult info1;
  ASSERT_OK(summarizer_->Query("q", &info1));
  ASSERT_TRUE(info1.exists);
  EXPECT_FALSE(info1.is_view) << "GROUP BY query should be a table";
  std::string old_name = info1.table_name;

  // Change to no GROUP BY (should become a view).
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec2;
  {
    auto* query = spec2->add_query();
    query->set_id("q");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("ts");
  }
  auto spec_data2 = spec2.SerializeAsArray();

  SummarizerUpdateSpecResult result2;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data2.data(), spec_data2.size(), &result2));
  SummarizerQueryResult info2;
  ASSERT_OK(summarizer_->Query("q", &info2));
  ASSERT_TRUE(info2.exists);
  EXPECT_TRUE(info2.is_view) << "Without GROUP BY should be a view";
  EXPECT_NE(info2.table_name, old_name);

  // Old table should be dropped.
  auto check = tp_->ExecuteQuery("SELECT * FROM " + old_name + " LIMIT 0");
  check.Next();
  EXPECT_FALSE(check.Status().ok())
      << "Old TABLE should be dropped when query switches to VIEW";
}

TEST_F(SummarizerTest, InnerQueryIdSourceCreatesView) {
  // An inner_query_id source (referencing another query) should create a view.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* base_query = spec->add_query();
    base_query->set_id("base");
    auto* sql_source = base_query->set_sql();
    sql_source->set_sql("SELECT 1 as value");
    sql_source->add_column_names("value");
  }
  {
    auto* ref_query = spec->add_query();
    ref_query->set_id("ref");
    ref_query->set_inner_query_id("base");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult base_info;
  ASSERT_OK(summarizer_->Query("base", &base_info));
  EXPECT_FALSE(base_info.is_view) << "SQL source should be a table";

  SummarizerQueryResult ref_info;
  ASSERT_OK(summarizer_->Query("ref", &ref_info));
  ASSERT_TRUE(ref_info.exists);
  EXPECT_FALSE(ref_info.table_name.empty());
  EXPECT_TRUE(ref_info.is_view) << "inner_query_id source should be a view";
}

TEST_F(SummarizerTest, TableSourceWithOrderByMaterializesAsTable) {
  // A query with ORDER BY should always be materialized, even if source is
  // a simple table, since re-sorting on every downstream query is wasteful.
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("ordered_query");
    auto* table = query->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
    auto* order_by = query->set_order_by();
    auto* ordering = order_by->add_ordering_specs();
    ordering->set_column_name("ts");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("ordered_query", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_FALSE(info.is_view) << "ORDER BY query should be a table";
}

TEST_F(SummarizerTest, InnerQuerySourceMaterializesAsTable) {
  // An embedded inner_query source can be arbitrarily complex, so it should
  // always be materialized as a TABLE (not a VIEW).
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  {
    auto* query = spec->add_query();
    query->set_id("nested");
    auto* inner = query->set_inner_query();
    auto* table = inner->set_table();
    table->set_table_name("slice");
    table->add_column_names("id");
    table->add_column_names("ts");
  }
  auto spec_data = spec.SerializeAsArray();

  SummarizerUpdateSpecResult result;
  ASSERT_OK(
      summarizer_->UpdateSpec(spec_data.data(), spec_data.size(), &result));

  SummarizerQueryResult info;
  ASSERT_OK(summarizer_->Query("nested", &info));
  ASSERT_TRUE(info.exists);
  EXPECT_FALSE(info.table_name.empty());
  EXPECT_FALSE(info.is_view) << "inner_query source should be a table";
}

TEST_F(SummarizerTest, SimpleSlicesSourceShouldUseView) {
  // A query with a simple_slices source (no aggregation) should be classified
  // as a view by ShouldUseView. We test the classification directly because
  // simple_slices requires runtime PerfettoSQL modules that aren't available
  // in the unit test environment.
  protozero::HeapBuffered<protos::pbzero::PerfettoSqlStructuredQuery> query;
  {
    auto* slices = query->set_simple_slices();
    slices->set_slice_name_glob("*");
  }
  auto query_data = query.SerializeAsArray();
  EXPECT_TRUE(
      SummarizerImpl::ShouldUseView(query_data.data(), query_data.size()))
      << "simple_slices source should use a view";
}

TEST_F(SummarizerTest, SimpleSlicesWithGroupByShouldNotUseView) {
  // A simple_slices source with GROUP BY should NOT be a view.
  protozero::HeapBuffered<protos::pbzero::PerfettoSqlStructuredQuery> query;
  {
    auto* slices = query->set_simple_slices();
    slices->set_slice_name_glob("*");
    auto* group_by = query->set_group_by();
    group_by->add_column_names("slice_name");
  }
  auto query_data = query.SerializeAsArray();
  EXPECT_FALSE(
      SummarizerImpl::ShouldUseView(query_data.data(), query_data.size()))
      << "simple_slices with GROUP BY should materialize as a table";
}

}  // namespace
}  // namespace perfetto::trace_processor::summary
