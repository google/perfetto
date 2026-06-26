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

// Tracks tables/views the user created this session via CREATE PERFETTO
// TABLE/VIEW so the editor's diagnostics don't flag a later reference to them as
// "unknown table" (the stdlib SqlModules schema only knows stdlib objects).
//
// After a query runs, we parse its CREATE targets out of the SQL text and ask
// the engine (pragma_table_info) for each one's columns, caching the result.
// Best-effort and fully defensive: any failure just leaves that table out, so
// the worst case is the pre-existing soft "unknown table" underline — never a
// thrown error or a wrong result.

import type {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';
import {stripSqlNoise} from '../../components/sql_intelligence/completion';
import type {
  SqlSchema,
  SqlSchemaTable,
} from '../../components/sql_intelligence/schema';

// CREATE [OR REPLACE] [PERFETTO] TABLE|VIEW [IF NOT EXISTS] <name>
const CREATE_RE =
  /\bcreate\s+(?:or\s+replace\s+)?(?:perfetto\s+)?(?:table|view)\s+(?:if\s+not\s+exists\s+)?([a-z_][\w]*)/gi;

export class LiveSchemaCache {
  private readonly tables = new Map<string, SqlSchemaTable>();
  // A stable snapshot, rebuilt only when `tables` changes, so callers can use
  // its identity to memoize derived schemas (see QueryPage.getEditorIntelligence
  // + flattenCallables). Returning a fresh array each call would defeat that.
  private snapshot: SqlSchemaTable[] = [];
  private dirty = false;

  getTables(): SqlSchemaTable[] {
    if (this.dirty) {
      this.snapshot = Array.from(this.tables.values());
      this.dirty = false;
    }
    return this.snapshot;
  }

  // Parse CREATE targets from an executed query and introspect their columns.
  // Call after the query has run (so the objects exist).
  async recordFromExecutedSql(sql: string, engine: Engine): Promise<void> {
    const clean = stripSqlNoise(sql);
    const names = new Set<string>();
    let m: RegExpExecArray | null;
    CREATE_RE.lastIndex = 0;
    while ((m = CREATE_RE.exec(clean)) !== null) names.add(m[1]);

    for (const name of names) {
      try {
        // Identifiers come from our own regex ([A-Za-z_]\w*), so they're safe to
        // interpolate into the pragma's string argument.
        const res = await engine.query(
          `SELECT name FROM pragma_table_info('${name}')`,
        );
        const columns: Array<{name: string}> = [];
        for (const it = res.iter({name: STR}); it.valid(); it.next()) {
          columns.push({name: it.name});
        }
        if (columns.length > 0) {
          this.tables.set(name.toLowerCase(), {
            name,
            description: 'Created in this session',
            columns,
          });
          this.dirty = true;
        }
      } catch {
        // Best-effort: ignore objects we can't introspect.
      }
    }
  }
}

// Layers session-created tables over a base schema (the stdlib SqlModules),
// producing a combined SqlSchema. Base entries win on name collision.
export function withExtraTables(
  base: SqlSchema,
  extra: SqlSchemaTable[],
): SqlSchema {
  if (extra.length === 0) return base;
  const extraByLower = new Map(extra.map((t) => [t.name.toLowerCase(), t]));
  return {
    listTables: () => [...base.listTables(), ...extra],
    listTablesNames: () => [
      ...base.listTablesNames(),
      ...extra.map((t) => t.name),
    ],
    getTable: (n) => base.getTable(n) ?? extraByLower.get(n.toLowerCase()),
    listModules: () => base.listModules(),
  };
}
