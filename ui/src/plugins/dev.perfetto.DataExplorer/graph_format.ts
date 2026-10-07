// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Documentation for each node designed to be consumed by an LLM.
// TODO(stevegolton): Find a better way to keep this in sync with the actual
// JSON model for each node - this may involve defining a zod schema or similar
// on each node directly.

export const GRAPH_FORMAT = `
================================================================================
DATA EXPLORER GRAPH JSON FORMAT
================================================================================

Send the whole graph as a JSON string. Top-level shape:

{
  "nodes": [ <node>, ... ],          // every node in the graph
  "rootNodeIds": [ "<id>", ... ],    // ids of nodes with no input feeding them
  "nodeLayouts": { "<id>": {"x":0,"y":0} },  // OPTIONAL - omit for auto-layout
  "selectedNodeId": "<id>"           // OPTIONAL - node to select after loading
}

Each <node>:

{
  "nodeId": "<unique string id, e.g. \\"0\\", \\"1\\">",
  "type": "<one of the node types below>",
  "state": { ...type-specific fields, see catalog... },
  "nextNodes": [ "<downstream nodeId>", ... ],
  "primaryInputId": "<nodeId>",                  // OPTIONAL: the node above this one
  "secondaryInputIds": { "0": "<nodeId>", ... }  // OPTIONAL: side inputs, keyed by port
}

--------------------------------------------------------------------------------
CONNECTION MODEL (read carefully - this is where graphs break)
--------------------------------------------------------------------------------
Edges are stored BOTH directions and must agree:
- The upstream node lists the downstream id in its "nextNodes".
- The downstream node names the upstream id in EITHER "primaryInputId" (the
  single input that flows in from above) OR "secondaryInputIds" (side inputs,
  keyed by port number "0","1",... ).
If only one side is set the edge is dropped on load. Always set both.

- "rootNodeIds" = exactly the nodes that have NO input (no primaryInputId and no
  secondaryInputIds). Source nodes are always roots. Every other node must be
  reachable from a root via nextNodes, or it is lost on load.
- No cycles.

Which input a node uses:
- Source nodes (table, simple_slices, sql_source, time_range_source): no input.
- Single-input operations (aggregation, modify_columns, filter, sort,
  limit_and_offset, counter_to_intervals, visualisation, dashboard, metrics):
  use "primaryInputId".
- add_columns, filter_during, filter_in: "primaryInputId" for the main data,
  PLUS "secondaryInputIds":{"0": <the other input>}.
- Multi-source nodes (join, union, interval_intersect, create_slices,
  trace_summary): NO primaryInputId; all inputs go in "secondaryInputIds" by
  port. join/create_slices use ports "0" and "1"; union/interval_intersect take
  two or more ports "0","1","2",...

--------------------------------------------------------------------------------
SHARED SUB-TYPES (used inside several nodes' "state")
--------------------------------------------------------------------------------
ColumnInfo = {
  "name": "<column name>",
  "checked": true,            // true = keep/use this column (usually what you want)
  "alias": "<rename>",        // OPTIONAL
  "type": { "kind": "<k>" }   // OPTIONAL; kind is one of:
                              //   int double boolean string bytes timestamp duration arg_set_id
}
You can omit "type"; it is re-derived from the upstream schema on load.

--------------------------------------------------------------------------------
RECOMMENDED DEFAULT: real nodes (STRONGLY PREFERRED)
--------------------------------------------------------------------------------
STRONGLY prefer building the graph out of real, structured nodes (a "table" or
"slice" source feeding "filter", "sort", "aggregation", "modify_columns",
"limit_and_offset", joins, etc.) rather than packing everything into one
"sql_source" node. Each real node shows up in the UI as its own editable step:
the user can see exactly what every stage does, tweak it, reorder it, and learn
from it. A single "sql_source" collapses all of that into an opaque blob of SQL
the user cannot inspect or adjust without reading raw SQL.

So: decompose the request into one node per logical operation. Use a source node
for the data, then a separate node for each filter / sort / group-by / column
selection / limit.

Fall back to "sql_source" ONLY when no real node can express the operation (an
exotic SQL construct, a CTE, a window function, etc.), or when the user
explicitly asks for raw SQL. type "sql_source", state:
  { "sql": "<a single PerfettoSQL SELECT>" }
Rules for "sql":
- Exactly ONE SELECT (a leading "WITH ... SELECT" is allowed). No trailing ";".
- To use stdlib tables, prepend "INCLUDE PERFETTO MODULE <name>;" statements
  (each ending in ";"), then the SELECT last.
- It can read upstream nodes connected via secondaryInputIds as $input_0,
  $input_1, ... (port number).
- Confirm tables/columns with run_query / get_schema first; do not guess schema.

Preferred shape (the common case) - real nodes, one operation each:
{
  "nodes": [
    { "nodeId": "0", "type": "table",
      "state": { "sqlTable": "slice" },
      "nextNodes": ["1"] },
    { "nodeId": "1", "type": "sort",
      "state": { "sortCriteria": [ { "colName": "dur", "direction": "DESC" } ] },
      "primaryInputId": "0", "nextNodes": ["2"] },
    { "nodeId": "2", "type": "limit_and_offset",
      "state": { "limit": 20, "offset": 0 },
      "primaryInputId": "1", "nextNodes": [] }
  ],
  "rootNodeIds": ["0"]
}

Equivalent with sql_source (use ONLY as a fallback, less visible to the user):
{
  "nodes": [
    { "nodeId": "0", "type": "sql_source",
      "state": { "sql": "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20" },
      "nextNodes": [] }
  ],
  "rootNodeIds": ["0"]
}

================================================================================
FULL NODE CATALOG (the "state" object for each "type")
================================================================================

SOURCES (no input; always go in rootNodeIds)

- "sql_source"  -> see RECOMMENDED DEFAULT above. state: { "sql": string }

- "table"  -> read a whole trace table.
  state: { "sqlTable": "<table name, e.g. \\"slice\\">" }

- "simple_slices"  -> all slices in the trace. state: {} (no fields)

- "time_range_source"  -> a time window (ns).
  state: { "start": "<ns as string>", "end": "<ns as string>", "isDynamic": false }
  isDynamic true = follows the timeline selection; false = fixed snapshot.

SINGLE-INPUT OPERATIONS (set "primaryInputId" to the upstream node)

- "filter"  -> keep rows matching a condition.
  Freeform (preferred, simplest):
    state: { "filterMode": "freeform", "sqlExpression": "dur > 1000000 AND name GLOB 'foo*'" }
  Structured:
    state: {
      "filterMode": "structured",
      "filterOperator": "AND",            // or "OR"
      "filters": [
        { "column": "dur", "op": ">", "value": 1000000 }
      ]
    }
    op is one of: "=" "!=" "<" "<=" ">" ">=" "glob" | "in" "not in" (value is an
    array) | "is null" "is not null" (no value).

- "sort"  -> order rows.
  state: { "sortCriteria": [ { "colName": "dur", "direction": "DESC" } ] }  // ASC | DESC

- "limit_and_offset"  -> cap row count.
  state: { "limit": 100, "offset": 0 }

- "aggregation"  -> GROUP BY + aggregate.
  state: {
    "groupByColumns": [ { "name": "name", "checked": true } ],   // ColumnInfo[]
    "aggregations": [
      { "column": { "name": "dur" }, "aggregationOp": "SUM", "newColumnName": "total_dur" }
    ]
  }
  aggregationOp: COUNT | COUNT(*) | COUNT_DISTINCT | SUM | MIN | MAX | MEAN |
  MEDIAN | DURATION_WEIGHTED_MEAN | PERCENTILE. For COUNT(*) omit "column". For
  PERCENTILE add "percentile": <number 0-100>.

- "modify_columns"  -> select / rename columns.
  state: { "selectedColumns": [ { "name": "name", "checked": true, "alias": "slice_name" } ] }  // ColumnInfo[]

- "counter_to_intervals"  -> turn counter rows (ts, value) into intervals
  (adds dur, next_value, delta_value). Input must have id, ts, track_id, value
  and NOT already have dur. state: {} (no fields)

- "visualisation"  -> charts over the input rows.
  state: {
    "chartConfigs": [
      { "id": "c1", "column": "name", "chartType": "bar", "aggregation": "COUNT" }
    ]
  }
  chartType: bar | histogram | line | scatter | pie | treemap | boxplot |
  heatmap | cdf | scorecard. aggregation (bar/pie/treemap): COUNT (default) |
  SUM | MIN | MAX | MEAN ... For non-count add "measureColumn". histogram uses
  "column" (+ optional "binCount"); line/scatter need "yColumn".

SINGLE-INPUT + ONE SIDE INPUT (set "primaryInputId" AND "secondaryInputIds":{"0":...})

- "add_columns"  -> LEFT JOIN columns from the side node onto the main rows.
  state: {
    "selectedColumns": [ "<col from side node>", ... ],
    "leftColumn": "<join key in main input>",
    "rightColumn": "<join key in side input>",
    "columnAliases": { "<col>": "<alias>" }   // OPTIONAL
  }

- "filter_during"  -> keep only main-input intervals that overlap intervals from
  the side node. Both inputs need ts and dur.
  state: { "partitionColumns": ["utid"], "clipToIntervals": true }
  clipToIntervals true (default) = output uses the intersected ts/dur.

- "filter_in"  -> keep main rows whose value appears in the side node.
  state: { "baseColumn": "<col in main input>", "matchColumn": "<col in side input>" }

MULTI-SOURCE (no primaryInputId; inputs in "secondaryInputIds" by port)

- "join"  -> join two inputs. Ports "0" (left) and "1" (right).
  state: {
    "joinType": "INNER",            // or "LEFT"
    "conditionType": "equality",    // or "freeform"
    "leftColumn": "<left key>",     // for equality
    "rightColumn": "<right key>",   // for equality
    "sqlExpression": "",            // for freeform, e.g. "left.id = right.parent_id"
    "leftQueryAlias": "left",
    "rightQueryAlias": "right",
    "leftColumns": [ { "name": "...", "checked": true } ],   // ColumnInfo[], columns to keep
    "rightColumns": [ { "name": "...", "checked": true } ]   // ColumnInfo[]
  }

- "union"  -> stack rows from 2+ inputs (ports "0","1",...).
  state: { "selectedColumns": [ { "name": "...", "checked": true } ] }  // ColumnInfo[] common cols

- "interval_intersect"  -> intersect intervals from 2+ inputs (ports "0","1",...).
  All inputs need ts and dur.
  state: { "partitionColumns": ["utid"], "tsDurSource": "intersection" }
  tsDurSource: "intersection" or an input index number.

- "create_slices"  -> build slices by pairing start/end timestamps from two
  inputs. Ports "0" (starts) and "1" (ends).
  state: { "startsTsColumn": "ts", "endsTsColumn": "ts" }
  ( optional: "startsDurColumn", "endsDurColumn", "startsMode", "endsMode" )

EXPORT NODES (advanced; usually only when the user asks for metrics/dashboards)

- "metrics"  -> define a metric (primaryInputId = the data).
  state: {
    "metricIdPrefix": "my_metric",
    "valueColumns": [ { "column": "dur", "unit": "NS", "polarity": "POSITIVE" } ],
    "dimensionConfigs": { "<dim col>": { "displayName": "..." } },
    "dimensionUniqueness": ""
  }

- "trace_summary"  -> bundle metrics. Inputs are metrics nodes via
  "secondaryInputIds" ("0","1",...). state: {} (no fields)

- "dashboard"  -> export the input for use on dashboards (primaryInputId = data).
  state: { "exportName": "<name>" }

================================================================================
WORKFLOW
================================================================================
- Call get_graph FIRST when editing an existing graph; copy its exact shape and
  reuse its nodeIds rather than rebuilding from scratch.
- Prefer a single sql_source node for most requests.
- Use real node types only when the user explicitly wants that operation/UI.
- An invalid graph comes back as a tool error - read it, fix the JSON, retry.
`.trim();
