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
import type {EChartsCoreOption} from 'echarts/core';
import {formatBytesIec} from '../../../../base/bytes_format';
import type {time} from '../../../../base/time';
import {
  buildTooltipOption,
  renderTooltip,
} from '../../../../components/widgets/charts/chart_option_builder';
import {
  EChartView,
  type EChartClickParams,
  type EChartEventHandler,
} from '../../../../components/widgets/charts/echart_view';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM, STR} from '../../../../trace_processor/query_result';
import {
  loadNativeLibFunctionsForSunburst,
  loadTopNativeLibsForSunburst,
} from '../native/native_queries';

export type SunburstDomainId =
  'native' | 'java' | 'file' | 'graphics' | 'stack' | 'other';

export interface SunburstChildSlice {
  readonly id: string;
  readonly name: string;
  readonly fullLabel?: string;
  readonly domainId: SunburstDomainId;
  readonly domainLabel: string;
  readonly bytes: number;
  readonly color: string;
  readonly hint?: string;
}

export interface SunburstDomainSlice {
  readonly id: SunburstDomainId;
  readonly name: string;
  readonly bytes: number;
  readonly color: string;
  readonly children: ReadonlyArray<SunburstChildSlice>;
}

export interface MemorySunburstData {
  readonly totalBytes: number;
  readonly metricLabel: string;
  readonly sourceLabel?: string;
  readonly domains: ReadonlyArray<SunburstDomainSlice>;
}

interface DomainStyle {
  readonly label: string;
  readonly color: string;
}

const DOMAIN_STYLES: Record<SunburstDomainId, DomainStyle> = {
  file: {label: 'File-backed', color: '#2e9e4c'},
  native: {label: 'Native', color: '#3b82f6'},
  java: {label: 'Java', color: '#e59500'},
  graphics: {label: 'Graphics', color: '#9333ea'},
  stack: {label: 'Thread stacks', color: '#0891b2'},
  other: {label: 'Other', color: '#6b7280'},
};

const FILE_BUCKET_LABELS: Record<string, string> = {
  java_code: 'Java code',
  so: 'Native .so',
  resources: 'Resources & APKs',
  other_file: 'Other files',
};

function mixHex(hex: string, targetHex: string, t: number): string {
  const parse = (h: string): [number, number, number] => {
    const clean = h.replace('#', '');
    return [
      parseInt(clean.slice(0, 2), 16),
      parseInt(clean.slice(2, 4), 16),
      parseInt(clean.slice(4, 6), 16),
    ];
  };
  const [r1, g1, b1] = parse(hex);
  const [r2, g2, b2] = parse(targetHex);
  const clamp = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
  const r = clamp(r1 + (r2 - r1) * t);
  const g = clamp(g1 + (g2 - g1) * t);
  const b = clamp(b1 + (b2 - b1) * t);
  return `#${((1 << 24) | (r << 16) | (g << 8) | b).toString(16).slice(1)}`;
}

function childShade(parentHex: string, index: number): string {
  const steps = [0.16, 0.32, 0.46, 0.58, 0.68, 0.76];
  const t = steps[Math.min(index, steps.length - 1)]!;
  return mixHex(parentHex, '#ffffff', t);
}

function labelColorForHex(hex: string): string {
  const clean = hex.replace('#', '');
  const r = parseInt(clean.slice(0, 2), 16) / 255;
  const g = parseInt(clean.slice(2, 4), 16) / 255;
  const b = parseInt(clean.slice(4, 6), 16) / 255;
  const luma = 0.299 * r + 0.587 * g + 0.114 * b;
  return luma > 0.62 ? '#1f2937' : '#ffffff';
}

function shortSmapsLabel(path: string, category: SunburstDomainId): string {
  if (!path) return '[anonymous]';
  if (path === '[stack]') return 'Main stack';
  if (path === '[heap]') return 'Native [heap]';
  if (path.startsWith('[anon:') && path.endsWith(']')) {
    const inner = path.slice(6, -1).trim();
    return inner.length > 26 ? `${inner.slice(0, 25)}…` : inner;
  }
  if (category === 'file' || path.startsWith('/')) {
    const idx = path.lastIndexOf('/');
    const base = idx >= 0 ? path.slice(idx + 1) : path;
    return base.length > 24 ? `${base.slice(0, 23)}…` : base;
  }
  return path.length > 24 ? `${path.slice(0, 23)}…` : path;
}

function shortClassName(name: string): string {
  const idx = name.lastIndexOf('.');
  const short = idx >= 0 ? name.slice(idx + 1) : name;
  return short.length > 24 ? `${short.slice(0, 23)}…` : short;
}

export interface LoadSunburstOpts {
  readonly hasSmaps: boolean;
  readonly hasNative: boolean;
  readonly hasJava: boolean;
  readonly selTs?: time;
}

// Loads a 2-ring hierarchy for the Overview memory sunburst:
//   - When smaps exists: Ring 1 = smaps domains, Ring 2 = file buckets / top
//     normalized smaps arenas at the selected (or latest) snapshot.
//   - When smaps is absent: Ring 1 = captured deep profilers (Native / Java),
//     Ring 2 = top attributed .so libraries (heapprofd) and/or top Java classes
//     (java_hprof).
export async function loadMemorySunburstData(
  trace: Trace,
  upid: number,
  opts: LoadSunburstOpts,
): Promise<MemorySunburstData> {
  if (opts.hasSmaps) {
    return loadSmapsSunburst(trace, upid, opts.selTs);
  }
  return loadProfilerFallbackSunburst(
    trace,
    upid,
    opts.hasNative,
    opts.hasJava,
  );
}

async function loadSmapsSunburst(
  trace: Trace,
  upid: number,
  selTs?: time,
): Promise<MemorySunburstData> {
  // Resolve all snapshot timestamps so we can pick the nearest snapshot to selTs
  // (or latest) and show an accurate snapshot label.
  const snapsRes = await trace.engine.query(`
    SELECT DISTINCT ts
    FROM profiler_smaps
    WHERE upid = ${upid}
    ORDER BY ts ASC
  `);
  const snapTsList: bigint[] = [];
  for (const it = snapsRes.iter({ts: LONG}); it.valid(); it.next()) {
    snapTsList.push(it.ts);
  }
  if (snapTsList.length === 0) {
    return {totalBytes: 0, metricLabel: 'RSS + Swap', domains: []};
  }

  let targetIdx = snapTsList.length - 1;
  if (selTs !== undefined) {
    let bestDist = BigInt('9223372036854775807');
    for (let i = 0; i < snapTsList.length; i++) {
      const d =
        snapTsList[i]! >= selTs
          ? snapTsList[i]! - selTs
          : selTs - snapTsList[i]!;
      if (d < bestDist) {
        bestDist = d;
        targetIdx = i;
      }
    }
  }
  const targetTs = snapTsList[targetIdx]!;
  const relS = Number(targetTs - trace.traceInfo.start) / 1e9;
  const sourceLabel =
    snapTsList.length > 1
      ? `Snapshot #${targetIdx + 1} · ${relS.toFixed(1)}s`
      : `Snapshot at ${relS.toFixed(1)}s`;

  const res = await trace.engine.query(`
    INCLUDE PERFETTO MODULE memscope.smaps;
    SELECT
      memscope_smaps_category(path) AS category,
      CASE
        WHEN memscope_smaps_category(path) = 'file'
          THEN memscope_smaps_file_bucket(path)
        ELSE ''
      END AS file_bucket,
      ifnull(path, '') AS path,
      CAST(ifnull(SUM(rss_kb + swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps
    WHERE upid = ${upid} AND ts = ${targetTs}
    GROUP BY category, file_bucket, path
    HAVING bytes > 0
    ORDER BY bytes DESC
  `);

  type RawEntry = {
    path: string;
    fileBucket: string;
    bytes: number;
  };
  const byCat = new Map<SunburstDomainId, RawEntry[]>();
  let totalBytes = 0;

  for (
    const it = res.iter({
      category: STR,
      file_bucket: STR,
      path: STR,
      bytes: NUM,
    });
    it.valid();
    it.next()
  ) {
    const cat = (
      it.category in DOMAIN_STYLES ? it.category : 'other'
    ) as SunburstDomainId;
    let list = byCat.get(cat);
    if (list === undefined) {
      list = [];
      byCat.set(cat, list);
    }
    list.push({path: it.path, fileBucket: it.file_bucket, bytes: it.bytes});
    totalBytes += it.bytes;
  }

  const domains: SunburstDomainSlice[] = [];
  for (const [cat, entries] of byCat.entries()) {
    const style = DOMAIN_STYLES[cat];
    const catTotal = entries.reduce((acc, e) => acc + e.bytes, 0);
    if (catTotal <= 0) continue;

    let children: SunburstChildSlice[];
    if (cat === 'file') {
      // Group file-backed mappings by semantic bucket, keeping largest file hint.
      const buckets = new Map<
        string,
        {bytes: number; topFile: string; topBytes: number; count: number}
      >();
      for (const e of entries) {
        const key = e.fileBucket || 'other_file';
        const cur = buckets.get(key);
        if (cur === undefined) {
          buckets.set(key, {
            bytes: e.bytes,
            topFile: e.path,
            topBytes: e.bytes,
            count: 1,
          });
        } else {
          cur.bytes += e.bytes;
          cur.count += 1;
          if (e.bytes > cur.topBytes) {
            cur.topFile = e.path;
            cur.topBytes = e.bytes;
          }
        }
      }
      const sorted = Array.from(buckets.entries()).sort(
        (a, b) => b[1].bytes - a[1].bytes,
      );
      children = sorted.map(([bKey, info], idx) => {
        const label = FILE_BUCKET_LABELS[bKey] ?? bKey;
        const topShort = shortSmapsLabel(info.topFile, 'file');
        return {
          id: `file:${bKey}`,
          name: label,
          fullLabel: label,
          domainId: 'file',
          domainLabel: style.label,
          bytes: info.bytes,
          color: childShade(style.color, idx),
          hint:
            info.count === 1
              ? topShort
              : `Largest: ${topShort} (${formatBytesIec(info.topBytes)}) · ${info.count} files`,
        };
      });
    } else {
      // Top 5 normalized arenas/mappings + collapsed tail.
      const MAX_CHILDREN = 5;
      const top = entries.slice(0, MAX_CHILDREN);
      const tailBytes = entries
        .slice(MAX_CHILDREN)
        .reduce((acc, e) => acc + e.bytes, 0);
      children = top.map((e, idx) => ({
        id: `${cat}:${idx}:${e.path}`,
        name: shortSmapsLabel(e.path, cat),
        fullLabel: e.path || '[anonymous]',
        domainId: cat,
        domainLabel: style.label,
        bytes: e.bytes,
        color: childShade(style.color, idx),
      }));
      if (tailBytes > 0) {
        const restCount = entries.length - MAX_CHILDREN;
        children.push({
          id: `${cat}:tail`,
          name: `Other (${restCount})`,
          fullLabel: `${restCount} other ${style.label.toLowerCase()} mappings`,
          domainId: cat,
          domainLabel: style.label,
          bytes: tailBytes,
          color: childShade(style.color, MAX_CHILDREN),
        });
      }
    }

    domains.push({
      id: cat,
      name: style.label,
      bytes: catTotal,
      color: style.color,
      children,
    });
  }

  domains.sort((a, b) => b.bytes - a.bytes);
  return {
    totalBytes,
    metricLabel: 'RSS + Swap',
    sourceLabel,
    domains,
  };
}

const NATIVE_LIB_PALETTE = [
  '#2563eb',
  '#0284c7',
  '#4f46e5',
  '#0d9488',
  '#7c3aed',
  '#64748b',
];

const JAVA_PKG_PALETTE = [
  '#d97706',
  '#ea580c',
  '#ca8a04',
  '#b45309',
  '#0284c7',
  '#64748b',
];

function shortSymbolLabel(raw: string): string {
  // Strip C++ parameter lists for concise sunburst labels (full name in tooltip).
  const noArgs = raw.replace(/\(.*$/, '').trim();
  const parts = noArgs.split('::');
  const tail = parts.length >= 2 ? parts.slice(-2).join('::') : noArgs;
  return tail.length > 22 ? `${tail.slice(0, 21)}…` : tail;
}

function javaPackageName(clsName: string): string {
  const idx = clsName.lastIndexOf('.');
  if (idx < 0) return '<default>';
  const pkg = clsName.slice(0, idx);
  const segs = pkg.split('.');
  return segs.length > 2 ? segs.slice(0, 2).join('.') : pkg;
}

async function queryNativeUnreleasedTotal(
  trace: Trace,
  upid: number,
): Promise<number> {
  const res = await trace.engine.query(`
    WITH per_site AS (
      SELECT callsite_id, SUM(size) AS net
      FROM heap_profile_allocation
      WHERE upid = ${upid}
      GROUP BY callsite_id
    )
    SELECT CAST(ifnull(SUM(CASE WHEN net > 0 THEN net ELSE 0 END), 0) AS INT) AS total
    FROM per_site
  `);
  return res.firstRow({total: NUM}).total;
}

async function queryLatestJavaClasses(
  trace: Trace,
  upid: number,
): Promise<{total: number; classes: {name: string; bytes: number}[]}> {
  const latestRes = await trace.engine.query(`
    SELECT MAX(graph_sample_ts) AS ts FROM heap_graph_object WHERE upid = ${upid}
  `);
  const latestTs = latestRes.firstRow({ts: LONG}).ts;
  const clsRes = await trace.engine.query(`
    SELECT
      coalesce(c.deobfuscated_name, c.name, '<unknown>') AS cls_name,
      CAST(SUM(o.self_size + o.native_size) AS INT) AS bytes
    FROM heap_graph_object o
    JOIN heap_graph_class c ON c.id = o.type_id
    WHERE o.upid = ${upid}
      AND o.graph_sample_ts = ${latestTs}
      AND o.reachable != 0
    GROUP BY cls_name
    HAVING bytes > 0
    ORDER BY bytes DESC
  `);
  const classes: {name: string; bytes: number}[] = [];
  let total = 0;
  for (
    const it = clsRes.iter({cls_name: STR, bytes: NUM});
    it.valid();
    it.next()
  ) {
    classes.push({name: it.cls_name, bytes: it.bytes});
    total += it.bytes;
  }
  return {total, classes};
}

function buildRingChildren(opts: {
  readonly domainId: SunburstDomainId;
  readonly domainLabel: string;
  readonly baseColor: string;
  readonly items: ReadonlyArray<{
    id: string;
    name: string;
    fullLabel?: string;
    bytes: number;
  }>;
  readonly maxItems: number;
  readonly tailBytes?: number;
  readonly tailLabel: (restCount: number) => string;
  readonly tailFullLabel?: string;
}): SunburstChildSlice[] {
  const top = opts.items.slice(0, opts.maxItems);
  const tail =
    opts.tailBytes ??
    opts.items.slice(opts.maxItems).reduce((acc, x) => acc + x.bytes, 0);
  const children: SunburstChildSlice[] = top.map((x, idx) => ({
    id: x.id,
    name: x.name,
    fullLabel: x.fullLabel,
    domainId: opts.domainId,
    domainLabel: opts.domainLabel,
    bytes: x.bytes,
    color: childShade(opts.baseColor, idx),
  }));
  if (tail > 0) {
    const restCount = Math.max(1, opts.items.length - opts.maxItems);
    children.push({
      id: `${opts.domainId}:${opts.domainLabel}:other`,
      name: opts.tailLabel(restCount),
      fullLabel: opts.tailFullLabel,
      domainId: opts.domainId,
      domainLabel: opts.domainLabel,
      bytes: tail,
      color: childShade(opts.baseColor, top.length),
    });
  }
  return children;
}

function buildTwoLevelGroupedDomains(opts: {
  readonly domainId: SunburstDomainId;
  readonly groups: ReadonlyArray<{
    readonly key: string;
    readonly name: string;
    readonly bytes: number;
    readonly items: ReadonlyArray<{
      id: string;
      name: string;
      fullLabel?: string;
      bytes: number;
    }>;
  }>;
  readonly totalBytes: number;
  readonly palette: ReadonlyArray<string>;
  readonly maxGroups: number;
  readonly maxPerGroup: number;
  readonly restGroupName: string;
  readonly restChildName: string;
  readonly restDomainLabel: string;
}): SunburstDomainSlice[] {
  const sorted = [...opts.groups].sort((a, b) => b.bytes - a.bytes);
  const domains: SunburstDomainSlice[] = [];
  let accounted = 0;

  sorted.slice(0, opts.maxGroups).forEach((g, idx) => {
    const color = opts.palette[idx % opts.palette.length]!;
    accounted += g.bytes;
    domains.push({
      id: opts.domainId,
      name: g.name,
      bytes: g.bytes,
      color,
      children: buildRingChildren({
        domainId: opts.domainId,
        domainLabel: g.name,
        baseColor: color,
        items: g.items,
        maxItems: opts.maxPerGroup,
        tailLabel: (n) => `Other (${n})`,
      }),
    });
  });

  const restBytes = Math.max(0, opts.totalBytes - accounted);
  if (restBytes > 0) {
    const restColor = opts.palette[opts.palette.length - 1]!;
    domains.push({
      id: opts.domainId,
      name: opts.restGroupName,
      bytes: restBytes,
      color: restColor,
      children: [
        {
          id: `${opts.domainId}:tail`,
          name: opts.restChildName,
          domainId: opts.domainId,
          domainLabel: opts.restDomainLabel,
          bytes: restBytes,
          color: childShade(restColor, 1),
        },
      ],
    });
  }
  return domains;
}

async function loadProfilerFallbackSunburst(
  trace: Trace,
  upid: number,
  hasNative: boolean,
  hasJava: boolean,
): Promise<MemorySunburstData> {
  if (hasNative && !hasJava) {
    try {
      const [nativeTotal, funcs] = await Promise.all([
        queryNativeUnreleasedTotal(trace, upid),
        loadNativeLibFunctionsForSunburst(trace, upid, 60),
      ]);
      if (nativeTotal > 0 && funcs.length > 0) {
        const byLib = new Map<
          string,
          {
            key: string;
            name: string;
            bytes: number;
            items: {
              id: string;
              name: string;
              fullLabel: string;
              bytes: number;
            }[];
          }
        >();
        for (const f of funcs) {
          let g = byLib.get(f.mapping);
          if (!g) {
            g = {key: f.mapping, name: f.shortMapping, bytes: 0, items: []};
            byLib.set(f.mapping, g);
          }
          g.bytes += f.bytes;
          g.items.push({
            id: `native-fn:${f.mapping}:${f.frame}`,
            name: shortSymbolLabel(f.frame),
            fullLabel: f.frame,
            bytes: f.bytes,
          });
        }
        return {
          totalBytes: nativeTotal,
          metricLabel: 'Unreleased',
          sourceLabel: 'heapprofd libraries → callsites',
          domains: buildTwoLevelGroupedDomains({
            domainId: 'native',
            groups: Array.from(byLib.values()),
            totalBytes: nativeTotal,
            palette: NATIVE_LIB_PALETTE,
            maxGroups: 5,
            maxPerGroup: 5,
            restGroupName: 'Other libs',
            restChildName: 'Other callsites',
            restDomainLabel: 'Native',
          }),
        };
      }
    } catch {
      // Fall through to generic fallback below.
    }
  }

  if (hasJava && !hasNative) {
    try {
      const {total: javaTotal, classes} = await queryLatestJavaClasses(
        trace,
        upid,
      );
      if (javaTotal > 0) {
        const byPkg = new Map<
          string,
          {
            key: string;
            name: string;
            bytes: number;
            items: {
              id: string;
              name: string;
              fullLabel: string;
              bytes: number;
            }[];
          }
        >();
        for (const c of classes) {
          const pkg = javaPackageName(c.name);
          let g = byPkg.get(pkg);
          if (!g) {
            g = {key: pkg, name: pkg, bytes: 0, items: []};
            byPkg.set(pkg, g);
          }
          g.bytes += c.bytes;
          g.items.push({
            id: `java-cls:${c.name}`,
            name: shortClassName(c.name),
            fullLabel: c.name,
            bytes: c.bytes,
          });
        }
        return {
          totalBytes: javaTotal,
          metricLabel: 'Java heap',
          sourceLabel: 'java_hprof packages → classes',
          domains: buildTwoLevelGroupedDomains({
            domainId: 'java',
            groups: Array.from(byPkg.values()),
            totalBytes: javaTotal,
            palette: JAVA_PKG_PALETTE,
            maxGroups: 5,
            maxPerGroup: 4,
            restGroupName: 'Other packages',
            restChildName: 'Other classes',
            restDomainLabel: 'Java',
          }),
        };
      }
    } catch {
      // Fall through to combined fallback below.
    }
  }

  const domains: SunburstDomainSlice[] = [];
  if (hasNative) {
    try {
      const [libs, nativeTotal] = await Promise.all([
        loadTopNativeLibsForSunburst(trace, upid, 6),
        queryNativeUnreleasedTotal(trace, upid),
      ]);
      if (nativeTotal > 0) {
        const style = DOMAIN_STYLES.native;
        const topSum = libs.slice(0, 6).reduce((acc, l) => acc + l.bytes, 0);
        domains.push({
          id: 'native',
          name: style.label,
          bytes: nativeTotal,
          color: style.color,
          children: buildRingChildren({
            domainId: 'native',
            domainLabel: style.label,
            baseColor: style.color,
            items: libs.map((l) => ({
              id: `native-lib:${l.mapping}`,
              name: l.shortName,
              fullLabel: l.mapping,
              bytes: l.bytes,
            })),
            maxItems: 6,
            tailBytes: Math.max(0, nativeTotal - topSum),
            tailLabel: () => 'Other libraries',
            tailFullLabel: 'Remaining attributed libraries',
          }),
        });
      }
    } catch {
      // Ignore query errors if native tables unavailable.
    }
  }

  if (hasJava) {
    try {
      const {total: javaTotal, classes} = await queryLatestJavaClasses(
        trace,
        upid,
      );
      if (javaTotal > 0) {
        const style = DOMAIN_STYLES.java;
        domains.push({
          id: 'java',
          name: style.label,
          bytes: javaTotal,
          color: style.color,
          children: buildRingChildren({
            domainId: 'java',
            domainLabel: style.label,
            baseColor: style.color,
            items: classes.map((c) => ({
              id: `java-cls:${c.name}`,
              name: shortClassName(c.name),
              fullLabel: c.name,
              bytes: c.bytes,
            })),
            maxItems: 6,
            tailLabel: (n) => `Other (${n})`,
            tailFullLabel: 'Other Java classes',
          }),
        });
      }
    } catch {
      // Ignore if heap_graph_object unavailable.
    }
  }

  domains.sort((a, b) => b.bytes - a.bytes);
  return {
    totalBytes: domains.reduce((acc, d) => acc + d.bytes, 0),
    metricLabel:
      hasNative && hasJava
        ? 'Profiled'
        : hasNative
          ? 'Unreleased'
          : 'Java heap',
    sourceLabel:
      hasNative && !hasJava
        ? 'heapprofd attributed libraries'
        : hasJava && !hasNative
          ? 'java_hprof reachable classes'
          : 'Profiled native + Java',
    domains,
  };
}

interface SunburstNodeMeta {
  readonly domainId: SunburstDomainId;
  readonly domainLabel: string;
  readonly label: string;
  readonly fullLabel?: string;
  readonly bytes: number;
  readonly parentBytes?: number;
  readonly color: string;
  readonly hint?: string;
  readonly isLeaf: boolean;
}

interface HoveredSlice {
  readonly domainId: SunburstDomainId;
  readonly label: string;
  readonly parentLabel?: string;
  readonly bytes: number;
  readonly pct: number;
  readonly color: string;
}

export interface MemorySunburstAttrs {
  readonly data: MemorySunburstData | undefined;
  readonly onOpenDomain: (id: SunburstDomainId) => void;
  readonly onHoverDomain?: (id: SunburstDomainId | undefined) => void;
  readonly height?: number;
}

// Interactive 2-ring ECharts sunburst chart with a live center-hole readout
// and click-to-open-domain navigation. The ECharts option is memoized on `data`
// so hover updates only redraw the lightweight HTML center readout without
// calling setOption or resetting ECharts emphasis.
export class MemorySunburst implements m.ClassComponent<MemorySunburstAttrs> {
  private hovered?: HoveredSlice;
  private cachedData?: MemorySunburstData;
  private cachedOption?: EChartsCoreOption;
  private cachedHandlers: ReadonlyArray<EChartEventHandler> = [];
  private latestAttrs?: MemorySunburstAttrs;

  private ensureOption(attrs: MemorySunburstAttrs): {
    option: EChartsCoreOption | undefined;
    handlers: ReadonlyArray<EChartEventHandler>;
  } {
    this.latestAttrs = attrs;
    const {data} = attrs;
    if (data === this.cachedData) {
      return {option: this.cachedOption, handlers: this.cachedHandlers};
    }
    this.cachedData = data;
    this.hovered = undefined;

    if (
      data === undefined ||
      data.totalBytes <= 0 ||
      data.domains.length === 0
    ) {
      this.cachedOption = undefined;
      this.cachedHandlers = [];
      return {option: undefined, handlers: []};
    }

    const total = data.totalBytes;
    const seriesData = data.domains.map((dom) => {
      const domMeta: SunburstNodeMeta = {
        domainId: dom.id,
        domainLabel: dom.name,
        label: dom.name,
        bytes: dom.bytes,
        color: dom.color,
        isLeaf: false,
      };
      return {
        name: dom.name,
        value: dom.bytes,
        meta: domMeta,
        itemStyle: {color: dom.color},
        label: {color: labelColorForHex(dom.color)},
        children: dom.children.map((ch) => {
          const chMeta: SunburstNodeMeta = {
            domainId: ch.domainId,
            domainLabel: ch.domainLabel,
            label: ch.name,
            fullLabel: ch.fullLabel,
            bytes: ch.bytes,
            parentBytes: dom.bytes,
            color: ch.color,
            hint: ch.hint,
            isLeaf: true,
          };
          return {
            name: ch.name,
            value: ch.bytes,
            meta: chMeta,
            itemStyle: {color: ch.color},
            label: {color: labelColorForHex(ch.color)},
          };
        }),
      };
    });

    this.cachedOption = {
      animation: false,
      tooltip: buildTooltipOption({
        trigger: 'item' as const,
        confine: true,
        extraCssText: 'pointer-events: none;',
        formatter: (params: {data?: {meta?: SunburstNodeMeta}}) => {
          const meta = params.data?.meta;
          if (meta === undefined) return '';
          const totalPct = total > 0 ? (meta.bytes / total) * 100 : 0;
          const title = meta.isLeaf
            ? `${meta.domainLabel} › ${meta.fullLabel ?? meta.label}`
            : meta.label;
          const lines: m.Children[] = [
            m('div', {style: {fontWeight: 600, marginBottom: '2px'}}, title),
            m(
              'div',
              `${formatBytesIec(meta.bytes)} (${totalPct.toFixed(1)}% of total)`,
            ),
          ];
          if (
            meta.isLeaf &&
            meta.parentBytes !== undefined &&
            meta.parentBytes > 0
          ) {
            const parentPct = (meta.bytes / meta.parentBytes) * 100;
            lines.push(
              m(
                'div',
                {style: {opacity: 0.85}},
                `${parentPct.toFixed(1)}% of ${meta.domainLabel}`,
              ),
            );
          }
          if (meta.hint) {
            lines.push(m('div', {style: {opacity: 0.8}}, meta.hint));
          }
          lines.push(
            m(
              'div',
              {style: {opacity: 0.65, fontSize: '11px', marginTop: '3px'}},
              `Click to open ${DOMAIN_STYLES[meta.domainId].label}`,
            ),
          );
          return renderTooltip(lines);
        },
      }),
      series: [
        {
          type: 'sunburst',
          data: seriesData,
          nodeClick: false,
          sort: undefined,
          center: ['50%', '50%'],
          radius: ['35%', '93%'],
          emphasis: {
            focus: 'ancestor',
          },
          levels: [
            {},
            {
              r0: '35%',
              r: '63%',
              itemStyle: {
                borderWidth: 2,
                borderRadius: 4,
              },
              label: {
                rotate: 'tangential',
                fontSize: 11,
                fontWeight: 600,
                minAngle: 22,
                overflow: 'truncate',
                width: 72,
              },
            },
            {
              r0: '64%',
              r: '93%',
              itemStyle: {
                borderWidth: 1.5,
                borderRadius: 3,
              },
              label: {
                rotate: 'tangential',
                fontSize: 10,
                fontWeight: 500,
                minAngle: 25,
                overflow: 'truncate',
                width: 58,
              },
            },
          ],
        },
      ],
    };

    const extractMeta = (params: unknown): SunburstNodeMeta | undefined => {
      const p = params as EChartClickParams;
      return (p.data as {meta?: SunburstNodeMeta} | undefined)?.meta;
    };

    this.cachedHandlers = [
      {
        eventName: 'click',
        handler: (params) => {
          const meta = extractMeta(params);
          if (meta !== undefined) {
            this.latestAttrs?.onOpenDomain(meta.domainId);
          }
        },
      },
      {
        eventName: 'mouseover',
        handler: (params) => {
          const meta = extractMeta(params);
          if (meta === undefined) return;
          this.hovered = {
            domainId: meta.domainId,
            label: meta.label,
            parentLabel: meta.isLeaf ? meta.domainLabel : undefined,
            bytes: meta.bytes,
            pct: total > 0 ? (meta.bytes / total) * 100 : 0,
            color: meta.color,
          };
          this.latestAttrs?.onHoverDomain?.(meta.domainId);
        },
      },
      {
        eventName: 'globalout',
        handler: () => {
          this.hovered = undefined;
          this.latestAttrs?.onHoverDomain?.(undefined);
        },
      },
    ];

    return {option: this.cachedOption, handlers: this.cachedHandlers};
  }

  view({attrs}: m.CVnode<MemorySunburstAttrs>): m.Children {
    const {data, height = 340} = attrs;
    const {option, handlers} = this.ensureOption(attrs);
    const isEmpty =
      data !== undefined && (data.totalBytes <= 0 || data.domains.length === 0);

    const h = this.hovered;
    return m('.pf-memscope-sunburst', [
      m('.pf-memscope-sunburst__chart', [
        m(EChartView, {
          option,
          height,
          empty: isEmpty,
          eventHandlers: handlers,
          resolveOption: (opt, colors) => {
            // Apply theme card background to slice separators so borders match
            // both light and dark modes cleanly.
            const raw = opt as {
              series?: Array<{
                levels?: Array<{itemStyle?: Record<string, unknown>}>;
              }>;
            };
            const s = raw.series?.[0];
            if (s?.levels) {
              const patchedLevels = s.levels.map((lvl, idx) =>
                idx === 0
                  ? lvl
                  : {
                      ...lvl,
                      itemStyle: {
                        ...lvl.itemStyle,
                        borderColor: colors.backgroundColor,
                      },
                    },
              );
              return {
                ...opt,
                series: [{...s, levels: patchedLevels}],
              };
            }
            return opt;
          },
        }),
        data !== undefined &&
          data.totalBytes > 0 &&
          m('.pf-memscope-sunburst__center', [
            h !== undefined
              ? [
                  h.parentLabel &&
                    m('.pf-memscope-sunburst__center-parent', h.parentLabel),
                  m('.pf-memscope-sunburst__center-name', h.label),
                  m(
                    '.pf-memscope-sunburst__center-val',
                    formatBytesIec(h.bytes),
                  ),
                  m(
                    '.pf-memscope-sunburst__center-sub',
                    `${h.pct.toFixed(1)}% of total`,
                  ),
                ]
              : [
                  m('.pf-memscope-sunburst__center-parent', data.metricLabel),
                  m(
                    '.pf-memscope-sunburst__center-val',
                    formatBytesIec(data.totalBytes),
                  ),
                  m('.pf-memscope-sunburst__center-sub', 'Click any slice'),
                ],
          ]),
      ]),
      data?.sourceLabel &&
        m('.pf-memscope-sunburst__caption', data.sourceLabel),
    ]);
  }
}
