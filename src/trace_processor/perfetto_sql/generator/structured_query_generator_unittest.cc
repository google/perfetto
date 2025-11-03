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
                    SELECT ii.ts, ii.dur, iibase.*, iisource0.*
                    FROM _interval_intersect!((iibase, iisource0), ()) ii
                    JOIN iibase ON ii.id_0 = iibase.id
                    JOIN iisource0 ON ii.id_1 = iisource0.id
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
        SELECT * FROM sq_1 UNION SELECT * FROM sq_2
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
        SELECT * FROM sq_1 UNION ALL SELECT * FROM sq_2
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
        SELECT * FROM sq_1 UNION ALL SELECT * FROM sq_2 UNION ALL SELECT * FROM sq_3
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
        SELECT * FROM sq_1 UNION SELECT * FROM sq_2
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
  ASSERT_THAT(res, EqualsIgnoringWhitespace(R"(
    WITH sq_0 AS (
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
    SELECT * FROM sq_0 ORDER BY slice_name ASC
  )"));
  ASSERT_THAT(gen.ComputeReferencedModules(),
              UnorderedElementsAre("slices.with_context"));
}

}  // namespace perfetto::trace_processor::perfetto_sql::generator
