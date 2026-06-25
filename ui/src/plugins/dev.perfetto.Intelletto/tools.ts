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

import type {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import type {SqlValue} from '../../trace_processor/query_result';
import type {ToolImpl} from './provider';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import SpaghettiPlugin from '../dev.perfetto.Spaghetti';

const MAX_QUERY_ROWS = 5000;

// Minimal type for the Web MCP navigator.modelContext API.
interface ModelContextToolRegistration {
  unregister(): void;
}

interface ModelContext {
  registerTool(tool: {
    name: string;
    description: string;
    inputSchema: Record<string, unknown>;
    execute(args: Record<string, unknown>): Promise<{
      content: {type: string; text: string}[];
    }>;
  }): ModelContextToolRegistration;
}

declare global {
  interface Navigator {
    modelContext?: ModelContext;
  }
}

/**
 * Register tools with the Web MCP API (navigator.modelContext) if available.
 * This allows external AI agents (e.g. Claude Code via a browser) to call
 * these tools directly.
 */
export function registerWebMcpTools(tools: ToolImpl[]): void {
  if (!navigator.modelContext) return;
  const mc = navigator.modelContext;
  for (const tool of tools) {
    mc.registerTool({
      name: tool.def.name,
      description: tool.def.description,
      inputSchema: tool.def.input_schema,
      async execute(args) {
        const result = await tool.handle(args);
        return {content: [{type: 'text', text: result}]};
      },
    });
  }
}

/**
 * Creates the set of tools available to the LLM for interacting with the
 * trace and the UI.
 */
export function createTools(trace: Trace): ToolImpl[] {
  return [
    createExecuteQueryTool(trace),
    createGetSelectionTool(trace),
    createSelectEventTool(trace),
    createScrollTrackIntoViewTool(trace),
    createListTablesTool(trace),
    createGetTableSchemaTool(trace),
    createListTracksTool(trace),
    createSelectTrackTool(trace),
    createGetSpaghettiGraphTool(trace),
    createValidateSpaghettiGraphTool(trace),
    createSetSpaghettiGraphTool(trace),
    createSelectSpaghettiNodeTool(trace),
    createPinSpaghettiNodeTool(trace),
  ];
}

function createExecuteQueryTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'execute_query',
      description: `Execute a SQL query against the Perfetto trace processor.

The trace processor uses SQLite-based SQL with extensions. Key tables include:
- slice: scheduling/function call slices with ts, dur, name, track_id
- thread: thread info with utid, tid, name, upid
- process: process info with upid, pid, name
- counter: counter values with ts, value, track_id
- sched_slice: CPU scheduling with ts, dur, cpu, utid
- android_logs: Android logcat with ts, prio, tag, msg

Use 'INCLUDE PERFETTO MODULE <name>' to load stdlib modules (in a separate
query from the one that uses the module).

The stdlib is documented at https://perfetto.dev/docs/analysis/stdlib-docs

Prefer aggregated results over large result sets. Max ${MAX_QUERY_ROWS} rows.`,
      input_schema: {
        type: 'object',
        properties: {
          query: {
            type: 'string',
            description: 'The SQL query to execute',
          },
        },
        required: ['query'],
      },
    },
    async handle(input) {
      const query = input.query as string;
      const result = await trace.engine.query(query);
      const columns = result.columns();
      const rows: Record<string, SqlValue>[] = [];

      for (const it = result.iter({}); it.valid(); it.next()) {
        if (rows.length >= MAX_QUERY_ROWS) {
          return JSON.stringify({
            error: `Query returned too many rows (>${MAX_QUERY_ROWS}). Use aggregation or LIMIT.`,
            partial_rows: rows,
          });
        }
        const row: Record<string, SqlValue> = {};
        for (const col of columns) {
          let value = it.get(col);
          // Convert bigint to number for JSON serialization
          if (typeof value === 'bigint') {
            value = Number(value);
          }
          row[col] = value;
        }
        rows.push(row);
      }

      return JSON.stringify(rows);
    },
  };
}

function createGetSelectionTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'get_selection',
      description: `Get information about the currently selected item in the Perfetto UI.

Returns the current selection state including:
- For track events: the track URI, event ID, timestamp, and duration
- For area selections: the time range and selected tracks
- For no selection: an empty result

This is useful to understand what the user is looking at before answering
questions about their trace.`,
      input_schema: {
        type: 'object',
        properties: {},
      },
    },
    async handle() {
      const selection = trace.selection.selection;

      switch (selection.kind) {
        case 'track_event':
          return JSON.stringify({
            kind: 'track_event',
            trackUri: selection.trackUri,
            eventId: selection.eventId,
          });

        case 'area': {
          return JSON.stringify({
            kind: 'area',
            start: Number(selection.start),
            end: Number(selection.end),
            trackCount: selection.trackUris.length,
          });
        }

        case 'track':
          return JSON.stringify({
            kind: 'track',
            trackUri: selection.trackUri,
          });

        default:
          return JSON.stringify({kind: 'empty'});
      }
    },
  };
}

function createSelectEventTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'select_event',
      description: `Select and scroll to a specific event in the Perfetto UI timeline.

This highlights the event and shows its details in the selection panel.
Use this to point the user to specific events you've found via SQL queries.

The table parameter is the SQL table name (e.g., 'slice', 'sched_slice').
The id parameter is the row ID from that table.`,
      input_schema: {
        type: 'object',
        properties: {
          table: {
            type: 'string',
            description:
              'The SQL table name (e.g., "slice", "sched_slice", "counter")',
          },
          id: {
            type: 'number',
            description: 'The row ID in the table to select',
          },
        },
        required: ['table', 'id'],
      },
    },
    async handle(input) {
      const table = input.table as string;
      const id = input.id as number;

      trace.selection.selectSqlEvent(table, id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });

      return JSON.stringify({status: 'ok', table, id});
    },
  };
}

function createScrollTrackIntoViewTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'scroll_track_into_view',
      description: `Scroll a track into view in the Perfetto UI timeline.

Given a track URI, this scrolls the timeline so the track is visible,
expanding any parent track groups if needed.

You can find track URIs by querying the track table, e.g.:
  SELECT uri FROM track WHERE name LIKE '%cpu%'

Optionally provide a timestamp range to also pan the viewport to that time.`,
      input_schema: {
        type: 'object',
        properties: {
          track_uri: {
            type: 'string',
            description: 'The track URI to scroll into view',
          },
          start_ts: {
            type: 'number',
            description: 'Optional start timestamp (ns) to pan the viewport to',
          },
          end_ts: {
            type: 'number',
            description: 'Optional end timestamp (ns) for the time range',
          },
        },
        required: ['track_uri'],
      },
    },
    async handle(input) {
      const trackUri = input.track_uri as string;
      const startTs = input.start_ts as number | undefined;
      const endTs = input.end_ts as number | undefined;

      trace.scrollTo({
        track: {uri: trackUri, expandGroup: true},
        ...(startTs !== undefined
          ? {
              time: {
                start: Time.fromRaw(BigInt(Math.round(startTs))),
                end:
                  endTs !== undefined
                    ? Time.fromRaw(BigInt(Math.round(endTs)))
                    : undefined,
                behavior: 'focus' as const,
              },
            }
          : {}),
      });

      return JSON.stringify({status: 'ok', track_uri: trackUri});
    },
  };
}

function createListTablesTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'list_sql_tables',
      description: `List available SQL tables and views from the Perfetto stdlib.

Returns table names with their descriptions and importance level.
Use this to discover what tables are available before writing queries
or building query builder graphs.

Optionally filter by a search term to narrow results.`,
      input_schema: {
        type: 'object',
        properties: {
          filter: {
            type: 'string',
            description:
              'Optional search term to filter table names (case-insensitive)',
          },
        },
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(SqlModulesPlugin);
        const sqlModules = plugin.getSqlModules();
        if (!sqlModules) {
          return JSON.stringify({
            status: 'error',
            message: 'SQL modules not loaded yet',
          });
        }

        const filterTerm = ((input.filter as string) ?? '').toLowerCase();
        const tables = sqlModules.listTables();
        const filtered = filterTerm
          ? tables.filter((t) => t.name.toLowerCase().includes(filterTerm))
          : tables;

        return JSON.stringify(
          filtered.map((t) => ({
            name: t.name,
            type: t.type,
            description: t.description || undefined,
            importance: t.importance || undefined,
            columnCount: t.columns.length,
          })),
        );
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createGetTableSchemaTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'get_table_schema',
      description: `Get the column schema for a specific SQL table or view.

Returns the column names, types, and descriptions for the given table.
Use this to understand the structure of a table before querying it.`,
      input_schema: {
        type: 'object',
        properties: {
          table_name: {
            type: 'string',
            description: 'The name of the table to get the schema for',
          },
        },
        required: ['table_name'],
      },
    },
    async handle(input) {
      const tableName = input.table_name as string;
      try {
        const plugin = trace.plugins.getPlugin(SqlModulesPlugin);
        const sqlModules = plugin.getSqlModules();
        if (!sqlModules) {
          return JSON.stringify({
            status: 'error',
            message: 'SQL modules not loaded yet',
          });
        }

        const table = sqlModules.getTable(tableName);
        if (!table) {
          // Fall back to pragma for non-stdlib tables
          const result = await trace.engine.query(
            `PRAGMA table_info('${tableName}')`,
          );
          const columns: {name: string; type: string}[] = [];
          for (const it = result.iter({}); it.valid(); it.next()) {
            columns.push({
              name: String(it.get('name')),
              type: String(it.get('type')),
            });
          }
          if (columns.length === 0) {
            return JSON.stringify({
              status: 'not_found',
              message: `Table "${tableName}" not found`,
            });
          }
          return JSON.stringify({name: tableName, columns});
        }

        const module = sqlModules.getModuleForTable(tableName);
        return JSON.stringify({
          name: table.name,
          type: table.type,
          description: table.description || undefined,
          module: module?.includeKey,
          columns: table.columns.map((c) => ({
            name: c.name,
            type: c.type ? c.type.kind : undefined,
            description: c.description || undefined,
          })),
        });
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createListTracksTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'list_tracks',
      description: `List tracks visible in the Perfetto UI timeline.

Returns tracks with their URIs and tags (kind, trackIds, cpu, utid, upid, type).
Use the URIs with scroll_track_into_view or to understand the timeline layout.

Optionally filter by a search term to narrow results (matches against URI and tag values).`,
      input_schema: {
        type: 'object',
        properties: {
          filter: {
            type: 'string',
            description:
              'Optional search term to filter tracks (case-insensitive, matches URI and tags)',
          },
          limit: {
            type: 'number',
            description: 'Max number of tracks to return (default 100)',
          },
        },
      },
    },
    async handle(input) {
      const filterTerm = ((input.filter as string) ?? '').toLowerCase();
      const limit = (input.limit as number) ?? 100;

      const allTracks = trace.tracks.getAllTracks();
      const results: {uri: string; tags?: Record<string, unknown>}[] = [];

      for (const track of allTracks) {
        if (results.length >= limit) break;

        const uri = track.uri;
        const tags = track.tags;

        if (filterTerm) {
          const haystack = [
            uri,
            ...(tags ? Object.values(tags).map((v) => String(v)) : []),
          ]
            .join(' ')
            .toLowerCase();
          if (!haystack.includes(filterTerm)) continue;
        }

        const entry: {uri: string; tags?: Record<string, unknown>} = {uri};
        if (tags && Object.keys(tags).length > 0) {
          entry.tags = {...tags};
        }
        results.push(entry);
      }

      return JSON.stringify(results);
    },
  };
}

function createSelectTrackTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'select_track',
      description: `Select a track in the Perfetto UI timeline and scroll it into view.

This highlights the track in the timeline, expanding any parent groups if needed.
Prefer this over scroll_track_into_view when you want to draw the user's attention
to a specific track, as it both selects and scrolls.

You can find track URIs using the list_tracks tool or by querying:
  SELECT uri FROM track WHERE name LIKE '%cpu%'`,
      input_schema: {
        type: 'object',
        properties: {
          track_uri: {
            type: 'string',
            description: 'The track URI to select',
          },
        },
        required: ['track_uri'],
      },
    },
    async handle(input) {
      const trackUri = input.track_uri as string;

      trace.selection.selectTrack(trackUri, {
        scrollToSelection: true,
      });

      return JSON.stringify({status: 'ok', track_uri: trackUri});
    },
  };
}

// ---------------------------------------------------------------------------
// Spaghetti query builder tools
// ---------------------------------------------------------------------------

function createValidateSpaghettiGraphTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'validate_spaghetti_graph',
      description: `Validate a Spaghetti query builder graph JSON without applying it.

Returns a list of validation errors. An empty errors array means the graph
is structurally valid. Use this before set_spaghetti_graph to check your
work, especially when building complex graphs.

Checks performed:
- All node types are known
- Each node's config passes isValid()
- next chains are valid (correct types, no cycles)
- inputs[] entries reference known node IDs
- inputs[] indices are within the target node's port count`,
      input_schema: {
        type: 'object',
        properties: {
          graph: {
            type: 'object',
            description: 'The graph JSON to validate',
          },
        },
        required: ['graph'],
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(SpaghettiPlugin);
        const errors = plugin.validateGraphJson(JSON.stringify(input.graph));
        return JSON.stringify({
          valid: errors.length === 0,
          errors,
        });
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createGetSpaghettiGraphTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'get_spaghetti_graph',
      description: `Get the current graph state from the Spaghetti query builder.

Returns the serialized JSON of the graph, including all nodes and labels.
Wired connections are stored as inputs[] arrays on each node. Returns null
if the Spaghetti page is not open.

Use this to understand the user's current analysis pipeline before modifying it.
Navigate to #!/spaghetti first if the page is not open.`,
      input_schema: {
        type: 'object',
        properties: {},
      },
    },
    async handle() {
      try {
        const plugin = trace.plugins.getPlugin(SpaghettiPlugin);
        const json = plugin.getGraphJson();
        if (json === undefined) {
          return JSON.stringify({
            status: 'empty',
            message:
              'Spaghetti page is not open. Navigate to #!/spaghetti first.',
            graph: null,
          });
        }
        return JSON.stringify({status: 'ok', graph: JSON.parse(json)});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createSetSpaghettiGraphTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'set_spaghetti_graph',
      description: `Set or replace the Spaghetti query builder graph.

This replaces the entire graph and validates the input first — if validation
fails, the graph is NOT applied and errors are returned instead.

Use get_spaghetti_graph first to modify rather than replace.
The Spaghetti page must be open (#!/spaghetti).

## Store format

{
  "nodes": [<RootNodeData>, ...],  // Flat array of root node objects
  "labels": []                     // Usually empty
}

## RootNodeData (top-level node, has canvas position)

{
  "type": "<type>",    // node type string (see below)
  "id": "<id>",        // stable ID used in inputs references
  "x": 150, "y": 100, // canvas position — ONLY on root nodes
  "config": {...},     // type-specific config (see each node below)
  "next": <NodeData>,  // OPTIONAL: inline child node (docked primary input)
  "inputs": [...]      // OPTIONAL: wired input connections (see below)
}

## NodeData (chain node, nested inside "next")

{
  "type": "<type>",  // node type string
  "id": "<id>",      // stable ID (may be referenced in other nodes' inputs)
  "config": {...},
  "next": <NodeData>, // further chaining — no x/y, inherits root's canvas position
  "inputs": [...]     // OPTIONAL: wired input connections (see below)
}

## Inputs (wired connections)

Each node has an optional "inputs" array. Each element is either a node ID
(the upstream node connected to that port) or null (port slot is unconnected).
The array is indexed by port number:

  []                 – no wired inputs (or only a docked parent via "next")
  ["n1", "n2"]       – port 0 comes from node "n1", port 1 from "n2"
  [null, "n1"]       – port 0 is unconnected, port 1 comes from "n1"

All nodes have a single output, so you only need to record the source node ID —
no fromPort field is needed.

The docked parent (via "next") provides port 0 implicitly and is NOT listed in
inputs. Use "inputs" only for explicit wired connections.

## Stacking (next) input/output semantics

When node A has "next": B, the stack means:
  A's OUTPUT → B's FIRST INPUT (port 0 / the primary/left input)

This is always port 0 regardless of node type. For a join node, port 0 is the
LEFT input, so stacking feeds the left side. The right input (port 1) must
always be wired via inputs: [null, "<rightNodeId>"] on the join node.

IMPORTANT — stacking and wired inputs are mutually exclusive for port 0:
- If node A has "next": B, the stack occupies A's output and B's port 0.
  Do NOT also put A's id in B's inputs[0].
- This means stacking and fanout are incompatible. If A's output needs to feed
  more than one consumer, do NOT use "next" — use inputs[] on each consumer
  instead. Restructure so A is a standalone root node and all consumers reference
  it in their inputs arrays.

## Docking as a grouping convention

Use next (docking) not just as a wiring mechanism but as a *semantic signal*:
docked nodes are visually stacked and signal that they form a single logical
unit — e.g. "load slices, keep only long ones, sort by duration" is one block.

PREFER docking (next) when:
- The edge is a simple linear chain — one output feeds one input, nothing else
  branches off or fans in.
- The nodes are semantically part of the same pipeline stage (e.g. a from +
  a chain of filters/sorts/limits all operating on the same dataset).

USE inputs[] when:
- The data fans out (one node feeds multiple consumers).
- The target has multiple inputs (join left/right, union, interval_intersect).
- The edge crosses pipeline stages — e.g. a subquery result feeding a join as
  the right-hand side is conceptually separate and should use inputs[].

Example — "from slice → filter → sort" is one logical unit: fully dock it.
A "from thread" node feeding a join's right side should use inputs[] on the join.

## Node reference

### "from" — read a table or view
  config: { "table": "slice" }
  No inputs. isValid: table must be non-empty.

### "time_range" — emit one row: (id=0, ts, dur)
  config: { "ts": "123456789", "dur": "5000000" }  // strings
  No inputs. isValid: ts != "0" or dur != "0".
  Useful as an input to interval_intersect.

### "filter" — WHERE clause
  config: {
    "conditions": [                  // structured conditions — add as many as needed
      { "column": "dur", "op": ">", "value": "1000000" },
      { "column": "name", "op": "LIKE", "value": "foo%" }
    ],
    "conjunction": "AND"             // "AND" | "OR" — applies between ALL conditions
  }
  Ops: "=" | "!=" | ">" | ">=" | "<" | "<=" | "LIKE" | "NOT LIKE" | "GLOB"
       | "IS NULL" | "IS NOT NULL"
  Multiple conditions are combined with the conjunction: all joined by AND, or
  all joined by OR. Use a single filter node with multiple conditions rather
  than chaining multiple filter nodes when the conditions share the same logic.

### "select" — project/rename/add computed columns
  config: {
    "entries": [                            // column references with optional alias
      { "column": "ts", "alias": "" },
      { "column": "dur", "alias": "duration" }
    ],
    "expressions": [                        // computed expressions with alias
      { "expression": "dur / 1e6", "alias": "dur_ms" }
    ]
  }
  Output columns = entries (using alias if set) + expressions.

  IMPORTANT — to add computed columns while keeping ALL upstream columns,
  leave entries as [] and put only your expressions in the expressions array.
  The node then generates: SELECT *, expr1 AS alias1, ... FROM upstream.
  Do NOT put {"column":"*","alias":""} in entries — while that technically
  produces the same SQL, it bypasses the intended code path and is fragile.
  The correct pattern for "keep everything, add a column" is entries:[], expressions:[...].

### "sort" — ORDER BY
  config: {
    "sortColumn": "",                // legacy single column (leave empty)
    "sortOrder": "ASC",              // legacy (leave "ASC")
    "conditions": [                  // use this array instead
      { "column": "dur", "order": "DESC" },
      { "column": "ts",  "order": "ASC"  }
    ]
  }
  isValid: always true.

### "limit" — LIMIT N
  config: { "limitCount": "100" }   // string digits
  isValid: limitCount is non-empty digits.

### "groupby" — GROUP BY + aggregations
  config: {
    "groupColumns": ["name", "upid"],
    "aggregations": [
      { "func": "SUM",   "column": "dur",   "alias": "total_dur" },
      { "func": "COUNT", "column": "dur",   "alias": "cnt" },
      { "func": "AVG",   "column": "dur",   "alias": "avg_dur" },
      { "func": "MIN",   "column": "dur",   "alias": "min_dur" },
      { "func": "MAX",   "column": "dur",   "alias": "max_dur" }
    ]
  }
  isValid: always true.

### "join" — LEFT JOIN or INNER JOIN two inputs
  config: {
    "joinType": "LEFT",              // "LEFT" | "INNER" — required
    "leftColumn": "utid",            // join key from the left input
    "rightColumn": "utid",           // join key from the right input
    "columns": [                     // columns to ADD from the right input only
      { "column": "name", "alias": "" },   // alias="" auto-generates "right_name" if collision
      { "column": "pid",  "alias": "proc_pid" }
    ]
  }
  Output = ALL columns from left (SELECT l.*) + the listed columns from right.
  Do NOT list left-input columns in "columns" — they are always included
  automatically. Only list the extra columns you want pulled in from the right.
  Static inputs: port 0 = "left", port 1 = "right".
  Wire via inputs: ["<leftNodeId>", "<rightNodeId>"] on the join node.
  Or if left is docked (next), wire only the right: inputs: [null, "<rightNodeId>"].
  isValid: leftColumn and rightColumn must be non-empty.
  If right is disconnected, emits passthrough SELECT * FROM left.

### "union" — UNION / UNION ALL of N inputs
  config: { "distinct": false, "numInputs": 2 }  // numInputs controls port count
  Wire each source via inputs[], index matching the port number (0-based).

### "interval_intersect" — intersect time intervals (_interval_intersect!())
  config: {
    "numInputs": 2,                  // number of input ports
    "partitionColumns": ["upid"],    // columns to partition by (can be [])
    "filterNegativeDur": true
  }
  Wire each source via inputs[], index matching the port number.
  IMPORTANT: every input MUST have columns named EXACTLY "id", "ts", and "dur"
  (case-sensitive). If an upstream table uses different names, add a select node
  before the interval_intersect to rename the columns to id/ts/dur.

### "extract_arg" — extract values from the args table
  config: {
    "extractions": [
      { "column": "arg_set_id", "argName": "my.key", "alias": "my_key" }
    ]
  }
  Joins args on column=arg_set_id, extracts display_value as alias.
  isValid: always true.

### "sql" — raw SQL escape hatch
  config: {
    "sql": "SELECT e.ts, e.dur, e.name, t.name AS thread\nFROM events e\nJOIN threads t USING (utid)",
    "inputPorts": ["events", "threads"],  // alias per input; length = number of ports
    "columns": [                          // manually declare output column types (optional)
      { "name": "ts",     "type": "timestamp" },
      { "name": "dur",    "type": "duration" },
      { "name": "name",   "type": "string" },
      { "name": "thread", "type": "string" }
    ]
  }
  Wire inputs via inputs[] (index matches inputPorts index).
  Each wired input is injected as a named CTE before the SQL body:
    WITH events AS (SELECT * FROM <ref>), threads AS (SELECT * FROM <ref>)
    <sql>
  The alias used in the WITH clause comes from inputPorts[i] (falls back to
  "input_i" if blank). Reference that alias by name inside the sql field.
  Use this when no combination of other nodes can express the query.
  The sql field must be a complete SELECT statement. It becomes a CTE whose
  output can be wired to downstream nodes via inputs[] or next.
  columns is optional — leave it as [] if you don't need type-aware column
  picking downstream. Valid type values: "int" | "double" | "string" |
  "boolean" | "timestamp" | "duration" | "bytes" | "" (unknown).
  isValid: sql must be non-empty.

### "chart" — bar chart dashboard (variable inputs)
  config: {
    "numInputs": 1,
    "charts": [
      { "xCol": "name", "yCol": "" },    // yCol="" → COUNT(*)
      { "xCol": "name", "yCol": "dur" }  // yCol set → SUM(yCol)
    ]
  }
  Wire sources via inputs[]. Shows charts in details panel.

## Fan-out: one node as input to multiple consumers

A single node can feed multiple downstream nodes simultaneously. Reference its
ID in each consumer's inputs[]. Do NOT create duplicate "from" nodes for the
same table just because you need the data in two places.

Example — slice feeds both a groupby (for aggregation) and a join (as left):
  n3 join:    inputs: ["n1", "n2"]  (left=n1, right=n2)
  n2 groupby: inputs: ["n1"]        (input from n1)

n1 is referenced in two inputs arrays — that is fine. No "next" needed here
since n1 docks to neither; both relationships are expressed via inputs[].

## Layout conventions

Ports and flow direction:
- All node outputs are on the RIGHT side.
- All node inputs are on the LEFT side, indexed from top (0) to bottom.
- Data flow is LEFT → RIGHT across the canvas.
- Docked nodes (next) stack TOP → BOTTOM at the same x position; visually
  they form a vertical tower, but data still enters from the left and exits
  to the right of the whole tower.

Implication for positioning:
- Pipeline stages that are connected via connections should be at increasing
  x values (left-to-right progression).
- Nodes within the same docked chain share the same x (only the root has x/y).

## Positioning guidelines
- Source nodes (from, time_range): x=150, stagger y by 180
- Linear chain (docked via next): root has x/y; chain nodes have no x/y
- Multi-input collector (join, union, ii): x=480, y centered between inputs
- Further downstream stages: x=800, x=1100, etc.

## Examples

### Slice pipeline: from → filter → sort (docked chain)
{
  "nodes": [
    {
      "type": "from", "id": "n1", "x": 150, "y": 100,
      "config": {"table": "slice"},
      "next": {
        "type": "filter", "id": "n2",
        "config": {"filterExpression": "", "conditions": [{"column": "dur", "op": ">", "value": "1000000"}], "conjunction": "AND"},
        "next": {
          "type": "sort", "id": "n3",
          "config": {"sortColumn": "", "sortOrder": "ASC", "conditions": [{"column": "dur", "order": "DESC"}]}
        }
      }
    }
  ],
  "labels": []
}

### Interval intersect: sched_slice ∩ slice partitioned by utid
{
  "nodes": [
    {"type": "from", "id": "n1", "x": 150, "y": 100, "config": {"table": "sched_slice"}},
    {"type": "from", "id": "n2", "x": 150, "y": 280, "config": {"table": "slice"}},
    {"type": "interval_intersect", "id": "n3", "x": 480, "y": 190,
     "config": {"numInputs": 2, "partitionColumns": ["utid"], "filterNegativeDur": true},
     "inputs": ["n1", "n2"]}
  ],
  "labels": []
}`,
      input_schema: {
        type: 'object',
        properties: {
          graph: {
            type: 'object',
            description:
              'The graph JSON object with nodes (RootNodeData[] array) and labels. Connections are encoded as inputs[] arrays on each node.',
          },
        },
        required: ['graph'],
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(SpaghettiPlugin);
        const json = JSON.stringify(input.graph);
        const errors = plugin.validateGraphJson(json);
        if (errors.length > 0) {
          return JSON.stringify({status: 'validation_error', errors});
        }
        plugin.loadGraphJson(json);
        return JSON.stringify({status: 'ok'});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createPinSpaghettiNodeTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'pin_spaghetti_node',
      description: `Pin a node in the Spaghetti query builder so the details panel always shows its results.

When a node is pinned, the details panel is locked to that node regardless
of what the user clicks. A pin icon appears in the toolbar; the user can
click it to unpin. Use this after set_spaghetti_graph to point the user at
the most relevant output node.

Pass node_id as an empty string to unpin without selecting a new node.`,
      input_schema: {
        type: 'object',
        properties: {
          node_id: {
            type: 'string',
            description: 'The ID of the node to pin, or empty string to unpin',
          },
        },
        required: ['node_id'],
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(SpaghettiPlugin);
        const nodeId = input.node_id as string;
        plugin.pinNode(nodeId || undefined);
        return JSON.stringify({status: 'ok', node_id: nodeId || null});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createSelectSpaghettiNodeTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'select_spaghetti_node',
      description: `Select a node in the Spaghetti query builder by ID.

Selects the node on the canvas so it is highlighted. To lock the details
panel to always show that node's results, use pin_spaghetti_node instead.

Use get_spaghetti_graph first to discover available node IDs.`,
      input_schema: {
        type: 'object',
        properties: {
          node_id: {
            type: 'string',
            description: 'The ID of the node to select',
          },
        },
        required: ['node_id'],
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(SpaghettiPlugin);
        const nodeId = input.node_id as string;
        plugin.selectNode(nodeId);
        return JSON.stringify({status: 'ok', node_id: nodeId});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}
