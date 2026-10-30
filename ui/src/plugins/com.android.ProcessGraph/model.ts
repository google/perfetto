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

import {type CancellationSignal, TASK_CANCELLED} from '../../base/async_memo';
import {Time, type time} from '../../base/time';
import {
  computeBarycenterOrder,
  DEFAULT_GRAPH_CANVAS_W,
  DEFAULT_GRAPH_LEFT_MARGIN_X,
  DEFAULT_GRAPH_ROW_LABEL_X,
} from '../../components/widgets/charts/graph_chart';
import type {Trace} from '../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export type EdgeKind = 'service' | 'provider';
export type StepMode = 'edge' | 'any' | 1000 | 5000 | 10000 | 60000;

export interface ProcessInfo {
  readonly upid: number;
  readonly pid: number;
  readonly name: string;
  readonly uid?: number;
  readonly packageName?: string;
}

export interface StateInterval {
  readonly upid: number;
  readonly ts: time;
  readonly endTs: time;
  readonly state: string;
  readonly stateRank: number;
  readonly reason?: string;
}

export interface ConnectionInterval {
  readonly kind: EdgeKind;
  readonly ts: time;
  readonly endTs: time;
  readonly clientUpid: number;
  readonly serverUpid: number;
  readonly label?: string;
  readonly detail?: string;
}

export interface ProcessGraphFilters {
  readonly showServices: boolean;
  readonly showProviders: boolean;
  readonly showSelfLoops: boolean;
  readonly showIsolated: boolean;
  readonly searchQuery: string;
}

export interface SnapshotNode {
  readonly upid: number;
  readonly pid: number;
  readonly name: string;
  readonly uid?: number;
  readonly packageName?: string;
  readonly state: string;
  readonly stateRank: number;
  readonly reason?: string;
}

export interface CollapsedEdge {
  readonly key: string;
  readonly kind: EdgeKind;
  readonly client: number;
  readonly server: number;
  readonly count: number;
  readonly components: ReadonlyArray<string>;
  readonly details: ReadonlyArray<string>;
}

export interface GraphSnapshot {
  readonly ts: time;
  readonly useTraceColumns: boolean;
  readonly allNodesMap: ReadonlyMap<number, SnapshotNode>;
  readonly rawEdges: ReadonlyArray<CollapsedEdge>;
  readonly nodes: ReadonlyArray<SnapshotNode>;
  readonly edges: ReadonlyArray<CollapsedEdge>;
}

export interface ProcessGraphInput {
  readonly traceStart: time;
  readonly traceEnd: time;
  readonly processes: ReadonlyArray<ProcessInfo>;
  readonly stateIntervals: ReadonlyArray<StateInterval>;
  readonly connections: ReadonlyArray<ConnectionInterval>;
}

export interface ProcessGraphModel {
  readonly traceStart: time;
  readonly traceEnd: time;
  readonly procsByUpid: ReadonlyMap<number, ProcessInfo>;
  readonly stateByUpid: ReadonlyMap<number, ReadonlyArray<StateInterval>>;
  readonly connections: ReadonlyArray<ConnectionInterval>;
  readonly globalRanks: ReadonlyArray<number>;
  readonly globalRankToState: ReadonlyMap<number, string>;
  readonly canvasW: number;
  readonly canvasH: number;
  readonly fixedRowY: ReadonlyMap<number, number>;
  readonly traceConnX: ReadonlyMap<number, number>;
  readonly fullX: ReadonlyMap<number, number>;
  readonly edgeEvents: ReadonlyArray<time>;
  readonly anyEvents: ReadonlyArray<time>;
}

export interface StateStyle {
  readonly fill: string;
  readonly lightText: string;
  readonly short: string;
}

const UNKNOWN_RANK = 999;
const DEFAULT_FALLBACK_RANK = 19;
export const GRAPH_CANVAS_W = DEFAULT_GRAPH_CANVAS_W;
export const GRAPH_LEFT_MARGIN_X = DEFAULT_GRAPH_LEFT_MARGIN_X;
export const GRAPH_ROW_LABEL_X = DEFAULT_GRAPH_ROW_LABEL_X;

const STATE_STYLES: Readonly<
  Record<string, readonly [fill: string, lightText: string, short?: string]>
> = {
  PERSISTENT: ['#ab47bc', '#7b1fa2'],
  PERSISTENT_UI: ['#ba68c8', '#8e24aa'],
  TOP: ['#ef5350', '#c62828'],
  BOUND_TOP: ['#ff7043', '#d84315'],
  FOREGROUND_SERVICE: ['#ffa726', '#c25e00', 'FGS'],
  BOUND_FOREGROUND_SERVICE: ['#ffca28', '#9e6b00', 'BOUND_FGS'],
  IMPORTANT_FOREGROUND: ['#ffee58', '#827717', 'IMP_FG'],
  IMPORTANT_BACKGROUND: ['#d4e157', '#558b2f', 'IMP_BG'],
  TRANSIENT_BACKGROUND: ['#9ccc65', '#33691e', 'TRANSIENT_BG'],
  BACKUP: ['#66bb6a', '#2e7d32'],
  SERVICE: ['#26a69a', '#00695c'],
  RECEIVER: ['#26c6da', '#00838f'],
  TOP_SLEEPING: ['#29b6f6', '#0277bd'],
  HEAVY_WEIGHT: ['#42a5f5', '#1565c0'],
  HOME: ['#5c6bc0', '#283593'],
  LAST_ACTIVITY: ['#7e57c2', '#4527a0'],
  CACHED_ACTIVITY: ['#a1887f', '#5d4037', 'CACHED_ACT'],
  CACHED_ACTIVITY_CLIENT: ['#bcaaa4', '#6d4c41', 'CACHED_ACT_CLI'],
  CACHED_RECENT: ['#90a4ae', '#455a64'],
  CACHED_EMPTY: ['#78909c', '#37474f'],
  NONEXISTENT: ['#607d8b', '#455a64'],
  EXITED: ['#78909c', '#455a64'],
  UNKNOWN: ['#9e9e9e', '#616161'],
};

export function stateStyle(state: string): StateStyle {
  const entry = STATE_STYLES[state];
  return {
    fill: entry?.[0] ?? '#9e9e9e',
    lightText: entry?.[1] ?? '#475569',
    short: entry?.[2] ?? state,
  };
}

export function yForRank(model: ProcessGraphModel, rank: number): number {
  return model.fixedRowY.get(rank) ?? model.canvasH + 55;
}

export function upperBoundTime(arr: ReadonlyArray<time>, t: time): number {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    const val = arr[mid];
    if (val !== undefined && val <= t) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

function dedupSortedTimes(raw: time[]): ReadonlyArray<time> {
  raw.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const out: time[] = [];
  for (const t of raw) {
    if (out.length === 0 || out[out.length - 1] !== t) {
      out.push(t);
    }
  }
  return out;
}

export function buildProcessGraphModel(
  input: ProcessGraphInput,
): ProcessGraphModel {
  const {traceStart, traceEnd} = input;
  const procsByUpid = new Map<number, ProcessInfo>();
  for (const p of input.processes) {
    procsByUpid.set(p.upid, p);
  }

  const globalRankToState = new Map<number, string>();
  const stateByUpidMut = new Map<number, StateInterval[]>();
  const dominantRank = new Map<number, number>();
  const bestDur = new Map<number, bigint>();

  for (const s of input.stateIntervals) {
    globalRankToState.set(s.stateRank, s.state);
    let list = stateByUpidMut.get(s.upid);
    if (list === undefined) {
      list = [];
      stateByUpidMut.set(s.upid, list);
    }
    list.push(s);

    const dur = s.endTs - s.ts;
    const prevBest = bestDur.get(s.upid);
    if (prevBest === undefined || dur > prevBest) {
      bestDur.set(s.upid, dur);
      dominantRank.set(s.upid, s.stateRank);
    }
  }

  for (const list of stateByUpidMut.values()) {
    list.sort((a, b) => (a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0));
  }

  for (const c of input.connections) {
    if (
      !stateByUpidMut.has(c.clientUpid) ||
      !stateByUpidMut.has(c.serverUpid)
    ) {
      globalRankToState.set(UNKNOWN_RANK, 'UNKNOWN');
    }
    for (const u of [c.clientUpid, c.serverUpid]) {
      if (!procsByUpid.has(u)) {
        procsByUpid.set(u, {upid: u, pid: 0, name: `upid ${u}`});
      }
    }
  }

  if (globalRankToState.size === 0) {
    globalRankToState.set(UNKNOWN_RANK, 'UNKNOWN');
  }

  const globalRanks = [...globalRankToState.keys()].sort((a, b) => a - b);
  const canvasW = GRAPH_CANVAS_W;
  const canvasH = Math.max(520, (globalRanks.length - 1) * 40);
  const fixedRowY = new Map<number, number>();
  const denom = Math.max(1, globalRanks.length - 1);
  for (let i = 0; i < globalRanks.length; i++) {
    const r = globalRanks[i];
    if (r !== undefined) {
      fixedRowY.set(r, 30 + (i / denom) * canvasH);
    }
  }

  const sortedConnections = [...input.connections].sort((a, b) =>
    a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0,
  );

  const connUpidSet = new Set<number>();
  for (const c of sortedConnections) {
    connUpidSet.add(c.clientUpid);
    connUpidSet.add(c.serverUpid);
  }
  const allActiveUpids = new Set<number>(connUpidSet);
  for (const u of stateByUpidMut.keys()) {
    allActiveUpids.add(u);
  }

  const order = computeBarycenterOrder(
    [...allActiveUpids].map((u) => ({
      id: String(u),
      layer: dominantRank.get(u) ?? DEFAULT_FALLBACK_RANK,
      tieBreak: `${procsByUpid.get(u)?.name ?? ''}\0${String(u).padStart(10, '0')}`,
    })),
    sortedConnections.map((c) => ({
      source: String(c.clientUpid),
      target: String(c.serverUpid),
    })),
  );

  const sortedTraceUpids = [...connUpidSet].sort(
    (a, b) =>
      (order.normX.get(String(a)) ?? 0) - (order.normX.get(String(b)) ?? 0) ||
      a - b,
  );
  const traceConnX = new Map<number, number>();
  const connLen = Math.max(1, sortedTraceUpids.length);
  for (let i = 0; i < sortedTraceUpids.length; i++) {
    const u = sortedTraceUpids[i];
    if (u !== undefined) {
      traceConnX.set(u, GRAPH_LEFT_MARGIN_X + ((i + 0.5) / connLen) * canvasW);
    }
  }

  const fullX = new Map<number, number>();
  for (const layer of order.layers) {
    const len = Math.max(1, layer.length);
    for (let i = 0; i < layer.length; i++) {
      const id = layer[i];
      if (id !== undefined) {
        fullX.set(
          Number(id),
          GRAPH_LEFT_MARGIN_X + ((i + 0.5) / len) * canvasW,
        );
      }
    }
  }

  const openSentinel = Time.fromRaw(traceEnd + 1n);
  const rawEdgeEvents: time[] = [traceStart];
  for (const c of sortedConnections) {
    rawEdgeEvents.push(Time.clamp(c.ts, traceStart, traceEnd));
    if (c.endTs < openSentinel) {
      rawEdgeEvents.push(Time.clamp(c.endTs, traceStart, traceEnd));
    }
  }
  const edgeEvents = dedupSortedTimes(rawEdgeEvents);

  const rawAnyEvents: time[] = [...edgeEvents];
  for (const s of input.stateIntervals) {
    rawAnyEvents.push(Time.clamp(s.ts, traceStart, traceEnd));
    if (s.endTs < openSentinel) {
      rawAnyEvents.push(Time.clamp(s.endTs, traceStart, traceEnd));
    }
  }
  const anyEvents = dedupSortedTimes(rawAnyEvents);

  return {
    traceStart,
    traceEnd,
    procsByUpid,
    stateByUpid: stateByUpidMut,
    connections: sortedConnections,
    globalRanks,
    globalRankToState,
    canvasW,
    canvasH,
    fixedRowY,
    traceConnX,
    fullX,
    edgeEvents,
    anyEvents,
  };
}

function findStateAt(
  intervals: ReadonlyArray<StateInterval>,
  ts: time,
): StateInterval | undefined {
  let lo = 0;
  let hi = intervals.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    const iv = intervals[mid];
    if (iv !== undefined && iv.ts <= ts) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo === 0) return undefined;
  const candidate = intervals[lo - 1];
  return candidate !== undefined && candidate.ts <= ts && ts < candidate.endTs
    ? candidate
    : undefined;
}

function nodeMatchesQuery(node: SnapshotNode, qLower: string): boolean {
  return (
    node.name.toLowerCase().includes(qLower) ||
    String(node.pid).includes(qLower) ||
    (node.packageName?.toLowerCase().includes(qLower) ?? false)
  );
}

export function snapshotAt(
  model: ProcessGraphModel,
  ts: time,
  filters: ProcessGraphFilters,
): GraphSnapshot {
  const edgeMap = new Map<
    string,
    {
      key: string;
      kind: EdgeKind;
      client: number;
      server: number;
      count: number;
      compSet: Set<string>;
      components: string[];
      detailSet: Set<string>;
      details: string[];
    }
  >();

  for (const iv of model.connections) {
    if (iv.ts > ts) break;
    if (ts >= iv.endTs) continue;
    if (!filters.showServices && iv.kind === 'service') continue;
    if (!filters.showProviders && iv.kind === 'provider') continue;
    if (!filters.showSelfLoops && iv.clientUpid === iv.serverUpid) continue;

    const key = `${iv.kind}:${iv.clientUpid}>${iv.serverUpid}`;
    let acc = edgeMap.get(key);
    if (acc === undefined) {
      acc = {
        key,
        kind: iv.kind,
        client: iv.clientUpid,
        server: iv.serverUpid,
        count: 0,
        compSet: new Set<string>(),
        components: [],
        detailSet: new Set<string>(),
        details: [],
      };
      edgeMap.set(key, acc);
    }
    acc.count += 1;
    if (iv.label && !acc.compSet.has(iv.label)) {
      acc.compSet.add(iv.label);
      acc.components.push(iv.label);
    }
    if (iv.detail && !acc.detailSet.has(iv.detail)) {
      acc.detailSet.add(iv.detail);
      acc.details.push(iv.detail);
    }
  }

  const rawEdges: CollapsedEdge[] = [...edgeMap.values()].map((e) => ({
    key: e.key,
    kind: e.kind,
    client: e.client,
    server: e.server,
    count: e.count,
    components: e.components,
    details: e.details,
  }));

  const allNodesMap = new Map<number, SnapshotNode>();
  for (const [upid, list] of model.stateByUpid) {
    const st = findStateAt(list, ts);
    if (st !== undefined) {
      const p = model.procsByUpid.get(upid);
      allNodesMap.set(upid, {
        upid,
        pid: p?.pid ?? 0,
        name: p?.name ?? `upid ${upid}`,
        uid: p?.uid,
        packageName: p?.packageName,
        state: st.state,
        stateRank: st.stateRank,
        reason: st.reason,
      });
    }
  }

  for (const e of rawEdges) {
    for (const u of [e.client, e.server]) {
      if (!allNodesMap.has(u)) {
        const p = model.procsByUpid.get(u);
        allNodesMap.set(u, {
          upid: u,
          pid: p?.pid ?? 0,
          name: p?.name ?? `upid ${u}`,
          uid: p?.uid,
          packageName: p?.packageName,
          state: 'UNKNOWN',
          stateRank: UNKNOWN_RANK,
        });
      }
    }
  }

  const useTraceColumns = !filters.showIsolated;
  const qLower = filters.searchQuery.trim().toLowerCase();

  let candidateNodes = [...allNodesMap.values()].filter(
    (n) => filters.showIsolated || model.traceConnX.has(n.upid),
  );

  const baseKeep = new Set(candidateNodes.map((n) => n.upid));
  let finalEdges = rawEdges.filter(
    (e) => baseKeep.has(e.client) && baseKeep.has(e.server),
  );

  if (qLower !== '') {
    const directMatchedUpids = new Set<number>();
    for (const n of candidateNodes) {
      if (nodeMatchesQuery(n, qLower)) {
        directMatchedUpids.add(n.upid);
      }
    }
    finalEdges = finalEdges.filter(
      (e) =>
        directMatchedUpids.has(e.client) ||
        directMatchedUpids.has(e.server) ||
        e.components.some((c) => c.toLowerCase().includes(qLower)) ||
        e.details.some((d) => d.toLowerCase().includes(qLower)),
    );
    const searchKeepUpids = new Set<number>(directMatchedUpids);
    for (const e of finalEdges) {
      searchKeepUpids.add(e.client);
      searchKeepUpids.add(e.server);
    }
    candidateNodes = candidateNodes.filter((n) => searchKeepUpids.has(n.upid));
  }

  return {
    ts,
    useTraceColumns,
    allNodesMap,
    rawEdges,
    nodes: candidateNodes,
    edges: finalEdges,
  };
}

function snapshotSignature(snap: GraphSnapshot, mode: StepMode): string {
  const edgeSig = snap.edges
    .map((e) => e.key)
    .sort()
    .join(';');
  if (mode === 'edge') {
    return edgeSig;
  }
  const nodeSig = snap.nodes
    .map((n) => `${n.upid}:${n.state}`)
    .sort()
    .join(';');
  return `${edgeSig}#${nodeSig}`;
}

function nextTimeCandidate(
  model: ProcessGraphModel,
  curTs: time,
  dir: 1 | -1,
  mode: StepMode,
): time {
  if (typeof mode === 'number') {
    const deltaNs = BigInt(mode) * 1_000_000n * BigInt(dir);
    return Time.clamp(
      Time.fromRaw(curTs + deltaNs),
      model.traceStart,
      model.traceEnd,
    );
  }
  const evs = mode === 'edge' ? model.edgeEvents : model.anyEvents;
  const idx = upperBoundTime(evs, curTs);
  if (dir > 0) {
    return idx < evs.length ? (evs[idx] ?? curTs) : curTs;
  }
  if (idx >= 2) {
    return evs[idx - 2] ?? curTs;
  }
  if (idx === 1 && evs[0] !== undefined && evs[0] < curTs) {
    return evs[0];
  }
  return curTs;
}

export function stepTimestamp(
  model: ProcessGraphModel,
  curTs: time,
  dir: 1 | -1,
  mode: StepMode,
  filters: ProcessGraphFilters,
): time | undefined {
  const isEventStep = mode === 'edge' || mode === 'any';
  const beforeSig = isEventStep
    ? snapshotSignature(snapshotAt(model, curTs, filters), mode)
    : '';

  let t = curTs;
  for (let guard = 0; guard < 400; guard++) {
    const nt = nextTimeCandidate(model, t, dir, mode);
    if (nt === t) return undefined;
    t = nt;
    if (
      !isEventStep ||
      snapshotSignature(snapshotAt(model, t, filters), mode) !== beforeSig
    ) {
      return t;
    }
  }
  return t === curTs ? undefined : t;
}

export async function loadProcessGraphModel(
  trace: Trace,
  signal?: CancellationSignal,
): Promise<ProcessGraphModel | typeof TASK_CANCELLED> {
  await trace.engine.query(`
    INCLUDE PERFETTO MODULE android.process_metadata;
    INCLUDE PERFETTO MODULE android.process_state;
    INCLUDE PERFETTO MODULE android.process_graph;
  `);
  if (signal?.isCancelled) return TASK_CANCELLED;

  const traceStart = trace.traceInfo.start;
  const traceEnd = trace.traceInfo.end;
  const openEndTs = Time.fromRaw(traceEnd + 1n);

  const procRes = await trace.engine.query(`
    SELECT
      p.upid,
      p.pid,
      coalesce(m.process_name, p.name, 'pid ' || p.pid) AS process_name,
      m.uid,
      m.package_name
    FROM process AS p
    LEFT JOIN android_process_metadata AS m USING (upid)
  `);
  if (signal?.isCancelled) return TASK_CANCELLED;

  const processes: ProcessInfo[] = [];
  const procIt = procRes.iter({
    upid: NUM,
    pid: NUM,
    process_name: STR,
    uid: NUM_NULL,
    package_name: STR_NULL,
  });
  for (; procIt.valid(); procIt.next()) {
    processes.push({
      upid: procIt.upid,
      pid: procIt.pid,
      name: procIt.process_name,
      uid: procIt.uid ?? undefined,
      packageName: procIt.package_name ?? undefined,
    });
  }

  // Exclude synthesized post-death EXITED intervals so exited processes vanish
  // cleanly instead of accumulating at the bottom row.
  const stateRes = await trace.engine.query(`
    SELECT
      upid,
      ts,
      dur,
      state,
      state_rank,
      state_change_reason
    FROM _android_process_state_intervals
    WHERE state != 'EXITED'
  `);
  if (signal?.isCancelled) return TASK_CANCELLED;

  const stateIntervals: StateInterval[] = [];
  const stIt = stateRes.iter({
    upid: NUM,
    ts: LONG,
    dur: LONG,
    state: STR,
    state_rank: NUM,
    state_change_reason: STR_NULL,
  });
  for (; stIt.valid(); stIt.next()) {
    const startTs = Time.fromRaw(stIt.ts);
    const endTs = stIt.dur < 0n ? openEndTs : Time.fromRaw(stIt.ts + stIt.dur);
    if (endTs <= traceStart || startTs > traceEnd) continue;
    stateIntervals.push({
      upid: stIt.upid,
      ts: startTs,
      endTs,
      state: stIt.state,
      stateRank: stIt.state_rank,
      reason: stIt.state_change_reason ?? undefined,
    });
  }

  const connRes = await trace.engine.query(`
    SELECT
      'service' AS kind,
      ts,
      dur,
      client_upid,
      server_upid,
      component_name AS label,
      CASE
        WHEN coalesce(intent_action, '') != '' AND coalesce(bind_flags, '') != ''
          THEN 'action=' || intent_action || ' · flags=' || bind_flags
        WHEN coalesce(intent_action, '') != '' THEN 'action=' || intent_action
        WHEN coalesce(bind_flags, '') != '' THEN 'flags=' || bind_flags
        ELSE NULL
      END AS detail
    FROM _android_service_connections
    UNION ALL
    SELECT
      'provider' AS kind,
      ts,
      dur,
      client_upid,
      server_upid,
      authority AS label,
      CASE
        WHEN is_stable IS NULL THEN NULL
        WHEN is_stable != 0 THEN 'stable'
        ELSE 'unstable'
      END AS detail
    FROM _android_content_provider_connections
  `);
  if (signal?.isCancelled) return TASK_CANCELLED;

  const connections: ConnectionInterval[] = [];
  const connIt = connRes.iter({
    kind: STR,
    ts: LONG,
    dur: LONG,
    client_upid: NUM,
    server_upid: NUM,
    label: STR_NULL,
    detail: STR_NULL,
  });
  for (; connIt.valid(); connIt.next()) {
    const startTs = Time.fromRaw(connIt.ts);
    const endTs =
      connIt.dur < 0n ? openEndTs : Time.fromRaw(connIt.ts + connIt.dur);
    if (endTs <= traceStart || startTs > traceEnd) continue;
    connections.push({
      kind: connIt.kind === 'provider' ? 'provider' : 'service',
      ts: startTs,
      endTs,
      clientUpid: connIt.client_upid,
      serverUpid: connIt.server_upid,
      label: connIt.label ?? undefined,
      detail: connIt.detail ?? undefined,
    });
  }

  return buildProcessGraphModel({
    traceStart,
    traceEnd,
    processes,
    stateIntervals,
    connections,
  });
}
