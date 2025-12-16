/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/generator/structured_query_generator.h"

#include <cctype>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/trace_processor/perfetto_sql/generator/perfettosql.descriptor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/perfetto_sql/structured_query.gen.h"

namespace perfetto::trace_processor::perfetto_sql::generator {
namespace {

using testing::UnorderedElementsAre;

using Query = ::perfetto::protos::gen::PerfettoSqlStructuredQuery;

std::vector<uint8_t> ToProto(const std::string& input) {
  base::StatusOr<std::vector<uint8_t>> output = protozero::TextToProto(
      kPerfettosqlDescriptor.data(), kPerfettosqlDescriptor.size(),
      ".perfetto.protos.PerfettoSqlStructuredQuery", "-", input);
  EXPECT_OK(output);
  if (!output.ok()) {
    return {};
  }
  EXPECT_FALSE(output->empty());
  return *output;
}

MATCHER_P(EqualsIgnoringWhitespace, param, "") {
  auto RemoveAllWhitespace = [](const std::string& input) {
    std::string result;
    result.reserve(input.length());
    std::copy_if(input.begin(), input.end(), std::back_inserter(result),
                 [](char c) { return !std::isspace(c); });
    return result;
  };
  return RemoveAllWhitespace(arg) == RemoveAllWhitespace(param);
}

TEST(StructuredQueryGeneratorTest, Operations) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "thread_slice_cpu_time"
    }
    referenced_modules: "linux.memory.process"
    filters: {
      column_name: "thread_name"
      op: EQUAL
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: NOT_EQUAL
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: LESS_THAN
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: LESS_THAN_EQUAL
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: GREATER_THAN
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: GREATER_THAN_EQUAL
      string_rhs: "bar"
    }
    filters: {
      column_name: "thread_name"
      op: IS_NULL
    }
    filters: {
      column_name: "thread_name"
      op: IS_NOT_NULL
    }
    filters: {
      column_name: "thread_name"
      op: GLOB
      string_rhs: "bar"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS
    (
        SELECT * FROM thread_slice_cpu_time
        WHERE thread_name = 'bar'
        AND thread_name != 'bar'
        AND thread_name < 'bar'
        AND thread_name <= 'bar'
        AND thread_name > 'bar'
        AND thread_name >= 'bar'
        AND thread_name IS NULL
        AND thread_name IS NOT NULL
        AND thread_name GLOB 'bar'
      ) SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, TableSource) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "memory_rss_and_swap_per_process"
    }
    referenced_modules: "linux.memory.process"
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
                WITH sq_0 AS (
                  SELECT
                    process_name,
                    SUM(
                      cast_double!(rss_and_swap * dur)) / cast_double!(SUM(dur))
                      AS avg_rss_and_swap
                  FROM memory_rss_and_swap_per_process
                  GROUP BY process_name
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("linux.memory.process"));
}

TEST(StructuredQueryGeneratorTest, GroupBySelectColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "memory_rss_and_swap_per_process"
    }
    referenced_modules: "linux.memory.process"
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
    select_columns: {column_name: "process_name"}
    select_columns: {
      column_name: "avg_rss_and_swap"
      alias : "cheese"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
                WITH sq_0 AS (
                  SELECT
                    process_name,
                    SUM(
                    cast_double!(rss_and_swap * dur))
                    / cast_double!(SUM(dur)) AS cheese
                  FROM memory_rss_and_swap_per_process
                  GROUP BY process_name
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("linux.memory.process"));
}

TEST(StructuredQueryGeneratorTest, SqlSource) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT id, ts, dur FROM slice"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT id, ts, dur FROM slice)
      )
    )
    SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithPreamble) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT id, ts, dur FROM slice"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
      preamble: "SELECT 1; SELECT 2;"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT id, ts, dur FROM slice)
      )
    )
    SELECT * FROM sq_0
    )"));
  ASSERT_THAT(gen.ComputePreambles(),
              UnorderedElementsAre("SELECT 1; SELECT 2;"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithMultistatement) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "; ;SELECT 1; SELECT 2;; SELECT id, ts, dur FROM slice"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT id, ts, dur FROM slice)
      )
    )
    SELECT * FROM sq_0
    )"));
  ASSERT_THAT(gen.ComputePreambles(),
              UnorderedElementsAre("SELECT 1; SELECT 2;; "));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithMultistatementWithSemicolon) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "; ;SELECT 1; SELECT 2;; SELECT id, ts, dur FROM slice;"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT id, ts, dur FROM slice)
      )
    )
    SELECT * FROM sq_0
    )"));
  ASSERT_THAT(gen.ComputePreambles(),
              UnorderedElementsAre("SELECT 1; SELECT 2;; "));
}

TEST(StructuredQueryGeneratorTest, IntervalIntersectSource) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
        referenced_modules: "linux.memory.process"
        filters: {
          column_name: "thread_name"
          op: EQUAL
          string_rhs: "bar"
        }
      }
      interval_intersect: {
        simple_slices: {
          slice_name_glob: "baz"
          process_name_glob: "system_server"
        }
      }
    }
    group_by: {
      aggregates: {
        column_name: "cpu_time"
        op: SUM
        result_column_name: "sum_cpu_time"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_2 AS (
                  SELECT * FROM (
                    SELECT
                      id,
                      ts,
                      dur,
                      name AS slice_name,
                      thread_name,
                      process_name,
                      track_name
                    FROM thread_or_process_slice
                    WHERE slice_name GLOB 'baz'
                      AND process_name GLOB 'system_server'
                  )
                ),
                sq_1 AS (
                  SELECT * FROM thread_slice_cpu_time
                  WHERE thread_name = 'bar'
                ),
                sq_0 AS (
                  SELECT SUM(cpu_time) AS sum_cpu_time
                  FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2)
                    SELECT ii.ts, ii.dur, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*
                    FROM _interval_intersect!((iibase, iisource0), ()) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                  )
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(
      gen.ComputeReferencedModules(),
      UnorderedElementsAre("intervals.intersect", "linux.memory.process",
                           "slices.with_context"));
}

TEST(StructuredQueryGeneratorTest, ColumnSelection) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "table_source_thread_slice"
    table: {
      table_name: "thread_slice"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
    referenced_modules: "slices.with_context"
    select_columns: {column_name: "id"}
    select_columns: {
      column_name: "dur"
      alias: "cheese"
    }
    select_columns: {column_name: "ts"}
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
    WITH sq_table_source_thread_slice AS
      (SELECT
        id,
        dur AS cheese,
        ts
      FROM thread_slice)
    SELECT * FROM sq_table_source_thread_slice
  )"));
}

TEST(StructuredQueryGeneratorTest, Median) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "table_source_thread_slice"
    table: {
      table_name: "thread_slice"
      column_names: "name"
      column_names: "dur"
    }
    referenced_modules: "slices.with_context"
    group_by: {
      column_names: "name"
      aggregates: {
        column_name: "dur"
        op: MEDIAN
        result_column_name: "cheese"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
    WITH sq_table_source_thread_slice AS
      (SELECT
        name,
        PERCENTILE(dur, 50) AS cheese
      FROM thread_slice
      GROUP BY name)
    SELECT * FROM sq_table_source_thread_slice
  )"));
}

TEST(StructuredQueryGeneratorTest, Percentile) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "table_source_thread_slice"
    table: {
      table_name: "thread_slice"
      column_names: "name"
      column_names: "dur"
    }
    referenced_modules: "slices.with_context"
    group_by: {
      column_names: "name"
      aggregates: {
        column_name: "dur"
        op: PERCENTILE
        result_column_name: "cheese"
        percentile: 99
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
    WITH sq_table_source_thread_slice AS
      (SELECT
        name,
        PERCENTILE(dur, 99.000000) AS cheese
      FROM thread_slice
      GROUP BY name)
    SELECT * FROM sq_table_source_thread_slice
  )"));
}

TEST(StructuredQueryGeneratorTest, CycleDetection) {
  StructuredQueryGenerator gen;
  auto proto_a = ToProto(R"(
    id: "a"
    inner_query_id: "b"
  )");
  ASSERT_OK(gen.AddQuery(proto_a.data(), proto_a.size()));

  auto proto_b = ToProto(R"(
    id: "b"
    inner_query_id: "a"
  )");
  ASSERT_OK(gen.AddQuery(proto_b.data(), proto_b.size()));

  auto ret = gen.GenerateById("a");
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Cycle detected in structured query"));
}

TEST(StructuredQueryGeneratorTest, SelfCycleDetection) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "a"
    inner_query_id: "a"
  )");
  ASSERT_OK(gen.AddQuery(proto.data(), proto.size()));

  auto ret = gen.GenerateById("a");
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Cycle detected in structured query"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithDependencies) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT s.id, s.ts, s.dur, t.track_name FROM $slice_table s JOIN $track_table t ON s.track_id = t.id"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
      column_names: "track_name"
      dependencies: {
        alias: "slice_table"
        query: {
          table: {
            table_name: "slice"
          }
        }
      }
      dependencies: {
        alias: "track_table"
        query: {
          table: {
            table_name: "track"
          }
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur, track_name
        FROM (SELECT s.id, s.ts, s.dur, t.track_name FROM sq_1 s JOIN sq_2 t ON s.track_id = t.id)
      )
    )
    SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithNoDependencies) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT s.id, s.ts, s.dur FROM slice s"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT s.id, s.ts, s.dur FROM slice s)
      )
    )
    SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithNoColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT s.id, s.ts, s.dur FROM slice s"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_0 AS (
      SELECT * FROM (
        SELECT *
        FROM (SELECT s.id, s.ts, s.dur FROM slice s)
      )
    )
    SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithUnusedDependencies) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT s.id, s.ts, s.dur FROM slice s"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
      dependencies: {
        alias: "unused_table"
        query: {
          table: {
            table_name: "slice"
          }
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT s.id, s.ts, s.dur FROM slice s)
      )
    )
    SELECT * FROM sq_0
    )"));
}

TEST(StructuredQueryGeneratorTest, SqlSourceWithNonExistentDependency) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT s.id, s.ts, s.dur FROM $non_existent_table s"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_0 AS (
      SELECT * FROM (
        SELECT id, ts, dur
        FROM (SELECT s.id, s.ts, s.dur FROM $non_existent_table s)
      )
    )
    SELECT * FROM sq_0
    )"));
}

}  // namespace

TEST(StructuredQueryGeneratorTest, ColumnTransformation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "table_source_thread_slice"
    table: {
      table_name: "thread_slice"
      column_names: "id"
      column_names: "ts"
      column_names: "dur"
    }
    referenced_modules: "slices.with_context"
    select_columns: {column_name_or_expression: "id"}
    select_columns: {
      alias: "ts_ms"
      column_name_or_expression: "ts / 1000"
    }
    select_columns: {
      alias: "ts_plus_dur"
      column_name_or_expression: "ts + dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
    WITH sq_table_source_thread_slice AS
      (SELECT
        id,
        ts / 1000 AS ts_ms,
        ts + dur AS ts_plus_dur
      FROM thread_slice)
    SELECT * FROM sq_table_source_thread_slice
  )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("slices.with_context"));
}

TEST(StructuredQueryGeneratorTest, ReferencedModulesInQuery) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    referenced_modules: "slices.with_context"
    referenced_modules: "module1"
    referenced_modules: "module2"
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK(ret);
  ASSERT_THAT(
      gen.ComputeReferencedModules(),
      UnorderedElementsAre("slices.with_context", "module1", "module2"));
}

TEST(StructuredQueryGeneratorTest, TableSourceWithDeprecatedModuleName) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "memory_rss_and_swap_per_process"
      module_name: "linux.memory.process"
    }
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
                WITH sq_0 AS (
                  SELECT
                    process_name,
                    SUM(
                      cast_double!(rss_and_swap * dur)) / cast_double!(SUM(dur))
                      AS avg_rss_and_swap
                  FROM memory_rss_and_swap_per_process
                  GROUP BY process_name
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("linux.memory.process"));
}

TEST(StructuredQueryGeneratorTest, CountAllAggregation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    group_by: {
      column_names: "name"
      aggregates: {
        op: COUNT
        result_column_name: "slice_count"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT
        name,
        COUNT(*) AS slice_count
      FROM slice
      GROUP BY name
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, CountDistinctAggregation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    group_by: {
      column_names: "track_id"
      aggregates: {
        column_name: "name"
        op: COUNT_DISTINCT
        result_column_name: "distinct_slice_names"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT
        track_id,
        COUNT(DISTINCT name) AS distinct_slice_names
      FROM slice
      GROUP BY track_id
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, CustomAggregation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"pb(
    table: { table_name: "slice" }
    group_by: {
      column_names: "name"
      aggregates: {
        op: CUSTOM
        custom_sql_expression: "SUM(dur * priority) / SUM(dur)"
        result_column_name: "weighted_avg_dur"
      }
    }
  )pb");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT
        name,
        SUM(dur * priority) / SUM(dur) AS weighted_avg_dur
      FROM slice
      GROUP BY name
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, AggregateToStringValidation) {
  // SUM without column name.
  {
    StructuredQueryGenerator gen;
    auto proto = ToProto(R"(
      table: {
        table_name: "slice"
      }
      group_by: {
        column_names: "name"
        aggregates: {
          op: SUM
          result_column_name: "slice_sum"
        }
      }
    )");
    auto ret = gen.Generate(proto.data(), proto.size());
    ASSERT_FALSE(ret.ok());
  }

  // PERCENTILE without percentile.
  {
    StructuredQueryGenerator gen;
    auto proto = ToProto(R"(
      table: {
        table_name: "slice"
      }
      group_by: {
        column_names: "name"
        aggregates: {
          op: PERCENTILE
          column_name: "dur"
          result_column_name: "slice_percentile"
        }
      }
    )");
    auto ret = gen.Generate(proto.data(), proto.size());
    ASSERT_FALSE(ret.ok());
  }

  // PERCENTILE without column name.
  {
    StructuredQueryGenerator gen;
    auto proto = ToProto(R"(
      table: {
        table_name: "slice"
      }
      group_by: {
        column_names: "name"
        aggregates: {
          op: PERCENTILE
          percentile: 99
          result_column_name: "slice_percentile"
        }
      }
    )");
    auto ret = gen.Generate(proto.data(), proto.size());
    ASSERT_FALSE(ret.ok());
  }

  // COUNT_DISTINCT without column name.
  {
    StructuredQueryGenerator gen;
    auto proto = ToProto(R"(
      table: {
        table_name: "slice"
      }
      group_by: {
        column_names: "name"
        aggregates: {
          op: COUNT_DISTINCT
          result_column_name: "distinct_count"
        }
      }
    )");
    auto ret = gen.Generate(proto.data(), proto.size());
    ASSERT_FALSE(ret.ok());
  }

  // CUSTOM without custom_sql_expression.
  {
    StructuredQueryGenerator gen;
    auto proto = ToProto(R"(
      table: {
        table_name: "slice"
      }
      group_by: {
        column_names: "name"
        aggregates: {
          op: CUSTOM
          result_column_name: "custom_agg"
        }
      }
    )");
    auto ret = gen.Generate(proto.data(), proto.size());
    ASSERT_FALSE(ret.ok());
  }
}

TEST(StructuredQueryGeneratorTest, ColumnTransformationAndAggregation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "outer_query"
    inner_query: {
      table: {
        table_name: "thread_slice"
      }
      select_columns: {
        alias: "dur_ms"
        column_name_or_expression: "dur / 1000"
      }
      select_columns: {
        column_name_or_expression: "name"
      }
    }
    group_by: {
      column_names: "name"
      aggregates: {
        column_name: "dur_ms"
        op: SUM
        result_column_name: "total_dur_ms"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
    WITH
      sq_1 AS (
        SELECT
          dur / 1000 AS dur_ms,
          name
        FROM thread_slice
      ),
      sq_outer_query AS (
        SELECT
          name,
          SUM(dur_ms) AS total_dur_ms
        FROM sq_1
        GROUP BY name
      )
    SELECT * FROM sq_outer_query
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinInnerJoin) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
      }
      right_query: {
        table: {
          table_name: "track"
        }
      }
      equality_columns: {
        left_column: "track_id"
        right_column: "id"
      }
      type: INNER
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 INNER JOIN sq_2 ON sq_1.track_id = sq_2.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinLeftJoin) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
      }
      right_query: {
        table: {
          table_name: "track"
        }
      }
      equality_columns: {
        left_column: "track_id"
        right_column: "id"
      }
      type: LEFT
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 LEFT JOIN sq_2 ON sq_1.track_id = sq_2.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinComplex) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
      }
      right_query: {
        table: {
          table_name: "track"
        }
      }
      equality_columns: {
        left_column: "track_id"
        right_column: "id"
      }
      type: INNER
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice WHERE dur > 1000),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 INNER JOIN sq_2 ON sq_1.track_id = sq_2.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinFreeformCondition) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
      }
      right_query: {
        table: {
          table_name: "track"
        }
      }
      freeform_condition: {
        left_query_alias: "s"
        right_query_alias: "t"
        sql_expression: "s.track_id = t.id"
      }
      type: INNER
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 AS s INNER JOIN sq_2 AS t ON s.track_id = t.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinFreeformConditionComplex) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
      }
      right_query: {
        table: {
          table_name: "slice"
        }
      }
      freeform_condition: {
        left_query_alias: "parent"
        right_query_alias: "child"
        sql_expression: "child.parent_id = parent.id AND child.ts >= parent.ts AND child.ts < parent.ts + parent.dur"
      }
      type: INNER
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM slice),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 AS parent INNER JOIN sq_2 AS child ON child.parent_id = parent.id AND child.ts >= parent.ts AND child.ts < parent.ts + parent.dur
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, JoinFreeformConditionLeftJoin) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_join: {
      left_query: {
        table: {
          table_name: "slice"
        }
      }
      right_query: {
        table: {
          table_name: "track"
        }
      }
      freeform_condition: {
        left_query_alias: "s"
        right_query_alias: "t"
        sql_expression: "s.track_id = t.id AND t.name LIKE '%gpu%'"
      }
      type: LEFT
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT * FROM sq_1 AS s LEFT JOIN sq_2 AS t ON s.track_id = t.id AND t.name LIKE '%gpu%'
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, UnionBasic) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
      }
      queries: {
        table: {
          table_name: "track"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        WITH union_query_0 AS (SELECT * FROM sq_1), union_query_1 AS (SELECT * FROM sq_2)
        SELECT * FROM union_query_0 UNION SELECT * FROM union_query_1
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, UnionAll) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
      }
      queries: {
        table: {
          table_name: "track"
        }
      }
      use_union_all: true
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        WITH union_query_0 AS (SELECT * FROM sq_1), union_query_1 AS (SELECT * FROM sq_2)
        SELECT * FROM union_query_0 UNION ALL SELECT * FROM union_query_1
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, UnionMultipleQueries) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
      }
      queries: {
        table: {
          table_name: "track"
        }
      }
      queries: {
        table: {
          table_name: "thread"
        }
      }
      use_union_all: true
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_3 AS (SELECT * FROM thread),
    sq_2 AS (SELECT * FROM track),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        WITH union_query_0 AS (SELECT * FROM sq_1), union_query_1 AS (SELECT * FROM sq_2), union_query_2 AS (SELECT * FROM sq_3)
        SELECT * FROM union_query_0 UNION ALL SELECT * FROM union_query_1 UNION ALL SELECT * FROM union_query_2
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, UnionWithFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
      }
      queries: {
        table: {
          table_name: "slice"
        }
        filters: {
          column_name: "name"
          op: GLOB
          string_rhs: "*gpu*"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM slice WHERE name GLOB '*gpu*'),
    sq_1 AS (SELECT * FROM slice WHERE dur > 1000),
    sq_0 AS (
      SELECT * FROM (
        WITH union_query_0 AS (SELECT * FROM sq_1), union_query_1 AS (SELECT * FROM sq_2)
        SELECT * FROM union_query_0 UNION SELECT * FROM union_query_1
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, UnionWithSingleQueryFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Union must specify at least two queries"));
}

TEST(StructuredQueryGeneratorTest, UnionWithMatchingColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "dur"
        }
      }
      queries: {
        table: {
          table_name: "sched"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "dur"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK(ret);
}

TEST(StructuredQueryGeneratorTest, UnionWithDifferentColumnCountFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
      }
      queries: {
        table: {
          table_name: "sched"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "dur"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("different column counts"));
}

TEST(StructuredQueryGeneratorTest, UnionWithDifferentColumnNamesFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "dur"
        }
      }
      queries: {
        table: {
          table_name: "sched"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "name"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("different column sets"));
}

TEST(StructuredQueryGeneratorTest, UnionWithDifferentColumnOrderSucceeds) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_union: {
      queries: {
        table: {
          table_name: "slice"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
        select_columns: {
          column_name: "dur"
        }
      }
      queries: {
        table: {
          table_name: "sched"
        }
        select_columns: {
          column_name: "dur"
        }
        select_columns: {
          column_name: "id"
        }
        select_columns: {
          column_name: "ts"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_TRUE(ret.ok()) << ret.status().message();
  EXPECT_EQ(*ret, R"(WITH sq_2 AS (
  SELECT dur, id, ts
  FROM sched
),
sq_1 AS (
  SELECT id, ts, dur
  FROM slice
),
sq_0 AS (
  SELECT *
  FROM (
    WITH union_query_0 AS (
    SELECT *
    FROM sq_1), union_query_1 AS (
    SELECT *
    FROM sq_2)
    SELECT *
    FROM union_query_0
    UNION
    SELECT *
    FROM union_query_1)
)
SELECT *
FROM sq_0)");
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithEqualityColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM process),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT core.*, input.name
        FROM sq_1 AS core
        LEFT JOIN sq_2 AS input ON core.upid = input.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithFreeformCondition) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "thread"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      input_columns: {column_name_or_expression: "tid"}
      freeform_condition: {
        left_query_alias: "core"
        right_query_alias: "input"
        sql_expression: "core.utid = input.id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM thread),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT core.*, input.name, input.tid
        FROM sq_1 AS core
        LEFT JOIN sq_2 AS input ON core.utid = input.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsMultipleColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      input_columns: {column_name_or_expression: "pid"}
      input_columns: {column_name_or_expression: "cmdline"}
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM process),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT core.*, input.name, input.pid, input.cmdline
        FROM sq_1 AS core
        LEFT JOIN sq_2 AS input ON core.upid = input.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
        filters: {
          column_name: "pid"
          op: NOT_EQUAL
          int64_rhs: 0
        }
      }
      input_columns: {column_name_or_expression: "name"}
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM process WHERE pid != 0),
    sq_1 AS (SELECT * FROM slice WHERE dur > 1000),
    sq_0 AS (
      SELECT * FROM (
        SELECT core.*, input.name
        FROM sq_1 AS core
        LEFT JOIN sq_2 AS input ON core.upid = input.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsMissingCoreQueryFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("AddColumns must specify a core query"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsMissingInputQueryFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("AddColumns must specify an input query"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsNoInputColumnsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(
      ret.status().message(),
      testing::HasSubstr("AddColumns must specify at least one input column"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsNoConditionFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("AddColumns must specify either "
                                 "equality_columns or freeform_condition"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithInvalidLeftAliasFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      freeform_condition: {
        left_query_alias: "left"
        right_query_alias: "input"
        sql_expression: "left.upid = input.id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(
      ret.status().message(),
      testing::HasSubstr("FreeformCondition left_query_alias must be 'core'"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithInvalidRightAliasFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {column_name_or_expression: "name"}
      freeform_condition: {
        left_query_alias: "core"
        right_query_alias: "right"
        sql_expression: "core.upid = right.id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr(
                  "FreeformCondition right_query_alias must be 'input'"));
}

TEST(StructuredQueryGeneratorTest, AddColumnsWithAlias) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_add_columns: {
      core_query: {
        table: {
          table_name: "slice"
        }
      }
      input_query: {
        table: {
          table_name: "process"
        }
      }
      input_columns: {
        column_name_or_expression: "name"
        alias: "process_name"
      }
      input_columns: {
        column_name_or_expression: "pid"
        alias: "process_id"
      }
      equality_columns: {
        left_column: "upid"
        right_column: "id"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM process),
    sq_1 AS (SELECT * FROM slice),
    sq_0 AS (
      SELECT * FROM (
        SELECT core.*, input.name AS process_name, input.pid AS process_id
        FROM sq_1 AS core
        LEFT JOIN sq_2 AS input ON core.upid = input.id
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, LimitWithoutOffset) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: 10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice LIMIT 10
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, LimitAndOffset) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: 100
    offset: 50
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice LIMIT 100 OFFSET 50
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, LimitWithFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    filters: {
      column_name: "dur"
      op: GREATER_THAN
      int64_rhs: 1000
    }
    limit: 5
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice WHERE dur > 1000 LIMIT 5
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, LimitWithGroupBy) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    group_by: {
      column_names: "name"
      aggregates: {
        column_name: "dur"
        op: SUM
        result_column_name: "total_dur"
      }
    }
    limit: 20
    offset: 10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT name, SUM(dur) AS total_dur
      FROM slice
      GROUP BY name
      LIMIT 20 OFFSET 10
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OffsetWithoutLimitFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    offset: 10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("OFFSET requires LIMIT"));
}

TEST(StructuredQueryGeneratorTest, OrderByAsc) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    order_by: {
      ordering_specs: {
        column_name: "ts"
        direction: ASC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice ORDER BY ts ASC
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByDesc) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    order_by: {
      ordering_specs: {
        column_name: "dur"
        direction: DESC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice ORDER BY dur DESC
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByMultipleColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    order_by: {
      ordering_specs: {
        column_name: "name"
        direction: ASC
      }
      ordering_specs: {
        column_name: "ts"
        direction: DESC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice ORDER BY name ASC, ts DESC
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByWithFiltersAndLimit) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    filters: {
      column_name: "dur"
      op: GREATER_THAN
      int64_rhs: 1000
    }
    order_by: {
      ordering_specs: {
        column_name: "dur"
        direction: DESC
      }
    }
    limit: 10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice WHERE dur > 1000 ORDER BY dur DESC LIMIT 10
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByWithGroupBy) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    group_by: {
      column_names: "name"
      aggregates: {
        column_name: "dur"
        op: SUM
        result_column_name: "total_dur"
      }
    }
    order_by: {
      ordering_specs: {
        column_name: "total_dur"
        direction: DESC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT name, SUM(dur) AS total_dur
      FROM slice
      GROUP BY name
      ORDER BY total_dur DESC
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByWithoutDirectionDefaultsToAsc) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    order_by: {
      ordering_specs: {
        column_name: "ts"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice ORDER BY ts
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, NegativeLimitFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: -10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("LIMIT must be non-negative"));
}

TEST(StructuredQueryGeneratorTest, NegativeOffsetFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: 10
    offset: -5
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("OFFSET must be non-negative"));
}

TEST(StructuredQueryGeneratorTest, LimitZeroIsValid) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: 0
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice LIMIT 0
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OffsetZeroIsValid) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
    }
    limit: 10
    offset: 0
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice LIMIT 10 OFFSET 0
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, OrderByWithInnerQuerySimpleSlices) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    inner_query: {
      id: "0"
      simple_slices: {
      }
    }
    order_by: {
      ordering_specs: {
        column_name: "slice_name"
        direction: ASC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Note: The inner_query has id="0" which would collide with the root query's
  // auto-generated name (sq_0), so the collision avoidance renames it to
  // sq_0_0.
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0_0 AS (
      SELECT * FROM (
        SELECT
          id,
          ts,
          dur,
          name AS slice_name,
          thread_name,
          process_name,
          track_name
        FROM thread_or_process_slice
      )
    )
    SELECT * FROM sq_0_0 ORDER BY slice_name ASC
  )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("slices.with_context"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupSimpleOr) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
        column_name: "name"
        op: EQUAL
        string_rhs: "foo"
      }
      filters: {
        column_name: "name"
        op: EQUAL
        string_rhs: "bar"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo' OR name = 'bar'
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupSimpleAnd) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
      column_names: "dur"
    }
    experimental_filter_group: {
      op: AND
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
      filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo' AND dur > 1000
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupNestedAndOr) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
      column_names: "dur"
    }
    experimental_filter_group: {
      op: AND
      groups: {
        op: OR
        filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
        filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "bar"
        }
      }
      groups: {
        op: OR
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
        filters: {
          column_name: "dur"
          op: LESS_THAN
          int64_rhs: 100
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE (name = 'foo' OR name = 'bar') AND (dur > 1000 OR dur < 100)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupComplexNested) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
      column_names: "dur"
      column_names: "ts"
    }
    experimental_filter_group: {
      op: OR
      groups: {
        op: AND
        filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "critical"
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 5000
        }
      }
      groups: {
        op: AND
        filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "important"
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 10000
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE (name = 'critical' AND dur > 5000) OR (name = 'important' AND dur > 10000)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupWithMultipleValues) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
          string_rhs: "bar"
          string_rhs: "baz"
        }
      filters: {
          column_name: "name"
          op: GLOB
          string_rhs: "test*"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo' OR name = 'bar' OR name = 'baz' OR name GLOB 'test*'
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalFilterGroupTakesPrecedenceOverFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    filters: {
      column_name: "name"
      op: EQUAL
      string_rhs: "should_not_appear"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "bar"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo' OR name = 'bar'
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupWithIsNull) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "name"
          op: IS_NULL
        }
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name IS NULL OR name = 'foo'
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalFilterGroupMissingOperatorFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must specify an operator"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalFilterGroupUnspecifiedOperatorFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: UNSPECIFIED
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must specify an operator"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupEmptyItemsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: AND
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must have at least one"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupSingleItem) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo'
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupWithInt64AndDouble) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "dur"
      column_names: "cpu"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
          int64_rhs: 5000
        }
      filters: {
          column_name: "cpu"
          op: LESS_THAN
          double_rhs: 50.5
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE dur > 1000 OR dur > 5000 OR cpu < 50.500000
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupDeepNesting) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
      column_names: "dur"
      column_names: "ts"
    }
    experimental_filter_group: {
      op: OR
      groups: {
        op: AND
        groups: {
          op: OR
          filters: {
            column_name: "name"
            op: EQUAL
            string_rhs: "a"
          }
          filters: {
            column_name: "name"
            op: EQUAL
            string_rhs: "b"
          }
        }
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 100
        }
      }
      filters: {
        column_name: "ts"
        op: LESS_THAN
        int64_rhs: 1000
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE ts < 1000 OR (dur > 100 AND (name = 'a' OR name = 'b'))
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, FilterGroupMissingOperatorFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: AND
      groups: {
        filters: {
          column_name: "name"
          op: EQUAL
          string_rhs: "foo"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must specify an operator"));
}

TEST(StructuredQueryGeneratorTest, FilterGroupEmptyItemsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: AND
      groups: {
        op: OR
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must have at least one"));
}

TEST(StructuredQueryGeneratorTest, FilterWithoutRhsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
          column_name: "name"
          op: EQUAL
        }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().c_message(),
              testing::HasSubstr("must specify a right-hand side"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupWithSqlExpression) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
    }
    experimental_filter_group: {
      op: OR
      filters: {
        column_name: "name"
        op: EQUAL
        string_rhs: "foo"
      }
      sql_expressions: "LENGTH(name) > 10"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'foo' OR LENGTH(name) > 10
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalFilterGroupMixedTypes) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table: {
      table_name: "slice"
      column_names: "id"
      column_names: "name"
      column_names: "dur"
    }
    experimental_filter_group: {
      op: OR
      filters: {
        column_name: "name"
        op: EQUAL
        string_rhs: "critical"
      }
      sql_expressions: "dur * 2 > ts"
      groups: {
        op: AND
        filters: {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
        sql_expressions: "name LIKE '%slow%'"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT * FROM slice
      WHERE name = 'critical' OR (dur > 1000 AND name LIKE '%slow%') OR dur * 2 > ts
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, IntervalIntersectWithPartitionColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
        referenced_modules: "linux.memory.process"
      }
      interval_intersect: {
        simple_slices: {
          slice_name_glob: "baz"
          process_name_glob: "system_server"
        }
      }
      partition_columns: "utid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_2 AS (
                  SELECT * FROM (
                    SELECT
                      id,
                      ts,
                      dur,
                      name AS slice_name,
                      thread_name,
                      process_name,
                      track_name
                    FROM thread_or_process_slice
                    WHERE slice_name GLOB 'baz'
                      AND process_name GLOB 'system_server'
                  )
                ),
                sq_1 AS (
                  SELECT * FROM thread_slice_cpu_time
                ),
                sq_0 AS (
                  SELECT * FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2)
                    SELECT ii.ts, ii.dur, ii.utid, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*
                    FROM _interval_intersect!((iibase, iisource0), (utid)) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                  )
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(
      gen.ComputeReferencedModules(),
      UnorderedElementsAre("intervals.intersect", "linux.memory.process",
                           "slices.with_context"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithMultiplePartitionColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "utid"
      partition_columns: "upid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_2 AS (
                  SELECT * FROM slice
                ),
                sq_1 AS (
                  SELECT * FROM thread_slice_cpu_time
                ),
                sq_0 AS (
                  SELECT * FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2)
                    SELECT ii.ts, ii.dur, ii.utid, ii.upid, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*
                    FROM _interval_intersect!((iibase, iisource0), (utid, upid)) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                  )
                )
                SELECT * FROM sq_0
              )"));
}

TEST(StructuredQueryGeneratorTest, IntervalIntersectWithEmptyPartitionColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {
          slice_name_glob: "baz"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_2 AS (
                  SELECT * FROM (
                    SELECT
                      id,
                      ts,
                      dur,
                      name AS slice_name,
                      thread_name,
                      process_name,
                      track_name
                    FROM thread_or_process_slice
                    WHERE slice_name GLOB 'baz'
                  )
                ),
                sq_1 AS (
                  SELECT * FROM thread_slice_cpu_time
                ),
                sq_0 AS (
                  SELECT * FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2)
                    SELECT ii.ts, ii.dur, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*
                    FROM _interval_intersect!((iibase, iisource0), ()) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                  )
                )
                SELECT * FROM sq_0
              )"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithReservedPartitionColumnIdFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "id"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'id' is reserved"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithReservedPartitionColumnTsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'ts' is reserved"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithReservedPartitionColumnDurFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "dur"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'dur' is reserved"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithMixedPartitionColumnsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "utid"
      partition_columns: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'ts' is reserved"));
}

// Edge case 1: Duplicate partition columns
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithDuplicatePartitionColumnsFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "utid"
      partition_columns: "utid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'utid' is duplicated"));
}

// Edge case 2: Empty string partition column
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithEmptyStringPartitionColumnFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: ""
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column cannot be empty"));
}

// Edge case 3: Case variations of reserved columns
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithUppercaseIdPartitionColumnFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "ID"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'ID' is reserved"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithMixedCaseTsPartitionColumnFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "Ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'Ts' is reserved"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithUppercaseDurPartitionColumnFails) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        simple_slices: {}
      }
      partition_columns: "DUR"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  ASSERT_THAT(ret.status().message(),
              testing::HasSubstr("Partition column 'DUR' is reserved"));
}

// Edge case 4: Whitespace in column names
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithLeadingWhitespacePartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: " utid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should include the space in the generated SQL
  ASSERT_THAT(
      res.c_str(),
      testing::HasSubstr("_interval_intersect!((iibase, iisource0), ( utid))"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithTrailingWhitespacePartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "utid "
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should include the space in the generated SQL
  ASSERT_THAT(
      res.c_str(),
      testing::HasSubstr("_interval_intersect!((iibase, iisource0), (utid ))"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithWhitespaceOnlyPartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "   "
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should include the whitespace in the generated SQL as-is (no normalization)
  ASSERT_THAT(
      res.c_str(),
      testing::HasSubstr("_interval_intersect!((iibase, iisource0), (   ))"));
}

// Edge case 5: Multiple interval_intersect sources with partition columns
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithMultipleSourcesAndPartitionColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      interval_intersect: {
        simple_slices: {
          slice_name_glob: "foo"
        }
      }
      partition_columns: "utid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_3 AS (
                  SELECT * FROM (
                    SELECT
                      id,
                      ts,
                      dur,
                      name AS slice_name,
                      thread_name,
                      process_name,
                      track_name
                    FROM thread_or_process_slice
                    WHERE slice_name GLOB 'foo'
                  )
                ),
                sq_2 AS (
                  SELECT * FROM slice
                ),
                sq_1 AS (
                  SELECT * FROM thread_slice_cpu_time
                ),
                sq_0 AS (
                  SELECT * FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2),
                      iisource1 AS (SELECT * FROM sq_3)
                    SELECT ii.ts, ii.dur, ii.utid, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*, source_2.id AS id_2, source_2.ts AS ts_2, source_2.dur AS dur_2, source_2.*
                    FROM _interval_intersect!((iibase, iisource0, iisource1), (utid)) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                    JOIN iisource1 AS source_2 ON ii.id_2 = source_2.id
                  )
                )
                SELECT * FROM sq_0
              )"));
  ASSERT_THAT(
      gen.ComputeReferencedModules(),
      UnorderedElementsAre("intervals.intersect", "slices.with_context"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithMultipleSourcesAndMultiplePartitionColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "base_table"
        }
      }
      interval_intersect: {
        table: {
          table_name: "source1"
        }
      }
      interval_intersect: {
        table: {
          table_name: "source2"
        }
      }
      interval_intersect: {
        table: {
          table_name: "source3"
        }
      }
      partition_columns: "utid"
      partition_columns: "upid"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res.c_str(), EqualsIgnoringWhitespace(R"(
                WITH sq_4 AS (
                  SELECT * FROM source3
                ),
                sq_3 AS (
                  SELECT * FROM source2
                ),
                sq_2 AS (
                  SELECT * FROM source1
                ),
                sq_1 AS (
                  SELECT * FROM base_table
                ),
                sq_0 AS (
                  SELECT * FROM (
                    WITH
                      iibase AS (SELECT * FROM sq_1),
                      iisource0 AS (SELECT * FROM sq_2),
                      iisource1 AS (SELECT * FROM sq_3),
                      iisource2 AS (SELECT * FROM sq_4)
                    SELECT ii.ts, ii.dur, ii.utid, ii.upid, base_0.id AS id_0, base_0.ts AS ts_0, base_0.dur AS dur_0, base_0.*, source_1.id AS id_1, source_1.ts AS ts_1, source_1.dur AS dur_1, source_1.*, source_2.id AS id_2, source_2.ts AS ts_2, source_2.dur AS dur_2, source_2.*, source_3.id AS id_3, source_3.ts AS ts_3, source_3.dur AS dur_3, source_3.*
                    FROM _interval_intersect!((iibase, iisource0, iisource1, iisource2), (utid, upid)) ii
                    JOIN iibase AS base_0 ON ii.id_0 = base_0.id
                    JOIN iisource0 AS source_1 ON ii.id_1 = source_1.id
                    JOIN iisource1 AS source_2 ON ii.id_2 = source_2.id
                    JOIN iisource2 AS source_3 ON ii.id_3 = source_3.id
                  )
                )
                SELECT * FROM sq_0
              )"));
}

// Edge case 7: Special characters in column names
TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithHyphenInPartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "col-name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should not escape special characters
  ASSERT_THAT(res.c_str(),
              testing::HasSubstr(
                  "_interval_intersect!((iibase, iisource0), (col-name))"));
}

TEST(StructuredQueryGeneratorTest, IntervalIntersectWithDotInPartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "col.name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should not escape special characters
  ASSERT_THAT(res.c_str(),
              testing::HasSubstr(
                  "_interval_intersect!((iibase, iisource0), (col.name))"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithSpaceInPartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "col name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should not escape or modify the space
  ASSERT_THAT(res.c_str(),
              testing::HasSubstr(
                  "_interval_intersect!((iibase, iisource0), (col name))"));
}

TEST(StructuredQueryGeneratorTest,
     IntervalIntersectWithBacktickInPartitionColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "thread_slice_cpu_time"
        }
      }
      interval_intersect: {
        table: {
          table_name: "slice"
        }
      }
      partition_columns: "col`name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should not escape the backtick
  ASSERT_THAT(res.c_str(),
              testing::HasSubstr(
                  "_interval_intersect!((iibase, iisource0), (col`name))"));
}

// Regression test for CTE name collision bug where queries with explicit IDs
// could collide with auto-generated index-based names.
TEST(StructuredQueryGeneratorTest, IntervalIntersectNoDuplicateCteNames) {
  StructuredQueryGenerator gen;
  // This test reproduces a bug where:
  // - A nested query at index 2 would get table_name="sq_2"
  // - An inner_query with id="2" would also get table_name="sq_2"
  // - This caused duplicate CTE definitions
  auto proto = ToProto(R"(
    id: "4"
    interval_intersect {
      base {
        inner_query {
          id: "2"
          table {
            table_name: "thread_or_process_slice"
          }
        }
        filters {
          column_name: "dur"
          op: GREATER_THAN_EQUAL
          int64_rhs: 0
        }
        limit: 10
      }
      interval_intersect {
        inner_query {
          id: "0"
          table {
            table_name: "thread_or_process_slice"
          }
        }
        filters {
          column_name: "dur"
          op: GREATER_THAN_EQUAL
          int64_rhs: 0
        }
        limit: 10
      }
      partition_columns: "process_name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // The bug would produce SQL with duplicate "sq_2 AS" definitions.
  // With the fix, we should have unique names like sq_2 and sq_2_0.
  // Simple check: count "sq_2 AS" occurrences - should be exactly 1
  size_t count = 0;
  size_t pos = 0;
  std::string search_str = "sq_2 AS";
  while ((pos = res.find(search_str, pos)) != std::string::npos) {
    count++;
    pos += search_str.length();
  }
  EXPECT_EQ(count, 1u) << "Expected exactly one 'sq_2 AS' in: " << res;

  // Verify the collision was resolved (should have sq_2_0 or similar)
  EXPECT_THAT(res, testing::AnyOf(testing::HasSubstr("sq_2_0"),
                                  testing::HasSubstr("sq_0")));

  // Verify interval intersect macro is present
  EXPECT_THAT(res, testing::HasSubstr("_interval_intersect!"));
}

// Test multiple levels of nesting with ID collisions
TEST(StructuredQueryGeneratorTest, NestedQueriesWithIdCollisions) {
  StructuredQueryGenerator gen;
  // Create a scenario where index-based names would collide with explicit IDs
  // Root at index 0, inner with id="1", innermost with id="0"
  // Without collision avoidance, both root and innermost want "sq_0"
  auto proto = ToProto(R"(
    id: "root"
    inner_query {
      id: "1"
      inner_query {
        id: "0"
        table {
          table_name: "test_table"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Should have multiple CTEs with unique names
  EXPECT_THAT(res, testing::HasSubstr("WITH"));
  EXPECT_THAT(res, testing::HasSubstr(" AS ("));

  // With the fix, collision between root (index 0) and inner (id="0") is
  // avoided We should see sq_0 appear exactly once (or sq_0_1 if there was a
  // collision)
  size_t count_sq_0 = 0;
  size_t pos = 0;
  while ((pos = res.find("sq_0 AS", pos)) != std::string::npos) {
    count_sq_0++;
    pos += 7;
  }
  EXPECT_LE(count_sq_0, 1u) << "sq_0 appears multiple times in: " << res;

  // Should reference the test_table
  EXPECT_THAT(res, testing::HasSubstr("test_table"));
}

// Test a complex query with interval intersect and aggregation, no explicit IDs
TEST(StructuredQueryGeneratorTest,
     ComplexIntervalIntersectWithAggregationNoIds) {
  StructuredQueryGenerator gen;
  // Complex scenario: interval intersect with filters, followed by aggregation
  // This tests that auto-generated index-based names work correctly
  auto proto = ToProto(R"(
    interval_intersect {
      base {
        table {
          table_name: "slice"
        }
        filters {
          column_name: "dur"
          op: GREATER_THAN
          int64_rhs: 1000
        }
      }
      interval_intersect {
        table {
          table_name: "thread_slice"
        }
        filters {
          column_name: "name"
          op: GLOB
          string_rhs: "important*"
        }
      }
      interval_intersect {
        inner_query {
          table {
            table_name: "process_slice"
          }
          filters {
            column_name: "dur"
            op: GREATER_THAN_EQUAL
            int64_rhs: 500
          }
        }
      }
      partition_columns: "process_name"
    }
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "dur"
        op: SUM
        result_column_name: "total_dur"
      }
      aggregates: {
        op: COUNT
        result_column_name: "count"
      }
    }
    order_by: {
      ordering_specs: {
        column_name: "total_dur"
        direction: DESC
      }
    }
    limit: 100
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify the query contains expected components
  EXPECT_THAT(res, testing::HasSubstr("_interval_intersect!"));
  EXPECT_THAT(res, testing::HasSubstr("GROUP BY process_name"));
  EXPECT_THAT(res, testing::HasSubstr("SUM(dur)"));
  EXPECT_THAT(res, testing::HasSubstr("COUNT(*)"));
  EXPECT_THAT(res, testing::HasSubstr("ORDER BY total_dur DESC"));
  EXPECT_THAT(res, testing::HasSubstr("LIMIT 100"));

  // Verify we have WITH clause (multiple CTEs)
  EXPECT_THAT(res, testing::HasSubstr("WITH"));

  // Check for multiple CTE definitions (look for multiple " AS (")
  size_t first_as = res.find(" AS (");
  ASSERT_NE(first_as, std::string::npos);
  size_t second_as = res.find(" AS (", first_as + 5);
  EXPECT_NE(second_as, std::string::npos) << "Expected multiple CTEs";
}

// Test deeply nested queries without IDs
TEST(StructuredQueryGeneratorTest, DeeplyNestedQueriesNoIds) {
  StructuredQueryGenerator gen;
  // Create a deeply nested structure to stress-test auto-generated names
  auto proto = ToProto(R"(
    inner_query {
      inner_query {
        inner_query {
          table {
            table_name: "slice"
          }
          filters {
            column_name: "dur"
            op: GREATER_THAN
            int64_rhs: 0
          }
        }
        filters {
          column_name: "ts"
          op: GREATER_THAN
          int64_rhs: 1000000
        }
        select_columns {
          column_name_or_expression: "ts"
        }
        select_columns {
          column_name_or_expression: "dur"
        }
        select_columns {
          column_name_or_expression: "name"
        }
      }
      group_by {
        column_names: "name"
        aggregates {
          column_name: "dur"
          op: SUM
          result_column_name: "total_duration"
        }
      }
    }
    order_by {
      ordering_specs {
        column_name: "total_duration"
        direction: DESC
      }
    }
    limit: 50
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify key query components
  EXPECT_THAT(res, testing::HasSubstr("WITH"));
  EXPECT_THAT(res, testing::HasSubstr("FROM slice"));
  EXPECT_THAT(res, testing::HasSubstr("GROUP BY name"));
  EXPECT_THAT(res, testing::HasSubstr("SUM(dur)"));
  EXPECT_THAT(res, testing::HasSubstr("ORDER BY total_duration DESC"));
  EXPECT_THAT(res, testing::HasSubstr("LIMIT 50"));

  // Should have multiple CTEs for nested structure
  size_t first_as = res.find(" AS (");
  ASSERT_NE(first_as, std::string::npos);
  size_t second_as = res.find(" AS (", first_as + 5);
  ASSERT_NE(second_as, std::string::npos);
  size_t third_as = res.find(" AS (", second_as + 5);
  EXPECT_NE(third_as, std::string::npos) << "Expected at least 3 nested CTEs";
}

// Test that string IDs (non-numeric) are used directly in table names
TEST(StructuredQueryGeneratorTest, StringIdInTableName) {
  StructuredQueryGenerator gen;
  // Test a query with a string ID like "foo"
  auto proto = ToProto(R"(
    id: "foo"
    table {
      table_name: "slice"
    }
    filters {
      column_name: "dur"
      op: GREATER_THAN
      int64_rhs: 1000
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // The table name should be "sq_foo"
  // Even though this is the root query, it creates a CTE named sq_foo
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_foo AS (
      SELECT * FROM slice WHERE dur > 1000
    )
    SELECT * FROM sq_foo
  )"));
}

// Test nested queries with string IDs
TEST(StructuredQueryGeneratorTest, NestedQueriesWithStringIds) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    id: "outer"
    inner_query {
      id: "inner"
      table {
        table_name: "test_table"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // The inner query should have the string-based name "sq_inner"
  // The outer query is root, so it doesn't create its own CTE
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_inner AS (
      SELECT * FROM test_table
    )
    SELECT * FROM sq_inner
  )"));
}

// Test that string IDs work correctly alongside auto-generated numeric names
TEST(StructuredQueryGeneratorTest, StringIdCollisionWithIndexBasedName) {
  StructuredQueryGenerator gen;
  // Create a scenario with both string ID and auto-generated index-based names
  auto proto = ToProto(R"(
    inner_query {
      id: "foo"
      inner_query {
        table {
          table_name: "table1"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Should have sq_foo for the query with id="foo" and sq_2 for the innermost
  // (The indexes are assigned based on state vector position during generation)
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_2 AS (
      SELECT * FROM table1
    ),
    sq_foo AS (
      SELECT * FROM sq_2
    )
    SELECT * FROM sq_foo
  )"));
}

// Test that SQL is formatted with newlines for better readability
TEST(StructuredQueryGeneratorTest, SqlFormattingWithNewlines) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    table {
      table_name: "test_table"
    }
    filters: {
      column_name: "id"
      op: GREATER_THAN
      int64_rhs: 100
    }
    group_by: {
      column_names: "category"
      aggregates: {
        column_name: "value"
        op: SUM
        result_column_name: "total_value"
      }
    }
    order_by: {
      ordering_specs: {
        column_name: "total_value"
        direction: DESC
      }
    }
    limit: 10
    offset: 5
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify the SQL is formatted with newlines and indentation
  // SELECT and FROM are always on separate lines at the same indentation
  EXPECT_EQ(res, R"(WITH sq_0 AS (
  SELECT category, SUM(value) AS total_value
  FROM test_table
  WHERE id > 100
  GROUP BY category
  ORDER BY total_value DESC
  LIMIT 10
  OFFSET 5
)
SELECT *
FROM sq_0)");
}

// Test that CTEs with multiple queries are formatted with newlines
TEST(StructuredQueryGeneratorTest, CteFormattingWithNewlines) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    inner_query {
      inner_query {
        table {
          table_name: "table1"
        }
      }
      filters: {
        column_name: "id"
        op: GREATER_THAN
        int64_rhs: 100
      }
    }
    select_columns: {
      column_name_or_expression: "id"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify CTEs are formatted with newlines, indentation, and proper separation
  // SELECT and FROM are always on separate lines at the same indentation
  EXPECT_EQ(res, R"(WITH sq_2 AS (
  SELECT *
  FROM table1
),
sq_1 AS (
  SELECT *
  FROM sq_2
  WHERE id > 100
),
sq_0 AS (
  SELECT id
  FROM sq_1
)
SELECT *
FROM sq_0)");
}

// Test nested WITH statements (a CTE containing a WITH statement)
TEST(StructuredQueryGeneratorTest, NestedWithStatements) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    inner_query {
      sql: {
        sql: "WITH inner_cte AS (SELECT id, name FROM table1) SELECT id FROM inner_cte WHERE id > 100"
        column_names: "id"
      }
    }
    select_columns: {
      column_name_or_expression: "id"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify that SQL we generate is nicely formatted with SELECT/FROM on
  // separate lines User-provided SQL (the WITH statement) is kept as-is
  EXPECT_EQ(res, R"(WITH sq_1 AS (
  SELECT *
  FROM (
    SELECT id
    FROM (
      WITH inner_cte AS (SELECT id, name FROM table1) SELECT id FROM inner_cte WHERE id > 100
    ))
),
sq_0 AS (
  SELECT id
  FROM sq_1
)
SELECT *
FROM sq_0)");
}

// Test that multi-line SQL inside CTEs is properly indented
TEST(StructuredQueryGeneratorTest, MultiLineSqlIndentation) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    inner_query {
      sql: {
        sql: "SELECT id, name
FROM table1
WHERE id > 100"
        column_names: "id"
        column_names: "name"
      }
    }
    select_columns: {
      column_name_or_expression: "id"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // Verify that SQL we generate is nicely formatted with SELECT/FROM on
  // separate lines User-provided SQL is indented but kept as-is
  EXPECT_EQ(res, R"(WITH sq_1 AS (
  SELECT *
  FROM (
    SELECT id, name
    FROM (
      SELECT id, name
      FROM table1
      WHERE id > 100
    ))
),
sq_0 AS (
  SELECT id
  FROM sq_1
)
SELECT *
FROM sq_0)");
}

// Tests for sql.column_names with transformations that change the schema.
// The column_names field describes what the SQL query returns before
// transformations, but group_by and select_columns change the output schema.
TEST(StructuredQueryGeneratorTest, SqlColumnNamesWithGroupBy) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "
        INCLUDE PERFETTO MODULE android.memory.dmabuf;
        SELECT
          process_name,
          value AS metric_val,
          LEAD(ts, 1, (SELECT end_ts FROM trace_bounds))
          OVER(PARTITION BY COALESCE(upid, utid) ORDER BY ts) - ts AS dur
        FROM android_memory_cumulative_dmabuf
        WHERE upid IS NOT NULL
      "
      column_names: "process_name"
      column_names: "metric_val"
      column_names: "dur"
    }
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "metric_val"
        op: MIN
        result_column_name: "min_val"
      }
      aggregates: {
        column_name: "metric_val"
        op: MAX
        result_column_name: "max_val"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // The final output should have process_name, min_val, max_val
  // NOT process_name, metric_val, dur (which is what column_names specifies)
  EXPECT_THAT(res, testing::HasSubstr("SELECT process_name, MIN(metric_val) AS "
                                      "min_val, MAX(metric_val) AS max_val"));
  EXPECT_THAT(res, testing::HasSubstr("GROUP BY process_name"));
}

TEST(StructuredQueryGeneratorTest, SqlColumnNamesWithSelectColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT id, name, value FROM my_table"
      column_names: "id"
      column_names: "name"
      column_names: "value"
    }
    select_columns: {
      column_name_or_expression: "id"
    }
    select_columns: {
      column_name_or_expression: "name"
      alias: "renamed_name"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // The final output should have id, renamed_name
  // NOT id, name, value (which is what column_names specifies)
  EXPECT_THAT(res, testing::HasSubstr("SELECT id, name AS renamed_name"));
}

TEST(StructuredQueryGeneratorTest, SqlColumnNamesWithoutTransformations) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    sql: {
      sql: "SELECT id, name, value FROM my_table WHERE id > 10"
      column_names: "id"
      column_names: "name"
      column_names: "value"
    }
    filters: {
      column_name: "value"
      op: GREATER_THAN
      int64_rhs: 100
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);

  // When there's no group_by or select_columns, the column_names should match
  // The SQL source wraps with: SELECT col1, col2, col3 FROM (user SQL)
  EXPECT_THAT(res, testing::HasSubstr("SELECT id, name, value"));
  EXPECT_THAT(res, testing::HasSubstr("WHERE value > 100"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesBasic) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM end_events),
    sq_1 AS (SELECT * FROM start_events),
    sq_0 AS (
      SELECT * FROM (
        WITH starts AS (SELECT * FROM sq_1),
             ends AS (SELECT * FROM sq_2),
             matched AS (
               SELECT
                 starts.ts AS start_ts,
                 (SELECT MIN(ends.ts) FROM ends WHERE ends.ts > starts.ts) AS end_ts
               FROM starts
             )
        SELECT
          start_ts AS ts,
          end_ts - start_ts AS dur
        FROM matched
        WHERE end_ts IS NOT NULL
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesWithRealTablesSliceBeginEnd) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "slice"
          column_names: "ts"
          column_names: "track_id"
          column_names: "name"
        }
        filters: {
          column_name: "name"
          op: GLOB
          string_rhs: "*_begin"
        }
      }
      ends_query: {
        table: {
          table_name: "slice"
          column_names: "ts"
          column_names: "track_id"
          column_names: "name"
        }
        filters: {
          column_name: "name"
          op: GLOB
          string_rhs: "*_end"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM slice WHERE name GLOB '*_end'),
    sq_1 AS (SELECT * FROM slice WHERE name GLOB '*_begin'),
    sq_0 AS (
      SELECT * FROM (
        WITH starts AS (SELECT * FROM sq_1),
             ends AS (SELECT * FROM sq_2),
             matched AS (
               SELECT
                 starts.ts AS start_ts,
                 (SELECT MIN(ends.ts) FROM ends WHERE ends.ts > starts.ts) AS end_ts
               FROM starts
             )
        SELECT
          start_ts AS ts,
          end_ts - start_ts AS dur
        FROM matched
        WHERE end_ts IS NOT NULL
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesWithDifferentColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "lock_acquire"
          column_names: "acquire_ts"
        }
      }
      ends_query: {
        table: {
          table_name: "lock_release"
          column_names: "release_ts"
        }
      }
      starts_ts_column: "acquire_ts"
      ends_ts_column: "release_ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("starts.acquire_ts AS start_ts"));
  EXPECT_THAT(res, testing::HasSubstr("ends.release_ts"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "events"
          column_names: "ts"
          column_names: "type"
        }
        filters: {
          column_name: "type"
          op: EQUAL
          string_rhs: "BEGIN"
        }
      }
      ends_query: {
        table: {
          table_name: "events"
          column_names: "ts"
          column_names: "type"
        }
        filters: {
          column_name: "type"
          op: EQUAL
          string_rhs: "END"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Check that filters are applied in the subqueries
  EXPECT_THAT(res, testing::HasSubstr("type = 'BEGIN'"));
  EXPECT_THAT(res, testing::HasSubstr("type = 'END'"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithOrderBy) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
    order_by: {
      ordering_specs: {
        column_name: "dur"
        direction: DESC
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("ORDER BY dur DESC"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithLimit) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
    limit: 10
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("LIMIT 10"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesNestedInInnerQuery) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    inner_query: {
      experimental_create_slices: {
        starts_query: {
          table: {
            table_name: "start_events"
            column_names: "ts"
          }
        }
        ends_query: {
          table: {
            table_name: "end_events"
            column_names: "ts"
          }
        }
        starts_ts_column: "ts"
        ends_ts_column: "ts"
      }
    }
    filters: {
      column_name: "dur"
      op: GREATER_THAN
      int64_rhs: 1000
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Check that the create slices is nested and filters are applied on top
  EXPECT_THAT(res, testing::HasSubstr("dur > 1000"));
  EXPECT_THAT(res, testing::HasSubstr("start_ts AS ts"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesWithIntervalIntersect) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        experimental_create_slices: {
          starts_query: {
            table: {
              table_name: "start_events"
              column_names: "ts"
            }
          }
          ends_query: {
            table: {
              table_name: "end_events"
              column_names: "ts"
            }
          }
          starts_ts_column: "ts"
          ends_ts_column: "ts"
        }
      }
      interval_intersect: {
        simple_slices: {
          slice_name_glob: "important*"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Check that create slices is used as base for interval intersect
  EXPECT_THAT(res, testing::HasSubstr("_interval_intersect"));
  EXPECT_THAT(res, testing::HasSubstr("start_ts AS ts"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithSelectColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
    select_columns: {
      column_name_or_expression: "ts"
    }
    select_columns: {
      column_name_or_expression: "dur"
      alias: "duration"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("dur AS duration"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesMissingStartsQuery) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  EXPECT_THAT(ret.status().message(),
              testing::HasSubstr("CreateSlices must specify a starts_query"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesMissingEndsQuery) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  EXPECT_THAT(ret.status().message(),
              testing::HasSubstr("CreateSlices must specify an ends_query"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesDefaultStartsTsColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should default starts_ts_column to "ts"
  EXPECT_THAT(res, testing::HasSubstr("starts.ts AS start_ts"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesDefaultEndsTsColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should default ends_ts_column to "ts"
  EXPECT_THAT(res, testing::HasSubstr("ends.ts"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesDefaultBothTsColumns) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Should default both columns to "ts"
  EXPECT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM end_events),
    sq_1 AS (SELECT * FROM start_events),
    sq_0 AS (
      SELECT * FROM (
        WITH starts AS (SELECT * FROM sq_1),
             ends AS (SELECT * FROM sq_2),
             matched AS (
               SELECT
                 starts.ts AS start_ts,
                 (SELECT MIN(ends.ts) FROM ends WHERE ends.ts > starts.ts) AS end_ts
               FROM starts
             )
        SELECT
          start_ts AS ts,
          end_ts - start_ts AS dur
        FROM matched
        WHERE end_ts IS NOT NULL
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithSqlSource) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        sql: {
          sql: "SELECT ts FROM events WHERE type = 'BEGIN'"
        }
      }
      ends_query: {
        sql: {
          sql: "SELECT ts FROM events WHERE type = 'END'"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("type = 'BEGIN'"));
  EXPECT_THAT(res, testing::HasSubstr("type = 'END'"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesWithGroupBy) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
    group_by: {
      column_names: "ts"
      aggregates: {
        op: SUM
        column_name: "dur"
        result_column_name: "total_dur"
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, testing::HasSubstr("GROUP BY ts"));
  EXPECT_THAT(res, testing::HasSubstr("SUM(dur)"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesEmptyStartsTsColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: ""
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Empty string should default to "ts"
  EXPECT_THAT(res, testing::HasSubstr("starts.ts AS start_ts"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesEmptyEndsTsColumn) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: ""
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Empty string should default to "ts"
  EXPECT_THAT(res, testing::HasSubstr("ends.ts"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalCreateSlicesWithEmptySourceQueries) {
  // This test verifies that the SQL generation works correctly even when
  // the source queries might return no rows. The WHERE end_ts IS NOT NULL
  // clause ensures we get valid empty results.
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "start_events"
          column_names: "ts"
        }
        filters: {
          column_name: "ts"
          op: LESS_THAN
          int64_rhs: 0
        }
      }
      ends_query: {
        table: {
          table_name: "end_events"
          column_names: "ts"
        }
        filters: {
          column_name: "ts"
          op: LESS_THAN
          int64_rhs: 0
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM end_events WHERE ts < 0),
    sq_1 AS (SELECT * FROM start_events WHERE ts < 0),
    sq_0 AS (
      SELECT * FROM (
        WITH starts AS (SELECT * FROM sq_1),
             ends AS (SELECT * FROM sq_2),
             matched AS (
               SELECT
                 starts.ts AS start_ts,
                 (SELECT MIN(ends.ts) FROM ends WHERE ends.ts > starts.ts) AS end_ts
               FROM starts
             )
        SELECT
          start_ts AS ts,
          end_ts - start_ts AS dur
        FROM matched
        WHERE end_ts IS NOT NULL
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalCreateSlicesNoMatchingEnds) {
  // This test verifies the behavior when starts exist but no matching ends.
  // The WHERE end_ts IS NOT NULL clause should filter out all unmatched starts.
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_create_slices: {
      starts_query: {
        table: {
          table_name: "events"
          column_names: "ts"
        }
        filters: {
          column_name: "ts"
          op: LESS_THAN
          int64_rhs: 1000
        }
      }
      ends_query: {
        table: {
          table_name: "events"
          column_names: "ts"
        }
        filters: {
          column_name: "ts"
          op: GREATER_THAN
          int64_rhs: 10000
        }
      }
      starts_ts_column: "ts"
      ends_ts_column: "ts"
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  EXPECT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH
    sq_2 AS (SELECT * FROM events WHERE ts > 10000),
    sq_1 AS (SELECT * FROM events WHERE ts < 1000),
    sq_0 AS (
      SELECT * FROM (
        WITH starts AS (SELECT * FROM sq_1),
             ends AS (SELECT * FROM sq_2),
             matched AS (
               SELECT
                 starts.ts AS start_ts,
                 (SELECT MIN(ends.ts) FROM ends WHERE ends.ts > starts.ts) AS end_ts
               FROM starts
             )
        SELECT
          start_ts AS ts,
          end_ts - start_ts AS dur
        FROM matched
        WHERE end_ts IS NOT NULL
      )
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeSource) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: STATIC
      ts: 100
      dur: 400
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT *
      FROM (SELECT 0 AS id, 100 AS ts, 400 AS dur)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeSourceWithFilters) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: STATIC
      ts: 1000
      dur: 500
    }
    filters: {
      column_name: "dur"
      op: GREATER_THAN
      int64_rhs: 0
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT *
      FROM (SELECT 0 AS id, 1000 AS ts, 500 AS dur)
      WHERE dur > 0
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeSourceMissingTs) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: DYNAMIC
      dur: 400
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // When ts is missing in DYNAMIC mode, use trace_start()
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT *
      FROM (SELECT 0 AS id, trace_start() AS ts, 400 AS dur)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeSourceMissingDur) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: DYNAMIC
      ts: 100
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // When dur is missing in DYNAMIC mode, use trace_dur()
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT *
      FROM (SELECT 0 AS id, 100 AS ts, trace_dur() AS dur)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeSourceMissingBoth) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: DYNAMIC
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // When both are missing in DYNAMIC mode, use trace_start() and trace_dur()
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
      SELECT *
      FROM (SELECT 0 AS id, trace_start() AS ts, trace_dur() AS dur)
    )
    SELECT * FROM sq_0
  )"));
}

TEST(StructuredQueryGeneratorTest,
     ExperimentalTimeRangeSourceWithIntervalIntersect) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    interval_intersect: {
      base: {
        table: {
          table_name: "slice"
          column_names: "id"
          column_names: "ts"
          column_names: "dur"
          column_names: "name"
        }
      }
      interval_intersect: {
        experimental_time_range: {
          mode: STATIC
          ts: 100
          dur: 400
        }
      }
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_OK_AND_ASSIGN(std::string res, ret);
  // Verify that experimental_time_range can be used as an interval source
  EXPECT_THAT(res, testing::HasSubstr("SELECT 0 AS id, 100 AS ts, 400 AS dur"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("intervals.intersect"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeMissingMode) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      ts: 100
      dur: 400
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  EXPECT_THAT(ret.status().message(),
              testing::HasSubstr("mode field is required"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeStaticMissingTs) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: STATIC
      dur: 400
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  EXPECT_THAT(ret.status().message(),
              testing::HasSubstr("ts is required for STATIC mode"));
}

TEST(StructuredQueryGeneratorTest, ExperimentalTimeRangeStaticMissingDur) {
  StructuredQueryGenerator gen;
  auto proto = ToProto(R"(
    experimental_time_range: {
      mode: STATIC
      ts: 100
    }
  )");
  auto ret = gen.Generate(proto.data(), proto.size());
  ASSERT_FALSE(ret.ok());
  EXPECT_THAT(ret.status().message(),
              testing::HasSubstr("dur is required for STATIC mode"));
}

}  // namespace perfetto::trace_processor::perfetto_sql::generator
