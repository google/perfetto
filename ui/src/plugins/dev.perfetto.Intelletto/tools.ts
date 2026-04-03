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

import {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import {SqlValue} from '../../trace_processor/query_result';
import {ToolImpl} from './provider';
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
- nextId references point to existing nodes
- Connection fromNode/toNode exist
- Connection toPort is within the target node's input port count`,
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

Returns the serialized JSON of the graph, including all nodes, connections,
and labels. Returns null if the Spaghetti page is not open.

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
  "nodes": [["<id>", <NodeData>], ...],  // Array of [id, NodeData] pairs
  "connections": [...],                  // Connection objects
  "labels": []                           // Usually empty
}

## NodeData

{
  "type": "<type>",        // node type string (see below)
  "id": "<id>",            // must match the key in the nodes array
  "x": 150, "y": 100,      // canvas position
  "config": {...},         // type-specific config (see each node below)
  "inputs": [...],         // ONLY for variable-input nodes (union, interval_intersect, chart)
  "nextId": "<id>"         // OPTIONAL: docks a child node as primary input
}

## Connections

{
  "fromNode": "<id>",  "fromPort": 0,   // source node, port index
  "toNode": "<id>",    "toPort": 0      // target node, input port index
}

Connections wire output→input. For docked linear chains use nextId instead —
no connection needed. Connections are needed for join's right input (toPort=1)
and for wiring multi-input nodes (union, interval_intersect, chart).

IMPORTANT — nextId vs connections are mutually exclusive for the same edge:
- If node A has nextId="B", that already establishes A as B's primary input.
  Do NOT also add a connection from A to B. Using both double-wires the input
  and produces broken output. Pick one: use nextId for simple linear chains,
  or use a connection — never both for the same pair.
- When node A has nextId="B", A's first output (fromPort=0) and B's first input
  (toPort=0) are both fully occupied by the dock relationship. You cannot add
  any connection that targets B's toPort=0, nor any connection from A's
  fromPort=0 to B. If you need to fan out from A, do so from a different node
  upstream, or restructure so A is not docked.

## Docking as a grouping convention

Use nextId (docking) not just as a wiring mechanism but as a *semantic signal*:
docked nodes are visually stacked and signal that they form a single logical
unit — e.g. "load slices, keep only long ones, sort by duration" is one block.

PREFER docking (nextId) when:
- The edge is a simple linear chain — one output feeds one input, nothing else
  branches off or fans in.
- The nodes are semantically part of the same pipeline stage (e.g. a from +
  a chain of filters/sorts/limits all operating on the same dataset).

USE connections when:
- The data fans out (one node feeds multiple consumers).
- The target has multiple inputs (join left/right, union, interval_intersect).
- The edge crosses pipeline stages — e.g. a subquery result feeding a join as
  the right-hand side is conceptually separate and should use a connection.

Example — "from slice → filter → sort" is one logical unit: fully dock it.
A "from thread" node feeding a join's right side should use a connection.

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
    "filterExpression": "",          // raw SQL expression (alternative to conditions)
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
  Use filterExpression for a freeform SQL WHERE clause instead of conditions.

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

### "join" — LEFT JOIN two inputs
  config: {
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
  Wire via connections: left→toPort=0, right→toPort=1.
  isValid: leftColumn and rightColumn must be non-empty.
  If right is disconnected, emits passthrough SELECT * FROM left.

  IMPORTANT — join has STATIC ports named "left" and "right". Never set an
  "inputs" array on a join node. The inputs array is only for variable-input
  nodes (union, interval_intersect, chart). Setting inputs on join overrides
  the manifest port names, breaking getInputRef('left') / getInputRef('right')
  so the node emits nothing.

### "union" — UNION / UNION ALL of N inputs
  config: { "distinct": false }      // true = UNION, false = UNION ALL
  Variable inputs — set inputs array:
    "inputs": [
      {"name":"input_1","content":"Input 1","direction":"left"},
      {"name":"input_2","content":"Input 2","direction":"left"}
    ]
  Wire each source via connections with toPort matching the port index.

### "interval_intersect" — intersect time intervals (_interval_intersect!())
  config: {
    "partitionColumns": ["upid"],    // columns to partition by (can be [])
    "filterNegativeDur": true
  }
  Variable inputs — set inputs array (same pattern as union).
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

### "chart" — bar chart dashboard (variable inputs)
  config: {
    "charts": [
      { "xCol": "name", "yCol": "" },    // yCol="" → COUNT(*)
      { "xCol": "name", "yCol": "dur" }  // yCol set → SUM(yCol)
    ]
  }
  Variable inputs (same pattern as union). Shows charts in details panel.

## Fan-out: one node as input to multiple consumers

A single node can feed multiple downstream nodes simultaneously. Use
connections to wire it to each consumer. Do NOT create duplicate "from"
nodes for the same table just because you need the data in two places.

Example — slice feeds both a groupby (for aggregation) and a join (as left):
  n1 (from slice) → connection → n3 join left (toPort=0)
  n1 (from slice) → connection → n2 groupby (toPort=0)
  n2 groupby      → connection → n3 join right (toPort=1)

n1 has two outgoing connections — that is fine. No nextId needed here since
n1 docks to neither; both relationships are expressed as connections.

## Layout conventions

Ports and flow direction:
- All node outputs are on the RIGHT side (fromPort).
- All node inputs are on the LEFT side (toPort).
- Connections therefore always flow LEFT → RIGHT across the canvas.
- Docked nodes (nextId) stack TOP → BOTTOM at the same x position; visually
  they form a vertical tower, but data still enters from the left and exits
  to the right of the whole tower.

Implication for positioning:
- Pipeline stages that are connected via connections should be at increasing
  x values (left-to-right progression).
- Nodes within the same docked chain share the same x; only y increases.

## Positioning guidelines
- Source nodes (from, time_range): x=150, stagger y by 180
- Linear chain (docked via nextId): same x as parent, y += ~180 per node
- Multi-input collector (join, union, ii): x=480, y centered between inputs
- Further downstream stages: x=800, x=1100, etc.

## Examples

### Slice pipeline: from → filter → sort (docked chain)
{
  "nodes": [
    ["n1", {"type":"from","id":"n1","x":150,"y":100,"config":{"table":"slice"},"nextId":"n2"}],
    ["n2", {"type":"filter","id":"n2","x":150,"y":280,"config":{"filterExpression":"","conditions":[{"column":"dur","op":">","value":"1000000"}],"conjunction":"AND"},"nextId":"n3"}],
    ["n3", {"type":"sort","id":"n3","x":150,"y":460,"config":{"sortColumn":"","sortOrder":"ASC","conditions":[{"column":"dur","order":"DESC"}]}}]
  ],
  "connections": [],
  "labels": []
}

### Interval intersect: sched_slice ∩ slice partitioned by utid
{
  "nodes": [
    ["n1", {"type":"from","id":"n1","x":150,"y":100,"config":{"table":"sched_slice"}}],
    ["n2", {"type":"from","id":"n2","x":150,"y":280,"config":{"table":"slice"}}],
    ["n3", {"type":"interval_intersect","id":"n3","x":480,"y":190,
            "config":{"partitionColumns":["utid"],"filterNegativeDur":true},
            "inputs":[{"name":"input_1","content":"Input 1","direction":"left"},
                      {"name":"input_2","content":"Input 2","direction":"left"}]}]
  ],
  "connections": [
    {"fromNode":"n1","fromPort":0,"toNode":"n3","toPort":0},
    {"fromNode":"n2","fromPort":0,"toNode":"n3","toPort":1}
  ],
  "labels": []
}`,
      input_schema: {
        type: 'object',
        properties: {
          graph: {
            type: 'object',
            description:
              'The graph JSON object with nodes ([id, NodeData][] pairs), connections, and labels',
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
