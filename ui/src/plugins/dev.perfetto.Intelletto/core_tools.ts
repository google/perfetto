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

import {z} from 'zod';
import type {Trace} from '../../public/trace';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import {runQueryForModel} from './query';
import type {ToolRegistry} from './tools';
import {guides} from '../../virtual/guides';

/**
 * Register the core tools (run_query, get_schema, get_selection, show_query,
 * navigate) the assistant always has, backed by the open trace.
 */
export function registerCoreTools(reg: ToolRegistry, trace: Trace): void {
  const engine = trace.engine;

  reg.registerTool({
    name: 'run_query',
    description:
      'Run a PerfettoSQL query against the open trace and return the rows ' +
      'as JSON. Call this whenever the user asks about durations, counts, ' +
      'timings, or relationships between trace events. Prefer aggregation ' +
      '(COUNT / GROUP BY / LIMIT) over pulling raw rows - results are row-' +
      'capped and large raw result sets are truncated. Use get_schema first ' +
      "if you're unsure which tables/columns exist. Some stdlib tables need " +
      'an `INCLUDE PERFETTO MODULE <name>;` first, run as a separate query.',
    shape: {
      sql: z.string().describe('The PerfettoSQL query to execute.'),
    },
    callback: async ({sql}) => runQueryForModel(engine, sql),
  });

  reg.registerTool({
    name: 'get_schema',
    description:
      'List the trace tables/views (no argument) or the columns of one table ' +
      '(pass `table`). Use this to write valid SQL without guessing schema. ' +
      'Internal sqlite_* and _-prefixed tables are omitted from the table ' +
      'list.',
    shape: {
      table: z
        .string()
        .optional()
        .describe("If set, return this table's columns instead of the list."),
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

  reg.registerTool({
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

  reg.registerTool({
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

  reg.registerTool({
    name: 'read_guide',
    description:
      'Read a Perfetto trace-analysis guide: expert instructions, guided ' +
      'workflows, references, and queries (memory leak debugging, GPU ' +
      'performance analysis, etc.). The PERFETTO TRACE ANALYSIS GUIDES ' +
      'section of the system prompt lists the paths to pass here and says ' +
      'which one applies; guides reference further files by path as you go.',
    shape: {
      path: z
        .string()
        .describe(
          'Path of the guide to read, relative to the guide tree root, ' +
            'e.g. "workflows/android_memory/heap_dump.md".',
        ),
    },
    callback: async ({path}) => {
      let normalized = path;
      if (normalized.startsWith('$SKILL_ROOT/')) {
        normalized = normalized.slice('$SKILL_ROOT/'.length);
      } else if (normalized.startsWith('/')) {
        normalized = normalized.slice(1);
      }

      const content = guides[normalized];
      if (content === undefined) {
        return (
          `Error: guide "${path}" not found. Available guides:\n` +
          Object.keys(guides)
            .map((k) => `- ${k}`)
            .join('\n')
        );
      }
      return content;
    },
  });
}
