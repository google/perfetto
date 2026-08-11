/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/plugins/experimental_flamegraph/experimental_flamegraph.h"

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/tree/tree_from_dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/plugins/flamegraph/flamegraph.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::experimental_flamegraph {

namespace {

// Represents a time boundary for a column.
struct TimeConstraints {
  dataframe::Op op;
  int64_t value;
};

class ExperimentalFlamegraph : public StaticTableFunction {
 public:
  enum class ProfileType : uint8_t { kGraph, kHeapProfile, kPerf };

  struct InputValues {
    ProfileType profile_type;
    std::optional<int64_t> ts;
    std::vector<TimeConstraints> time_constraints;
    std::optional<UniquePid> upid;
    std::optional<std::string> upid_group;
    std::optional<std::string> focus_str;
  };

  class Cursor : public StaticTableFunction::Cursor {
   public:
    explicit Cursor(PerfettoSqlConnection* connection,
                    TraceProcessorContext* context);
    bool Run(const std::vector<SqlValue>& arguments) override;

   private:
    PerfettoSqlConnection* connection_ = nullptr;
    TraceProcessorContext* context_ = nullptr;
    tables::ExperimentalFlamegraphTable table_;
  };

  explicit ExperimentalFlamegraph(PerfettoSqlConnection* connection,
                                  TraceProcessorContext* context);
  ~ExperimentalFlamegraph() override;

  std::unique_ptr<StaticTableFunction::Cursor> MakeCursor() override;
  dataframe::DataframeSpec CreateSpec() override;
  std::string TableName() override;
  uint32_t GetArgumentCount() const override;

 private:
  PerfettoSqlConnection* connection_ = nullptr;
  TraceProcessorContext* context_ = nullptr;
};

base::StatusOr<ExperimentalFlamegraph::ProfileType> ExtractProfileType(
    const std::string& profile_name) {
  if (profile_name == "graph") {
    return ExperimentalFlamegraph::ProfileType::kGraph;
  }
  if (profile_name == "native") {
    return ExperimentalFlamegraph::ProfileType::kHeapProfile;
  }
  if (profile_name == "perf") {
    return ExperimentalFlamegraph::ProfileType::kPerf;
  }
  return base::ErrStatus(
      "experimental_flamegraph: Could not recognize profile type: %s.",
      profile_name.c_str());
}

base::StatusOr<int64_t> ParseTimeConstraintTs(const std::string& c,
                                              uint32_t offset) {
  std::optional<int64_t> ts = base::CStringToInt64(&c[offset]);
  if (!ts) {
    return base::ErrStatus(
        "experimental_flamegraph: Unable to parse timestamp");
  }
  return *ts;
}

base::StatusOr<TimeConstraints> ParseTimeConstraint(const std::string& c) {
  if (base::StartsWith(c, "=")) {
    ASSIGN_OR_RETURN(int64_t ts, ParseTimeConstraintTs(c, 1));
    return TimeConstraints{dataframe::Eq{}, ts};
  }
  if (base::StartsWith(c, ">=")) {
    ASSIGN_OR_RETURN(int64_t ts, ParseTimeConstraintTs(c, 2));
    return TimeConstraints{dataframe::Ge{}, ts};
  }
  if (base::StartsWith(c, ">")) {
    ASSIGN_OR_RETURN(int64_t ts, ParseTimeConstraintTs(c, 1));
    return TimeConstraints{dataframe::Gt{}, ts};
  }
  if (base::StartsWith(c, "<=")) {
    ASSIGN_OR_RETURN(int64_t ts, ParseTimeConstraintTs(c, 2));
    return TimeConstraints{dataframe::Le{}, ts};
  }
  if (base::StartsWith(c, "<")) {
    ASSIGN_OR_RETURN(int64_t ts, ParseTimeConstraintTs(c, 1));
    return TimeConstraints{dataframe::Lt{}, ts};
  }
  return base::ErrStatus("experimental_flamegraph: Unknown time constraint");
}

base::StatusOr<std::vector<TimeConstraints>> ExtractTimeConstraints(
    const SqlValue& value) {
  PERFETTO_DCHECK(value.is_null() || value.type == SqlValue::kString);
  std::vector<TimeConstraints> constraints;
  if (value.is_null()) {
    return constraints;
  }
  std::vector<std::string> raw_cs = base::SplitString(value.AsString(), ",");
  for (const std::string& c : raw_cs) {
    ASSIGN_OR_RETURN(TimeConstraints tc, ParseTimeConstraint(c));
    constraints.push_back(tc);
  }
  return constraints;
}

// For filtering, this method uses the same constraints as
// ExperimentalFlamegraph::ValidateConstraints and should therefore
// be kept in sync.
base::StatusOr<ExperimentalFlamegraph::InputValues> GetFlamegraphInputValues(
    const std::vector<SqlValue>& arguments) {
  PERFETTO_CHECK(arguments.size() == 6);

  const SqlValue& raw_profile_type = arguments[0];
  if (raw_profile_type.type != SqlValue::kString) {
    return base::ErrStatus(
        "experimental_flamegraph: profile_type must be an string");
  }
  const SqlValue& ts = arguments[1];
  if (ts.type != SqlValue::kLong && !ts.is_null()) {
    return base::ErrStatus("experimental_flamegraph: ts must be an integer");
  }
  const SqlValue& ts_constraints = arguments[2];
  if (ts_constraints.type != SqlValue::kString && !ts_constraints.is_null()) {
    return base::ErrStatus(
        "experimental_flamegraph: ts constraint must be an string");
  }
  const SqlValue& upid = arguments[3];
  if (upid.type != SqlValue::kLong && !upid.is_null()) {
    return base::ErrStatus("experimental_flamegraph: upid must be an integer");
  }
  const SqlValue& upid_group = arguments[4];
  if (upid_group.type != SqlValue::kString && !upid_group.is_null()) {
    return base::ErrStatus(
        "experimental_flamegraph: upid_group must be an string");
  }
  const SqlValue& focus_str = arguments[5];
  if (focus_str.type != SqlValue::kString && !focus_str.is_null()) {
    return base::ErrStatus(
        "experimental_flamegraph: focus_str must be an string");
  }

  if (ts.is_null() && ts_constraints.is_null()) {
    return base::ErrStatus(
        "experimental_flamegraph: one of ts and ts_constraints must not be "
        "null");
  }
  if (upid.is_null() && upid_group.is_null()) {
    return base::ErrStatus(
        "experimental_flamegraph: one of upid or upid_group must not be null");
  }
  ASSIGN_OR_RETURN(std::vector<TimeConstraints> time_constraints,
                   ExtractTimeConstraints(ts_constraints));
  ASSIGN_OR_RETURN(ExperimentalFlamegraph::ProfileType profile_type,
                   ExtractProfileType(raw_profile_type.AsString()));
  return ExperimentalFlamegraph::InputValues{
      profile_type,
      ts.is_null() ? std::nullopt : std::make_optional(ts.AsLong()),
      std::move(time_constraints),
      upid.is_null() ? std::nullopt
                     : std::make_optional(static_cast<uint32_t>(upid.AsLong())),
      upid_group.is_null() ? std::nullopt
                           : std::make_optional(upid_group.AsString()),
      focus_str.is_null() ? std::nullopt
                          : std::make_optional(focus_str.AsString()),
  };
}

class Matcher {
 public:
  explicit Matcher(const std::string& str) : focus_str_(base::ToLower(str)) {}
  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;

  bool matches(const std::string& s) const {
    // TODO(149833691): change to regex.
    // We cannot use regex.h (does not exist in windows) or std regex (throws
    // exceptions).
    return base::Contains(base::ToLower(s), focus_str_);
  }

 private:
  const std::string focus_str_;
};

enum class FocusedState : uint8_t {
  kNotFocused,
  kFocusedPropagating,
  kFocusedNotPropagating,
};

using tables::ExperimentalFlamegraphTable;
std::vector<FocusedState> ComputeFocusedState(
    const StringPool& pool,
    const ExperimentalFlamegraphTable& table,
    const Matcher& focus_matcher) {
  // Each row corresponds to a node in the flame chart tree with its parent
  // ptr. Root trees (no parents) will have a null parent ptr.
  std::vector<FocusedState> focused(table.row_count());

  for (auto it = table.IterateRows(); it; ++it) {
    auto parent_id = it.parent_id();
    // Constraint: all descendants MUST come after their parents.
    PERFETTO_DCHECK(!parent_id.has_value() || *parent_id < it.id());

    auto i = it.row_number().row_number();
    if (focus_matcher.matches(pool.Get(it.name()).ToStdString())) {
      // Mark as focused
      focused[i] = FocusedState::kFocusedPropagating;
      auto current = parent_id;
      // Mark all parent nodes as focused
      while (current.has_value()) {
        auto c = table[*current];
        uint32_t current_idx = c.ToRowNumber().row_number();
        if (focused[current_idx] != FocusedState::kNotFocused) {
          // We have already visited these nodes, skip
          break;
        }
        focused[current_idx] = FocusedState::kFocusedNotPropagating;
        current = c.parent_id();
      }
    } else if (parent_id.has_value() &&
               focused[table[*parent_id].ToRowNumber().row_number()] ==
                   FocusedState::kFocusedPropagating) {
      // Focus cascades downwards.
      focused[i] = FocusedState::kFocusedPropagating;
    } else {
      focused[i] = FocusedState::kNotFocused;
    }
  }
  return focused;
}

struct CumulativeCounts {
  int64_t size;
  int64_t count;
  int64_t alloc_size;
  int64_t alloc_count;
};
std::unique_ptr<tables::ExperimentalFlamegraphTable> FocusTable(
    TraceStorage* storage,
    std::unique_ptr<ExperimentalFlamegraphTable> in,
    const std::string& focus_str) {
  if (in->row_count() == 0 || focus_str.empty()) {
    return in;
  }
  std::vector<FocusedState> focused_state =
      ComputeFocusedState(storage->string_pool(), *in, Matcher(focus_str));
  std::unique_ptr<ExperimentalFlamegraphTable> tbl(
      new tables::ExperimentalFlamegraphTable(storage->mutable_string_pool()));

  // Recompute cumulative counts
  std::vector<CumulativeCounts> node_to_cumulatives(in->row_count());
  for (int64_t idx = in->row_count() - 1; idx >= 0; --idx) {
    auto i = static_cast<uint32_t>(idx);
    auto rr = (*in)[i];
    if (focused_state[i] == FocusedState::kNotFocused) {
      continue;
    }
    auto& cumulatives = node_to_cumulatives[i];
    cumulatives.size += rr.size();
    cumulatives.count += rr.count();
    cumulatives.alloc_size += rr.alloc_size();
    cumulatives.alloc_count += rr.alloc_count();

    auto parent_id = rr.parent_id();
    if (parent_id.has_value()) {
      uint32_t parent_row = (*in)[*parent_id].ToRowNumber().row_number();
      auto& parent_cumulatives = node_to_cumulatives[parent_row];
      parent_cumulatives.size += cumulatives.size;
      parent_cumulatives.count += cumulatives.count;
      parent_cumulatives.alloc_size += cumulatives.alloc_size;
      parent_cumulatives.alloc_count += cumulatives.alloc_count;
    }
  }

  // Mapping between the old rows ('node') to the new identifiers.
  std::vector<ExperimentalFlamegraphTable::Id> node_to_id(in->row_count());
  for (auto it = in->IterateRows(); it; ++it) {
    uint32_t i = it.row_number().row_number();
    if (focused_state[i] == FocusedState::kNotFocused) {
      continue;
    }

    tables::ExperimentalFlamegraphTable::Row alloc_row{};
    // We must reparent the rows as every insertion will get its own
    // identifier.
    auto original_parent_id = it.parent_id();
    if (original_parent_id.has_value()) {
      auto original_idx = (*in)[*original_parent_id].ToRowNumber().row_number();
      alloc_row.parent_id = node_to_id[original_idx];
    }

    alloc_row.ts = it.ts();
    alloc_row.upid = it.upid();
    alloc_row.profile_type = it.profile_type();
    alloc_row.depth = it.depth();
    alloc_row.name = it.name();
    alloc_row.map_name = it.map_name();
    alloc_row.count = it.count();
    alloc_row.size = it.size();
    alloc_row.alloc_count = it.alloc_count();
    alloc_row.alloc_size = it.alloc_size();

    const auto& cumulative = node_to_cumulatives[i];
    alloc_row.cumulative_count = cumulative.count;
    alloc_row.cumulative_size = cumulative.size;
    alloc_row.cumulative_alloc_count = cumulative.alloc_count;
    alloc_row.cumulative_alloc_size = cumulative.alloc_size;
    node_to_id[i] = tbl->Insert(alloc_row).id;
  }
  return tbl;
}

// Builds the SQL expansion for a heap graph profile: the shortest-path object
// tree (from the heap graph stdlib module) joined with class and size
// information, plus synthetic "[native]" children carrying native_size.
std::string BuildGraphFlamegraphSql(int64_t ts, UniquePid upid) {
  std::string sql = R"(
    INCLUDE PERFETTO MODULE android.memory.heap_graph.excluded_refs;
    INCLUDE PERFETTO MODULE graphs.search;
    WITH
      tree AS MATERIALIZED (
        SELECT node_id AS id, parent_node_id AS parent_id
        FROM graph_reachable_bfs!(
          (
            SELECT source_node_id, dest_node_id
            FROM (
              SELECT owner_id AS source_node_id, owned_id AS dest_node_id
              FROM heap_graph_reference AS r
              JOIN heap_graph_object AS owner ON owner.id = r.owner_id
              JOIN heap_graph_object AS owned ON owned.id = r.owned_id
              WHERE owner.upid = $upid AND owner.graph_sample_ts = $ts
                AND owned.upid = $upid AND owned.graph_sample_ts = $ts
                AND r.id NOT IN _excluded_refs
              UNION ALL
              -- Ensure the graph is non-empty (and thus the BFS yields the
              -- roots) even for dumps with no references at all.
              SELECT id AS source_node_id, NULL AS dest_node_id
              FROM heap_graph_object
              WHERE upid = $upid AND graph_sample_ts = $ts
                AND root_type IS NOT NULL
            )
            ORDER BY source_node_id, dest_node_id
          ),
          (
            SELECT o.id AS node_id
            FROM heap_graph_object AS o
            JOIN heap_graph_class AS c ON c.id = o.type_id
            WHERE o.upid = $upid AND o.graph_sample_ts = $ts
              AND o.root_type IS NOT NULL
            ORDER BY c.name, o.id
          )
        )
      ),
      object_frames AS MATERIALIZED (
        SELECT
          t.id AS id,
          t.parent_id AS parent_id,
          coalesce(c.deobfuscated_name, c.name)
            || iif(o.root_type IS NULL, '', ' [' || o.root_type || ']') AS name,
          'JAVA' AS map_name,
          NULL AS source_file,
          NULL AS line_number,
          NULL AS sample_ts,
          1 AS count,
          o.self_size AS size,
          0 AS alloc_count,
          0 AS alloc_size,
          o.native_size AS native_size
        FROM tree AS t
        JOIN heap_graph_object AS o USING (id)
        JOIN heap_graph_class AS c ON c.id = o.type_id
      ),
      native_id_offset AS (
        SELECT coalesce(max(id), -1) + 1 AS value FROM heap_graph_object
      ),
      frames AS (
        SELECT id, parent_id, name, map_name, source_file, line_number,
               sample_ts, count, size, alloc_count, alloc_size
        FROM object_frames
        UNION ALL
        SELECT
          (SELECT value FROM native_id_offset)
            + row_number() OVER (ORDER BY id) - 1 AS id,
          id AS parent_id,
          '[native] ' || name AS name,
          map_name, source_file, line_number, sample_ts,
          1 AS count, native_size AS size, 0 AS alloc_count, 0 AS alloc_size
        FROM object_frames
        WHERE native_size != 0
      )
    SELECT * FROM frames
  )";
  sql = base::ReplaceAll(sql, "$upid", std::to_string(upid));
  sql = base::ReplaceAll(sql, "$ts", std::to_string(ts));
  return sql;
}

// Builds the SQL expansion for a native (heapprofd) profile: the callstack
// tree reachable from the process's allocations, with per-callsite metrics
// attributed to the leaf frame of each callsite.
std::string BuildHeapProfileFlamegraphSql(int64_t ts, UniquePid upid) {
  std::string sql = R"(
    INCLUDE PERFETTO MODULE android.memory.heap_profile.callstacks;
    SELECT
      c.id AS id,
      c.parent_id AS parent_id,
      c.name AS name,
      c.mapping_name AS map_name,
      c.source_file AS source_file,
      c.line_number AS line_number,
      NULL AS sample_ts,
      c.self_count AS count,
      c.self_size AS size,
      c.self_alloc_count AS alloc_count,
      c.self_alloc_size AS alloc_size
    FROM _android_heap_profile_callstacks_for_allocations!((
      SELECT
        callsite_id,
        size,
        count,
        max(size, 0) AS alloc_size,
        max(count, 0) AS alloc_count
      FROM heap_profile_allocation
      WHERE upid = $upid AND ts <= $ts
    )) AS c
  )";
  sql = base::ReplaceAll(sql, "$upid", std::to_string(upid));
  sql = base::ReplaceAll(sql, "$ts", std::to_string(ts));
  return sql;
}

// Builds the SQL expansion for a perf (callstack sampling) profile: the
// callstack tree reachable from the samples in the requested processes and
// timestamp range, with one count per sample and the latest sample timestamp
// on each leaf.
std::string BuildPerfFlamegraphSql(
    const std::optional<UniquePid>& upid,
    const std::optional<std::string>& upid_group,
    const std::vector<TimeConstraints>& time_constraints) {
  std::vector<std::string> ts_filters;
  for (const TimeConstraints& tc : time_constraints) {
    const char* op = "=";
    if (tc.op.Is<dataframe::Gt>()) {
      op = ">";
    } else if (tc.op.Is<dataframe::Ge>()) {
      op = ">=";
    } else if (tc.op.Is<dataframe::Lt>()) {
      op = "<";
    } else if (tc.op.Is<dataframe::Le>()) {
      op = "<=";
    }
    ts_filters.push_back(
        base::StackString<64>("s.ts %s %" PRId64, op, tc.value).ToStdString());
  }
  std::string upid_filter = "1";
  if (upid) {
    upid_filter =
        base::StackString<64>("t.upid = %" PRIu32, *upid).ToStdString();
  } else if (upid_group) {
    std::vector<std::string> upids;
    for (base::StringSplitter sp(*upid_group, ','); sp.Next();) {
      std::optional<uint32_t> maybe = base::CStringToUInt32(sp.cur_token());
      if (maybe) {
        upids.push_back(std::to_string(*maybe));
      }
    }
    if (!upids.empty()) {
      upid_filter = "t.upid IN (" + base::Join(upids, ",") + ")";
    }
  }

  std::string sql = R"(
    INCLUDE PERFETTO MODULE callstacks.stack_profile;
    WITH
      samples AS MATERIALIZED (
        SELECT
          s.callsite_id,
          count() AS sample_count,
          max(s.ts) AS sample_ts
        FROM __intrinsic_profiler_sample AS s
        JOIN __intrinsic_profiler_task_context AS tc ON tc.id = s.task_context_id
        JOIN thread AS t ON t.utid = tc.utid
        WHERE s.callsite_id IS NOT NULL
          AND $upid_filter
          AND $ts_filters
        GROUP BY s.callsite_id
      )
    SELECT
      c.id AS id,
      c.parent_id AS parent_id,
      c.name AS name,
      c.mapping_name AS map_name,
      c.source_file AS source_file,
      c.line_number AS line_number,
      s.sample_ts AS sample_ts,
      iif(c.is_leaf_function_in_callsite_frame, coalesce(s.sample_count, 0), 0)
        AS count,
      iif(c.is_leaf_function_in_callsite_frame, coalesce(s.sample_count, 0), 0)
        AS size,
      0 AS alloc_count,
      0 AS alloc_size
    FROM _callstacks_for_stack_profile_samples!(samples) AS c
    LEFT JOIN samples AS s USING (callsite_id)
  )";
  sql = base::ReplaceAll(sql, "$upid_filter", upid_filter);
  sql = base::ReplaceAll(
      sql, "$ts_filters",
      ts_filters.empty() ? "1" : base::Join(ts_filters, " AND "));
  return sql;
}

// Returns the default timestamp used for nodes without a direct sample
// timestamp: taken from the query value, it only prevents such rows from
// being filtered out by SQLite, and is not meaningful per-row.
int64_t DefaultTimestamp(const ExperimentalFlamegraph::InputValues& values) {
  if (values.time_constraints.empty()) {
    return 0;
  }
  const TimeConstraints& tc = values.time_constraints[0];
  if (tc.op.Is<dataframe::Gt>()) {
    return tc.value + 1;
  }
  if (tc.op.Is<dataframe::Lt>()) {
    return tc.value - 1;
  }
  return tc.value;
}

// Returns true if the heap graph dump at (upid, ts) was recorded as
// incomplete during import (missing packets or never finalized).
base::StatusOr<bool> IsIncompleteHeapGraph(PerfettoSqlConnection* connection,
                                           int64_t ts,
                                           UniquePid upid) {
  std::string sql = R"(
    SELECT EXISTS(
      SELECT 1
      FROM __intrinsic_trace_import_logs AS l
      JOIN stats AS s ON s.key = l.stat_key
      WHERE s.name = 'heap_graph_incomplete_dump'
        AND l.ts = $ts
        AND extract_arg(l.arg_set_id, 'upid') = $upid
    )
  )";
  sql = base::ReplaceAll(sql, "$ts", std::to_string(ts));
  sql = base::ReplaceAll(sql, "$upid", std::to_string(upid));
  ASSIGN_OR_RETURN(
      auto stmt,
      connection->PrepareSqliteStatement(
          SqlSource::FromTraceProcessorImplementation(std::move(sql))));
  PERFETTO_CHECK(stmt.Step());
  return sqlite::column::Int64(stmt.sqlite_stmt(), 0) != 0;
}

// Runs the profile-specific SQL expansion, computes the merged flamegraph with
// the shared C++ flamegraph library (the same code path backing the
// __intrinsic_flamegraph virtual table) and populates the output table.
base::StatusOr<std::unique_ptr<tables::ExperimentalFlamegraphTable>>
BuildFlamegraphTable(PerfettoSqlConnection* connection,
                     TraceProcessorContext* context,
                     const ExperimentalFlamegraph::InputValues& values) {
  TraceStorage* storage = context->storage.get();
  const char* profile_type = "";
  switch (values.profile_type) {
    case ExperimentalFlamegraph::ProfileType::kGraph:
      profile_type = "graph";
      break;
    case ExperimentalFlamegraph::ProfileType::kHeapProfile:
      profile_type = "native";
      break;
    case ExperimentalFlamegraph::ProfileType::kPerf:
      profile_type = "perf";
      break;
  }
  StringPool* pool = storage->mutable_string_pool();
  StringPool::Id profile_type_id = pool->InternString(profile_type);
  std::optional<StringPool::Id> upid_group_id;
  if (values.upid_group) {
    upid_group_id = pool->InternString(base::StringView(*values.upid_group));
  }
  int64_t default_ts = DefaultTimestamp(values);
  if (values.ts) {
    default_ts = *values.ts;
  }

  std::string sql;
  switch (values.profile_type) {
    case ExperimentalFlamegraph::ProfileType::kGraph:
      sql = BuildGraphFlamegraphSql(*values.ts, *values.upid);
      break;
    case ExperimentalFlamegraph::ProfileType::kHeapProfile:
      sql = BuildHeapProfileFlamegraphSql(*values.ts, *values.upid);
      break;
    case ExperimentalFlamegraph::ProfileType::kPerf:
      sql = BuildPerfFlamegraphSql(values.upid, values.upid_group,
                                   values.time_constraints);
      break;
  }
  ASSIGN_OR_RETURN(
      auto execution,
      connection->ExecuteUntilLastStatement(
          SqlSource::FromTraceProcessorImplementation(std::move(sql))));

  SqliteDataframeBuilderOptions options;
  options.nullability = dataframe::NullabilityType::kDenseNull;
  std::vector<std::string> column_names = {
      "id",          "parent_id",   "name",       "map_name",
      "source_file", "line_number", "sample_ts",  "count",
      "size",        "alloc_count", "alloc_size",
  };
  ASSIGN_OR_RETURN(auto builder,
                   BuildRuntimeDataframeFromSqliteStatement(
                       pool, std::move(column_names), &execution.stmt,
                       "experimental_flamegraph", std::move(options)));
  ASSIGN_OR_RETURN(core::Tree tree, core::BuildTree(std::move(builder)));

  auto tbl = std::make_unique<tables::ExperimentalFlamegraphTable>(pool);
  if (tree.row_count == 0) {
    // Reproduce the legacy behaviour for empty inputs: a heap graph with no
    // roots produces either the incomplete-dump sentinel or an error; an
    // empty heap profile is an error. An empty perf profile is a valid empty
    // flamegraph.
    switch (values.profile_type) {
      case ExperimentalFlamegraph::ProfileType::kGraph: {
        ASSIGN_OR_RETURN(
            bool is_incomplete,
            IsIncompleteHeapGraph(connection, *values.ts, *values.upid));
        if (!is_incomplete) {
          return base::ErrStatus("Failed to build flamegraph");
        }
        tables::ExperimentalFlamegraphTable::Row alloc_row{};
        alloc_row.ts = *values.ts;
        alloc_row.upid = values.upid;
        alloc_row.profile_type = profile_type_id;
        alloc_row.depth = 0;
        alloc_row.name = pool->InternString(
            "ERROR: INCOMPLETE GRAPH (try increasing buffer size)");
        alloc_row.map_name = pool->InternString("JAVA");
        alloc_row.count = 1;
        alloc_row.cumulative_count = 1;
        alloc_row.size = 1;
        alloc_row.cumulative_size = 1;
        alloc_row.parent_id = std::nullopt;
        tbl->Insert(alloc_row);
        return tbl;
      }
      case ExperimentalFlamegraph::ProfileType::kHeapProfile:
        return base::ErrStatus("Failed to build flamegraph");
      case ExperimentalFlamegraph::ProfileType::kPerf:
        return tbl;
    }
  }

  flamegraph::Config config(*pool);
  config.view = flamegraph::Config::View(flamegraph::Config::TopDown{});
  config.name = *tree.Find("name");
  config.grouping_columns = {*tree.Find("map_name")};
  config.value_columns = {*tree.Find("count"), *tree.Find("size"),
                          *tree.Find("alloc_count"), *tree.Find("alloc_size")};
  config.aggregate_columns = {
      {*tree.Find("source_file"), flamegraph::Config::Aggregate::kOneOrNull,
       "source_file"},
      {*tree.Find("line_number"), flamegraph::Config::Aggregate::kOneOrNull,
       "line_number"},
      {*tree.Find("sample_ts"), flamegraph::Config::Aggregate::kMax,
       "sample_ts"},
  };
  ASSIGN_OR_RETURN(core::Tree result, flamegraph::Build(tree, config));

  auto find = [&result](const char* name) -> const core::Tree::Column* {
    auto column = result.Find(name);
    return column ? *column : nullptr;
  };
  const core::Tree::Column* depth_col = find("depth");
  const core::Tree::Column* name_col = find("name");
  const core::Tree::Column* map_name_col = find("map_name");
  const core::Tree::Column* self_count_col = find("self_count");
  const core::Tree::Column* cumulative_count_col = find("cumulative_count");
  const core::Tree::Column* self_size_col = find("self_size");
  const core::Tree::Column* cumulative_size_col = find("cumulative_size");
  const core::Tree::Column* self_alloc_count_col = find("self_alloc_count");
  const core::Tree::Column* cumulative_alloc_count_col =
      find("cumulative_alloc_count");
  const core::Tree::Column* self_alloc_size_col = find("self_alloc_size");
  const core::Tree::Column* cumulative_alloc_size_col =
      find("cumulative_alloc_size");
  const core::Tree::Column* source_file_col = find("source_file");
  const core::Tree::Column* line_number_col = find("line_number");
  const core::Tree::Column* sample_ts_col = find("sample_ts");
  PERFETTO_CHECK(depth_col && name_col && map_name_col && self_count_col &&
                 cumulative_count_col && self_size_col && cumulative_size_col &&
                 self_alloc_count_col && cumulative_alloc_count_col &&
                 self_alloc_size_col && cumulative_alloc_size_col);

  for (uint32_t row = 0; row < result.row_count; ++row) {
    tables::ExperimentalFlamegraphTable::Row alloc_row{};
    alloc_row.profile_type = profile_type_id;
    alloc_row.upid = values.upid;
    if (upid_group_id) {
      alloc_row.upid_group = upid_group_id;
    }
    // The 'ts' column is the latest sample timestamp directly attributed to
    // the node, falling back to the query's default timestamp for nodes which
    // never carried a sample.
    if (sample_ts_col && sample_ts_col->null_bv.is_set(row)) {
      alloc_row.ts = sample_ts_col->unchecked_data<int64_t>()[row];
    } else {
      alloc_row.ts = default_ts;
    }
    alloc_row.depth =
        static_cast<uint32_t>(depth_col->unchecked_data<int64_t>()[row] - 1);
    alloc_row.name = name_col->unchecked_data<StringPool::Id>()[row];
    alloc_row.map_name = map_name_col->unchecked_data<StringPool::Id>()[row];
    alloc_row.count = self_count_col->unchecked_data<int64_t>()[row];
    alloc_row.cumulative_count =
        cumulative_count_col->unchecked_data<int64_t>()[row];
    alloc_row.size = self_size_col->unchecked_data<int64_t>()[row];
    alloc_row.cumulative_size =
        cumulative_size_col->unchecked_data<int64_t>()[row];
    alloc_row.alloc_count =
        self_alloc_count_col->unchecked_data<int64_t>()[row];
    alloc_row.cumulative_alloc_count =
        cumulative_alloc_count_col->unchecked_data<int64_t>()[row];
    alloc_row.alloc_size = self_alloc_size_col->unchecked_data<int64_t>()[row];
    alloc_row.cumulative_alloc_size =
        cumulative_alloc_size_col->unchecked_data<int64_t>()[row];

    const uint32_t parent = result.parent[row];
    if (parent != core::Tree::kNullParent) {
      alloc_row.parent_id = ExperimentalFlamegraphTable::Id{parent};
    }
    if (source_file_col && source_file_col->null_bv.size() > 0 &&
        source_file_col->null_bv.is_set(row)) {
      alloc_row.source_file =
          source_file_col->unchecked_data<StringPool::Id>()[row];
    }
    if (line_number_col && line_number_col->null_bv.size() > 0 &&
        line_number_col->null_bv.is_set(row)) {
      alloc_row.line_number = static_cast<uint32_t>(
          line_number_col->unchecked_data<int64_t>()[row]);
    }
    tbl->Insert(alloc_row);
  }
  return tbl;
}

ExperimentalFlamegraph::Cursor::Cursor(PerfettoSqlConnection* connection,
                                       TraceProcessorContext* context)
    : connection_(connection),
      context_(context),
      table_(context->storage->mutable_string_pool()) {}

bool ExperimentalFlamegraph::Cursor::Run(
    const std::vector<SqlValue>& arguments) {
  base::StatusOr<InputValues> values_status =
      GetFlamegraphInputValues(arguments);
  if (!values_status.ok()) {
    return OnFailure(values_status.status());
  }
  InputValues values = std::move(values_status.value());
  switch (values.profile_type) {
    case ProfileType::kGraph:
      if (!values.ts || !values.upid) {
        return OnFailure(base::ErrStatus(
            "experimental_flamegraph: ts and upid must be present for heap "
            "graph"));
      }
      break;
    case ProfileType::kHeapProfile:
      if (!values.ts || !values.upid) {
        return OnFailure(base::ErrStatus(
            "experimental_flamegraph: ts and upid must be present for heap "
            "profile"));
      }
      break;
    case ProfileType::kPerf:
      break;
  }
  base::StatusOr<std::unique_ptr<tables::ExperimentalFlamegraphTable>>
      table_status = BuildFlamegraphTable(connection_, context_, values);
  if (!table_status.ok()) {
    return OnFailure(table_status.status());
  }
  std::unique_ptr<tables::ExperimentalFlamegraphTable> constructed_table =
      std::move(table_status.value());
  if (values.focus_str) {
    constructed_table =
        FocusTable(context_->storage.get(), std::move(constructed_table),
                   *values.focus_str);
  }
  table_ = std::move(*constructed_table);
  return OnSuccess(&table_.dataframe());
}

ExperimentalFlamegraph::ExperimentalFlamegraph(
    PerfettoSqlConnection* connection,
    TraceProcessorContext* context)
    : connection_(connection), context_(context) {}

ExperimentalFlamegraph::~ExperimentalFlamegraph() = default;

std::unique_ptr<StaticTableFunction::Cursor>
ExperimentalFlamegraph::MakeCursor() {
  return std::make_unique<Cursor>(connection_, context_);
}

dataframe::DataframeSpec ExperimentalFlamegraph::CreateSpec() {
  return tables::ExperimentalFlamegraphTable::kSpec.ToUntypedDataframeSpec();
}

std::string ExperimentalFlamegraph::TableName() {
  return tables::ExperimentalFlamegraphTable::Name();
}

uint32_t ExperimentalFlamegraph::GetArgumentCount() const {
  return 6;
}

class ExperimentalFlamegraphPlugin
    : public Plugin<ExperimentalFlamegraphPlugin> {
 public:
  ~ExperimentalFlamegraphPlugin() override;

  void RegisterStaticTableFunctions(
      PerfettoSqlConnection* connection,
      std::vector<std::unique_ptr<StaticTableFunction>>& fns) override {
    fns.emplace_back(
        std::make_unique<ExperimentalFlamegraph>(connection, trace_context_));
  }
};

ExperimentalFlamegraphPlugin::~ExperimentalFlamegraphPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<ExperimentalFlamegraphPlugin>();
      },
      ExperimentalFlamegraphPlugin::kPluginId,
      ExperimentalFlamegraphPlugin::kDepIds.data(),
      ExperimentalFlamegraphPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::experimental_flamegraph
