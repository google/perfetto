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

// The core tool surface the assistant uses to inspect and drive the trace UI.
// Read tools (run_query, get_schema, get_selection) return data; mutating tools
// (select_event, show_timeline, show_query, navigate) just ack. All operate on
// the already-open trace and nothing persists outside the session, so there is
// no per-action consent gate (see the RFC's "no consent model" note).

import {z} from 'zod';
import type {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import {runQueryForModel} from './query';
import type {ToolRegistry} from './tools';

export function registerCoreTools(reg: ToolRegistry, trace: Trace): void {
  const engine = trace.engine;

  reg.register({
    name: 'run_query',
    description:
      'Run a PerfettoSQL query against the open trace and return the rows ' +
      'as JSON. Call this whenever the user asks about durations, counts, ' +
      'timings, or relationships between trace events. Prefer aggregation ' +
      '(COUNT / GROUP BY / LIMIT) over pulling raw rows - results are row-' +
      'capped and large raw result sets are truncated. Use get_schema first ' +
      "if you're unsure which tables/columns exist. Some stdlib tables need " +
      "an `INCLUDE PERFETTO MODULE <name>;` first, run as a separate query.",
    shape: {
      sql: z.string().describe('The PerfettoSQL query to execute.'),
    },
    callback: async ({sql}) => runQueryForModel(engine, sql),
  });

  reg.register({
    name: 'get_schema',
    description:
      'List the trace tables/views (no argument) or the columns of one table ' +
      "(pass `table`). Use this to write valid SQL without guessing schema. " +
      'Internal sqlite_* and _-prefixed tables are omitted from the table ' +
      'list.',
    shape: {
      table: z
        .string()
        .optional()
        .describe('If set, return this table\'s columns instead of the list.'),
    },
    callback: async ({table}) => {
      if (table !== undefined && table !== '') {
        return runQueryForModel(
          engine,
          `SELECT name, type FROM pragma_table_info('${table.replace(/'/g, "''")}')`,
        );
      }
      return runQueryForModel(
        engine,
        `SELECT name, type FROM sqlite_schema
         WHERE type IN ('table', 'view')
           AND name NOT LIKE 'sqlite_%'
           AND name NOT LIKE '\\_%' ESCAPE '\\'
         ORDER BY name`,
      );
    },
  });

  reg.register({
    name: 'get_selection',
    description:
      'Read what the user currently has selected in the UI (a track event, a ' +
      'time-range area selection, a track, or nothing). Use this to resolve ' +
      'what the user means by "this" / "the selected slice".',
    shape: {},
    callback: async () => {
      const sel = trace.selection.selection;
      switch (sel.kind) {
        case 'track_event':
          return JSON.stringify({
            kind: 'track_event',
            trackUri: sel.trackUri,
            eventId: sel.eventId,
            ts: Number(sel.ts),
            dur: sel.dur === undefined ? undefined : Number(sel.dur),
          });
        case 'area':
          return JSON.stringify({
            kind: 'area',
            start: Number(sel.start),
            end: Number(sel.end),
            trackUris: sel.trackUris,
          });
        case 'track':
          return JSON.stringify({kind: 'track', trackUri: sel.trackUri});
        case 'note':
          return JSON.stringify({kind: 'note', id: sel.id});
        case 'empty':
          return JSON.stringify({kind: 'empty'});
      }
    },
  });

  reg.register({
    name: 'select_event',
    description:
      'Select a single trace event by its SQL table and row id (e.g. table ' +
      '"slice", id 1234), scrolling the timeline to it. Use this to point the ' +
      'user at a specific slice/event you found via run_query.',
    mutating: true,
    shape: {
      table: z.string().describe('SQL table the id refers to, e.g. "slice".'),
      id: z.number().describe('Row id within that table.'),
    },
    callback: async ({table, id}) => {
      trace.selection.selectSqlEvent(table, id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });
      return 'OK';
    },
  });

  reg.register({
    name: 'show_timeline',
    description:
      'Pan/zoom the timeline to a time range so the user can see it. ' +
      'Timestamps are trace-processor nanoseconds (bigints sent as strings); ' +
      'query the min/max of the relevant table to get a valid range.',
    mutating: true,
    shape: {
      startTime: z.string().describe('Range start, ns, as a string bigint.'),
      endTime: z.string().describe('Range end, ns, as a string bigint.'),
    },
    callback: async ({startTime, endTime}) => {
      const start = Time.fromRaw(BigInt(startTime));
      const end = Time.fromRaw(BigInt(endTime));
      trace.timeline.panSpanIntoView(start, end, {align: 'zoom'});
      return 'OK';
    },
  });

  reg.register({
    name: 'show_query',
    description:
      'Open a PerfettoSQL query in the Query page results view so the user ' +
      'can see the full table (use this instead of run_query when the user ' +
      'wants to browse results themselves, not have you summarise them).',
    mutating: true,
    shape: {
      sql: z.string().describe('The PerfettoSQL query to display.'),
      title: z.string().describe('A short title for the results tab.'),
    },
    callback: async ({sql, title}) => {
      trace.plugins
        .getPlugin(QueryPagePlugin)
        .addQueryResultsTab({query: sql, title});
      return 'OK';
    },
  });

  reg.register({
    name: 'navigate',
    description:
      'Switch the UI to a top-level page by its route, e.g. "/viewer" for ' +
      'the timeline or "/query" for the SQL page. Use when the user asks to ' +
      'go somewhere.',
    mutating: true,
    shape: {
      page: z.string().describe('Route to navigate to, e.g. "/viewer".'),
    },
    callback: async ({page}) => {
      trace.navigate(page);
      return 'OK';
    },
  });
}
