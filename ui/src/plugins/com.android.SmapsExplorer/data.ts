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
  type CellRenderResult,
  type SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {type SqlValue} from '../../trace_processor/query_result';
import {
  type SmapsAggregated,
  type SmapsEntry,
  type VmaString,
} from './smaps_connection';

// ── Formatters ──────────────────────────────────────────────────────────────

export function fmtSize(bytes: number): string {
  if (bytes === 0) return '\u2014';
  if (bytes < 1024) return `${bytes} B`;
  const kb = bytes / 1024;
  if (kb < 1024) return `${kb.toFixed(kb < 10 ? 1 : 0)} KiB`;
  const mb = kb / 1024;
  if (mb < 1024) return `${mb.toFixed(mb < 10 ? 1 : 0)} MiB`;
  const gb = mb / 1024;
  return `${gb.toFixed(1)} GiB`;
}

export function sizeRenderer(value: SqlValue): CellRenderResult {
  const n = Number(value);
  return {
    content: n > 0 ? fmtSize(n * 1024) : '\u2014',
    align: 'right',
    nullish: n === 0,
  };
}

export function hexAddrRenderer(value: SqlValue): CellRenderResult {
  const n = Number(value);
  return {
    content: n.toString(16).padStart(n > 0xffffffff ? 12 : 8, '0'),
    align: 'left',
  };
}

// ── VMA filters ─────────────────────────────────────────────────────────────

export type VmaType = 'all' | 'file' | 'anon';

export function classifyEntry(e: SmapsEntry): 'file' | 'anon' {
  if (e.dev !== '00:00' && e.inode !== 0) return 'file';
  return 'anon';
}

export interface VmaFilters {
  type: VmaType;
  r: boolean | null;
  w: boolean | null;
  x: boolean | null;
}

export function matchesFilters(e: SmapsEntry, f: VmaFilters): boolean {
  if (f.type !== 'all' && classifyEntry(e) !== f.type) return false;
  if (f.r !== null && (e.perms[0] === 'r') !== f.r) return false;
  if (f.w !== null && (e.perms[1] === 'w') !== f.w) return false;
  if (f.x !== null && (e.perms[2] === 'x') !== f.x) return false;
  return true;
}

export function filterAggregated(
  aggregated: SmapsAggregated[],
  filters: VmaFilters,
): SmapsAggregated[] {
  if (
    filters.type === 'all' &&
    filters.r === null &&
    filters.w === null &&
    filters.x === null
  ) {
    return aggregated;
  }
  return aggregated
    .map((g) => {
      const entries = g.entries.filter((e) => matchesFilters(e, filters));
      if (entries.length === 0) return null;
      if (entries.length === g.entries.length) return g;
      const agg: SmapsAggregated = {
        name: g.name,
        count: entries.length,
        sizeKb: 0,
        rssKb: 0,
        pssKb: 0,
        sharedCleanKb: 0,
        sharedDirtyKb: 0,
        privateCleanKb: 0,
        privateDirtyKb: 0,
        swapKb: 0,
        swapPssKb: 0,
        entries,
      };
      for (const e of entries) {
        agg.sizeKb += e.sizeKb;
        agg.rssKb += e.rssKb;
        agg.pssKb += e.pssKb;
        agg.sharedCleanKb += e.sharedCleanKb;
        agg.sharedDirtyKb += e.sharedDirtyKb;
        agg.privateCleanKb += e.privateCleanKb;
        agg.privateDirtyKb += e.privateDirtyKb;
        agg.swapKb += e.swapKb;
        agg.swapPssKb += e.swapPssKb;
      }
      return agg;
    })
    .filter((g): g is SmapsAggregated => g !== null);
}

// ── Duplicate string computation ────────────────────────────────────────────

export interface DuplicateGroup {
  value: string;
  count: number;
  totalBytes: number;
  vmaCount: number;
}

export function computeDuplicates(strings: VmaString[]): DuplicateGroup[] {
  const groups = new Map<
    string,
    {count: number; totalBytes: number; vmaIndices: Set<number>}
  >();
  for (const s of strings) {
    const existing = groups.get(s.str);
    if (existing !== undefined) {
      existing.count++;
      existing.totalBytes += s.str.length;
      existing.vmaIndices.add(s.vmaIndex);
    } else {
      groups.set(s.str, {
        count: 1,
        totalBytes: s.str.length,
        vmaIndices: new Set([s.vmaIndex]),
      });
    }
  }
  const result: DuplicateGroup[] = [];
  for (const [value, g] of groups) {
    if (g.count < 2) continue;
    result.push({
      value,
      count: g.count,
      totalBytes: g.totalBytes,
      vmaCount: g.vmaIndices.size,
    });
  }
  result.sort((a, b) => b.totalBytes - a.totalBytes);
  return result;
}

// ── VMA-centric aggregation ─────────────────────────────────────────────────

export interface VmaCrossProcess {
  name: string;
  perms: string;
  processCount: number;
  totalPssKb: number;
  totalRssKb: number;
  totalSizeKb: number;
  totalPrivDirtyKb: number;
  totalPrivCleanKb: number;
  totalSwapKb: number;
  pids: number[];
}

export function aggregateVmasCrossProcess(
  smapsData: Map<number, SmapsAggregated[]>,
  filters: VmaFilters,
): VmaCrossProcess[] {
  const byKey = new Map<
    string,
    {
      perms: string;
      pids: Set<number>;
      pss: number;
      rss: number;
      size: number;
      privDirty: number;
      privClean: number;
      swap: number;
    }
  >();
  for (const [pid, aggregated] of smapsData) {
    for (const g of aggregated) {
      for (const e of g.entries) {
        if (!matchesFilters(e, filters)) continue;
        const key = `${e.name}|${e.perms}`;
        const existing = byKey.get(key);
        if (existing !== undefined) {
          existing.pids.add(pid);
          existing.pss += e.pssKb;
          existing.rss += e.rssKb;
          existing.size += e.sizeKb;
          existing.privDirty += e.privateDirtyKb;
          existing.privClean += e.privateCleanKb;
          existing.swap += e.swapKb;
        } else {
          byKey.set(key, {
            perms: e.perms,
            pids: new Set([pid]),
            pss: e.pssKb,
            rss: e.rssKb,
            size: e.sizeKb,
            privDirty: e.privateDirtyKb,
            privClean: e.privateCleanKb,
            swap: e.swapKb,
          });
        }
      }
    }
  }
  const result: VmaCrossProcess[] = [];
  for (const [key, data] of byKey) {
    const name = key.split('|')[0];
    result.push({
      name: name || '[anonymous]',
      perms: data.perms,
      processCount: data.pids.size,
      totalPssKb: data.pss,
      totalRssKb: data.rss,
      totalSizeKb: data.size,
      totalPrivDirtyKb: data.privDirty,
      totalPrivCleanKb: data.privClean,
      totalSwapKb: data.swap,
      pids: [...data.pids],
    });
  }
  result.sort((a, b) => b.totalPssKb - a.totalPssKb);
  return result;
}

// ── Static DataGrid schemas ─────────────────────────────────────────────────

export const ALL_STRINGS_SCHEMA: SchemaRegistry = {
  string: {
    vmaAddr: {
      title: 'Address',
      columnType: 'quantitative',
      cellRenderer: (v: SqlValue) => ({
        content: Number(v).toString(16).padStart(8, '0'),
        align: 'left' as const,
      }),
    },
    vmaName: {title: 'VMA', columnType: 'text'},
    str: {title: 'String', columnType: 'text'},
  },
};

export const VMAS_CROSS_SCHEMA: SchemaRegistry = {
  vma: {
    name: {title: 'Mapping', columnType: 'text'},
    perms: {title: 'Perms', columnType: 'text'},
    processCount: {title: 'Processes', columnType: 'quantitative'},
    totalPssKb: {
      title: 'PSS',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    totalRssKb: {
      title: 'RSS',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    totalPrivDirtyKb: {
      title: 'Priv Dirty',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    totalPrivCleanKb: {
      title: 'Priv Clean',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    totalSwapKb: {
      title: 'Swap',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    totalSizeKb: {
      title: 'VSS',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  },
};
