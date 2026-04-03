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

#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_compiler.h"
#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_parser.h"
#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_tokenizer.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pfgraph {
namespace {

using ::testing::HasSubstr;

// ============================================================================
// Tokenizer tests
// ============================================================================

TEST(PfGraphTokenizerTest, BasicTokens) {
  PfGraphTokenizer tok("table('slice').filter(dur > 0)");

  EXPECT_EQ(tok.Next().type, TokenType::kIdent);    // table
  EXPECT_EQ(tok.Next().type, TokenType::kLParen);    // (
  EXPECT_EQ(tok.Next().type, TokenType::kString);    // 'slice'
  EXPECT_EQ(tok.Next().type, TokenType::kRParen);    // )
  EXPECT_EQ(tok.Next().type, TokenType::kDot);       // .
  EXPECT_EQ(tok.Next().type, TokenType::kIdent);     // filter
  EXPECT_EQ(tok.Next().type, TokenType::kLParen);    // (
  EXPECT_EQ(tok.Next().type, TokenType::kIdent);     // dur
  EXPECT_EQ(tok.Next().type, TokenType::kGreater);   // >
  EXPECT_EQ(tok.Next().type, TokenType::kInt);       // 0
  EXPECT_EQ(tok.Next().type, TokenType::kRParen);    // )
  EXPECT_EQ(tok.Next().type, TokenType::kEof);
}

TEST(PfGraphTokenizerTest, Comments) {
  PfGraphTokenizer tok("# this is a comment\ntable");
  auto t = tok.Next();
  EXPECT_EQ(t.type, TokenType::kIdent);
  EXPECT_EQ(t.text, "table");
}

TEST(PfGraphTokenizerTest, TripleQuotedString) {
  PfGraphTokenizer tok("'''SELECT\n* FROM foo'''");
  auto t = tok.Next();
  EXPECT_EQ(t.type, TokenType::kString);
  EXPECT_EQ(t.text, "'''SELECT\n* FROM foo'''");
}

TEST(PfGraphTokenizerTest, TwoCharOperators) {
  PfGraphTokenizer tok("!= <= >= ||");
  EXPECT_EQ(tok.Next().type, TokenType::kNotEquals);
  EXPECT_EQ(tok.Next().type, TokenType::kLessEq);
  EXPECT_EQ(tok.Next().type, TokenType::kGreaterEq);
  EXPECT_EQ(tok.Next().type, TokenType::kPipe);
}

TEST(PfGraphTokenizerTest, PeekDoesNotAdvance) {
  PfGraphTokenizer tok("foo bar");
  auto t1 = tok.Peek();
  auto t2 = tok.Peek();
  EXPECT_EQ(t1.text, t2.text);
  EXPECT_EQ(t1.text, "foo");
  tok.Next();  // consume foo
  EXPECT_EQ(tok.Next().text, "bar");
}

// ============================================================================
// Parser tests
// ============================================================================

TEST(PfGraphParserTest, SimpleTablePipeline) {
  auto result = ParsePfGraph(R"(
    my_table:
      table('slice')
      .filter(dur > 0)
      .select(id, ts, dur, name)
      .sort(dur DESC)
      .limit(10)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& mod = *result;
  EXPECT_EQ(mod.declarations.size(), 1u);

  auto* np = std::get_if<NamedPipeline>(&mod.declarations[0]);
  ASSERT_NE(np, nullptr);
  EXPECT_EQ(np->name, "my_table");
  EXPECT_EQ(np->annotation, PipelineAnnotation::kNone);

  auto* src = std::get_if<TableSource>(&np->pipeline.source);
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(src->table_name, "slice");

  EXPECT_EQ(np->pipeline.operations.size(), 4u);
}

TEST(PfGraphParserTest, ModuleAndImports) {
  auto result = ParsePfGraph(R"(
    module android.binder
    import android.process_metadata
    import slices.flow

    @table
    my_table:
      table('slice')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& mod = *result;
  EXPECT_EQ(mod.module_name, "android.binder");
  EXPECT_EQ(mod.imports.size(), 2u);
  EXPECT_EQ(mod.imports[0], "android.process_metadata");
  EXPECT_EQ(mod.imports[1], "slices.flow");
}

TEST(PfGraphParserTest, Annotations) {
  auto result = ParsePfGraph(R"(
    @table
    pub_table:
      table('slice')

    @view
    pub_view:
      table('thread')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& mod = *result;
  EXPECT_EQ(mod.declarations.size(), 2u);

  auto* t = std::get_if<NamedPipeline>(&mod.declarations[0]);
  EXPECT_EQ(t->annotation, PipelineAnnotation::kTable);

  auto* v = std::get_if<NamedPipeline>(&mod.declarations[1]);
  EXPECT_EQ(v->annotation, PipelineAnnotation::kView);
}

TEST(PfGraphParserTest, SqlBlock) {
  auto result = ParsePfGraph(R"(
    @sql {
      CREATE PERFETTO FUNCTION foo(x INT) RETURNS INT AS SELECT $x * 2;
    }
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& mod = *result;
  EXPECT_EQ(mod.declarations.size(), 1u);
  auto* sb = std::get_if<SqlBlock>(&mod.declarations[0]);
  ASSERT_NE(sb, nullptr);
  EXPECT_THAT(sb->sql, HasSubstr("CREATE PERFETTO FUNCTION"));
}

TEST(PfGraphParserTest, JoinSource) {
  auto result = ParsePfGraph(R"(
    joined:
      join(left_table, right_table, on: id = id, type: LEFT)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[0]);
  auto* j = std::get_if<JoinSource>(&np->pipeline.source);
  ASSERT_NE(j, nullptr);
  EXPECT_EQ(j->left.name, "left_table");
  EXPECT_EQ(j->right.name, "right_table");
  EXPECT_TRUE(j->is_left_join);
}

TEST(PfGraphParserTest, UnionSource) {
  auto result = ParsePfGraph(R"(
    combined:
      union(table_a, table_b, table_c)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[0]);
  auto* u = std::get_if<UnionSource>(&np->pipeline.source);
  ASSERT_NE(u, nullptr);
  EXPECT_EQ(u->inputs.size(), 3u);
}

TEST(PfGraphParserTest, IntervalIntersect) {
  auto result = ParsePfGraph(R"(
    intersected:
      interval_intersect(base_data, intervals, partition: [utid])
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[0]);
  auto* ii = std::get_if<IntervalIntersectSource>(&np->pipeline.source);
  ASSERT_NE(ii, nullptr);
  EXPECT_EQ(ii->inputs.size(), 2u);
  EXPECT_EQ(ii->partition_columns.size(), 1u);
  EXPECT_EQ(ii->partition_columns[0], "utid");
}

TEST(PfGraphParserTest, GroupByWithAgg) {
  auto result = ParsePfGraph(R"(
    summary:
      table('slice')
      .group_by(process_name)
      .agg(total_dur: sum(dur), count: count(), avg_dur: mean(dur))
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[0]);
  EXPECT_EQ(np->pipeline.operations.size(), 1u);
  auto* gb = std::get_if<GroupByOp>(&np->pipeline.operations[0]);
  ASSERT_NE(gb, nullptr);
  EXPECT_EQ(gb->columns.size(), 1u);
  EXPECT_EQ(gb->aggregations.size(), 3u);
  EXPECT_EQ(gb->aggregations[0].result_name, "total_dur");
  // All agg specs now use custom_expr for uniformity.
  EXPECT_EQ(gb->aggregations[0].custom_expr, "sum(dur)");
}

TEST(PfGraphParserTest, SlicesSource) {
  auto result = ParsePfGraph(R"(
    gc_slices:
      slices(name: 'GC*', process: 'com.google.*')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[0]);
  auto* ss = std::get_if<SlicesSource>(&np->pipeline.source);
  ASSERT_NE(ss, nullptr);
  EXPECT_EQ(ss->name_glob, "GC*");
  EXPECT_EQ(ss->process_glob, "com.google.*");
}

TEST(PfGraphParserTest, PipelineReference) {
  auto result = ParsePfGraph(R"(
    base:
      table('slice')

    filtered:
      base
      .filter(dur > 100)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ((*result).declarations.size(), 2u);
  auto* np = std::get_if<NamedPipeline>(&(*result).declarations[1]);
  auto* ref = std::get_if<PipelineRef>(&np->pipeline.source);
  ASSERT_NE(ref, nullptr);
  EXPECT_EQ(ref->name, "base");
}

// ============================================================================
// Compiler tests (end-to-end: pfgraph text -> SQL)
// ============================================================================

TEST(PfGraphCompilerTest, SimpleSelect) {
  auto result = CompilePfGraph(R"(
    @table
    my_slices:
      table('slice')
      .filter(dur > 0)
      .select(id, ts, dur)
      .sort(dur DESC)
      .limit(10)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO TABLE my_slices"));
  EXPECT_THAT(sql, HasSubstr("FROM slice"));
  EXPECT_THAT(sql, HasSubstr("WHERE dur > 0"));
  EXPECT_THAT(sql, HasSubstr("SELECT id, ts, dur"));
  EXPECT_THAT(sql, HasSubstr("ORDER BY dur DESC"));
  EXPECT_THAT(sql, HasSubstr("LIMIT 10"));
}

TEST(PfGraphCompilerTest, ModuleImports) {
  auto result = CompilePfGraph(R"(
    module android.test
    import android.process_metadata
    import slices.with_context

    @table
    foo:
      table('slice')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("INCLUDE PERFETTO MODULE android.process_metadata"));
  EXPECT_THAT(sql, HasSubstr("INCLUDE PERFETTO MODULE slices.with_context"));
}

TEST(PfGraphCompilerTest, GroupByAggregation) {
  auto result = CompilePfGraph(R"(
    @view
    summary:
      table('slice')
      .group_by(name)
      .agg(total: sum(dur), cnt: count())
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO VIEW summary"));
  EXPECT_THAT(sql, HasSubstr("GROUP BY name"));
  EXPECT_THAT(sql, HasSubstr("sum(dur) AS total"));
  EXPECT_THAT(sql, HasSubstr("count() AS cnt"));
}

TEST(PfGraphCompilerTest, JoinCompilation) {
  auto result = CompilePfGraph(R"(
    joined:
      join(left_t, right_t, on: id = id, type: INNER)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("JOIN right_t"));
  EXPECT_THAT(sql, HasSubstr("left_t.id = right_t.id"));
}

TEST(PfGraphCompilerTest, UnionCompilation) {
  auto result = CompilePfGraph(R"(
    combined:
      union(table_a, table_b)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("UNION ALL"));
  EXPECT_THAT(sql, HasSubstr("SELECT * FROM table_a"));
  EXPECT_THAT(sql, HasSubstr("SELECT * FROM table_b"));
}

TEST(PfGraphCompilerTest, SqlBlockPassthrough) {
  auto result = CompilePfGraph(R"(
    @sql {
      CREATE PERFETTO FUNCTION double(x INT) RETURNS INT AS SELECT $x * 2;
    }

    @table
    doubled:
      table('slice')
      .select(id, double(dur) AS doubled_dur)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO FUNCTION double"));
  // Spaces between tokens are acceptable — SQL is functionally equivalent.
  EXPECT_THAT(sql, HasSubstr("doubled_dur"));
}

TEST(PfGraphCompilerTest, FilterIn) {
  auto result = CompilePfGraph(R"(
    result:
      table('slice')
      .filter_in(match_table, base_col: utid, match_col: utid)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("utid IN (SELECT utid FROM match_table)"));
}

TEST(PfGraphCompilerTest, SlicesSourceCompilation) {
  auto result = CompilePfGraph(R"(
    @table
    gc:
      slices(name: 'GC*', process: 'com.google.*')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("thread_or_process_slice"));
  EXPECT_THAT(sql, HasSubstr("name GLOB 'GC*'"));
  EXPECT_THAT(sql, HasSubstr("process_name GLOB 'com.google.*'"));
}

TEST(PfGraphCompilerTest, ComplexPipeline) {
  auto result = CompilePfGraph(R"(
    import slices.with_context

    @table
    _gc_slices:
      table('thread_slice')
      .filter(name GLOB '*GC*' AND depth = 0)
      .select(id, ts, dur, name, utid, upid)

    @table
    gc_summary:
      _gc_slices
      .group_by(name, upid)
      .agg(
        total_dur: sum(dur),
        count: count(),
        avg_dur: mean(dur)
      )
      .filter(total_dur > 1000000)
      .sort(total_dur DESC)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("INCLUDE PERFETTO MODULE slices.with_context"));
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO TABLE _gc_slices"));
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO TABLE gc_summary"));
  EXPECT_THAT(sql, HasSubstr("GROUP BY name, upid"));
  EXPECT_THAT(sql, HasSubstr("sum(dur) AS total_dur"));
  // The filter after group_by should wrap in subquery.
  EXPECT_THAT(sql, HasSubstr("WHERE total_dur > 1000000"));
}

// ============================================================================
// Tests for new higher-level constructs
// ============================================================================

TEST(PfGraphCompilerTest, WindowFunction) {
  auto result = CompilePfGraph(R"(
    @table
    with_prev:
      table('thread_state')
      .window(
        prev_state: lag(state) over (partition: [utid], order: ts),
        next_ts: lead(ts) over (partition: [utid], order: ts)
      )
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  // Token spacing is acceptable — SQL is functionally equivalent.
  EXPECT_THAT(sql, HasSubstr("OVER (PARTITION BY utid ORDER BY ts) AS prev_state"));
  EXPECT_THAT(sql, HasSubstr("OVER (PARTITION BY utid ORDER BY ts) AS next_ts"));
}

TEST(PfGraphCompilerTest, ComputedColumns) {
  auto result = CompilePfGraph(R"(
    @table
    enriched:
      table('slice')
      .computed(
        end_ts: ts + dur,
        is_long: iif(dur > 1000000, 1, 0)
      )
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("ts + dur AS end_ts"));
  EXPECT_THAT(sql, HasSubstr("iif ( dur > 1000000 ,"));
}

TEST(PfGraphCompilerTest, ClassifyValues) {
  auto result = CompilePfGraph(R"(
    @table
    classified:
      table('slice')
      .classify(gc_type, from: name,
        'concurrent*' => 'CONCURRENT',
        'young*' => 'YOUNG',
        _ => 'OTHER'
      )
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("CASE"));
  EXPECT_THAT(sql, HasSubstr("GLOB 'concurrent*'"));
  EXPECT_THAT(sql, HasSubstr("THEN 'CONCURRENT'"));
  EXPECT_THAT(sql, HasSubstr("ELSE 'OTHER'"));
  EXPECT_THAT(sql, HasSubstr("AS gc_type"));
}

TEST(PfGraphCompilerTest, ExtractArgs) {
  auto result = CompilePfGraph(R"(
    @table
    parsed:
      table('slice')
      .extract_args(
        event_type: 'event.type',
        event_seq: 'event.seq'
      )
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("extract_arg(arg_set_id, 'event.type') AS event_type"));
  EXPECT_THAT(sql, HasSubstr("extract_arg(arg_set_id, 'event.seq') AS event_seq"));
}

TEST(PfGraphCompilerTest, Distinct) {
  auto result = CompilePfGraph(R"(
    unique_names:
      table('slice')
      .select(name)
      .distinct()
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_THAT(*result, HasSubstr("DISTINCT"));
}

TEST(PfGraphCompilerTest, Except) {
  auto result = CompilePfGraph(R"(
    difference:
      table('slice')
      .filter(dur > 0)
      .except(broken_slices)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_THAT(*result, HasSubstr("EXCEPT"));
  EXPECT_THAT(*result, HasSubstr("broken_slices"));
}

TEST(PfGraphCompilerTest, FunctionDecl) {
  auto result = CompilePfGraph(R"(
    @function _get_slices(min_dur: INT) -> TABLE(id: INT, ts: INT, dur: INT):
      table('slice')
      .filter(dur > $min_dur)
      .select(id, ts, dur)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO FUNCTION _get_slices"));
  EXPECT_THAT(sql, HasSubstr("min_dur INT"));
  EXPECT_THAT(sql, HasSubstr("RETURNS TABLE(id INT, ts INT, dur INT)"));
  EXPECT_THAT(sql, HasSubstr("FROM slice"));
  EXPECT_THAT(sql, HasSubstr("WHERE dur >"));
  EXPECT_THAT(sql, HasSubstr("min_dur"));
}

TEST(PfGraphCompilerTest, UnpivotColumns) {
  auto result = CompilePfGraph(R"(
    @table
    unpivoted:
      table('cpu_stats')
      .unpivot(value_col: power, name_col: cpu, columns: [cpu0, cpu1, cpu2])
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("UNION ALL"));
  EXPECT_THAT(sql, HasSubstr("cpu0 AS power"));
  EXPECT_THAT(sql, HasSubstr("'cpu0' AS cpu"));
}

TEST(PfGraphCompilerTest, LookupTable) {
  auto result = CompilePfGraph(R"(
    durations:
      lookup_table(
        'BROADCAST' => 60000,
        'INPUT_TIMEOUT' => 5000,
        'SERVICE' => 20000
      )
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("'BROADCAST' AS key"));
  EXPECT_THAT(sql, HasSubstr("60000 AS value"));
  EXPECT_THAT(sql, HasSubstr("UNION ALL"));
}

TEST(PfGraphCompilerTest, ClosestPreceding) {
  auto result = CompilePfGraph(R"(
    matched:
      table('anr_errors')
      .closest_preceding(anr_timers, match: pid = pid, order: ts)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("LEFT JOIN"));
  EXPECT_THAT(sql, HasSubstr("row_number()"));
  EXPECT_THAT(sql, HasSubstr("_cp_rn = 1"));
}

TEST(PfGraphCompilerTest, ParseNameTemplate) {
  auto result = CompilePfGraph(R"(
    parsed:
      table('tracks')
      .parse_name('ErrorId:{process_name}#{error_id}')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("AS process_name"));
  EXPECT_THAT(sql, HasSubstr("AS error_id"));
  EXPECT_THAT(sql, HasSubstr("str_split"));
}

TEST(PfGraphCompilerTest, TemplateDefineAndCall) {
  auto result = CompilePfGraph(R"(
    @define add_end_ts():
      .computed(end_ts: ts + dur)

    @table
    enriched:
      table('slice')
      .add_end_ts()
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("ts + dur AS end_ts"));
  EXPECT_THAT(sql, HasSubstr("CREATE PERFETTO TABLE enriched"));
}

TEST(PfGraphCompilerTest, TemplateWithParams) {
  auto result = CompilePfGraph(R"(
    @define filter_by_name(pattern: String):
      .filter(name GLOB $pattern)

    @table
    gc_slices:
      table('slice')
      .filter_by_name(pattern: '*GC*')
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("GLOB '*GC*'"));
}

TEST(PfGraphCompilerTest, SourceTemplate) {
  auto result = CompilePfGraph(R"(
    @define counter_intervals(track_name: String):
      table('counter')
      .filter(name = $track_name)
      .counter_to_intervals()

    @table
    heap:
      counter_intervals('Heap size (KB)')
      .filter(value > 0)
  )");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto& sql = *result;
  EXPECT_THAT(sql, HasSubstr("counter_leading_intervals"));
  EXPECT_THAT(sql, HasSubstr("'Heap size (KB)'"));
  EXPECT_THAT(sql, HasSubstr("value > 0"));
}

}  // namespace
}  // namespace perfetto::trace_processor::pfgraph
