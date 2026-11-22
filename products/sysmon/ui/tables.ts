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
import {DataGrid} from '../../../ui/src/components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../../ui/src/components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../../ui/src/trace_processor/query_result';
import {Anchor} from '../../../ui/src/widgets/anchor';
import {
  fmtCount,
  fmtDeltaBytes,
  fmtFreqMhz,
  fmtMs,
  fmtPct,
  fmtPowerMw,
  fmtTempC,
} from '../../common/ui/kit/format';

export function simpleGrid(
  schema: ColumnSchema,
  rows: readonly Row[],
): m.Child {
  return m(DataGrid, {
    schema,
    data: rows,
    initialColumns: Object.keys(schema).map((k) => ({id: k, field: k})),
  });
}

export const msCell = (v: unknown) => ({
  content: typeof v === 'number' && v > 0 ? fmtMs(v) : '—',
  align: 'right' as const,
});

export const pctCell = (v: unknown) => ({
  content: typeof v === 'number' ? fmtPct(v) : '—',
  align: 'right' as const,
});

export const countCell = (v: unknown) => ({
  content: typeof v === 'number' ? fmtCount(v) : '—',
  align: 'right' as const,
});

export const mibCell = (v: unknown) => ({
  content: typeof v === 'number' && v > 0 ? `${v.toFixed(1)} MiB` : '—',
  align: 'right' as const,
});

export const mibZeroCell = (v: unknown) => ({
  content: typeof v === 'number' ? `${v.toFixed(1)} MiB` : '—',
  align: 'right' as const,
});

export const mbCell = (v: unknown) => ({
  content: typeof v === 'number' && v > 0 ? `${v.toFixed(2)} MB` : '—',
  align: 'right' as const,
});

export const deltaMibCell = (v: unknown) => ({
  content:
    typeof v === 'number' && Math.abs(v) >= 0.05
      ? fmtDeltaBytes(v * 1024 * 1024)
      : '0 B',
  align: 'right' as const,
});

export const freqKhzCell = (v: unknown) => ({
  content: typeof v === 'number' && v > 0 ? fmtFreqMhz(v) : '—',
  align: 'right' as const,
});

export const tempCell = (v: unknown) => ({
  content: typeof v === 'number' ? fmtTempC(v) : '—',
  align: 'right' as const,
});

export const powerMwCell = (v: unknown) => ({
  content: typeof v === 'number' ? fmtPowerMw(v) : '—',
  align: 'right' as const,
});

export const codeCell = (v: unknown) =>
  v === null || v === undefined || v === '' ? '—' : m('code', String(v));

export function processLinkCell(onOpen: (processName: string) => void) {
  return (v: unknown) => {
    if (typeof v !== 'string' || v === '') return '—';
    return m(
      Anchor,
      {
        onclick: () => onOpen(v),
      },
      m('code', v),
    );
  };
}

export function memscopeActionCell(onOpen: (processName: string) => void) {
  return (v: unknown) => {
    if (typeof v !== 'string' || v === '') return '—';
    return m(
      Anchor,
      {
        icon: 'open_in_new',
        onclick: () => onOpen(v),
      },
      'Memscope',
    );
  };
}
