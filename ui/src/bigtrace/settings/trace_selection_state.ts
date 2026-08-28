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

import {
  SingleFieldStorage,
  parseNullableStringArray,
} from './single_field_storage';
import {linkNameFirst} from './column_order';

// Persisted trace-grid display state. The trace filter, processing order and
// result-metadata columns are per-query (they live on the tab and its saved
// presets); only which columns the grid SHOWS is a lasting display preference.

// Subset of a /trace_metadata_schema column the resolvers below need.
interface SchemaColumn {
  readonly name: string;
  readonly defaultVisible: boolean;
}

// Columns shown in the trace-selection grid; backs the DataGrid's controlled
// `columns`. An explicit array, else null = "use the schema's defaultVisible".
class TraceColumnsState extends SingleFieldStorage<readonly string[] | null> {
  constructor() {
    super('bigtraceTraceColumns', 'chosen', parseNullableStringArray, null);
  }

  // null → defaultVisible columns; else the selection ∩ schema (dropping removed
  // columns). `link` hoisted first.
  effective(schema: ReadonlyArray<SchemaColumn>): string[] {
    const chosen = this.get();
    if (chosen === null) {
      return linkNameFirst(
        schema.filter((c) => c.defaultVisible).map((c) => c.name),
      );
    }
    const known = new Set(schema.map((c) => c.name));
    return linkNameFirst(chosen.filter((c) => known.has(c)));
  }
}
export const traceColumnsState = new TraceColumnsState();

// Resolves a tab's chosen result-metadata columns against the live schema
// (link first): null → defaultVisible (so an untouched picker shows defaults);
// [...] → these ∩ schema; [] → nothing.
export function effectiveQueryColumns(
  chosen: readonly string[] | null,
  schema: ReadonlyArray<SchemaColumn>,
): string[] {
  if (chosen === null) {
    return linkNameFirst(
      schema.filter((c) => c.defaultVisible).map((c) => c.name),
    );
  }
  const known = new Set(schema.map((c) => c.name));
  return linkNameFirst(chosen.filter((c) => known.has(c)));
}
