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

import type {Filter} from '../../components/widgets/datagrid/model';
import {
  SingleFieldStorage,
  parseNullableStringArray,
} from './single_field_storage';
import {linkNameFirst} from './column_order';

// Persisted trace-selection state: the trace filter, processing order, the
// trace-grid shown columns, and the metadata columns attached to results.

// Subset of a /trace_metadata_schema column the resolvers below need.
interface SchemaColumn {
  readonly name: string;
  readonly defaultVisible: boolean;
}

// Filter chips for the trace-selection grid: the active filter IS the trace set
// a query runs over. Shipped as `trace_filters` on /execute_*. get() returns []
// for nothing/malformed/cleared.
export const traceFilterState = new SingleFieldStorage<readonly Filter[]>(
  'bigtraceTraceFilters',
  'filters',
  (raw) => (Array.isArray(raw) ? (raw as Filter[]) : []),
  [],
);

// AIP-132 ordering string, shipped as `trace_order_by` on /execute_*. Matters
// under a trace cap (picks which first-N survive). get() returns '' (default
// ordering) for nothing/non-string.
export const traceOrderByState = new SingleFieldStorage<string>(
  'bigtraceTraceOrderBy',
  'orderBy',
  (raw) => (typeof raw === 'string' ? raw : ''),
  '',
);

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

// Resolver for traceQueryColumnsState against the live schema (link first):
// null → defaultVisible (so an untouched picker shows defaults); [...] → these ∩
// schema; [] → nothing.
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

// Metadata columns attached to result rows (`trace_metadata_columns` on
// /execute_*) — distinct from traceColumnsState (the trace-list grid's shown
// columns). null = defaultVisible; [] = nothing; [...] = these. [] must NOT
// collapse to null, else "nothing" is unexpressible.
export const traceQueryColumnsState = new SingleFieldStorage<
  readonly string[] | null
>(
  'bigtraceTraceQueryColumns',
  'chosen',
  (raw) =>
    Array.isArray(raw)
      ? raw.filter((v): v is string => typeof v === 'string')
      : null,
  null,
);
