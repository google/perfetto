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

// Schema-aware PerfettoSQL autocomplete shared by the main UI and BigTrace
// editors. Sources its suggestions from a live SqlSchema (so it improves as the
// schema loads), with these context modes:
//   0. `INCLUDE PERFETTO MODULE <partial>` -> stdlib module names
//   1. `table.` / `alias.`                 -> that table's columns
//   2. right after FROM / JOIN             -> table names ranked first
//   3. anywhere else                       -> keywords + functions + tables +
//      columns of the tables already referenced in this query's FROM/JOIN.
//
// Intentionally catalog-driven (not parser-driven): the catalog scopes columns
// to referenced tables and carries types/doc tooltips/`name(arg) -> RET`
// signatures that syntaqlite's bare {label, kind} lacks (see engine.ts).

import {perfettoSqlTypeToString} from '../../trace_processor/perfetto_sql_type';
import type {
  CompletionContextLike,
  CompletionOption,
  CompletionResultLike,
  EditorCompletionSource,
} from '../../widgets/editor';
import {
  flattenCallables,
  type SqlSchema,
  type SqlSchemaArg,
  type SqlSchemaTable,
} from './schema';

// Common PerfettoSQL keywords (the grammar highlights these but offers no
// completion).
const KEYWORDS = [
  'SELECT',
  'FROM',
  'WHERE',
  'GROUP BY',
  'ORDER BY',
  'LIMIT',
  'OFFSET',
  'JOIN',
  'LEFT JOIN',
  'INNER JOIN',
  'ON',
  'USING',
  'AS',
  'AND',
  'OR',
  'NOT',
  'IN',
  'LIKE',
  'GLOB',
  'BETWEEN',
  'IS NULL',
  'IS NOT NULL',
  'DISTINCT',
  'WITH',
  'UNION',
  'UNION ALL',
  'HAVING',
  'CASE',
  'WHEN',
  'THEN',
  'ELSE',
  'END',
  'DESC',
  'ASC',
  'INCLUDE PERFETTO MODULE',
];

// SQL built-in functions (the stdlib functions come from the schema).
const FUNCTIONS = [
  'COUNT',
  'SUM',
  'AVG',
  'MIN',
  'MAX',
  'CAST',
  'IFNULL',
  'COALESCE',
  'ROW_NUMBER',
  'RANK',
  'LAG',
  'LEAD',
  'GROUP_CONCAT',
];

// Words that can't be a table alias (so `from slice where ...` doesn't treat
// `where` as the alias of `slice`).
const NON_ALIAS = new Set([
  'where',
  'on',
  'using',
  'group',
  'order',
  'limit',
  'join',
  'left',
  'inner',
  'cross',
  'union',
  'having',
  'as',
]);

// Blanks out SQL string literals and comments so a `from`/`join` (or `include`)
// inside them isn't mistaken for real code. A small hand scanner — regexes
// can't get the interleaving of strings/comments right.
export function stripSqlNoise(sql: string): string {
  let out = '';
  let i = 0;
  const n = sql.length;
  while (i < n) {
    const c = sql[i];
    const c2 = sql[i + 1];
    if (c === '-' && c2 === '-') {
      while (i < n && sql[i] !== '\n') i++;
      out += ' ';
    } else if (c === '/' && c2 === '*') {
      i += 2;
      while (i < n && !(sql[i] === '*' && sql[i + 1] === '/')) i++;
      i += 2;
      out += ' ';
    } else if (c === "'") {
      i++;
      while (i < n && sql[i] !== "'") {
        if (sql[i] === '\\') i++;
        i++;
      }
      i++;
      out += "''";
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

// Names bound by a `WITH … AS (…)` clause — these are CTEs, not stdlib tables.
function collectCteNames(cleanSql: string): Set<string> {
  const names = new Set<string>();
  const re =
    /(?:\bwith\b|,)\s*(?:recursive\s+)?([a-z_]\w*)(?:\s*\([^)]*\))?\s+as\s*\(/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(cleanSql)) !== null) names.add(m[1].toLowerCase());
  return names;
}

// Scan the query for the tables (and aliases) referenced in FROM/JOIN clauses,
// so column suggestions + missing-include detection are scoped to what the
// query actually uses. Ignores FROM/JOIN inside strings/comments and CTE names.
export function scanReferencedTables(doc: string): {
  tables: Set<string>;
  aliases: Map<string, string>;
} {
  const clean = stripSqlNoise(doc);
  const ctes = collectCteNames(clean);
  const tables = new Set<string>();
  const aliases = new Map<string, string>();
  const re =
    /(?:\bfrom|\bjoin)\s+([a-z_][\w]*)(?:\s+(?:as\s+)?([a-z_][\w]*))?/gi;
  let m: RegExpExecArray | null;
  while ((m = re.exec(clean)) !== null) {
    const table = m[1];
    if (ctes.has(table.toLowerCase())) continue; // a CTE, not a stdlib table
    tables.add(table.toLowerCase());
    aliases.set(table.toLowerCase(), table);
    const alias = m[2];
    if (alias && !NON_ALIAS.has(alias.toLowerCase())) {
      aliases.set(alias.toLowerCase(), table);
    }
  }
  return {tables, aliases};
}

function columnOptions(table: SqlSchemaTable): CompletionOption[] {
  return table.columns.map((c) => ({
    label: c.name,
    type: 'property',
    detail: c.type ? perfettoSqlTypeToString(c.type) : table.name,
    info: c.description,
    boost: 20,
  }));
}

function tableOption(t: SqlSchemaTable, boost: number): CompletionOption {
  return {
    label: t.name,
    type: 'class',
    detail: 'table',
    info: t.description,
    boost,
  };
}

// Is the cursor immediately after a FROM/JOIN keyword (table position)?
function inTablePosition(textBefore: string): boolean {
  return /(?:\bfrom|\bjoin)\s+[\w]*$/i.test(textBefore);
}

// Renders a callable's signature + first line of docs for the completion popup.
function signatureInfo(
  name: string,
  args: ReadonlyArray<SqlSchemaArg>,
  ret: string,
  description: string,
): string {
  const sig = `${name}(${args.map((a) => `${a.name} ${a.type}`).join(', ')}) → ${ret}`;
  const firstLine = description.split('\n')[0].trim();
  return firstLine ? `${sig}\n${firstLine}` : sig;
}

function buildResult(
  ctx: CompletionContextLike,
  schema: SqlSchema | undefined,
): CompletionResultLike | null {
  const textBefore = ctx.state.doc.toString().slice(0, ctx.pos);

  // Mode 0: `INCLUDE PERFETTO MODULE <partial>` → stdlib module names.
  const incMatch = /include\s+perfetto\s+module\s+([\w.]*)$/i.exec(textBefore);
  if (incMatch) {
    if (!schema) return null;
    const partial = incMatch[1];
    return {
      from: ctx.pos - partial.length,
      options: schema
        .listModules()
        .map((mod) => mod.includeKey)
        .filter((k) => !k.startsWith('prelude'))
        .map((k) => ({
          label: k,
          type: 'namespace',
          detail: 'module',
          boost: 30,
        })),
      validFor: /[\w.]*/,
    };
  }

  // Mode 1: `table.` or `alias.` → that table's columns.
  const dotted = ctx.matchBefore(/[A-Za-z_][\w]*\.[\w]*/);
  if (dotted) {
    const dot = dotted.text.indexOf('.');
    const lhs = dotted.text.slice(0, dot).toLowerCase();
    if (schema) {
      const {aliases} = scanReferencedTables(ctx.state.doc.toString());
      const tableName = aliases.get(lhs) ?? lhs;
      const table = schema.getTable(tableName);
      if (table) {
        return {
          from: dotted.from + dot + 1,
          options: columnOptions(table),
          validFor: /\w*/,
        };
      }
    }
    return null;
  }

  const word = ctx.matchBefore(/[\w]+/);
  if (!word && !ctx.explicit) return null;
  const from = word ? word.from : ctx.pos;
  const tablePos = inTablePosition(textBefore);

  const options: CompletionOption[] = [];

  // Keywords + SQL built-in functions (low priority).
  for (const k of KEYWORDS) {
    options.push({label: k, type: 'keyword', boost: -20});
  }
  for (const f of FUNCTIONS) {
    options.push({label: f, type: 'function', boost: -18});
  }

  if (schema) {
    const {functions, tableFunctions, macros} = flattenCallables(schema);
    // Stdlib scalar functions + macros — with their signatures + docs.
    for (const fn of functions) {
      options.push({
        label: fn.name,
        type: 'function',
        detail: fn.returnType,
        info: signatureInfo(fn.name, fn.args, fn.returnType, fn.description),
        boost: -8,
      });
    }
    for (const mac of macros) {
      options.push({
        label: mac.name,
        type: 'function',
        detail: 'macro',
        info: signatureInfo(
          mac.name,
          mac.args,
          mac.returnType,
          mac.description,
        ),
        boost: -10,
      });
    }
    // Tables + table-valued functions — ranked first in a table position.
    for (const t of schema.listTables()) {
      options.push(tableOption(t, tablePos ? 60 : 0));
    }
    for (const tf of tableFunctions) {
      options.push({
        label: tf.name,
        type: 'class',
        detail: 'table function',
        info: signatureInfo(tf.name, tf.args, 'TABLE', tf.description),
        boost: tablePos ? 55 : -8,
      });
    }
    // Columns of the tables this query already references.
    if (!tablePos) {
      const {tables} = scanReferencedTables(textBefore);
      for (const name of tables) {
        const table = schema.getTable(name);
        if (table) options.push(...columnOptions(table));
      }
    }
  }

  return {from, options, validFor: /[\w]*/};
}

// Builds an EditorCompletionSource bound to a (late-resolved) schema. The schema
// is read fresh on every keystroke so completion improves as it loads.
export function createPerfettoSqlCompletionSource(
  getSchema: () => SqlSchema | undefined,
): EditorCompletionSource {
  return (ctx) => buildResult(ctx, getSchema());
}

// ---------------------------------------------------------------------------
// INCLUDE PERFETTO MODULE tracking — which modules a query has already
// included. Used by the diagnostics source to turn an unknown stdlib table into
// an "add this INCLUDE" hint.
// ---------------------------------------------------------------------------

export function alreadyIncluded(
  key: string,
  included: ReadonlySet<string>,
): boolean {
  const k = key.toLowerCase();
  if (included.has(k)) return true;
  // A wildcard include (`android.*`) covers everything under that prefix.
  for (const inc of included) {
    if (inc.endsWith('*') && k.startsWith(inc.slice(0, -1))) return true;
  }
  return false;
}

// The set of modules a query already includes (ignoring strings/comments).
export function includedModules(query: string): Set<string> {
  const included = new Set<string>();
  const re = /include\s+perfetto\s+module\s+([\w.]+\*?)/gi;
  let m: RegExpExecArray | null;
  const clean = stripSqlNoise(query);
  while ((m = re.exec(clean)) !== null) included.add(m[1].toLowerCase());
  return included;
}
