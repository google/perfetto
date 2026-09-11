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

import {formatNumber} from '../../base/number_format';
import {type duration, type time, Time} from '../../base/time';
import {
  counterValueExpression,
  type YMode,
} from '../../components/tracks/counter_track';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createVirtualTable} from '../../trace_processor/sql_utils';

// Per-line stroke style.
export type LineStyle = 'solid' | 'dashed' | 'dotted';

// A counter with at most this many samples is loaded in full (all points) once
// and reused for every viewport: zoom/pan then just remaps existing points with
// no re-query and no re-bucketing, so it is perfectly smooth. Larger counters
// fall back to the per-viewport mipmap. The bound also keeps the drawn point
// count reasonable (only on-screen lanes are ever drawn).
const FULL_FETCH_MAX_ROWS = 50_000;

// Fixed colour choices for the per-line override picker. Index N is kept in
// sync with the .pf-tsv__color-dot--N classes in styles.scss.
export const COLOR_CHOICES: ReadonlyArray<string> = [
  '#4285f4',
  '#ea4335',
  '#fbbc04',
  '#34a853',
  '#ff6d01',
  '#46bdc6',
  '#9334e6',
  '#e91e63',
  '#00acc1',
  '#9e9e9e',
];

// The CSS class that renders a series' colour dot: the palette swatch for its
// index, or the matching override-colour dot when the user picked one.
export function swatchClassFor(
  colorOverride: string | undefined,
  i: number,
): string {
  const ci = colorOverride ? COLOR_CHOICES.indexOf(colorOverride) : -1;
  return ci >= 0
    ? `pf-tsv__color-dot--${ci}`
    : `pf-tsv__swatch--${(i % 8) + 1}`;
}

// How a counter line is labelled by default (before any per-line override).
export type NameBy = 'metric' | 'process' | 'thread';

// The owning process/thread of a counter (when it is process/thread-scoped).
export interface Owner {
  readonly name: string; // may be '' if the trace has no name
  readonly id: number; // pid or tid
}

// Metadata about a single counter track available in the trace.
export interface CounterTrackInfo {
  readonly id: number;
  readonly name: string; // the metric name, e.g. "mem.rss.anon"
  readonly unit?: string;
  readonly type?: string;
  readonly rows: number;
  // Owner, if this is a process- or thread-scoped counter (used for naming).
  readonly process?: Owner;
  readonly thread?: Owner;
  // True if the counter only ever increases (a cumulative/monotonic total);
  // such counters are far more informative shown as a rate than a ramp.
  readonly cumulative: boolean;
}

// The default y-mode for a freshly added counter: cumulative counters default
// to 'rate' (their raw value is an uninformative ramp), everything else to
// 'value'.
export function defaultYMode(info: CounterTrackInfo): YMode {
  return info.cumulative ? 'rate' : 'value';
}

// Units that base/number_format already prefixes well; for these formatMetric
// defers to it, everything else uses the SI prefixes below.
const SI_NATIVE_UNITS = new Set([
  'Hz',
  'W',
  'V',
  'A',
  'J',
  'B',
  'b',
  'bytes',
  'bits',
  'seconds',
  'hertz',
  'watts',
  'volts',
  'amps',
  'joules',
]);

const SI_PREFIXES: ReadonlyArray<[number, string]> = [
  [1e12, 'T'],
  [1e9, 'G'],
  [1e6, 'M'],
  [1e3, 'K'],
  [1, ''],
  [1e-3, 'm'],
  [1e-6, 'µ'],
  [1e-9, 'n'],
];

// Compact metric formatting: 2e8 -> "200M", 1536 -> "1.5K", 85 -> "85".
function siCompact(value: number): string {
  if (!Number.isFinite(value)) return String(value);
  if (value === 0) return '0';
  const abs = Math.abs(value);
  for (const [factor, prefix] of SI_PREFIXES) {
    if (abs >= factor) {
      const m = value / factor;
      const am = Math.abs(m);
      const s =
        am >= 100 ? m.toFixed(0) : am >= 10 ? m.toFixed(1) : m.toFixed(2);
      return s.replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1') + prefix;
    }
  }
  return value.toExponential(1); // sub-nano: rare, keep it terse
}

// Axis/tooltip value formatter: base/number_format for units it prefixes well
// (bytes -> "200 MB"), else metric SI with the raw unit appended ("200M", not
// the engineering "200e6").
export function formatMetric(value: number, unit?: string): string {
  if (unit !== undefined && SI_NATIVE_UNITS.has(unit)) {
    return formatNumber(value, unit);
  }
  const num = siCompact(value);
  return unit !== undefined && unit.length > 0 ? `${num} ${unit}` : num;
}

// The unit for a counter: the counter_track.unit column when present, otherwise
// inferred from a common trailing unit suffix in the name (e.g. "Heap size
// (KB)" -> "KB"), which many Android counters use in lieu of the column.
export function counterUnit(
  rawUnit: string | undefined,
  name: string,
): string | undefined {
  if (rawUnit !== undefined && rawUnit.length > 0) return rawUnit;
  const m = /\((KB|MB|GB|bytes|kB|ns|us|ms|%|Hz|kHz|MHz)\)\s*$/.exec(name);
  return m ? m[1] : undefined;
}

// A process/thread owner formatted as "name pid" (or "process pid" when the
// trace carries no name).
function ownerLabel(o: Owner, kind: 'process' | 'thread'): string {
  return o.name.length > 0 ? `${o.name} ${o.id}` : `${kind} ${o.id}`;
}

// The display label for a counter under the given naming scheme. Process/Thread
// fall back to the metric name for counters with no such owner.
export function counterLabel(info: CounterTrackInfo, nameBy: NameBy): string {
  const proc = info.process && ownerLabel(info.process, 'process');
  const thr = info.thread && ownerLabel(info.thread, 'thread');
  switch (nameBy) {
    case 'process':
      return proc ?? thr ?? info.name;
    case 'thread':
      return thr ?? proc ?? info.name;
    case 'metric':
    default:
      return info.name;
  }
}

// A fully-disambiguating descriptor (owner + metric) for the picker, so
// same-named counters (e.g. mem.rss.anon across processes) are always
// distinguishable there regardless of the chosen naming scheme.
export function counterDescriptor(info: CounterTrackInfo): string {
  const proc = info.process && ownerLabel(info.process, 'process');
  const thr = info.thread && ownerLabel(info.thread, 'thread');
  if (thr !== undefined) return `${thr} · ${info.name}`;
  if (proc !== undefined) return `${proc} · ${info.name}`;
  return info.name;
}

// A window of downsampled samples for one counter, ready for rendering.
// Timestamps are stored as nanoseconds relative to the trace start so they fit
// comfortably in a float64 even for multi-week traces.
export interface SeriesWindow {
  readonly start: time;
  readonly end: time;
  readonly tsRel: Float64Array; // ns since trace start, ascending
  readonly minV: Float32Array;
  readonly maxV: Float32Array;
  readonly lastV: Float32Array;
  readonly count: number;
}

// Lists all counter tracks that actually carry samples, most-populated first.
// A single grouped scan over `counter` keeps this fast even with hundreds of
// tracks in weeks-long traces.
export async function listCounterTracks(
  engine: Engine,
): Promise<CounterTrackInfo[]> {
  // Rank by the number of DISTINCT values, which is a good proxy for "looks
  // like a real time series": smooth counters (memory, frequency, heap size,
  // temperature) have many distinct values, while binary/state counters
  // (cpuidle, *_IDLE) have very few and are pushed down. Dead-constant counters
  // carry no signal, so they are dropped entirely.
  // LEFT JOIN the process/thread that owns each counter (when any), so lines can
  // be labelled by their owner — the effective process is the process counter's
  // own, or the owning thread's process for thread counters.
  const res = await engine.query(`
    SELECT
      ct.id AS id,
      ct.name AS name,
      ct.unit AS unit,
      ct.type AS type,
      s.c AS rows,
      s.decs AS decs,
      p.pid AS pid,
      p.name AS pname,
      th.tid AS tid,
      th.name AS tname
    FROM counter_track ct
    JOIN (
      -- Per-track stats in one scan. decs counts value decreases (via LAG);
      -- 0 decreases over a varying counter => monotonic/cumulative.
      SELECT track_id, COUNT(*) AS c, MIN(value) AS mn, MAX(value) AS mx,
             COUNT(DISTINCT value) AS nd,
             SUM(dec) AS decs
      FROM (
        SELECT track_id, value,
          (value < LAG(value) OVER (PARTITION BY track_id ORDER BY ts)) AS dec
        FROM counter
      )
      GROUP BY track_id
    ) s ON s.track_id = ct.id
    LEFT JOIN process_counter_track pct ON pct.id = ct.id
    LEFT JOIN thread_counter_track tct ON tct.id = ct.id
    LEFT JOIN thread th ON th.utid = tct.utid
    LEFT JOIN process p ON p.upid = COALESCE(pct.upid, th.upid)
    WHERE s.c > 0 AND s.mx > s.mn
    ORDER BY s.nd DESC, s.c DESC, ct.name ASC
  `);

  const it = res.iter({
    id: NUM,
    name: STR_NULL,
    unit: STR_NULL,
    type: STR_NULL,
    rows: NUM,
    decs: NUM,
    pid: NUM_NULL,
    pname: STR_NULL,
    tid: NUM_NULL,
    tname: STR_NULL,
  });

  const out: CounterTrackInfo[] = [];
  for (; it.valid(); it.next()) {
    const name = it.name ?? `counter_track ${it.id}`;
    out.push({
      id: it.id,
      name,
      unit: counterUnit(it.unit ?? undefined, name),
      type: it.type ?? undefined,
      rows: it.rows,
      cumulative: it.decs === 0,
      process: it.pid !== null ? {name: it.pname ?? '', id: it.pid} : undefined,
      thread: it.tid !== null ? {name: it.tname ?? '', id: it.tid} : undefined,
    });
  }
  return out;
}

// One counter's data source. Lazily builds a mipmap virtual table the first
// time it is queried, then answers per-viewport requests in O(pixels) rows by
// asking the mipmap for min/max/last per bucket. Cheap to keep hundreds of
// these around; only the selected ones ever build a table.
export class CounterSeries {
  private tableName?: string;
  private disposeTable?: () => Promise<void>;
  private fetchSeq = 0;
  private lastKey = '';
  // True once the whole series has been loaded (small counters); the chart then
  // never needs to refetch on zoom/pan.
  private full = false;

  // Whether this series is hidden (legend / double-click toggle).
  hidden = false;
  // Emphasised (stacked-mode multi-select): when any lane is emphasised, the
  // rest are greyed and sink to the bottom.
  emphasized = false;
  // Per-line appearance overrides (undefined colour = palette default).
  lineStyle: LineStyle = 'solid';
  colorOverride?: string;
  // Per-line display-name override (undefined = derived from the naming scheme).
  nameOverride?: string;
  // Per-lane height override in px (stacked mode; undefined = the global height).
  laneHeightOverride?: number;

  // The line's display name: the explicit override, else derived from the
  // counter's owner/metric under the current naming scheme.
  label(nameBy: NameBy): string {
    return this.nameOverride ?? counterLabel(this.info, nameBy);
  }

  // The most recently fetched window, kept so the chart can keep drawing
  // (re-mapping by time) while a refined fetch is in flight.
  window?: SeriesWindow;

  // Whole-trace value range (in the current mode), for the "global" y-range
  // option. `globalMode` records which mode it was computed for so a mode switch
  // recomputes it.
  globalMin?: number;
  globalMax?: number;
  globalMode?: YMode;

  constructor(
    private readonly engine: Engine,
    readonly info: CounterTrackInfo,
    private readonly traceStart: time,
    private yMode: YMode = 'value',
  ) {}

  private async ensureTable(): Promise<string> {
    if (this.tableName !== undefined) return this.tableName;
    // The mipmap is built over the mode's display value (value/delta/rate),
    // so bucket min/max/last already reflect the selected mode.
    const table = await createVirtualTable({
      engine: this.engine,
      using: `__intrinsic_counter_mipmap((
        SELECT ts, ${counterValueExpression(this.yMode)} AS value
        FROM (SELECT ts, value FROM counter WHERE track_id = ${this.info.id})
      ))`,
    });
    this.tableName = table.name;
    this.disposeTable = async () => {
      await table[Symbol.asyncDispose]();
    };
    return this.tableName;
  }

  // This series' current value/delta/rate mode.
  get mode(): YMode {
    return this.yMode;
  }

  // Switches value/delta/rate; drops the mipmap so it rebuilds on next fetch.
  async setYMode(mode: YMode): Promise<void> {
    if (mode === this.yMode) return;
    this.yMode = mode;
    this.fetchSeq++;
    this.window = undefined;
    this.lastKey = '';
    this.full = false;
    if (this.disposeTable !== undefined) {
      await this.disposeTable();
      this.disposeTable = undefined;
      this.tableName = undefined;
    }
  }

  // Whether this counter is loaded in full (small enough to skip the mipmap).
  get isFull(): boolean {
    return this.info.rows <= FULL_FETCH_MAX_ROWS;
  }

  // Loads every sample once and caches it; subsequent viewports reuse it with
  // no re-query, so zoom/pan is perfectly smooth. min == max == last (no
  // coalescing); the chart derives the on-screen value range itself.
  private async fetchAll(): Promise<boolean> {
    if (this.full && this.window !== undefined) return false;
    const seq = ++this.fetchSeq;
    const res = await this.engine.query(`
      SELECT ts, ${counterValueExpression(this.yMode)} AS v
      FROM (SELECT ts, value FROM counter WHERE track_id = ${this.info.id})
      ORDER BY ts
    `);
    if (seq !== this.fetchSeq) return false;

    const cols = res.decodeColumns({ts: LONG, v: NUM});
    const count = res.numRows();
    const tsRel = new Float64Array(count);
    const v = new Float32Array(cols.v);
    const startRaw = this.traceStart;
    for (let i = 0; i < count; i++) {
      tsRel[i] = Number(cols.ts[i] - startRaw);
    }
    const first = count > 0 ? this.relToTime(tsRel[0]) : this.traceStart;
    const last = count > 0 ? this.relToTime(tsRel[count - 1]) : this.traceStart;
    // min/max/last share one array — the samples are the raw values.
    this.window = {
      start: first,
      end: last,
      tsRel,
      minV: v,
      maxV: v,
      lastV: v,
      count,
    };
    this.full = true;
    this.lastKey = 'full';
    return true;
  }

  // Whole-trace value range in the current mode (for the "global"/"shared"
  // y-range options), cached until the mode changes. Full counters derive it
  // from their samples; larger ones ask the mipmap in one coarse query. Returns
  // true if it changed.
  async ensureGlobalRange(traceEnd: time): Promise<boolean> {
    if (this.globalMode === this.yMode && this.globalMin !== undefined) {
      return false;
    }
    if (this.isFull) {
      if (!this.full || this.window === undefined) await this.fetchAll();
      const w = this.window;
      if (w === undefined || w.count === 0) return false;
      let mn = Infinity;
      let mx = -Infinity;
      for (let i = 0; i < w.count; i++) {
        if (w.minV[i] < mn) mn = w.minV[i];
        if (w.maxV[i] > mx) mx = w.maxV[i];
      }
      this.globalMin = mn;
      this.globalMax = mx;
      this.globalMode = this.yMode;
      return true;
    }
    const table = await this.ensureTable();
    const seq = this.fetchSeq;
    const dur = traceEnd - this.traceStart;
    const step = dur > 0n ? dur : 1n; // one bucket spanning the whole trace
    const res = await this.engine.query(`
      SELECT MIN(min_value) AS mn, MAX(max_value) AS mx
      FROM ${table}(${this.traceStart}, ${traceEnd}, ${step})
    `);
    if (seq !== this.fetchSeq) return false;
    const row = res.firstRow({mn: NUM_NULL, mx: NUM_NULL});
    if (row.mn === null || row.mx === null) return false;
    this.globalMin = row.mn;
    this.globalMax = row.mx;
    this.globalMode = this.yMode;
    return true;
  }

  // Fetches (or refetches) the given window at the given bucket resolution.
  // Returns true if `window` was updated, false if the request was superseded
  // by a newer one or was identical to the last satisfied request.
  async fetch(start: time, end: time, resolution: duration): Promise<boolean> {
    if (this.isFull) return this.fetchAll();
    const key = `${start}/${end}/${resolution}`;
    if (key === this.lastKey && this.window !== undefined) return false;
    const seq = ++this.fetchSeq;

    const table = await this.ensureTable();
    const res = await this.engine.query(`
      SELECT
        min_value AS minV,
        max_value AS maxV,
        last_ts AS ts,
        last_value AS lastV
      FROM ${table}(${start}, ${end}, ${resolution})
      ORDER BY last_ts
    `);
    if (seq !== this.fetchSeq) return false; // superseded

    const cols = res.decodeColumns({
      ts: LONG,
      minV: NUM,
      maxV: NUM,
      lastV: NUM,
    });
    const count = res.numRows();
    const tsRel = new Float64Array(count);
    const minV = new Float32Array(cols.minV);
    const maxV = new Float32Array(cols.maxV);
    const lastV = new Float32Array(cols.lastV);

    const startRaw = this.traceStart;
    for (let i = 0; i < count; i++) {
      tsRel[i] = Number(cols.ts[i] - startRaw);
    }

    this.lastKey = key;
    this.window = {start, end, tsRel, minV, maxV, lastV, count};
    return true;
  }

  async dispose(): Promise<void> {
    this.fetchSeq++;
    this.window = undefined;
    this.full = false;
    if (this.disposeTable !== undefined) {
      await this.disposeTable();
      this.disposeTable = undefined;
      this.tableName = undefined;
    }
  }

  // Absolute timestamp (ns since trace start) helper for the trace start, so
  // callers can convert between rel/abs consistently.
  relToTime(rel: number): time {
    return Time.fromRaw(this.traceStart + BigInt(Math.round(rel)));
  }
}
