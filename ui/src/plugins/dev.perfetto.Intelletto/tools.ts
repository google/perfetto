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
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';

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
    createGetQueryBuilderGraphTool(trace),
    createSetQueryBuilderGraphTool(trace),
    createSelectQueryBuilderNodeTool(trace),
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

function createGetQueryBuilderGraphTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'get_query_builder_graph',
      description: `Get the current graph state from the Data Explorer's query builder.

Returns the serialized JSON of the active tab's graph, including all nodes,
connections, and layout information. Returns null if no graph is loaded.

Use this to understand the user's current analysis pipeline before modifying it.`,
      input_schema: {
        type: 'object',
        properties: {},
      },
    },
    async handle() {
      try {
        const plugin = trace.plugins.getPlugin(DataExplorerPlugin);
        const json = plugin.getActiveGraphJson();
        if (json === undefined) {
          return JSON.stringify({status: 'empty', graph: null});
        }
        return JSON.stringify({status: 'ok', graph: JSON.parse(json)});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createSetQueryBuilderGraphTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'set_query_builder_graph',
      description: `Set or replace the Data Explorer's query builder graph.

This replaces the entire active tab's graph. Use get_query_builder_graph first
if you want to modify rather than replace.

## Graph JSON Structure

{
  "nodes": [...],           // Array of serialized nodes
  "rootNodeIds": ["n1"],    // IDs of top-level (root) nodes
  "nodeLayouts": {"n1": {"x": 100, "y": 100}, ...},  // Canvas positions
  "labels": []              // Text labels on canvas (usually empty)
}

## Node Structure

Each node in the "nodes" array:
{
  "nodeId": "n1",           // Unique string ID
  "type": "table",          // NodeType (see below)
  "state": {...},           // Type-specific state (see below)
  "nextNodes": ["n2"]       // IDs of docked child nodes (linear chains)
}

## Connecting Nodes

**Docking (nextNodes):** For linear chains (source -> filter -> sort -> limit),
set the parent's nextNodes to [childId]. The child must have primaryInputId
set to the parent's ID. Docked nodes still need nodeLayouts entries.

**Multi-source connections:** For join, union, interval_intersect, etc., use
the node-specific state fields (leftNodeId/rightNodeId, unionNodes, etc.).
These nodes are separate root nodes with their own positions.

## Node Types and State Fields

### Source Nodes (no primaryInputId)

**"table"** - Query data from any table.
  state: { sqlTable: "slice" }

**"simple_slices"** - Pre-configured slice explorer.
  state: { }

**"sql_source"** - Custom SQL query as source.
  state: { sql: "SELECT * FROM slice WHERE dur > 1000" }

### Modification Nodes (require primaryInputId)

**"filter"** - Filter rows by conditions.
  state: {
    primaryInputId: "n1",
    filters: [{ column: "dur", op: "greater_than", value: 1000000 }],
    filterOperator: "AND",
    filterMode: "structured"
  }
  Filter ops: "equals", "not_equals", "greater_than", "greater_than_or_equals",
    "less_than", "less_than_or_equals", "like", "not_like", "glob",
    "is_null", "is_not_null".

**"sort"** - Sort rows by columns.
  state: {
    primaryInputId: "n1",
    sortCriteria: [{ colName: "dur", direction: "DESC" }]
  }

**"modify_columns"** - Select, rename, reorder columns.
  state: {
    primaryInputId: "n1",
    selectedColumns: [
      { name: "ts", checked: true },
      { name: "dur", checked: true, alias: "duration" },
      { name: "track_id", checked: false }
    ]
  }

**"aggregation"** - GROUP BY with aggregate functions.
  state: {
    primaryInputId: "n1",
    groupByColumns: [{ name: "name", checked: true }],
    aggregations: [
      { column: { name: "dur" }, aggregationOp: "SUM", newColumnName: "total_dur" },
      { column: { name: "dur" }, aggregationOp: "COUNT", newColumnName: "count" }
    ]
  }
  Aggregation ops: "SUM", "COUNT", "AVG", "MIN", "MAX", "PERCENTILE".

**"limit_and_offset"** - Limit row count.
  state: { primaryInputId: "n1", limit: 100, offset: 0 }

**"add_columns"** - Add columns from another node via LEFT JOIN.
  state: {
    primaryInputId: "n1",
    secondaryInputNodeId: "n2",
    selectedColumns: ["name"],
    leftColumn: "utid",
    rightColumn: "utid"
  }

**"counter_to_intervals"** - Convert counter data to intervals (adds dur).
  state: { primaryInputId: "n1" }

**"visualisation"** - Visualize data with charts.
  state: { primaryInputId: "n1" }

### Multi-Source Nodes (no primaryInputId, own connection fields)

**"join"** - Join two inputs.
  state: {
    leftNodeId: "n1",
    rightNodeId: "n2",
    leftQueryAlias: "left",
    rightQueryAlias: "right",
    conditionType: "equality",
    joinType: "INNER",
    leftColumn: "utid",
    rightColumn: "utid"
  }
  joinType: "INNER" | "LEFT". conditionType: "equality" | "freeform".
  For freeform: use sqlExpression instead of leftColumn/rightColumn.

**"union"** - Combine rows from multiple sources.
  state: { unionNodes: ["n1", "n2"], selectedColumns: [] }

**"interval_intersect"** - Intersect time intervals from multiple sources.
  state: { intervalNodes: ["n1", "n2"] }

**"create_slices"** - Create slices from start/end timestamp sources.
  state: { startsNodeId: "n1", endsNodeId: "n2", startsTsColumn: "ts", endsTsColumn: "ts" }

## Example: Table with filter and sort (docked chain)

{
  "nodes": [
    { "nodeId": "n1", "type": "table", "state": { "sqlTable": "slice" }, "nextNodes": ["n2"] },
    { "nodeId": "n2", "type": "filter", "state": {
        "primaryInputId": "n1",
        "filters": [{ "column": "dur", "op": "greater_than", "value": 1000000 }],
        "filterOperator": "AND", "filterMode": "structured"
      }, "nextNodes": ["n3"] },
    { "nodeId": "n3", "type": "sort", "state": {
        "primaryInputId": "n2",
        "sortCriteria": [{ "colName": "dur", "direction": "DESC" }]
      }, "nextNodes": [] }
  ],
  "rootNodeIds": ["n1"],
  "nodeLayouts": { "n1": { "x": 150, "y": 100 }, "n2": { "x": 150, "y": 200 }, "n3": { "x": 150, "y": 300 } },
  "labels": []
}

## Example: Join slice with thread

{
  "nodes": [
    { "nodeId": "n1", "type": "table", "state": { "sqlTable": "slice" }, "nextNodes": [] },
    { "nodeId": "n2", "type": "table", "state": { "sqlTable": "thread" }, "nextNodes": [] },
    { "nodeId": "n3", "type": "join", "state": {
        "leftNodeId": "n1", "rightNodeId": "n2",
        "leftQueryAlias": "left", "rightQueryAlias": "right",
        "conditionType": "equality", "joinType": "INNER",
        "leftColumn": "utid", "rightColumn": "utid", "sqlExpression": ""
      }, "nextNodes": [] }
  ],
  "rootNodeIds": ["n1", "n2", "n3"],
  "nodeLayouts": { "n1": { "x": 100, "y": 100 }, "n2": { "x": 100, "y": 250 }, "n3": { "x": 400, "y": 150 } },
  "labels": []
}`,
      input_schema: {
        type: 'object',
        properties: {
          graph: {
            type: 'object',
            description:
              'The graph JSON object with nodes, rootNodeIds, nodeLayouts, and labels',
          },
        },
        required: ['graph'],
      },
    },
    async handle(input) {
      try {
        const plugin = trace.plugins.getPlugin(DataExplorerPlugin);
        const json = JSON.stringify(input.graph);
        plugin.setActiveGraphJson(json);
        return JSON.stringify({status: 'ok'});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}

function createSelectQueryBuilderNodeTool(trace: Trace): ToolImpl {
  return {
    def: {
      name: 'select_query_builder_node',
      description: `Select a node in the Data Explorer's query builder graph.

Given a node ID, selects that node in the active tab's graph, which shows
its results in the details panel.

Use get_query_builder_graph first to discover available node IDs.`,
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
        const plugin = trace.plugins.getPlugin(DataExplorerPlugin);
        const nodeId = input.node_id as string;
        const found = plugin.selectNode(nodeId);
        if (!found) {
          return JSON.stringify({
            status: 'not_found',
            message: `Node "${nodeId}" not found in the active graph`,
          });
        }
        return JSON.stringify({status: 'ok', node_id: nodeId});
      } catch (e) {
        return JSON.stringify({status: 'error', message: String(e)});
      }
    },
  };
}
