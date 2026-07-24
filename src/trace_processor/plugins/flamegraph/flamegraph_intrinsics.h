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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_INTRINSICS_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_INTRINSICS_H_

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"

namespace perfetto::trace_processor {

// The SQL surface for computing flamegraphs. The three functions compose in
// a single statement (SQLite pointer values do not survive across
// statements):
//
//   SELECT __intrinsic_flamegraph(
//     __intrinsic_flamegraph_agg(
//         s.id, s.parentId, s.name, s.value, 'mapping', s.mapping),
//     __intrinsic_flamegraph_config(
//         'view', 'TOP_DOWN',
//         'filter', 'SHOW_STACK', 'foo.*',
//         'grouping', 'mapping'))
//   FROM frames s
//
// The result is a table pointer to be consumed with __intrinsic_table_ptr:
// one row per merged node, parents before children, with columns
//   id, parentId (-1 for roots), depth (1, 2, ... for the top-down part,
//   -1, -2, ... for the bottom-up part), name ('' becomes 'unknown'),
//   selfValue, cumulativeValue (LONG if every input value was integral,
//   DOUBLE otherwise), then one column per grouping column (the merged
//   frames share the value) and one per aggregate column (the merged
//   frames' values combined per the configured mode).
// Presentation (sibling ordering, x extents) is computed in SQL over this
// output; see the flamegraph stdlib module.

// Aggregate function collecting the input frames (tagged
// "FLAMEGRAPH_FRAMES").
//
// Arguments: (id, parentId, name, value) followed by ('column name', value)
// pairs, one per property column.
struct FlamegraphAgg : public sqlite::AggregateFunction<FlamegraphAgg> {
  static constexpr char kName[] = "__intrinsic_flamegraph_agg";
  static constexpr int kArgCount = -1;
  using UserData = StringPool;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
  static void Final(sqlite3_context* ctx);
};

// Pure function building the flamegraph configuration (tagged
// "FLAMEGRAPH_CONFIG") from a tagged token list (see sqlite_tagged_args.h):
//   'view', 'TOP_DOWN'|'BOTTOM_UP'|'PIVOT'
//   'pivot', <regex>              -- required iff view is PIVOT
//   'filter', 'SHOW_STACK'|'HIDE_STACK'|'SHOW_FROM_FRAME'|'HIDE_FRAME',
//             <regex>             -- repeatable
//   'grouping', <column>          -- repeatable
//   'aggregate', <column>, 'ONE_OR_SUMMARY'|'SUM'|'CONCAT_WITH_COMMA'
//                                 -- repeatable
struct FlamegraphConfig : public sqlite::Function<FlamegraphConfig> {
  static constexpr char kName[] = "__intrinsic_flamegraph_config";
  static constexpr int kArgCount = -1;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
};

// Scalar function running the flamegraph computation over the collected
// frames and the config, returning a dataframe tagged "TABLE".
struct FlamegraphBuild : public sqlite::Function<FlamegraphBuild> {
  static constexpr char kName[] = "__intrinsic_flamegraph";
  static constexpr int kArgCount = 2;
  using UserData = StringPool;

  static void Step(sqlite3_context*, int argc, sqlite3_value** argv);
};

}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::flamegraph {

// Registers the Flamegraph plugin with the global plugin set. Idempotent;
// only the first call has an effect. Must run before the first
// GetPluginSet() call (i.e. before constructing TraceProcessorImpl).
void RegisterPlugin();

}  // namespace perfetto::trace_processor::flamegraph

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_FLAMEGRAPH_FLAMEGRAPH_INTRINSICS_H_
