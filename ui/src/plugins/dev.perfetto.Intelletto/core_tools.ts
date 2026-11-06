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
import {sqliteString} from '../../base/string_utils';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import {runQueryForModel} from './query';
import type {ToolRegistry} from './tools';

/**
 * Register the core tools (run_query, find_sql_objects, describe_sql_object,
 * show_query, navigate) the assistant always has, backed by the open trace.
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
      'capped and large raw result sets are truncated. Use find_sql_objects / ' +
      "describe_sql_object first if you're unsure which tables/columns " +
      'exist. Stdlib tables, functions and macros need an `INCLUDE PERFETTO ' +
      'MODULE <name>;` first, run as a separate query - those tools tell you ' +
      'which module.',
    shape: {
      sql: z.string().describe('The PerfettoSQL query to execute.'),
    },
    callback: async ({sql}) => runQueryForModel(engine, sql),
  });

  reg.registerTool({
    name: 'find_sql_objects',
    description:
      'Find the tables, views, functions, table functions and macros you can ' +
      'use in PerfettoSQL, including those in standard library modules that ' +
      'have not been included yet. Pass `search` keywords (e.g. "startup", ' +
      '"cpu frequency") to get matching objects, each with a one-line ' +
      'description and the INCLUDE statement it needs. Omit `search` to list ' +
      'the tables/views that are always available. Then call ' +
      'describe_sql_object for the columns/args of the one you want.',
    shape: {
      search: z
        .string()
        .optional()
        .describe(
          'Space-separated keywords; every keyword must match. Omit to ' +
            'list the always-available tables.',
        ),
    },
    callback: async ({search}) => {
      const keywords = (search ?? '').split(/\s+/).filter((w) => w !== '');
      return keywords.length === 0
        ? listAvailableTables(engine)
        : findSqlObjects(engine, keywords);
    },
  });

  reg.registerTool({
    name: 'describe_sql_object',
    description:
      'Get the full docs for one PerfettoSQL table, view, function, table ' +
      'function or macro: its columns or arguments, return type, and the ' +
      'INCLUDE statement needed to use it. Also accepts a stdlib module name ' +
      '(e.g. "android.startup.startups") to list what it contains. Works for ' +
      'tables created at runtime too (returns their live columns). Use ' +
      'find_sql_objects first if you do not know the exact name.',
    shape: {
      name: z
        .string()
        .describe(
          'Exact name, e.g. "android_startups", ' +
            '"android.startup.startups.android_startups" or a module name.',
        ),
    },
    callback: async ({name}) => describeSqlObject(engine, name.trim()),
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
}

// Max objects returned by a keyword search or module listing.
const MAX_RESULTS = 30;

// Filters sqlite_schema down to user-visible tables/views.
const VISIBLE_TABLES = `
  type IN ('table', 'view')
  AND name NOT LIKE 'sqlite_%'
  AND name NOT LIKE '\\_%' ESCAPE '\\'
`;

// The INCLUDE line needed to use objects from `module`, or undefined for
// prelude modules, which are always available.
function includeFor(module: string): string | undefined {
  if (module.startsWith('prelude')) return undefined;
  return `INCLUDE PERFETTO MODULE ${module};`;
}

// One-line summary of a stdlib object, for search results/module listings.
function objectLine(
  name: string,
  type: string,
  module: string,
  desc: string,
): string {
  const include = includeFor(module);
  return (
    `- ${name} (${type})` +
    (include === undefined ? '' : ` [${include}]`) +
    (desc === '' ? '' : `: ${desc}`)
  );
}

// No search: the prelude tables/views (always available), plus any tables
// created at runtime that aren't part of the stdlib.
async function listAvailableTables(engine: Engine): Promise<string> {
  const prelude = await engine.query(`
    SELECT name FROM __intrinsic_stdlib_objects
    WHERE exposed = 1
      AND module GLOB 'prelude*'
      AND object_type IN ('TABLE', 'VIEW')
    ORDER BY name
  `);
  const preludeNames: string[] = [];
  for (const it = prelude.iter({name: STR}); it.valid(); it.next()) {
    preludeNames.push(it.name);
  }
  const runtime = await engine.query(`
    SELECT name FROM sqlite_schema
    WHERE ${VISIBLE_TABLES}
      AND name NOT IN (SELECT name FROM __intrinsic_stdlib_objects)
    ORDER BY name
  `);
  const runtimeNames: string[] = [];
  for (const it = runtime.iter({name: STR}); it.valid(); it.next()) {
    runtimeNames.push(it.name);
  }
  let out = `Always available (no INCLUDE needed): ${preludeNames.join(', ')}`;
  if (runtimeNames.length > 0) {
    out += `\n\nCreated at runtime: ${runtimeNames.join(', ')}`;
  }
  return (
    out +
    '\n\nThe standard library has many more tables, functions and macros in ' +
    'modules: call find_sql_objects with `search` keywords to find them.'
  );
}

// Keyword search over the stdlib catalog (every keyword must appear in the
// object's summary, which covers its name, module, description, args and
// columns) plus the names of runtime-created tables.
async function findSqlObjects(
  engine: Engine,
  keywords: readonly string[],
): Promise<string> {
  const matches = (col: string) =>
    keywords
      .map((w) => `instr(lower(${col}), lower(${sqliteString(w)})) > 0`)
      .join(' AND ');
  const result = await engine.query(`
    SELECT name, object_type AS type, module, short_description AS description
    FROM (
      SELECT qualified_name AS name, object_type, module, short_description,
        object_type NOT IN ('TABLE', 'VIEW') AS rank
      FROM __intrinsic_stdlib_objects
      WHERE exposed = 1 AND object_type != 'MODULE' AND ${matches('summary')}
      UNION ALL
      SELECT name, upper(type), '', '', 0
      FROM sqlite_schema
      WHERE ${VISIBLE_TABLES}
        AND name NOT IN (SELECT name FROM __intrinsic_stdlib_objects)
        AND ${matches('name')}
    )
    ORDER BY rank, length(name)
    LIMIT ${MAX_RESULTS + 1}
  `);
  const lines: string[] = [];
  const it = result.iter({name: STR, type: STR, module: STR, description: STR});
  for (; it.valid() && lines.length < MAX_RESULTS; it.next()) {
    lines.push(objectLine(it.name, it.type, it.module, it.description));
  }
  if (lines.length === 0) {
    return (
      `No SQL objects match "${keywords.join(' ')}". Try fewer or broader ` +
      'keywords.'
    );
  }
  let out = lines.join('\n');
  if (it.valid()) {
    out += `\n... more than ${MAX_RESULTS} matches; add keywords to narrow.`;
  }
  return out + '\n\nCall describe_sql_object for the full docs of one.';
}

// Full docs for a stdlib object, the contents of a stdlib module, or the live
// columns of a runtime-created table.
async function describeSqlObject(
  engine: Engine,
  name: string,
): Promise<string> {
  const quoted = sqliteString(name);

  const objects = await engine.query(`
    SELECT module, summary
    FROM __intrinsic_stdlib_objects
    WHERE exposed = 1
      AND object_type != 'MODULE'
      AND (name = ${quoted} OR qualified_name = ${quoted})
    ORDER BY module
  `);
  const docs: string[] = [];
  for (
    const it = objects.iter({module: STR, summary: STR});
    it.valid();
    it.next()
  ) {
    const include = includeFor(it.module);
    docs.push(
      it.summary +
        (include === undefined
          ? '\nUsage: always available, no INCLUDE needed.'
          : `\nUsage: run \`${include}\` first (as a separate query).`),
    );
  }
  if (docs.length > 0) {
    return docs.join('\n\n');
  }

  const contents = await engine.query(`
    SELECT name, object_type AS type, module, short_description AS description
    FROM __intrinsic_stdlib_objects
    WHERE exposed = 1 AND object_type != 'MODULE' AND module = ${quoted}
    ORDER BY object_type NOT IN ('TABLE', 'VIEW'), name
    LIMIT ${MAX_RESULTS + 1}
  `);
  const lines: string[] = [];
  const it = contents.iter({
    name: STR,
    type: STR,
    module: STR,
    description: STR,
  });
  for (; it.valid() && lines.length < MAX_RESULTS; it.next()) {
    lines.push(objectLine(it.name, it.type, it.module, it.description));
  }
  if (lines.length > 0) {
    let out = `Module ${name} contains:\n${lines.join('\n')}`;
    if (it.valid()) out += `\n... more than ${MAX_RESULTS} objects.`;
    return out + '\n\nCall describe_sql_object on one for its full docs.';
  }

  const cols = await engine.query(
    `SELECT name, type FROM pragma_table_info(${quoted})`,
  );
  if (cols.numRows() > 0) {
    const colLines: string[] = [];
    for (const c = cols.iter({name: STR, type: STR}); c.valid(); c.next()) {
      colLines.push(`- ${c.name}${c.type === '' ? '' : ` (${c.type})`}`);
    }
    return `Table ${name} (created at runtime) columns:\n${colLines.join('\n')}`;
  }

  return (
    `"${name}" is not a known table, view, function, macro or module. Use ` +
    'find_sql_objects to find the right name.'
  );
}
