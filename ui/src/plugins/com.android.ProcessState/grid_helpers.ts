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

import m from 'mithril';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';

// Build a DataGrid schema (one column per field). Pid-like columns render as
// links that drive selection via onPidClick.
export function gridSchema(
  columns: ReadonlyArray<string>,
  onPidClick?: (pid: number) => void,
  // Per-column renderer; receives the cell value and its row (so diff renderers
  // can read sibling fields like the baseline value).
  renderers?: {
    readonly [col: string]: (
      value: unknown,
      row: {[k: string]: unknown},
    ) => m.Children;
  },
): SchemaRegistry {
  const root: ColumnSchema = {};
  const isPid = (c: string) =>
    c === 'pid' ||
    c === 'client_pid' ||
    c === 'server_pid' ||
    c === 'owning_pid' ||
    c === 'source_pid' ||
    c === 'host_pid';
  for (const col of columns) {
    root[col] = {
      title: col,
      // Resolution order: a caller-supplied renderer (e.g. the diff delta), then
      // the pid-link renderer, else the raw value. Enum columns already arrive
      // as resolved name strings from the importer, so no enum rendering here.
      cellRenderer:
        renderers?.[col] ??
        (onPidClick && isPid(col)
          ? (value) =>
              value === null || value === undefined || Number(value) === 0
                ? '—' // 0 = no process (global/system event), not navigable
                : m(
                    'a.pf-ps-link',
                    {onclick: () => onPidClick(Number(value))},
                    String(value),
                  )
          : undefined),
    };
  }
  return {root};
}

// A labelled card whose body is a sortable/filterable DataGrid (or a "none"
// placeholder). Shared by the explorer page and the timeline details panel.
export function gridCard(
  title: string,
  cols: string[],
  rows: Row[],
  ds: InMemoryDataSource | undefined,
  onPid: (pid: number) => void,
): m.Children {
  // Controlled `columns` (not initialColumns): DataGrid reads initialColumns only
  // once at init, so a reused instance would keep a previous card's columns. With
  // `columns` the grid reflects the current columns every render — no per-card
  // Mithril key needed (a key here would clash with the unkeyed siblings in the
  // panel's renderProps and crash with "vnodes must either all/none have keys").
  return m('.pf-ps-card', [
    m('.pf-ps-card-h', title),
    ds && rows.length
      ? m(DataGrid, {
          schema: gridSchema(cols, onPid),
          rootSchema: 'root',
          data: ds,
          columns: cols.map((c) => ({id: c, field: c})),
        })
      : m('.pf-ps-card-b', m('.pf-ps-none', '— none —')),
  ]);
}
