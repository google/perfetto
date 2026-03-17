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

const MAX_QUERY_ROWS = 5000;

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
