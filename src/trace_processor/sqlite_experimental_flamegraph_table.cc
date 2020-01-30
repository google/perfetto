
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

#include "src/trace_processor/sqlite_experimental_flamegraph_table.h"

#include "src/trace_processor/heap_profile_tracker.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

SqliteExperimentalFlamegraphTable::InputValues GetInputValues(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  const auto& cs = qc.constraints();

  auto ts_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::ts) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  auto upid_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::upid) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  auto profile_type_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::profile_type) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };

  auto ts_idx = static_cast<uint32_t>(
      std::distance(cs.begin(), std::find_if(cs.begin(), cs.end(), ts_fn)));
  auto upid_idx = static_cast<uint32_t>(
      std::distance(cs.begin(), std::find_if(cs.begin(), cs.end(), upid_fn)));
  auto profile_type_idx = static_cast<uint32_t>(std::distance(
      cs.begin(), std::find_if(cs.begin(), cs.end(), profile_type_fn)));

  // We should always have valid indices here because BestIndex should only
  // allow the constraint set to be chosen when we have an equality constraint
  // on both ts and upid.
  PERFETTO_CHECK(ts_idx < cs.size());
  PERFETTO_CHECK(upid_idx < cs.size());
  PERFETTO_CHECK(profile_type_idx < cs.size());

  int64_t ts = sqlite3_value_int64(argv[ts_idx]);
  UniquePid upid = static_cast<UniquePid>(sqlite3_value_int64(argv[upid_idx]));
  std::string profile_type =
      reinterpret_cast<const char*>(sqlite3_value_text(argv[profile_type_idx]));

  return SqliteExperimentalFlamegraphTable::InputValues{ts, upid, profile_type};
}

}  // namespace

SqliteExperimentalFlamegraphTable::SqliteExperimentalFlamegraphTable(
    sqlite3*,
    TraceProcessorContext* context)
    : context_(context) {}

SqliteExperimentalFlamegraphTable::~SqliteExperimentalFlamegraphTable() =
    default;

void SqliteExperimentalFlamegraphTable::RegisterTable(
    sqlite3* db,
    TraceProcessorContext* context) {
  SqliteTable::Register<SqliteExperimentalFlamegraphTable>(
      db, context, "experimental_flamegraph");
}

util::Status SqliteExperimentalFlamegraphTable::Init(
    int,
    const char* const*,
    SqliteTable::Schema* schema) {
  // Create an empty table for the sake of getting the schema.
  tables::ExperimentalFlamegraphNodesTable table(nullptr, nullptr);
  *schema = DbSqliteTable::ComputeSchema(table, name().c_str());

  using T = tables::ExperimentalFlamegraphNodesTable;

  // TODO(lalitm): make it so that this happens on the macro table itself.
  auto& cols = *schema->mutable_columns();
  cols[static_cast<uint32_t>(T::ColumnIndex::ts)].set_hidden(true);
  cols[static_cast<uint32_t>(T::ColumnIndex::upid)].set_hidden(true);
  cols[static_cast<uint32_t>(T::ColumnIndex::profile_type)].set_hidden(true);

  return util::OkStatus();
}

int SqliteExperimentalFlamegraphTable::BestIndex(const QueryConstraints& qc,
                                                 BestIndexInfo*) {
  using T = tables::ExperimentalFlamegraphNodesTable;

  const auto& cs = qc.constraints();

  auto ts_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::ts) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_ts_cs = std::find_if(cs.begin(), cs.end(), ts_fn) != cs.end();

  auto upid_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::upid) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_upid_cs = std::find_if(cs.begin(), cs.end(), upid_fn) != cs.end();

  auto profile_type_fn = [](const QueryConstraints::Constraint& c) {
    return c.column == static_cast<int>(T::ColumnIndex::profile_type) &&
           c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_profile_type_cs =
      std::find_if(cs.begin(), cs.end(), profile_type_fn) != cs.end();

  return has_ts_cs && has_upid_cs && has_profile_type_cs ? SQLITE_OK
                                                         : SQLITE_CONSTRAINT;
}

std::unique_ptr<SqliteTable::Cursor>
SqliteExperimentalFlamegraphTable::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this, context_));
}

SqliteExperimentalFlamegraphTable::Cursor::Cursor(
    SqliteTable* sqlite_table,
    TraceProcessorContext* context)
    : DbSqliteTable::Cursor(sqlite_table, nullptr, nullptr),
      context_(context) {}

int SqliteExperimentalFlamegraphTable::Cursor::Filter(
    const QueryConstraints& qc,
    sqlite3_value** argv,
    FilterHistory) {
  // Extract the old table to free after we call the parent Filter function.
  // We need to do this to make sure that we don't get a use-after-free for
  // any pointers the parent is holding onto in this table.
  auto old_table = std::move(table_);

  // Get the input column values and compute the flamegraph using them.
  values_ = GetInputValues(qc, argv);

  if (values_.profile_type == "graph") {
    auto* tracker = HeapGraphTracker::GetOrCreate(context_);
    table_ = tracker->BuildFlamegraph(values_.ts, values_.upid);
  }
  if (values_.profile_type == "native") {
    table_ = BuildNativeFlamegraph(context_->storage.get(),
                                   values_.upid, values_.ts);
  }

  // table_ can be nullptr precisely where the constraints passed to us don't
  // make sense. Therefore, we can just return this to SQLite.
  if (!table_)
    return SQLITE_CONSTRAINT;

  // Set the table in the parent to the correct value and then filter.
  DbSqliteTable::Cursor::set_table(table_.get());
  return DbSqliteTable::Cursor::Filter(qc, argv, FilterHistory::kDifferent);
}

}  // namespace trace_processor
}  // namespace perfetto
