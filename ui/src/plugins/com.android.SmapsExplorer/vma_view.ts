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
import {Tabs, type TabsTab} from '../../widgets/tabs';
import {
  sizeRenderer,
  aggregateVmasCrossProcess,
  VMAS_CROSS_SCHEMA,
} from './data';
import {
  TAB_VMAS,
  TAB_MAPPING,
  TAB_STRINGS_ALL,
  TAB_STRINGS_DUPS,
  TAB_STRINGS_VMA,
  vmapTabKey,
  parseVmapTabKey,
  procTabKey,
  parseProcTabKey,
  closeTab,
  type MappingTabState,
  type VmaMappingTabState,
  type PageContext,
} from './state';
import {getDupsFor, buildStringsTabs, closeStringsTab} from './strings_view';
import {renderVmaFilterToolbar, renderMappingTab} from './mapping_view';

// ── VMA View (outer) ────────────────────────────────────────────────────────

export function renderVmaView(ctx: PageContext): m.Children {
  if (ctx.smapsData.size === 0) {
    return m(
      EmptyState,
      {
        icon: 'memory',
        title: 'No VMA data yet',
        fillHeight: true,
      },
      ctx.isRoot &&
        !ctx.scanningAllSmaps &&
        m(Button, {
          label: 'Scan All VMAs',
          icon: 'memory',
          onclick: () => ctx.scanAllSmaps(),
        }),
      !ctx.isRoot && 'Inspect individual processes first.',
    );
  }

  const outerTabs: TabsTab[] = [];
  const vmaCount = aggregateVmasCrossProcess(
    ctx.smapsData,
    ctx.vmaFilters,
  ).length;

  outerTabs.push({
    key: TAB_VMAS,
    title: `All VMAs (${vmaCount})`,
    content: renderVmasTab(ctx),
  });

  for (const name of ctx.s.openVmaMappingOrder) {
    const vs = ctx.s.openVmaMappings.get(name);
    if (vs === undefined) continue;
    outerTabs.push({
      key: vmapTabKey(name),
      title: name,
      closeButton: true,
      content: renderVmaMappingSubTabs(ctx, name, vs),
    });
  }

  const activeKey =
    ctx.s.activeVmaMapping !== null
      ? vmapTabKey(ctx.s.activeVmaMapping)
      : ctx.s.vmaTab;

  return m(Tabs, {
    tabs: outerTabs,
    activeTabKey: activeKey,
    onTabChange: (key) => {
      if (key === TAB_VMAS) {
        ctx.s.activeVmaMapping = null;
        ctx.s.vmaTab = key;
      } else {
        const name = parseVmapTabKey(key);
        if (name !== undefined) {
          ctx.s.activeVmaMapping = name;
          ctx.s.vmaTab = key;
        }
      }
    },
    onTabClose: (key) => {
      const name = parseVmapTabKey(key);
      if (name !== undefined) {
        const next = closeTab(
          ctx.s.openVmaMappings,
          ctx.s.openVmaMappingOrder,
          ctx.s.activeVmaMapping,
          name,
          null,
        );
        ctx.s.activeVmaMapping = next;
        ctx.s.vmaTab = next !== null ? vmapTabKey(next) : TAB_VMAS;
      }
    },
  });
}

// ── VMA mapping sub-tabs: Processes | {process}: VMAs ───────────────────────

function renderVmaMappingSubTabs(
  ctx: PageContext,
  mappingName: string,
  vs: VmaMappingTabState,
): m.Children {
  const subTabs: TabsTab[] = [];

  subTabs.push({
    key: 'procs',
    title: 'Processes',
    content: renderVmaProcsTab(ctx, mappingName, vs),
  });

  for (const pid of vs.openProcOrder) {
    const ms = vs.openProcs.get(pid);
    if (ms === undefined) continue;
    const proc = ctx.processes?.find((p) => p.pid === pid);
    const pname = proc?.name ?? `PID ${pid}`;
    subTabs.push({
      key: procTabKey(pid),
      title: pname,
      closeButton: true,
      content: renderVmaProcSubTabs(ctx, pid, mappingName, ms),
    });
  }

  const activeKey =
    vs.activeProc !== null ? procTabKey(vs.activeProc) : vs.subTab;

  return m(Tabs, {
    tabs: subTabs,
    activeTabKey: activeKey,
    onTabChange: (key) => {
      if (key === 'procs') {
        vs.activeProc = null;
        vs.subTab = 'procs';
      } else {
        const pid = parseProcTabKey(key);
        if (pid !== undefined) {
          vs.activeProc = pid;
          vs.subTab = key;
        }
      }
    },
    onTabClose: (key) => {
      const pid = parseProcTabKey(key);
      if (pid !== undefined) {
        const next = closeTab(
          vs.openProcs,
          vs.openProcOrder,
          vs.activeProc,
          pid,
          null,
        );
        vs.activeProc = next;
        vs.subTab = next !== null ? procTabKey(next) : 'procs';
      }
    },
  });
}

// ── VMAs + strings sub-tabs for a process within VMA View ───────────────────

function renderVmaProcSubTabs(
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

// ── Cross-process VMAs DataGrid ─────────────────────────────────────────────

function buildVmasCrossSchema(ctx: PageContext): SchemaRegistry {
  return {
    vma: {
      ...VMAS_CROSS_SCHEMA.vma,
      name: {
        title: 'Mapping',
        columnType: 'text' as const,
        cellRenderer: (value: SqlValue) => {
          const name = String(value);
          return m(
            Anchor,
            {
              onclick: () => ctx.openVmaProcesses(name),
              title: 'Show processes using this mapping',
            },
            name,
          );
        },
      },
    },
  };
}

function renderVmasTab(ctx: PageContext): m.Children {
  const vmas = aggregateVmasCrossProcess(ctx.smapsData, ctx.vmaFilters);

  const rows: Row[] = vmas.map((v) => ({
    name: v.name,
    perms: v.perms,
    processCount: v.processCount,
    totalPssKb: v.totalPssKb,
    totalRssKb: v.totalRssKb,
    totalPrivDirtyKb: v.totalPrivDirtyKb,
    totalPrivCleanKb: v.totalPrivCleanKb,
    totalSwapKb: v.totalSwapKb,
    totalSizeKb: v.totalSizeKb,
  }));

  return m('.pf-smaps-explorer__panel', [
    m('.pf-smaps-explorer__fixed', renderVmaFilterToolbar(ctx)),
    m(
      '.pf-smaps-explorer__grid-container',
      m(DataGrid, {
        key: ctx.smapsScanGeneration,
        schema: buildVmasCrossSchema(ctx),
        rootSchema: 'vma',
        data: rows,
        fillHeight: true,
        initialColumns: [
          {id: 'name', field: 'name'},
          {id: 'perms', field: 'perms'},
          {id: 'processCount', field: 'processCount'},
          {
            id: 'totalPssKb',
            field: 'totalPssKb',
            sort: 'DESC' as const,
          },
          {id: 'totalRssKb', field: 'totalRssKb'},
          {id: 'totalPrivDirtyKb', field: 'totalPrivDirtyKb'},
          {id: 'totalPrivCleanKb', field: 'totalPrivCleanKb'},
          {id: 'totalSwapKb', field: 'totalSwapKb'},
          {id: 'totalSizeKb', field: 'totalSizeKb'},
        ],
      }),
    ),
  ]);
}

// ── Per-mapping processes DataGrid ──────────────────────────────────────────

function renderVmaProcsTab(
  ctx: PageContext,
  mapping: string,
  vs: VmaMappingTabState,
): m.Children {
  if (ctx.processes === null) return null;

  const pids = new Set<number>();
  for (const [pid, agg] of ctx.smapsData) {
    for (const g of agg) {
      if ((g.name || '[anonymous]') === mapping) {
        pids.add(pid);
        break;
      }
    }
  }

  const procs = ctx.processes.filter((p) => pids.has(p.pid));
  const rows: Row[] = procs.map((p) => {
    const agg = ctx.smapsData.get(p.pid);
    let pssKb = 0;
    let rssKb = 0;
    let sizeKb = 0;
    let count = 0;
    if (agg !== undefined) {
      for (const g of agg) {
        for (const e of g.entries) {
          if ((e.name || '[anonymous]') === mapping) {
            pssKb += e.pssKb;
            rssKb += e.rssKb;
            sizeKb += e.sizeKb;
            count++;
          }
        }
      }
    }
    return {
      pid: p.pid,
      name: p.name,
      oomLabel: p.oomLabel,
      count,
      pssKb,
      rssKb,
      sizeKb,
    };
  });

  const schema: SchemaRegistry = {
    process: {
      pid: {title: 'PID', columnType: 'quantitative'},
      name: {
        title: 'Process',
        columnType: 'text',
        cellRenderer: (value: SqlValue, row: Row) => {
          const pid = Number(row.pid);
          return m(
            Anchor,
            {
              onclick: () => ctx.openVmaProcDetail(vs, pid),
              title: 'Show VMAs for this process',
            },
            String(value),
          );
        },
      },
      oomLabel: {title: 'State', columnType: 'text'},
      count: {title: 'VMAs', columnType: 'quantitative'},
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
      sizeKb: {
        title: 'VSS',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
    },
  };

  return m('.pf-smaps-explorer__panel', [
    m(
      '.pf-smaps-explorer__info-bar',
      `${procs.length} processes use this mapping`,
    ),
    m(
      '.pf-smaps-explorer__grid-container',
      m(DataGrid, {
        key: ctx.smapsScanGeneration,
        schema,
        rootSchema: 'process',
        data: rows,
        fillHeight: true,
        initialColumns: [
          {id: 'pid', field: 'pid'},
          {id: 'name', field: 'name'},
          {id: 'oomLabel', field: 'oomLabel'},
          {id: 'count', field: 'count'},
          {id: 'pssKb', field: 'pssKb', sort: 'DESC' as const},
          {id: 'rssKb', field: 'rssKb'},
          {id: 'sizeKb', field: 'sizeKb'},
        ],
      }),
    ),
  ]);
}
