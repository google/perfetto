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
import {type App} from '../../public/app';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {Spinner} from '../../widgets/spinner';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {type SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {type Row, type SqlValue} from '../../trace_processor/query_result';
import {Tabs, type TabsTab} from '../../widgets/tabs';
import {sizeRenderer, filterAggregated} from './data';
import {
  TAB_PROCESSES,
  TAB_INSPECT,
  TAB_MAPPING,
  TAB_STRINGS_ALL,
  TAB_STRINGS_DUPS,
  TAB_STRINGS_VMA,
  procTabKey,
  parseProcTabKey,
  mapTabKey,
  parseMapTabKey,
  closeTab,
  type ProcessTabState,
  type MappingTabState,
  type PageContext,
} from './state';
import {getDupsFor, buildStringsTabs, closeStringsTab} from './strings_view';
import {renderVmaFilterToolbar, renderMappingTab} from './mapping_view';

// ── Process View (outer tabs) ───────────────────────────────────────────────

export function renderProcessView(ctx: PageContext, app: App): m.Children {
  const outerTabs: TabsTab[] = [];

  outerTabs.push({
    key: TAB_PROCESSES,
    title: `Processes (${ctx.processes?.length ?? 0})`,
    content: renderProcessesTab(ctx),
  });

  for (const pid of ctx.s.openProcessOrder) {
    const ps = ctx.s.openProcesses.get(pid);
    if (ps === undefined) continue;
    const process = ctx.processes?.find((p) => p.pid === pid);
    const pname = process?.name ?? `PID ${pid}`;
    outerTabs.push({
      key: procTabKey(pid),
      title: pname,
      closeButton: true,
      content: renderProcessSubTabs(ctx, pid, ps, app),
    });
  }

  const activeKey =
    ctx.s.activeProcessPid !== null
      ? procTabKey(ctx.s.activeProcessPid)
      : ctx.s.processTab;

  return m(Tabs, {
    tabs: outerTabs,
    activeTabKey: activeKey,
    onTabChange: (key) => {
      if (key === TAB_PROCESSES) {
        ctx.s.activeProcessPid = null;
        ctx.s.processTab = TAB_PROCESSES;
      } else {
        const pid = parseProcTabKey(key);
        if (pid !== undefined) {
          ctx.s.activeProcessPid = pid;
          ctx.s.processTab = key;
        }
      }
    },
    onTabClose: (key) => {
      const pid = parseProcTabKey(key);
      if (pid !== undefined) {
        const next = closeTab(
          ctx.s.openProcesses,
          ctx.s.openProcessOrder,
          ctx.s.activeProcessPid,
          pid,
          null,
        );
        ctx.s.activeProcessPid = next;
        ctx.s.processTab = next !== null ? procTabKey(next) : TAB_PROCESSES;
      }
    },
  });
}

// ── Per-process sub-tabs ────────────────────────────────────────────────────

function renderProcessSubTabs(
  ctx: PageContext,
  pid: number,
  ps: ProcessTabState,
  app: App,
): m.Children {
  const subTabs: TabsTab[] = [];

  subTabs.push({
    key: TAB_INSPECT,
    title: 'Mappings',
    content: renderSmapsGrid(ctx, pid, ps, app),
  });

  for (const name of ps.openMappingOrder) {
    const ms = ps.openMappings.get(name);
    if (ms === undefined) continue;
    subTabs.push({
      key: mapTabKey(name),
      title: name,
      closeButton: true,
      content: renderMappingSubTabs(ctx, pid, name, ms),
    });
  }

  // Process-level strings tabs (from "All Strings" button)
  if (ps.processStringsData !== null) {
    const pss = ctx.getProcessStringsState(ps);
    const dups = getDupsFor(pss, ps.processStringsData.strings);
    subTabs.push(
      ...buildStringsTabs(
        pss,
        dups,
        () => {
          ps.activeMapping = null;
          ps.subTab = TAB_STRINGS_ALL;
        },
        'All ',
        '',
      ),
    );
  }

  const activeKey =
    ps.activeMapping !== null ? mapTabKey(ps.activeMapping) : ps.subTab;

  return m(Tabs, {
    tabs: subTabs,
    activeTabKey: activeKey,
    onTabChange: (key) => {
      if (key === TAB_INSPECT) {
        ps.activeMapping = null;
        ps.subTab = TAB_INSPECT;
      } else {
        const name = parseMapTabKey(key);
        if (name !== undefined) {
          ps.activeMapping = name;
          ps.subTab = key;
        } else {
          ps.activeMapping = null;
          ps.subTab = key;
        }
      }
    },
    onTabClose: (key) => {
      const name = parseMapTabKey(key);
      if (name !== undefined) {
        const next = closeTab(
          ps.openMappings,
          ps.openMappingOrder,
          ps.activeMapping,
          name,
          null,
        );
        ps.activeMapping = next;
        ps.subTab = next !== null ? mapTabKey(next) : TAB_INSPECT;
      } else if (
        key === TAB_STRINGS_ALL ||
        key === TAB_STRINGS_DUPS ||
        key === TAB_STRINGS_VMA
      ) {
        const pss = ctx.getProcessStringsState(ps);
        ps.subTab = closeStringsTab(pss, key, ps.subTab, TAB_INSPECT);
      }
    },
  });
}

// ── Per-mapping sub-tabs ────────────────────────────────────────────────────

function renderMappingSubTabs(
  ctx: PageContext,
  pid: number,
  mappingName: string,
  ms: MappingTabState,
): m.Children {
  const subTabs: TabsTab[] = [];

  subTabs.push({
    key: TAB_MAPPING,
    title: 'VMAs',
    content: renderMappingTab(ctx, pid, mappingName, ms),
  });

  if (ms.stringsData !== null) {
    const dups = getDupsFor(ms, ms.stringsData.strings);
    subTabs.push(
      ...buildStringsTabs(
        ms,
        dups,
        () => {
          ms.subTab = TAB_STRINGS_ALL;
        },
        '',
        ms.stringsData.processName,
      ),
    );
  }

  return m(Tabs, {
    tabs: subTabs,
    activeTabKey: ms.subTab,
    onTabChange: (key) => {
      ms.subTab = key;
    },
    onTabClose: (key) => {
      if (
        key === TAB_STRINGS_ALL ||
        key === TAB_STRINGS_DUPS ||
        key === TAB_STRINGS_VMA
      ) {
        ms.subTab = closeStringsTab(ms, key, ms.subTab, TAB_MAPPING);
      }
    },
  });
}

// ── Processes list DataGrid ─────────────────────────────────────────────────

function buildProcessSchema(ctx: PageContext): SchemaRegistry {
  return {
    process: {
      pid: {title: 'PID', columnType: 'quantitative'},
      name: {
        title: 'Process',
        columnType: 'text',
        cellRenderer: (value: SqlValue, row: Row) => {
          if (!ctx.isRoot) return String(value);
          const pid = Number(row.pid);
          return m(
            Anchor,
            {
              onclick: () => ctx.inspectProcess(pid),
              title: 'Inspect smaps',
            },
            String(value),
          );
        },
      },
      oomLabel: {title: 'State', columnType: 'text'},
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

function renderProcessesTab(ctx: PageContext): m.Children {
  if (ctx.processes === null) return null;

  const rows: Row[] = ctx.processes.map((p) => {
    const r = ctx.rollups.get(p.pid);
    return {
      pid: p.pid,
      name: p.name,
      oomLabel: p.oomLabel,
      pssKb: r?.pssKb ?? 0,
      rssKb: r?.rssKb ?? 0,
      privateDirtyKb: r?.privateDirtyKb ?? 0,
      privateCleanKb: r?.privateCleanKb ?? 0,
      swapKb: r?.swapKb ?? 0,
      sizeKb: r?.sizeKb ?? 0,
    };
  });

  return m(DataGrid, {
    key: ctx.enrichGeneration,
    schema: buildProcessSchema(ctx),
    rootSchema: 'process',
    data: rows,
    fillHeight: true,
    initialColumns: [
      {id: 'pid', field: 'pid'},
      {id: 'name', field: 'name'},
      {id: 'oomLabel', field: 'oomLabel'},
      {id: 'pssKb', field: 'pssKb', sort: 'DESC' as const},
      {id: 'rssKb', field: 'rssKb'},
      {id: 'privateDirtyKb', field: 'privateDirtyKb'},
      {id: 'privateCleanKb', field: 'privateCleanKb'},
      {id: 'swapKb', field: 'swapKb'},
      {id: 'sizeKb', field: 'sizeKb'},
    ],
  });
}

// ── Aggregated mappings DataGrid (per-process) ──────────────────────────────

function buildAggMappingSchema(
  ctx: PageContext,
  ps: ProcessTabState,
): SchemaRegistry {
  return {
    mapping: {
      name: {
        title: 'Mapping',
        columnType: 'text',
        cellRenderer: (value: SqlValue) => {
          const name = String(value);
          return m(
            Anchor,
            {
              onclick: () => ctx.openMapping(ps, name),
              title: 'Show individual VMAs',
            },
            name,
          );
        },
      },
      count: {title: 'Count', columnType: 'quantitative'},
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

function renderSmapsGrid(
  ctx: PageContext,
  pid: number,
  ps: ProcessTabState,
  app: App,
): m.Children {
  if (ctx.loadingPid === pid) {
    return m('.pf-smaps-explorer__loading', m(Spinner));
  }
  const rawAgg = ctx.smapsData.get(pid);
  if (rawAgg === undefined) {
    return m('.pf-smaps-explorer__loading--muted', 'Loading smaps\u2026');
  }

  const aggregated = filterAggregated(rawAgg, ctx.vmaFilters);
  const process = ctx.processes?.find((p) => p.pid === pid);
  const processName = process?.name ?? '';
  const totalEntries = aggregated.reduce((n, g) => n + g.entries.length, 0);

  const aggRows: Row[] = aggregated.map((g) => ({
    name: g.name || '[anonymous]',
    count: g.count,
    sizeKb: g.sizeKb,
    rssKb: g.rssKb,
    pssKb: g.pssKb,
    privateCleanKb: g.privateCleanKb,
    privateDirtyKb: g.privateDirtyKb,
    sharedCleanKb: g.sharedCleanKb,
    sharedDirtyKb: g.sharedDirtyKb,
    swapKb: g.swapKb,
  }));

  return m('.pf-smaps-explorer__panel', [
    // Action bar
    m('.pf-smaps-explorer__toolbar', [
      m(
        'span.pf-smaps-explorer__label',
        `${aggregated.length} mappings \u00b7 ${totalEntries} VMAs`,
      ),
      m('.pf-smaps-explorer__spacer'),
      renderVmaFilterToolbar(ctx),
      m(Button, {
        label: 'All Strings',
        icon: 'text_fields',
        compact: true,
        onclick: () => ctx.startStringsScan(pid, processName, ps),
      }),
      m(Button, {
        label: 'Heap Dump',
        icon: 'download',
        compact: true,
        onclick: () => ctx.captureHeap(pid, processName, app),
      }),
    ]),

    // Aggregated mappings DataGrid
    m(
      '.pf-smaps-explorer__grid-container',
      m(DataGrid, {
        key: ctx.smapsScanGeneration,
        schema: buildAggMappingSchema(ctx, ps),
        rootSchema: 'mapping',
        data: aggRows,
        fillHeight: true,
        initialColumns: [
          {id: 'name', field: 'name'},
          {id: 'count', field: 'count'},
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
