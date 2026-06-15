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

// Live, parser-grade SQL diagnostics shared by the main UI and BigTrace
// editors. Surfaces unknown_table / unknown_column / function_arity / parse
// errors as inline underlines as the user types — before the query is run.

import type {
  EditorDiagnostic,
  EditorDiagnosticSource,
} from '../../widgets/editor';
import {applySchema, engineDiagnostics, engineHasSchema} from './engine';
import {alreadyIncluded, includedModules} from './completion';
import {flattenCallables, type SqlSchema} from './schema';

// Builds an EditorDiagnosticSource bound to a (late-resolved) schema. Feeds the
// schema to the engine each run (cheap no-op once unchanged), then maps the
// engine's structured diagnostics to the editor's shape:
//   - until a schema has been applied, reference diagnostics (unknown_*) are
//     suppressed (the engine would flag every stdlib table) — only structural
//     parse errors show, so there are never false positives on valid SQL;
//   - unknown_function is suppressed for callables the catalog knows (the
//     dialect may not bake all stdlib functions in);
//   - an unknown stdlib table's hint is upgraded to the exact INCLUDE to add.
export function createPerfettoSqlDiagnosticsSource(
  getSchema: () => SqlSchema | undefined,
): EditorDiagnosticSource {
  return (text) => {
    const schema = getSchema();
    applySchema(schema);
    const diags = engineDiagnostics(text);
    if (!diags) return [];
    const hasSchema = engineHasSchema();
    const included = schema ? includedModules(text) : new Set<string>();
    // Flatten callables once per run (not once per diagnostic). flattenCallables
    // is memoized per schema instance, so this is also O(1) across keystrokes as
    // long as the schema object is referentially stable.
    const flat = schema ? flattenCallables(schema) : undefined;
    const isKnownCallable = (name: string): boolean => {
      if (!flat) return false;
      const n = name.toLowerCase();
      return (
        flat.functions.some((f) => f.name.toLowerCase() === n) ||
        flat.tableFunctions.some((f) => f.name.toLowerCase() === n) ||
        flat.macros.some((mc) => mc.name.toLowerCase() === n)
      );
    };
    const out: EditorDiagnostic[] = [];
    for (const d of diags) {
      // Read d.detail into a local so its `kind` checks narrow the union below.
      const detail = d.detail;
      const kind = detail?.kind;
      if (
        !hasSchema &&
        (kind === 'unknown_table' ||
          kind === 'unknown_column' ||
          kind === 'unknown_function')
      ) {
        continue;
      }
      if (detail?.kind === 'unknown_function' && isKnownCallable(detail.name)) {
        continue;
      }
      let help = d.help;
      if (detail?.kind === 'unknown_table' && schema) {
        const table = schema.getTable(detail.name);
        if (table?.includeKey && !alreadyIncluded(table.includeKey, included)) {
          help = `Add: INCLUDE PERFETTO MODULE ${table.includeKey};`;
        }
      }
      out.push({
        from: d.from,
        to: d.to,
        severity: d.severity,
        message: d.message,
        help,
      });
    }
    return out;
  };
}
