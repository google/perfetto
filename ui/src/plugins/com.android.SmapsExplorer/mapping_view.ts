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
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {EmptyState} from '../../widgets/empty_state';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {type SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {type Row, type SqlValue} from '../../trace_processor/query_result';
import {
  sizeRenderer,
  hexAddrRenderer,
  filterAggregated,
  type VmaType,
} from './data';
import {type MappingTabState, type PageContext} from './state';
import {type SmapsEntry} from './smaps_connection';

// ── VMA filter toolbar (shared between process and VMA views) ───────────────

// Cycle: null (unfiltered) → true (required) → false (excluded) → null.
function nextPermState(val: boolean | null): boolean | null {
  if (val === null) return true;
  if (val === true) return false;
  return null;
}

export function renderVmaFilterToolbar(ctx: PageContext): m.Children {
  const f = ctx.vmaFilters;
  const typeBtn = (type: VmaType, label: string) =>
    m(Button, {
      label,
      compact: true,
      active: f.type === type,
      onclick: () => ctx.setVmaFilters({...f, type}),
    });
  const permBtn = (perm: 'r' | 'w' | 'x', val: boolean | null) =>
    m(Button, {
      label: perm,
      compact: true,
      active: val !== null,
      className: val === false ? 'pf-smaps-explorer__perm-deselected' : '',
      onclick: () => {
        ctx.setVmaFilters({...f, [perm]: nextPermState(val)});
      },
    });

  return m('.pf-smaps-explorer__filter-toolbar', [
    m('.pf-smaps-explorer__btn-group', [
      typeBtn('all', 'All'),
      typeBtn('file', 'File'),
      typeBtn('anon', 'Anon'),
    ]),
    m('.pf-smaps-explorer__btn-group', [
      permBtn('r', f.r),
      permBtn('w', f.w),
      permBtn('x', f.x),
    ]),
  ]);
}

// ── Individual VMA schema for a mapping ─────────────────────────────────────

function buildVmaSchema(
  ctx: PageContext,
  pid: number,
  ms: MappingTabState,
  entries: SmapsEntry[],
): SchemaRegistry {
  // Index entries by numeric start address for O(1) lookup in cell renderer.
  const byAddr = new Map<number, SmapsEntry>();
  for (const e of entries) {
    byAddr.set(parseInt(e.addrStart, 16), e);
  }

  return {
    vma: {
      addrStart: {
        title: 'Start',
        columnType: 'quantitative',
        cellRenderer: (value: SqlValue) => {
          const entry = byAddr.get(Number(value));
          if (entry === undefined) return hexAddrRenderer(value);
          return m(
            Anchor,
            {
              onclick: () => {
                ctx.scanSingleVma(
                  pid,
                  ms,
                  entry.addrStart,
                  entry.addrEnd,
                  entry.perms,
                );
              },
              title: 'Scan strings in this VMA',
            },
            hexAddrRenderer(value).content,
          );
        },
      },
      addrEnd: {
        title: 'End',
        columnType: 'quantitative',
        cellRenderer: hexAddrRenderer,
      },
      perms: {title: 'Perms', columnType: 'text'},
      pssKb: {
        title: 'PSS',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      rssKb: {
        title: 'RSS',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      privateDirtyKb: {
        title: 'Priv Dirty',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      privateCleanKb: {
        title: 'Priv Clean',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      sharedDirtyKb: {
        title: 'Shared Dirty',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      sharedCleanKb: {
        title: 'Shared Clean',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      swapKb: {
        title: 'Swap',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      sizeKb: {
        title: 'VSS',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
    },
  };
}

// ── Mapping tab: individual VMAs for a specific mapping ─────────────────────

export function renderMappingTab(
  ctx: PageContext,
  pid: number,
  mappingName: string,
  ms: MappingTabState,
): m.Children {
  const rawAgg = ctx.smapsData.get(pid);
  if (rawAgg === undefined) return null;

  const aggregated = filterAggregated(rawAgg, ctx.vmaFilters);
  const entries = aggregated
    .filter((g) => (g.name || '[anonymous]') === mappingName)
    .flatMap((g) => g.entries);

  const rows: Row[] = entries.map((e) => ({
    addrStart: parseInt(e.addrStart, 16),
    addrEnd: parseInt(e.addrEnd, 16),
    perms: e.perms,
    sizeKb: e.sizeKb,
    rssKb: e.rssKb,
    pssKb: e.pssKb,
    privateCleanKb: e.privateCleanKb,
    privateDirtyKb: e.privateDirtyKb,
    sharedCleanKb: e.sharedCleanKb,
    sharedDirtyKb: e.sharedDirtyKb,
    swapKb: e.swapKb,
  }));

  return m('.pf-smaps-explorer__panel', [
    m('.pf-smaps-explorer__toolbar', [
      m('span.pf-smaps-explorer__label', `${entries.length} VMAs`),
      m('.pf-smaps-explorer__spacer'),
      renderVmaFilterToolbar(ctx),
    ]),
    entries.length === 0
      ? m(EmptyState, {
          icon: 'filter_alt',
          title: 'No VMAs match the current filters',
          fillHeight: true,
        })
      : m(
          '.pf-smaps-explorer__grid-container',
          m(DataGrid, {
            key: ctx.smapsScanGeneration,
            schema: buildVmaSchema(ctx, pid, ms, entries),
            rootSchema: 'vma',
            data: rows,
            fillHeight: true,
            initialColumns: [
              {id: 'addrStart', field: 'addrStart'},
              {id: 'addrEnd', field: 'addrEnd'},
              {id: 'perms', field: 'perms'},
              {id: 'pssKb', field: 'pssKb', sort: 'DESC' as const},
              {id: 'rssKb', field: 'rssKb'},
              {id: 'privateDirtyKb', field: 'privateDirtyKb'},
              {id: 'privateCleanKb', field: 'privateCleanKb'},
              {id: 'sharedDirtyKb', field: 'sharedDirtyKb'},
              {id: 'sharedCleanKb', field: 'sharedCleanKb'},
              {id: 'swapKb', field: 'swapKb'},
              {id: 'sizeKb', field: 'sizeKb'},
            ],
          }),
        ),
  ]);
}
