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
import type {
  TraceEntry,
  Slice,
  MergedSlice,
  Verdict,
  OverviewFilter,
  SortState,
} from './models/types';
import {buildMergeCache, getCompressed} from './models/compression';
import type {MergeCache} from './models/compression';
import type {CrossCompareState} from './models/cross_compare';
import {
  createCrossCompareState,
  recordComparison as ccRecord,
  nextPair,
  nextPairForAnchor,
  getResults as ccResults,
  undoComparison as ccUndo,
  discardTrace as ccDiscard,
  skipCurrentPair as ccSkip,
} from './models/cross_compare';

export interface TraceState {
  // Immutable data
  trace: TraceEntry;
  _key: string;
  totalDur: number;
  origN: number;

  // Mutable compression cache (lazily initialized)
  _mergeCache: MergeCache | null;
  sliderValue: number;
  currentSeq: MergedSlice[];
}

export function traceKey(t: TraceEntry): string {
  const startupId = t.extra?.startup_id ?? '';
  return `${t.trace_uuid}|${t.package_name}|${startupId}|${t.startup_dur}`;
}

interface VerdictCounts {
  positive: number;
  negative: number;
  pending: number;
  discarded: number;
}

export interface Cluster {
  // -- Data --
  id: string;
  name: string;
  traces: TraceState[];
  verdicts: Map<string, Verdict>;
  counts: VerdictCounts;

  // -- View state --
  overviewFilter: OverviewFilter;
  splitView: boolean;
  splitFilters: [OverviewFilter, OverviewFilter];
  splitRatio: number;

  // -- Sort / filter state --
  tableSortState: Record<string, SortState>;
  sortField: 'index' | 'startup_dur';
  sortDir: 1 | -1;
  propFilters: Map<string, Set<string>>;

  // -- Compression state --
  globalSlider: number; // 1-100 percentage
}

function makeCluster(name: string, traces: TraceState[]): Cluster {
  return {
    id: crypto.randomUUID(),
    name,
    traces,
    verdicts: new Map(),
    overviewFilter: 'all',
    counts: {positive: 0, negative: 0, pending: traces.length, discarded: 0},
    tableSortState: {},
    splitView: false,
    splitFilters: ['pending', 'positive'],
    splitRatio: 0.5,
    sortField: 'index',
    sortDir: 1,
    propFilters: new Map(),
    globalSlider: 100,
  };
}

interface AppState {
  clusters: Cluster[];
  activeClusterId: string | null;
  importMsg: {text: string; ok: boolean} | null;
  loadProgress: {message: string; pct?: number} | null;
}

export const S: AppState = {
  clusters: [],
  activeClusterId: null,
  importMsg: null,
  loadProgress: null,
};

export function activeCluster(): Cluster | null {
  return S.clusters.find((c) => c.id === S.activeClusterId) ?? null;
}

export function recomputeCounts(cl: Cluster): void {
  let positive = 0;
  let negative = 0;
  let discarded = 0;
  for (const v of cl.verdicts.values()) {
    if (v === 'like') positive++;
    else if (v === 'dislike') negative++;
    else if (v === 'discard') discarded++;
  }
  cl.counts = {
    positive,
    negative,
    discarded,
    pending: cl.traces.length - positive - negative - discarded,
  };
}

export function initTraceLazy(trace: TraceEntry): TraceState {
  // Sort slices by timestamp to ensure totalDur computation is correct.
  const slices = trace.slices;
  slices.sort((a, b) => a.ts - b.ts);

  const totalDur =
    slices.length > 0
      ? slices.reduce((mx, d) => Math.max(mx, d.ts - slices[0].ts + d.dur), 0)
      : 0;
  return {
    trace,
    _key: traceKey(trace),
    _mergeCache: null,
    totalDur,
    origN: slices.length,
    sliderValue: slices.length,
    currentSeq: [],
  };
}

export function ensureCache(ts: TraceState): void {
  if (ts._mergeCache !== null) return;
  ts._mergeCache = buildMergeCache(ts.trace.slices);
  ts.currentSeq = getCompressed(ts._mergeCache, ts.origN, ts.sliderValue);
}

export function updateSlider(ts: TraceState, value: number): void {
  ensureCache(ts);
  ts.sliderValue = value;
  ts.currentSeq = getCompressed(ts._mergeCache!, ts.origN, value);
}

export function updateGlobalSlider(cl: Cluster, pct: number): void {
  cl.globalSlider = pct;
  const frac = pct / 100;
  for (const ts of cl.traces) {
    const target = Math.max(2, Math.round(2 + (ts.origN - 2) * frac));
    updateSlider(ts, target);
  }
  m.redraw();
}

export function addCluster(name: string, entries: TraceEntry[]): void {
  const allStates = entries.map(initTraceLazy);
  // Deduplicate by composite key
  const seen = new Set<string>();
  const states = allStates.filter((ts) => {
    if (seen.has(ts._key)) return false;
    seen.add(ts._key);
    return true;
  });
  const cl = makeCluster(name, states);
  if (states.length > 0) ensureCache(states[0]);
  S.clusters.push(cl);
  S.activeClusterId = cl.id;
  m.redraw();
}

export function loadSingleJson(
  data: Slice[],
  uuid?: string,
  pkg?: string,
  dur?: number,
): void {
  addCluster('Import', [
    {
      trace_uuid: uuid || crypto.randomUUID(),
      package_name: pkg || 'unknown',
      startup_dur: dur ?? 0,
      slices: data,
    },
  ]);
}

export function loadMultipleTraces(name: string, traces: TraceEntry[]): void {
  addCluster(name, traces);
}

/** Deep-copy filtered traces into a new independent tab, carrying over verdicts. */
export function copyFilteredToNewTab(
  sourceCl: Cluster,
  filteredStates: TraceState[],
): void {
  if (filteredStates.length === 0) return;
  const entries: TraceEntry[] = filteredStates.map((ts) => ({
    trace_uuid: ts.trace.trace_uuid,
    package_name: ts.trace.package_name,
    startup_dur: ts.trace.startup_dur,
    slices: ts.trace.slices,
    extra: ts.trace.extra ? {...ts.trace.extra} : undefined,
  }));
  const newStates = entries.map(initTraceLazy);
  const cl = makeCluster(sourceCl.name + ' (copy)', newStates);
  // Carry over verdicts from source
  for (const ts of newStates) {
    const v = sourceCl.verdicts.get(ts._key);
    if (v) cl.verdicts.set(ts._key, v);
  }
  recomputeCounts(cl);
  if (newStates.length > 0) ensureCache(newStates[0]);
  S.clusters.push(cl);
  S.activeClusterId = cl.id;
  m.redraw();
}

export function removeCluster(id: string): void {
  S.clusters = S.clusters.filter((c) => c.id !== id);
  if (S.activeClusterId === id) {
    S.activeClusterId = S.clusters.length > 0 ? S.clusters[0].id : null;
  }
  // Clean up cross-compare state if it belonged to the removed cluster.
  if (_ccState) {
    const remaining = S.clusters.find((c) =>
      c.traces.some((ts) => _ccState!.traceKeys.includes(ts._key)),
    );
    if (!remaining) _ccState = null;
  }
  m.redraw();
}

export function switchCluster(id: string): void {
  S.activeClusterId = id;
  m.redraw();
}

export function renameCluster(id: string, name: string): void {
  const cl = S.clusters.find((c) => c.id === id);
  if (cl && name.trim()) cl.name = name.trim();
  m.redraw();
}

export function setVerdict(cl: Cluster, uuid: string, verdict: Verdict): void {
  if (cl.verdicts.get(uuid) === verdict) {
    cl.verdicts.delete(uuid);
  } else {
    cl.verdicts.set(uuid, verdict);
  }
  recomputeCounts(cl);
  m.redraw();
}

function applyPropFilters(cl: Cluster, traces: TraceState[]): TraceState[] {
  if (cl.propFilters.size === 0) return traces;
  return traces.filter((ts) => {
    for (const [field, allowed] of cl.propFilters) {
      const val = traceFieldValue(ts, field);
      if (!allowed.has(val)) return false;
    }
    return true;
  });
}

function applySorting(cl: Cluster, traces: TraceState[]): TraceState[] {
  if (cl.sortField === 'index') return traces;
  const sorted = [...traces];
  sorted.sort(
    (a, b) => (a.trace.startup_dur - b.trace.startup_dur) * cl.sortDir,
  );
  return sorted;
}

export function filterTraces(
  cl: Cluster,
  filter: OverviewFilter,
): TraceState[] {
  let result: TraceState[];
  switch (filter) {
    case 'positive':
      result = cl.traces.filter((ts) => cl.verdicts.get(ts._key) === 'like');
      break;
    case 'negative':
      result = cl.traces.filter((ts) => cl.verdicts.get(ts._key) === 'dislike');
      break;
    case 'pending':
      result = cl.traces.filter((ts) => {
        const v = cl.verdicts.get(ts._key);
        return !v;
      });
      break;
    case 'discarded':
      result = cl.traces.filter((ts) => cl.verdicts.get(ts._key) === 'discard');
      break;
    default:
      result = cl.traces;
  }
  result = applyPropFilters(cl, result);
  return applySorting(cl, result);
}

export function filteredTraces(): TraceState[] {
  const cl = activeCluster();
  if (!cl) return [];
  return filterTraces(cl, cl.overviewFilter);
}

// Resolve a filterable field value -- checks top-level trace fields first, then extra
function traceFieldValue(ts: TraceState, field: string): string {
  const trace = ts.trace as unknown as Record<string, unknown>;
  if (field in trace) return String(trace[field] ?? '');
  return String(ts.trace.extra?.[field] ?? '');
}

// Collect unique values for a given field across all traces
export function getFieldValues(cl: Cluster, field: string): string[] {
  const vals = new Set<string>();
  for (const ts of cl.traces) {
    vals.add(traceFieldValue(ts, field));
  }
  return [...vals].sort();
}

// Only these fields appear in the filter dropdown
const FILTERABLE_FIELDS = [
  'startup_type',
  'package_name',
  'device_name',
  'unique_session_name',
];

// Get list of filterable extra fields that have multiple distinct values
export function getFilterableFields(cl: Cluster): string[] {
  return FILTERABLE_FIELDS.filter((field) => {
    const vals = new Set<string>();
    for (const ts of cl.traces) {
      vals.add(traceFieldValue(ts, field));
      if (vals.size >= 2) return true;
    }
    return false;
  });
}

export function togglePropFilter(
  cl: Cluster,
  field: string,
  value: string,
): void {
  let allowed = cl.propFilters.get(field);
  if (!allowed) {
    // First click: select only this value (deselect all others)
    allowed = new Set([value]);
    cl.propFilters.set(field, allowed);
  } else if (allowed.has(value)) {
    allowed.delete(value);
    if (allowed.size === 0) cl.propFilters.delete(field);
  } else {
    allowed.add(value);
    // If all values selected, remove filter entirely
    const all = getFieldValues(cl, field);
    if (allowed.size === all.length) cl.propFilters.delete(field);
  }
  m.redraw();
}

export function clearPropFilter(cl: Cluster, field: string): void {
  cl.propFilters.delete(field);
  m.redraw();
}

// -- Cross Compare --

let _ccState: CrossCompareState | null = null;

export function getCrossCompareState(): CrossCompareState | null {
  return _ccState;
}

export function startCrossCompare(cl: Cluster): void {
  const keys = cl.traces.map((ts) => ts._key);
  // Resume if trace set matches, otherwise start fresh
  if (
    _ccState &&
    _ccState.traceKeys.length === keys.length &&
    _ccState.traceKeys.every((k, i) => k === keys[i])
  ) {
    m.redraw();
    return;
  }
  _ccState = createCrossCompareState(keys);
  m.redraw();
}

export function closeCrossCompare(): void {
  _ccState = null;
  m.redraw();
}

function isPureAnchor(anchorKey: string): boolean {
  if (!_ccState || _ccState.history.length === 0) return false;
  return _ccState.history.every(
    (e) => e.type === 'discard' || e.keyA === anchorKey || e.keyB === anchorKey,
  );
}

function advancePair(anchorKey?: string): void {
  if (!_ccState) return;
  if (anchorKey && !_ccState.discardedKeys.has(anchorKey)) {
    _ccState.currentPair = nextPairForAnchor(_ccState, anchorKey);
    if (!_ccState.currentPair) {
      // Pure anchor session: all comparisons involved the anchor -> done
      if (!isPureAnchor(anchorKey)) {
        _ccState.currentPair = nextPair(_ccState);
      }
    }
  } else {
    _ccState.currentPair = nextPair(_ccState);
  }
  if (!_ccState.currentPair) _ccState.isComplete = true;
  _ccState.selectedSide = null;
}

export function recordCrossComparison(
  result: 'positive' | 'negative',
  anchorKey?: string,
): void {
  if (!_ccState || !_ccState.currentPair) return;
  const [a, b] = _ccState.currentPair;
  ccRecord(_ccState, a, b, result);
  advancePair(anchorKey);
  m.redraw();
}

export function skipCrossComparison(anchorKey?: string): void {
  if (!_ccState || !_ccState.currentPair) return;
  ccSkip(_ccState);
  advancePair(anchorKey);
  m.redraw();
}

export function undoCrossComparison(anchorKey?: string): void {
  if (!_ccState || _ccState.history.length === 0) return;
  ccUndo(_ccState);
  // Re-advance with anchor if set
  if (anchorKey && !_ccState.discardedKeys.has(anchorKey)) {
    const pair = nextPairForAnchor(_ccState, anchorKey);
    if (pair) {
      _ccState.currentPair = pair;
      _ccState.isComplete = false;
    }
  }
  m.redraw();
}

export function discardCrossCompareTrace(
  cl: Cluster,
  side: 'left' | 'right',
  anchorKey?: string,
): void {
  if (!_ccState || !_ccState.currentPair) return;
  const key =
    side === 'left' ? _ccState.currentPair[0] : _ccState.currentPair[1];
  cl.verdicts.set(key, 'discard');
  recomputeCounts(cl);
  ccDiscard(_ccState, key);
  advancePair(anchorKey);
  m.redraw();
}

export function applyCrossCompareResults(
  cl: Cluster,
  positiveIdx = 0,
  negativeIdx = 1,
): void {
  if (!_ccState) return;
  const {groups} = ccResults(_ccState);
  if (positiveIdx >= 0 && positiveIdx < groups.length) {
    for (const key of groups[positiveIdx]) cl.verdicts.set(key, 'like');
  }
  if (negativeIdx === -1) {
    // Pure anchor: all groups except positive -> negative
    for (let i = 0; i < groups.length; i++) {
      if (i === positiveIdx) continue;
      for (const key of groups[i]) cl.verdicts.set(key, 'dislike');
    }
  } else if (negativeIdx >= 0 && negativeIdx < groups.length) {
    for (const key of groups[negativeIdx]) {
      cl.verdicts.set(key, 'dislike');
    }
  }
  recomputeCounts(cl);
  _ccState = null;
  // Switch to split view: negative left, positive right
  cl.splitView = true;
  cl.splitFilters = ['negative', 'positive'];
  m.redraw();
}

export function resetCrossCompare(cl: Cluster): void {
  const keys = cl.traces.map((ts) => ts._key);
  _ccState = createCrossCompareState(keys);
  m.redraw();
}

// -- Session save / restore --

interface SessionData {
  version: 1;
  activeClusterId: string | null;
  clusters: {
    id: string;
    name: string;
    traces: TraceEntry[];
    verdicts: [string, Verdict][];
    overviewFilter: OverviewFilter;
    splitView: boolean;
    splitFilters: [OverviewFilter, OverviewFilter];
    splitRatio: number;
    sortField?: 'index' | 'startup_dur';
    sortDir?: 1 | -1;
    propFilters?: [string, string[]][];
    globalSlider?: number;
  }[];
}

export function exportSession(): string {
  const data: SessionData = {
    version: 1,
    activeClusterId: S.activeClusterId,
    clusters: S.clusters.map((cl) => ({
      id: cl.id,
      name: cl.name,
      traces: cl.traces.map((ts) => ts.trace),
      verdicts: [...cl.verdicts.entries()],
      overviewFilter: cl.overviewFilter,
      splitView: cl.splitView,
      splitFilters: cl.splitFilters,
      splitRatio: cl.splitRatio,
      sortField: cl.sortField,
      sortDir: cl.sortDir,
      propFilters: [...cl.propFilters.entries()].map(([k, v]) => [k, [...v]]),
      globalSlider: cl.globalSlider,
    })),
  };
  return JSON.stringify(data);
}

/** Hydrate pre-parsed session data into app state synchronously. */
export function importSessionData(data: SessionData): void {
  S.clusters = data.clusters.map((sc) => hydrateCluster(sc));
  S.activeClusterId = data.activeClusterId;
  m.redraw();
}

function hydrateCluster(sc: SessionData['clusters'][0]): Cluster {
  const traces = sc.traces.map(initTraceLazy);
  const cl: Cluster = {
    id: sc.id,
    name: sc.name,
    traces,
    verdicts: new Map(sc.verdicts),
    overviewFilter: sc.overviewFilter,
    counts: {positive: 0, negative: 0, pending: 0, discarded: 0},
    tableSortState: {},
    splitView: sc.splitView,
    splitFilters: sc.splitFilters,
    splitRatio: sc.splitRatio,
    sortField: sc.sortField || 'index',
    sortDir: sc.sortDir || 1,
    propFilters: new Map(
      (sc.propFilters || []).map(([k, v]) => [k, new Set(v)]),
    ),
    globalSlider: sc.globalSlider ?? 100,
  };
  recomputeCounts(cl);
  if (traces.length > 0) ensureCache(traces[0]);
  return cl;
}

/**
 * Hydrate session data asynchronously, yielding to the event loop
 * between clusters so the progress bar can update.
 */
export async function importSessionDataAsync(
  data: SessionData,
  onProgress?: (message: string, pct: number) => void,
): Promise<void> {
  const clusters: Cluster[] = [];
  const total = data.clusters.length;
  let processedTraces = 0;
  const totalTraces = data.clusters.reduce(
    (sum, sc) => sum + sc.traces.length,
    0,
  );

  for (let i = 0; i < total; i++) {
    const sc = data.clusters[i];
    onProgress?.(
      `Hydrating cluster ${i + 1}/${total}: ${sc.name} (${sc.traces.length} traces)`,
      totalTraces > 0
        ? (processedTraces / totalTraces) * 100
        : (i / total) * 100,
    );
    // Yield so the UI can repaint the progress bar
    await new Promise<void>((r) => setTimeout(r, 0));

    clusters.push(hydrateCluster(sc));
    processedTraces += sc.traces.length;
  }

  S.clusters = clusters;
  S.activeClusterId = data.activeClusterId;
  onProgress?.('Done', 100);
  m.redraw();
}

/** Parse + hydrate a session JSON string. Sync -- use parseSessionAsync for large files. */
export function importSession(json: string): void {
  const data: SessionData = JSON.parse(json);
  if (data.version !== 1) throw new Error('Unknown session version');
  importSessionData(data);
}
