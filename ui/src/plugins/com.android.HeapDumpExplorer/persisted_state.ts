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
import {FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import type {SqlValue} from '../../trace_processor/query_result';
import type {Column, Filter} from '../../components/widgets/datagrid/model';
import {base64Decode, base64Encode} from '../../base/string_utils';

// Schema for the slice of Heap Dump Explorer state that survives in a shared
// permalink. The session writes this on every state change via mountStore;
// the core serializes it into the permalink and restores it before the plugin
// loads (see core/state_serialization.ts).
//
// Timestamps are heap dump `ts` values (bigint) and are stored as decimal
// strings because JSON has no bigint.

const DUMP_REF_SCHEMA = z.object({
  upid: z.number(),
  ts: z.string(),
});

// A flamegraph tab always belongs to the active dump (tabs reset on dump
// switch), so the dump is taken from activeDump rather than stored per tab.
const FLAMEGRAPH_TAB_SCHEMA = z.object({
  pathHashes: z.string(),
  isDominator: z.boolean(),
});

const INSTANCE_TAB_SCHEMA = z.object({
  objId: z.number(),
  label: z.string(),
});

// DataGrid filter values are SqlValue, which includes bigint and Uint8Array.
// Neither survives JSON, so they are wrapped in a tagged object on the way out
// and unwrapped on the way back in; string / number / null pass through.

const SQL_VALUE_JSON_SCHEMA = z.union([
  z.null(),
  z.string(),
  z.number(),
  z.object({bigint: z.string()}).readonly(),
  z.object({bytes: z.string()}).readonly(),
]);
type SqlValueJson = z.infer<typeof SQL_VALUE_JSON_SCHEMA>;

function encodeSqlValue(v: SqlValue): SqlValueJson {
  if (typeof v === 'bigint') return {bigint: v.toString()};
  if (v instanceof Uint8Array) return {bytes: base64Encode(v)};
  return v;
}

function decodeSqlValue(v: SqlValueJson): SqlValue {
  if (v !== null && typeof v === 'object') {
    return 'bigint' in v ? BigInt(v.bigint) : base64Decode(v.bytes);
  }
  return v;
}

const GRID_FILTER_SCHEMA = z
  .object({
    field: z.string(),
    op: z.string(),
    // Absent for null filters (is null / is not null); an array for in / not in.
    value: z
      .union([SQL_VALUE_JSON_SCHEMA, z.array(SQL_VALUE_JSON_SCHEMA)])
      .optional(),
  })
  .readonly();

const GRID_COLUMN_SCHEMA = z
  .object({
    id: z.string(),
    field: z.string(),
    sort: z.enum(['ASC', 'DESC']).optional(),
    aggregate: z.unknown().optional(),
  })
  .readonly();

const GRID_STATE_SCHEMA = z
  .object({
    columns: z.array(GRID_COLUMN_SCHEMA).optional(),
    filters: z.array(GRID_FILTER_SCHEMA).optional(),
  })
  .readonly();

export type GridStateJson = z.infer<typeof GRID_STATE_SCHEMA>;

export const HDE_STATE_SCHEMA = z
  .object({
    // The selected heap dump; identifies which dump the rest of the state
    // belongs to. Restore is skipped if it no longer matches a loaded dump.
    activeDump: DUMP_REF_SCHEMA.optional(),
    // The active navigation, as a stateToSubpage subpage string.
    nav: z.string().optional(),
    // Open "Flamegraph objects" drill-down tabs. The active one is not stored;
    // it is re-derived from nav (which encodes the tab's pathHashes) on restore.
    flamegraphTabs: z.array(FLAMEGRAPH_TAB_SCHEMA).optional(),
    // Open object/instance inspector tabs. The active one is not stored; it is
    // re-derived from nav (which encodes the object id) on restore.
    instanceTabs: z.array(INSTANCE_TAB_SCHEMA).optional(),
    // Filter / pivot / view state of the main Flamegraph tab.
    flamegraphPanelState: FLAMEGRAPH_STATE_SCHEMA.optional(),
    // Per-tab DataGrid state (column order/visibility/sort + filters), keyed by
    // a stable tab key (static tab name, or a per-open-tab key for drill-downs).
    grids: z.record(z.string(), GRID_STATE_SCHEMA).optional(),
  })
  .readonly();

export type HdeState = z.infer<typeof HDE_STATE_SCHEMA>;

// A view's DataGrid state in runtime (non-serialized) form. `columns` is
// undefined until the user customises them, so the view keeps using its own
// defaults until then.
export interface GridSlot {
  readonly columns?: readonly Column[];
  readonly filters: readonly Filter[];
}

export function encodeGridSlot(slot: GridSlot): GridStateJson {
  return {
    columns: slot.columns?.map((c) => ({
      id: c.id,
      field: c.field,
      sort: c.sort,
      aggregate: c.aggregate,
    })),
    filters: slot.filters.map(encodeFilter),
  };
}

export function decodeGridSlot(json: GridStateJson): GridSlot {
  return {
    columns: json.columns?.map(
      (c) =>
        ({
          id: c.id,
          field: c.field,
          sort: c.sort,
          aggregate: c.aggregate,
        }) as Column,
    ),
    filters: (json.filters ?? []).map(decodeFilter),
  };
}

function encodeFilter(f: Filter): z.infer<typeof GRID_FILTER_SCHEMA> {
  if (!('value' in f)) {
    return {field: f.field, op: f.op};
  }
  const value = f.value;
  // Array.isArray doesn't narrow a `readonly SqlValue[]` union, hence the cast
  // in the scalar branch — runtime has already ruled out the array case.
  if (Array.isArray(value)) {
    return {field: f.field, op: f.op, value: value.map(encodeSqlValue)};
  }
  return {field: f.field, op: f.op, value: encodeSqlValue(value as SqlValue)};
}

function decodeFilter(f: z.infer<typeof GRID_FILTER_SCHEMA>): Filter {
  if (f.value === undefined) {
    return {field: f.field, op: f.op} as Filter;
  }
  if (Array.isArray(f.value)) {
    return {
      field: f.field,
      op: f.op,
      value: f.value.map(decodeSqlValue),
    } as Filter;
  }
  return {field: f.field, op: f.op, value: decodeSqlValue(f.value)} as Filter;
}

// An unparseable or older permalink falls back to empty state rather than
// throwing.
export function migrateHdeState(init: unknown): HdeState {
  return HDE_STATE_SCHEMA.safeParse(init).data ?? {};
}
