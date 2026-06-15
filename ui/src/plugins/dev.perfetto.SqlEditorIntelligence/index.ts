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

// Adds schema-aware autocomplete + live, pre-run diagnostics to the
// dev.perfetto.QueryPage editor. This is an OPT-IN plugin: when it isn't
// enabled it doesn't activate, registers nothing, and the query editor behaves
// exactly as it did before (no completion, no diagnostics, no WASM engine).

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import QueryPagePlugin, {
  type EditorIntelligence,
} from '../dev.perfetto.QueryPage';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {
  createPerfettoSqlCompletionSource,
  createPerfettoSqlDiagnosticsSource,
  onSqlEngineReady,
  onSqlSchemaApplied,
  type SqlSchema,
  type SqlSchemaTable,
} from '../../components/sql_intelligence';
import {LiveSchemaCache, withExtraTables} from './live_schema';

export default class SqlEditorIntelligencePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.SqlEditorIntelligence';
  static readonly description =
    'Schema-aware autocomplete and live, pre-run error diagnostics in the ' +
    'PerfettoSQL query editor (powered by the syntaqlite parser).';
  static readonly dependencies = [QueryPagePlugin, SqlModulesPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const sqlModulesPlugin = trace.plugins.getPlugin(SqlModulesPlugin);
    const liveSchema = new LiveSchemaCache();

    // The schema streams in (stdlib catalog + per-trace availability + tables
    // created this session), so it's read fresh on each call. The merged result
    // is memoized by input identity so its own identity stays stable across
    // keystrokes (keeps flattenCallables' memoization + the engine's
    // schema-change detection effective). SqlModules satisfies SqlSchema
    // structurally.
    let merged:
      | {base: SqlSchema; extra: SqlSchemaTable[]; schema: SqlSchema}
      | undefined;
    const getSchema = (): SqlSchema | undefined => {
      const modules = sqlModulesPlugin.getSqlModules();
      if (!modules) return undefined;
      const extra = liveSchema.getTables();
      if (merged?.base !== modules || merged?.extra !== extra) {
        merged = {
          base: modules,
          extra,
          schema: withExtraTables(modules, extra),
        };
      }
      return merged.schema;
    };

    const intel: EditorIntelligence = {
      completions: createPerfettoSqlCompletionSource(getSchema),
      diagnostics: createPerfettoSqlDiagnosticsSource(getSchema),
      onDiagnosticsRefresh: (refresh) => {
        onSqlEngineReady(refresh);
        onSqlSchemaApplied(refresh);
        // Nudge the per-trace schema to load, then refresh once ready.
        sqlModulesPlugin
          .getSqlModules()
          ?.ensureInitialized()
          .then(refresh)
          .catch(() => {});
      },
      recordExecutedSql: (sql) => {
        void liveSchema.recordFromExecutedSql(sql, trace.engine);
      },
    };

    trace.plugins.getPlugin(QueryPagePlugin).setEditorIntelligence(intel);
  }
}
