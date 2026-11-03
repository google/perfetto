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
import {computeBarycenterOrder} from '../../components/widgets/charts/graph_chart';
import type {Trace} from '../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export type EdgeKind = 'service' | 'provider';
export type ScopeMode = 'chains' | 'neighbors' | 'component';
export type StepMode = 'any' | 'edge' | 100 | 1000 | 5000;

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

export interface FreezerInterval {
  readonly upid: number;
  readonly ts: time;
  readonly endTs: time;
}

export interface ConnectionInterval {
  readonly kind: EdgeKind;
  readonly ts: time;
  readonly endTs: time;
  readonly clientUpid: number;
  readonly serverUpid: number;
  readonly label?: string;
  readonly detail?: string;
  readonly waivesPriority?: boolean;
}

export interface ProcessGraphFilters {
  readonly showServices: boolean;
  readonly showProviders: boolean;
  readonly showSelfLoops: boolean;
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
  readonly isFrozen: boolean;
  readonly isTarget: boolean;
}

export interface CollapsedEdge {
  readonly key: string;
  readonly kind: EdgeKind;
  readonly client: number;
  readonly server: number;
  readonly count: number;
  readonly components: ReadonlyArray<string>;
  readonly details: ReadonlyArray<string>;
  readonly waivesPriority?: boolean;
}

export interface GraphSnapshot {
  readonly ts: time;
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
  readonly freezerIntervals?: ReadonlyArray<FreezerInterval>;
}

export interface ProcessGraphModel {
  readonly traceStart: time;
  readonly traceEnd: time;
  readonly procsByUpid: ReadonlyMap<number, ProcessInfo>;
  readonly stateByUpid: ReadonlyMap<number, ReadonlyArray<StateInterval>>;
  readonly freezerByUpid: ReadonlyMap<number, ReadonlyArray<FreezerInterval>>;
  readonly connections: ReadonlyArray<ConnectionInterval>;
  readonly globalRankToState: ReadonlyMap<number, string>;
}

export interface ContextualSubgraphSpec {
  readonly windowStart: time;
  readonly windowEnd: time;
  readonly targetUpids: ReadonlyArray<number>;
  readonly targetStates?: ReadonlyArray<string>;
  readonly scopeMode: ScopeMode;
  readonly filters: ProcessGraphFilters;
}

export interface ContextualSubgraph {
  readonly windowStart: time;
  readonly windowEnd: time;
  readonly scopeMode: ScopeMode;
  readonly explicitTargetUpids: ReadonlySet<number>;
  readonly targetStates: ReadonlySet<string>;
  readonly soloExitedFallbackUpid?: number;
  readonly targetUpidSet: ReadonlySet<number>;
  readonly subgraphUpids: ReadonlySet<number>;
  readonly windowConnections: ReadonlyArray<ConnectionInterval>;
  readonly orderedRanks: ReadonlyArray<number>;
  readonly rankToState: ReadonlyMap<number, string>;
  readonly normYByUpid: ReadonlyMap<number, number>;
  readonly edgeEvents: ReadonlyArray<time>;
  readonly anyEvents: ReadonlyArray<time>;
}

export interface StateStyle {
  readonly fill: string;
  readonly lightText: string;
  readonly short: string;
}

const EXITED_RANK = 21;
const UNKNOWN_RANK = 999;

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

export function hasPriorityWaivingFlag(
  flagsOrDetail: string | null | undefined,
): boolean {
  if (!flagsOrDetail) return false;
  const flagsIdx = flagsOrDetail.indexOf('flags=');
  const afterFlags =
    flagsIdx >= 0
      ? flagsOrDetail.slice(flagsIdx + 'flags='.length)
      : flagsOrDetail;
  const sepIdx = afterFlags.indexOf(' · ');
  const raw = sepIdx >= 0 ? afterFlags.slice(0, sepIdx) : afterFlags;
  for (const token of raw.split(',')) {
    const t = token.trim();
    if (t === 'WAIVE_PRIORITY' || t === 'NOT_FOREGROUND') {
      return true;
    }
  }
  return false;
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
  for (const s of input.stateIntervals) {
    globalRankToState.set(s.stateRank, s.state);
    let list = stateByUpidMut.get(s.upid);
    if (list === undefined) {
      list = [];
      stateByUpidMut.set(s.upid, list);
    }
    list.push(s);
  }
  for (const list of stateByUpidMut.values()) {
    list.sort((a, b) => (a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0));
  }

  const freezerByUpidMut = new Map<number, FreezerInterval[]>();
  for (const f of input.freezerIntervals ?? []) {
    let list = freezerByUpidMut.get(f.upid);
    if (list === undefined) {
      list = [];
      freezerByUpidMut.set(f.upid, list);
    }
    list.push(f);
  }
  for (const list of freezerByUpidMut.values()) {
    list.sort((a, b) => (a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0));
  }

  for (const c of input.connections) {
    for (const u of [c.clientUpid, c.serverUpid]) {
      if (!procsByUpid.has(u)) {
        procsByUpid.set(u, {upid: u, pid: 0, name: `upid ${u}`});
      }
    }
  }

  const sortedConnections = [...input.connections].sort((a, b) =>
    a.ts < b.ts ? -1 : a.ts > b.ts ? 1 : 0,
  );

  return {
    traceStart,
    traceEnd,
    procsByUpid,
    stateByUpid: stateByUpidMut,
    freezerByUpid: freezerByUpidMut,
    connections: sortedConnections,
    globalRankToState,
  };
}

function findIntervalAt<T extends {readonly ts: time; readonly endTs: time}>(
  intervals: ReadonlyArray<T> | undefined,
  ts: time,
): T | undefined {
  if (intervals === undefined || intervals.length === 0) return undefined;
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

function pushAdj(map: Map<number, number[]>, from: number, to: number): void {
  let list = map.get(from);
  if (list === undefined) {
    list = [];
    map.set(from, list);
  }
  list.push(to);
}

function bfsReachable(
  seeds: Iterable<number>,
  adj: ReadonlyMap<number, ReadonlyArray<number>>,
  out: Set<number>,
): void {
  const seen = new Set<number>(seeds);
  const queue = [...seen];
  for (let head = 0; head < queue.length; head++) {
    const u = queue[head];
    if (u === undefined) continue;
    for (const v of adj.get(u) ?? []) {
      out.add(v);
      if (!seen.has(v)) {
        seen.add(v);
        queue.push(v);
      }
    }
  }
}

interface TimeSpan {
  ts: time;
  endTs: time;
}

function appendMergedSpan(
  spansByUpid: Map<number, TimeSpan[]>,
  upid: number,
  ts: time,
  endTs: time,
): void {
  if (endTs <= ts) return;
  const spans = spansByUpid.get(upid);
  if (spans === undefined) {
    spansByUpid.set(upid, [{ts, endTs}]);
    return;
  }
  const last = spans[spans.length - 1];
  if (last !== undefined && ts <= last.endTs) {
    if (endTs > last.endTs) {
      last.endTs = endTs;
    }
  } else {
    spans.push({ts, endTs});
  }
}

function getStateAt(
  model: ProcessGraphModel,
  upid: number,
  ts: time,
): {
  readonly state: string;
  readonly stateRank: number;
  readonly reason?: string;
} {
  const sList = model.stateByUpid.get(upid);
  const st = findIntervalAt(sList, ts);
  if (st !== undefined) {
    return {state: st.state, stateRank: st.stateRank, reason: st.reason};
  }
  const hasStateTrack = sList !== undefined && sList.length > 0;
  return hasStateTrack
    ? {state: 'EXITED', stateRank: EXITED_RANK}
    : {state: 'UNKNOWN', stateRank: UNKNOWN_RANK};
}

export function buildContextualSubgraph(
  model: ProcessGraphModel,
  spec: ContextualSubgraphSpec,
): ContextualSubgraph {
  const windowStart = Time.clamp(
    spec.windowStart,
    model.traceStart,
    model.traceEnd,
  );
  const windowEnd = Time.clamp(
    spec.windowEnd < windowStart ? windowStart : spec.windowEnd,
    model.traceStart,
    model.traceEnd,
  );
  const testEnd =
    windowEnd > windowStart ? windowEnd : Time.fromRaw(windowStart + 1n);
  const inclusiveWindowEnd = Time.fromRaw(windowEnd + 1n);

  const explicitTargetUpids = new Set<number>(spec.targetUpids);
  const targetStates = new Set<string>(spec.targetStates ?? []);
  const targetUpidSet = new Set<number>(explicitTargetUpids);
  if (targetStates.size > 0) {
    for (const [upid, list] of model.stateByUpid) {
      for (const iv of list) {
        if (iv.ts >= inclusiveWindowEnd) break;
        if (iv.endTs > windowStart && targetStates.has(iv.state)) {
          targetUpidSet.add(upid);
          break;
        }
      }
    }
  }

  const {filters, scopeMode} = spec;
  const subgraphUpids = new Set<number>(targetUpidSet);
  let windowConnections: ConnectionInterval[];

  if (scopeMode === 'neighbors') {
    windowConnections = [];
    for (const c of model.connections) {
      if (c.ts >= testEnd) break;
      if (c.endTs <= windowStart) continue;
      if (!filters.showServices && c.kind === 'service') continue;
      if (!filters.showProviders && c.kind === 'provider') continue;
      if (!filters.showSelfLoops && c.clientUpid === c.serverUpid) continue;
      if (targetUpidSet.has(c.clientUpid) || targetUpidSet.has(c.serverUpid)) {
        subgraphUpids.add(c.clientUpid);
        subgraphUpids.add(c.serverUpid);
        windowConnections.push(c);
      }
    }
  } else {
    const activeConns: ConnectionInterval[] = [];
    const undirectedAdj = new Map<number, number[]>();
    const forwardAdj = new Map<number, number[]>();
    const reverseAdj = new Map<number, number[]>();

    for (const c of model.connections) {
      if (c.ts >= testEnd) break;
      if (c.endTs <= windowStart) continue;
      if (!filters.showServices && c.kind === 'service') continue;
      if (!filters.showProviders && c.kind === 'provider') continue;
      if (!filters.showSelfLoops && c.clientUpid === c.serverUpid) continue;
      activeConns.push(c);
      if (c.clientUpid !== c.serverUpid) {
        if (scopeMode === 'component') {
          pushAdj(undirectedAdj, c.clientUpid, c.serverUpid);
          pushAdj(undirectedAdj, c.serverUpid, c.clientUpid);
        } else {
          pushAdj(forwardAdj, c.clientUpid, c.serverUpid);
          pushAdj(reverseAdj, c.serverUpid, c.clientUpid);
        }
      }
    }

    if (scopeMode === 'component') {
      bfsReachable(targetUpidSet, undirectedAdj, subgraphUpids);
    } else {
      bfsReachable(targetUpidSet, forwardAdj, subgraphUpids);
      bfsReachable(targetUpidSet, reverseAdj, subgraphUpids);
    }

    windowConnections = activeConns.filter(
      (c) => subgraphUpids.has(c.clientUpid) && subgraphUpids.has(c.serverUpid),
    );
  }

  const soloTargetUpid =
    explicitTargetUpids.size === 1 && targetStates.size === 0
      ? [...explicitTargetUpids][0]
      : undefined;
  let soloExitedFallbackUpid: number | undefined;
  if (soloTargetUpid !== undefined) {
    const sList = model.stateByUpid.get(soloTargetUpid);
    if (sList !== undefined && sList.length > 0) {
      const hasLiveInWindow = sList.some(
        (s) => s.ts < inclusiveWindowEnd && s.endTs > windowStart,
      );
      if (!hasLiveInWindow) {
        soloExitedFallbackUpid = soloTargetUpid;
      }
    }
  }

  const connSpansByUpid = new Map<number, TimeSpan[]>();
  for (const c of windowConnections) {
    const spStart = c.ts > windowStart ? c.ts : windowStart;
    const spEnd = c.endTs < inclusiveWindowEnd ? c.endTs : inclusiveWindowEnd;
    appendMergedSpan(connSpansByUpid, c.clientUpid, spStart, spEnd);
    if (c.serverUpid !== c.clientUpid) {
      appendMergedSpan(connSpansByUpid, c.serverUpid, spStart, spEnd);
    }
  }
  const soloWindowSpans: ReadonlyArray<TimeSpan> = [
    {ts: windowStart, endTs: inclusiveWindowEnd},
  ];

  const rankToState = new Map<number, string>(model.globalRankToState);
  const visitedRanks = new Set<number>();
  const primaryRankByUpid = new Map<number, number>();

  for (const upid of subgraphUpids) {
    const visSpans =
      upid === soloTargetUpid ? soloWindowSpans : connSpansByUpid.get(upid);
    if (visSpans === undefined || visSpans.length === 0) {
      continue;
    }

    const sList = model.stateByUpid.get(upid);
    if (sList === undefined || sList.length === 0) {
      rankToState.set(UNKNOWN_RANK, 'UNKNOWN');
      visitedRanks.add(UNKNOWN_RANK);
      primaryRankByUpid.set(upid, UNKNOWN_RANK);
      continue;
    }

    const rankOverlap = new Map<number, bigint>();
    let bestOverlap = -1n;
    let bestRank = UNKNOWN_RANK;
    let spanIdx = 0;
    for (const s of sList) {
      if (s.ts >= inclusiveWindowEnd) break;
      if (s.endTs <= windowStart) continue;
      while (
        spanIdx < visSpans.length &&
        (visSpans[spanIdx]?.endTs ?? 0n) <= s.ts
      ) {
        spanIdx++;
      }
      for (let j = spanIdx; j < visSpans.length; j++) {
        const sp = visSpans[j];
        if (sp === undefined || sp.ts >= s.endTs) break;
        const ovStart = s.ts > sp.ts ? s.ts : sp.ts;
        const ovEnd = s.endTs < sp.endTs ? s.endTs : sp.endTs;
        if (ovEnd > ovStart) {
          visitedRanks.add(s.stateRank);
          rankToState.set(s.stateRank, s.state);
          const totalOv =
            (rankOverlap.get(s.stateRank) ?? 0n) + (ovEnd - ovStart);
          rankOverlap.set(s.stateRank, totalOv);
          if (totalOv > bestOverlap) {
            bestOverlap = totalOv;
            bestRank = s.stateRank;
          }
        }
      }
    }

    if (bestOverlap >= 0n) {
      primaryRankByUpid.set(upid, bestRank);
    } else if (soloExitedFallbackUpid === upid) {
      rankToState.set(EXITED_RANK, 'EXITED');
      visitedRanks.add(EXITED_RANK);
      primaryRankByUpid.set(upid, EXITED_RANK);
    }
  }

  // Order descending so CACHED / EXITED / UNKNOWN sit on the left and
  // PERSISTENT (rank 0) sits on the far right.
  const orderedRanks = [...visitedRanks].sort((a, b) => b - a);
  const colIndexByRank = new Map<number, number>();
  for (let i = 0; i < orderedRanks.length; i++) {
    const r = orderedRanks[i];
    if (r !== undefined) {
      colIndexByRank.set(r, i);
    }
  }

  const seenEdgePairs = new Set<string>();
  const unionEdges: Array<{readonly source: string; readonly target: string}> =
    [];
  for (const c of windowConnections) {
    if (c.clientUpid === c.serverUpid) continue;
    const pk = `${c.clientUpid}>${c.serverUpid}`;
    if (!seenEdgePairs.has(pk)) {
      seenEdgePairs.add(pk);
      unionEdges.push({
        source: String(c.clientUpid),
        target: String(c.serverUpid),
      });
    }
  }

  const bary = computeBarycenterOrder(
    [...subgraphUpids].map((u) => ({
      id: String(u),
      layer: colIndexByRank.get(primaryRankByUpid.get(u) ?? UNKNOWN_RANK) ?? 0,
      tieBreak: `${model.procsByUpid.get(u)?.name ?? ''}\0${String(u).padStart(10, '0')}`,
    })),
    unionEdges,
  );
  const normYByUpid = new Map<number, number>();
  for (const [id, norm] of bary.normX) {
    normYByUpid.set(Number(id), norm);
  }

  const openSentinel = Time.fromRaw(model.traceEnd + 1n);
  const rawEdgeEvents: time[] = [windowStart, windowEnd];
  for (const c of windowConnections) {
    if (c.ts >= windowStart && c.ts <= windowEnd) {
      rawEdgeEvents.push(c.ts);
    }
    if (
      c.endTs < openSentinel &&
      c.endTs >= windowStart &&
      c.endTs <= windowEnd
    ) {
      rawEdgeEvents.push(c.endTs);
    }
  }
  const edgeEvents = dedupSortedTimes(rawEdgeEvents);

  const rawAnyEvents: time[] = [...edgeEvents];
  for (const u of subgraphUpids) {
    const visSpans =
      u === soloTargetUpid ? soloWindowSpans : connSpansByUpid.get(u);
    if (visSpans === undefined || visSpans.length === 0) continue;

    for (const s of model.stateByUpid.get(u) ?? []) {
      if (s.ts > windowEnd) break;
      if (
        s.ts >= windowStart &&
        s.ts <= windowEnd &&
        findIntervalAt(visSpans, s.ts) !== undefined
      ) {
        rawAnyEvents.push(s.ts);
      }
      if (
        s.endTs < openSentinel &&
        s.endTs >= windowStart &&
        s.endTs <= windowEnd &&
        findIntervalAt(visSpans, s.endTs) !== undefined
      ) {
        rawAnyEvents.push(s.endTs);
      }
    }
    for (const f of model.freezerByUpid.get(u) ?? []) {
      if (f.ts > windowEnd) break;
      if (
        f.ts >= windowStart &&
        f.ts <= windowEnd &&
        findIntervalAt(visSpans, f.ts) !== undefined
      ) {
        rawAnyEvents.push(f.ts);
      }
      if (
        f.endTs < openSentinel &&
        f.endTs >= windowStart &&
        f.endTs <= windowEnd &&
        findIntervalAt(visSpans, f.endTs) !== undefined
      ) {
        rawAnyEvents.push(f.endTs);
      }
    }
  }
  const anyEvents = dedupSortedTimes(rawAnyEvents);

  return {
    windowStart,
    windowEnd,
    scopeMode,
    explicitTargetUpids,
    targetStates,
    soloExitedFallbackUpid,
    targetUpidSet,
    subgraphUpids,
    windowConnections,
    orderedRanks,
    rankToState,
    normYByUpid,
    edgeEvents,
    anyEvents,
  };
}

function nodeMatchesQuery(node: SnapshotNode, qLower: string): boolean {
  return (
    node.name.toLowerCase().includes(qLower) ||
    String(node.pid).includes(qLower) ||
    node.state.toLowerCase().includes(qLower) ||
    (node.packageName?.toLowerCase().includes(qLower) ?? false)
  );
}

export function snapshotAt(
  model: ProcessGraphModel,
  subgraph: ContextualSubgraph,
  ts: time,
  filters: ProcessGraphFilters,
): GraphSnapshot {
  const seedUpids = new Set<number>();
  for (const u of subgraph.explicitTargetUpids) {
    const st = getStateAt(model, u, ts);
    if (st.state !== 'EXITED' || subgraph.soloExitedFallbackUpid === u) {
      seedUpids.add(u);
    }
  }
  if (subgraph.targetStates.size > 0) {
    for (const u of subgraph.targetUpidSet) {
      if (seedUpids.has(u)) continue;
      if (subgraph.targetStates.has(getStateAt(model, u, ts).state)) {
        seedUpids.add(u);
      }
    }
  }

  const candidateConns: ConnectionInterval[] = [];
  for (const iv of subgraph.windowConnections) {
    if (iv.ts > ts) break;
    if (ts >= iv.endTs) continue;
    if (!filters.showServices && iv.kind === 'service') continue;
    if (!filters.showProviders && iv.kind === 'provider') continue;
    if (!filters.showSelfLoops && iv.clientUpid === iv.serverUpid) continue;
    candidateConns.push(iv);
  }

  let scopedConns: ReadonlyArray<ConnectionInterval>;
  if (subgraph.scopeMode === 'neighbors') {
    scopedConns = candidateConns.filter(
      (iv) => seedUpids.has(iv.clientUpid) || seedUpids.has(iv.serverUpid),
    );
  } else if (subgraph.scopeMode === 'component') {
    const undirectedAdj = new Map<number, number[]>();
    for (const iv of candidateConns) {
      if (iv.clientUpid !== iv.serverUpid) {
        pushAdj(undirectedAdj, iv.clientUpid, iv.serverUpid);
        pushAdj(undirectedAdj, iv.serverUpid, iv.clientUpid);
      }
    }
    const compSet = new Set<number>(seedUpids);
    bfsReachable(seedUpids, undirectedAdj, compSet);
    scopedConns = candidateConns.filter(
      (iv) => compSet.has(iv.clientUpid) && compSet.has(iv.serverUpid),
    );
  } else {
    const forwardAdj = new Map<number, number[]>();
    const reverseAdj = new Map<number, number[]>();
    for (const iv of candidateConns) {
      if (iv.clientUpid !== iv.serverUpid) {
        pushAdj(forwardAdj, iv.clientUpid, iv.serverUpid);
        pushAdj(reverseAdj, iv.serverUpid, iv.clientUpid);
      }
    }
    const reachSet = new Set<number>(seedUpids);
    bfsReachable(seedUpids, forwardAdj, reachSet);
    bfsReachable(seedUpids, reverseAdj, reachSet);
    scopedConns = candidateConns.filter(
      (iv) => reachSet.has(iv.clientUpid) && reachSet.has(iv.serverUpid),
    );
  }

  const edgeMap = new Map<
    string,
    {
      key: string;
      kind: EdgeKind;
      client: number;
      server: number;
      count: number;
      waivedCount: number;
      compSet: Set<string>;
      components: string[];
      detailSet: Set<string>;
      details: string[];
    }
  >();

  for (const iv of scopedConns) {
    const key = `${iv.kind}:${iv.clientUpid}>${iv.serverUpid}`;
    let acc = edgeMap.get(key);
    if (acc === undefined) {
      acc = {
        key,
        kind: iv.kind,
        client: iv.clientUpid,
        server: iv.serverUpid,
        count: 0,
        waivedCount: 0,
        compSet: new Set<string>(),
        components: [],
        detailSet: new Set<string>(),
        details: [],
      };
      edgeMap.set(key, acc);
    }
    acc.count += 1;
    if (
      iv.kind === 'service' &&
      (iv.waivesPriority ?? hasPriorityWaivingFlag(iv.detail))
    ) {
      acc.waivedCount += 1;
    }
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
    waivesPriority:
      e.kind === 'service' && e.count > 0 && e.waivedCount === e.count,
  }));

  const visibleAtTsUpids = new Set<number>();
  for (const e of rawEdges) {
    visibleAtTsUpids.add(e.client);
    visibleAtTsUpids.add(e.server);
  }
  if (
    subgraph.explicitTargetUpids.size === 1 &&
    subgraph.targetStates.size === 0
  ) {
    for (const u of seedUpids) {
      visibleAtTsUpids.add(u);
    }
  }

  const allNodesMap = new Map<number, SnapshotNode>();
  for (const upid of visibleAtTsUpids) {
    const p = model.procsByUpid.get(upid);
    const st = getStateAt(model, upid, ts);
    const isFrozen =
      findIntervalAt(model.freezerByUpid.get(upid), ts) !== undefined;
    allNodesMap.set(upid, {
      upid,
      pid: p?.pid ?? 0,
      name: p?.name ?? `upid ${upid}`,
      uid: p?.uid,
      packageName: p?.packageName,
      state: st.state,
      stateRank: st.stateRank,
      reason: st.reason,
      isFrozen,
      isTarget: seedUpids.has(upid),
    });
  }

  let candidateNodes = [...allNodesMap.values()];
  let finalEdges = rawEdges;
  const qLower = filters.searchQuery.trim().toLowerCase();

  if (qLower !== '') {
    const directMatchedUpids = new Set<number>();
    for (const n of candidateNodes) {
      if (nodeMatchesQuery(n, qLower)) {
        directMatchedUpids.add(n.upid);
      }
    }
    finalEdges = rawEdges.filter(
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
    allNodesMap,
    rawEdges,
    nodes: candidateNodes,
    edges: finalEdges,
  };
}

function snapshotSignature(snap: GraphSnapshot, mode: StepMode): string {
  const edgeSig = snap.edges
    .map((e) => `${e.key}:${e.count}:${e.waivesPriority === true ? 1 : 0}`)
    .sort()
    .join(';');
  if (mode === 'edge') {
    return edgeSig;
  }
  const nodeSig = snap.nodes
    .map((n) => `${n.upid}:${n.state}:${n.isFrozen ? 1 : 0}`)
    .sort()
    .join(';');
  return `${edgeSig}#${nodeSig}`;
}

export function computeDistinctStepTimes(
  model: ProcessGraphModel,
  subgraph: ContextualSubgraph,
  mode: 'any' | 'edge',
  filters: ProcessGraphFilters,
): ReadonlyArray<time> {
  const evs = mode === 'edge' ? subgraph.edgeEvents : subgraph.anyEvents;
  if (evs.length <= 1 || evs.length > 5_000) {
    return evs;
  }
  const out: time[] = [];
  let prevSig: string | undefined;
  for (const t of evs) {
    const sig = snapshotSignature(
      snapshotAt(model, subgraph, t, filters),
      mode,
    );
    if (sig !== prevSig) {
      out.push(t);
      prevSig = sig;
    }
  }
  return out;
}

function nextTimeCandidate(
  subgraph: ContextualSubgraph,
  curTs: time,
  dir: 1 | -1,
  mode: StepMode,
): time {
  if (typeof mode === 'number') {
    const deltaNs = BigInt(mode) * 1_000_000n * BigInt(dir);
    return Time.clamp(
      Time.fromRaw(curTs + deltaNs),
      subgraph.windowStart,
      subgraph.windowEnd,
    );
  }
  const evs = mode === 'edge' ? subgraph.edgeEvents : subgraph.anyEvents;
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
  subgraph: ContextualSubgraph,
  curTs: time,
  dir: 1 | -1,
  mode: StepMode,
  filters: ProcessGraphFilters,
): time | undefined {
  const isEventStep = mode === 'edge' || mode === 'any';
  const beforeSig = isEventStep
    ? snapshotSignature(snapshotAt(model, subgraph, curTs, filters), mode)
    : '';

  let t = curTs;
  for (let guard = 0; guard < 400; guard++) {
    const nt = nextTimeCandidate(subgraph, t, dir, mode);
    if (nt === t) return undefined;
    t = nt;
    if (
      !isEventStep ||
      snapshotSignature(snapshotAt(model, subgraph, t, filters), mode) !==
        beforeSig
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
    INCLUDE PERFETTO MODULE android.freezer;
    INCLUDE PERFETTO MODULE android.process_bindings;
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
      AND dur != 0
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

  const freezerIntervals: FreezerInterval[] = [];
  const freezerRes = await trace.engine.tryQuery(`
    SELECT
      upid,
      ts,
      dur
    FROM _android_freezer_intervals
    WHERE state = 'FROZEN'
      AND dur != 0
  `);
  if (signal?.isCancelled) return TASK_CANCELLED;
  if (freezerRes.ok) {
    const frzIt = freezerRes.value.iter({
      upid: NUM,
      ts: LONG,
      dur: LONG,
    });
    for (; frzIt.valid(); frzIt.next()) {
      const startTs = Time.fromRaw(frzIt.ts);
      const endTs =
        frzIt.dur < 0n ? openEndTs : Time.fromRaw(frzIt.ts + frzIt.dur);
      if (endTs <= traceStart || startTs > traceEnd) continue;
      freezerIntervals.push({
        upid: frzIt.upid,
        ts: startTs,
        endTs,
      });
    }
  }

  const connRes = await trace.engine.query(`
    SELECT
      'service' AS kind,
      ts,
      dur,
      client_upid,
      server_upid,
      component_name AS label,
      bind_flags,
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
      CAST(NULL AS STRING) AS bind_flags,
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
    bind_flags: STR_NULL,
    detail: STR_NULL,
  });
  for (; connIt.valid(); connIt.next()) {
    const startTs = Time.fromRaw(connIt.ts);
    const endTs =
      connIt.dur < 0n ? openEndTs : Time.fromRaw(connIt.ts + connIt.dur);
    if (endTs <= traceStart || startTs > traceEnd) continue;
    const edgeKind = connIt.kind === 'provider' ? 'provider' : 'service';
    connections.push({
      kind: edgeKind,
      ts: startTs,
      endTs,
      clientUpid: connIt.client_upid,
      serverUpid: connIt.server_upid,
      label: connIt.label ?? undefined,
      detail: connIt.detail ?? undefined,
      waivesPriority:
        edgeKind === 'service' && hasPriorityWaivingFlag(connIt.bind_flags),
    });
  }

  return buildProcessGraphModel({
    traceStart,
    traceEnd,
    processes,
    stateIntervals,
    connections,
    freezerIntervals,
  });
}
