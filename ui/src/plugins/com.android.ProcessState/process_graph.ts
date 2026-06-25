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
import type {Trace} from '../../public/trace';
import type {Row} from '../../trace_processor/query_result';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {LayeredGraph} from '../../components/widgets/charts_svg/layered_graph';
import type {
  GraphEdge,
  GraphNode,
} from '../../components/widgets/charts_svg/layered_graph';

export interface EdgeSel {
  readonly from: number;
  readonly to: number;
  readonly kind: 'service' | 'provider';
  readonly count: number;
  readonly names: string;
  readonly fg: boolean;
}

// Identity of an edge for selection toggling (count/names/fg are payload).
export function sameEdge(a: EdgeSel, b: EdgeSel): boolean {
  return a.from === b.from && a.to === b.to && a.kind === b.kind;
}

// Per-node diff status vs a baseline snapshot. Absent = unchanged.
export type DiffStatus = 'added' | 'removed' | 'changed';

export interface ProcessGraphAttrs {
  readonly trace: Trace;
  readonly processes: ReadonlyArray<Row>;
  readonly bindingsQuery: number; // snapshot id
  // Diff mode: status per pid vs the baseline, and the baseline snapshot id so
  // edges can be diffed too. `processes` is expected to already include the
  // removed (baseline-only) processes so they have a position.
  readonly diffNodes?: ReadonlyMap<number, DiffStatus>;
  readonly diffBaseline?: number;
  readonly selectedPids?: ReadonlySet<number>;
  readonly selectedEdge?: EdgeSel;
  readonly onSelect: (pid: number, additive: boolean) => void;
  readonly onEdgeSelect?: (e: EdgeSel) => void;
  readonly onDeselect?: () => void;
}

// One service / content-provider binding edge (client -> hosting process),
// summarised across the underlying ConnectionRecords.
interface Edge {
  readonly from: number;
  readonly to: number;
  readonly fg: boolean;
  readonly kind: 'service' | 'provider';
  readonly count: number;
  readonly names: string;
  diff?: DiffStatus; // 'added' / 'removed' vs the baseline (diff mode only)
}

const edgeKey = (from: number, to: number, kind: string) =>
  `${from}->${to}:${kind}`;

// oom-adj importance columns (buckets): lower adj = more important = further
// left. The order / label / colour / inclusive adj upper-bound of each bucket
// is the layout config LayeredGraph is driven by.
// Importance tiers, ordered most→least important. A process belongs to the
// first bucket whose maxAdj its oom_score is <= (i.e. the lower bound is the
// previous bucket's maxAdj, exclusive). Shared with the snapshot track's nested
// per-tier process-count counters, so this stays the single source of truth.
export interface Bucket {
  readonly label: string;
  readonly color: string; // Tableau 10 — muted, even-weight
  readonly maxAdj: number;
}
export const BUCKETS: ReadonlyArray<Bucket> = [
  {label: 'persistent', color: '#e15759', maxAdj: -1},
  {label: 'foreground & visible', color: '#59a14f', maxAdj: 100},
  {label: 'perceptible', color: '#4e79a7', maxAdj: 200},
  {label: 'service', color: '#b07aa1', maxAdj: 250},
  {label: 'background', color: '#f28e2b', maxAdj: 899},
  {label: 'cached', color: '#bab0ac', maxAdj: Infinity},
];
function tier(adj: number): number {
  const i = BUCKETS.findIndex((b) => adj <= b.maxAdj);
  return i < 0 ? BUCKETS.length - 1 : i;
}
const BUCKET_LABELS = BUCKETS.map((b) => b.label);

const EDGE_SERVICE = '#bab0ac';
const EDGE_PROVIDER = '#4e79a7';
const EDGE_FG = '#e15759';
// Diff palette: green = added, red = removed. Shared with the node CSS classes
// (.pf-lgraph-diff-*) and the page's in-place column deltas.
const DIFF_ADDED = '#43a047';
const DIFF_REMOVED = '#e53935';
// In diff mode every node is neutral grey so the green/amber/red diff rings
// pop instead of competing with the tier colours (e.g. red persistent vs red
// removed).
const DIFF_NEUTRAL = '#c4c4c4';

// Adapter that turns an ActivityManager snapshot (processes + their service /
// content-provider bindings) into the generic LayeredGraph's data model: this is
// the only place that knows about processes, oom-adj tiers and bindings.
export class ProcessGraph implements m.ClassComponent<ProcessGraphAttrs> {
  private edges: Edge[] = [];
  private loadedKey = '';

  private async loadEdgesFor(
    attrs: ProcessGraphAttrs,
    snap: number,
  ): Promise<Edge[]> {
    const q = await attrs.trace.engine.query(`
      SELECT b.client_pid AS f, s.owning_pid AS t,
             IFNULL(max(b.foreground), 0) AS fg, count(*) AS cnt,
             group_concat(DISTINCT s.name) AS names
      FROM _ps_service_binding b
      JOIN _ps_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${snap} AND b.client_pid != s.owning_pid
      GROUP BY b.client_pid, s.owning_pid`);
    const it = q.iter({f: NUM, t: NUM, fg: NUM, cnt: NUM, names: STR_NULL});
    const edges: Edge[] = [];
    for (; it.valid(); it.next()) {
      edges.push({
        from: it.f,
        to: it.t,
        fg: it.fg > 0,
        kind: 'service',
        count: it.cnt,
        names: it.names ?? '',
      });
    }
    const pq = await attrs.trace.engine.query(`
      SELECT pb.client_pid AS f, p.owning_pid AS t, count(*) AS cnt,
             group_concat(DISTINCT p.authority) AS names
      FROM _ps_provider_binding pb
      JOIN _ps_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${snap} AND pb.client_pid != p.owning_pid
      GROUP BY pb.client_pid, p.owning_pid`);
    const pit = pq.iter({f: NUM, t: NUM, cnt: NUM, names: STR_NULL});
    for (; pit.valid(); pit.next()) {
      edges.push({
        from: pit.f,
        to: pit.t,
        fg: false,
        kind: 'provider',
        count: pit.cnt,
        names: pit.names ?? '',
      });
    }
    return edges;
  }

  private async load(attrs: ProcessGraphAttrs) {
    // Mark as loaded up front so a query error can't re-trigger load() every
    // render (the edges just keep their previous value on failure).
    this.loadedKey = `${attrs.bindingsQuery}:${attrs.diffBaseline ?? ''}`;
    const cur = await this.loadEdgesFor(attrs, attrs.bindingsQuery);
    if (attrs.diffBaseline === undefined) {
      this.edges = cur;
    } else {
      // Diff mode: tag current-only edges 'added', append baseline-only edges
      // as 'removed'; same-key edges are left unchanged.
      const base = await this.loadEdgesFor(attrs, attrs.diffBaseline);
      const baseKeys = new Set(base.map((e) => edgeKey(e.from, e.to, e.kind)));
      const curKeys = new Set(cur.map((e) => edgeKey(e.from, e.to, e.kind)));
      const merged: Edge[] = cur.map((e) => ({
        ...e,
        diff: baseKeys.has(edgeKey(e.from, e.to, e.kind))
          ? undefined
          : ('added' as DiffStatus),
      }));
      for (const e of base) {
        if (!curKeys.has(edgeKey(e.from, e.to, e.kind))) {
          merged.push({...e, diff: 'removed'});
        }
      }
      this.edges = merged;
    }
    m.redraw();
  }

  // Tooltips surface what the graph can't show — the full name and the binding
  // kind; everything else is in the detail tables on click.
  private nodeTooltip(name: string, pid: number): m.Children {
    return [
      m('.pf-lgraph-tip-name', name),
      m('.pf-lgraph-tip-sub', `pid ${pid}`),
    ];
  }
  private edgeTooltip(e: Edge, nm: (pid: number) => string): m.Children {
    const kind =
      e.kind === 'provider'
        ? 'content-provider binding'
        : e.fg
          ? 'foreground service binding'
          : 'service binding';
    return [
      m('.pf-lgraph-tip-name', kind),
      m('.pf-lgraph-tip-sub', `${nm(e.from)} → ${nm(e.to)}`),
    ];
  }

  view({attrs}: m.Vnode<ProcessGraphAttrs>) {
    const loadKey = `${attrs.bindingsQuery}:${attrs.diffBaseline ?? ''}`;
    if (this.loadedKey !== loadKey) {
      this.load(attrs).catch((e) => console.error('ProcessGraph', e));
    }

    const fullName = (p: Row) => String(p['name'] ?? p['pid']);
    const nameByPid = new Map<number, string>(
      attrs.processes.map((p) => [Number(p['pid']), fullName(p)]),
    );
    const nm = (pid: number) => nameByPid.get(pid) ?? String(pid);

    const nodes: GraphNode[] = attrs.processes.map((p) => {
      const pid = Number(p['pid']);
      const t = tier(Number(p['oom_score'] ?? 999));
      return {
        id: pid,
        label: fullName(p).replace(/^.*\//, ''),
        layer: t,
        fill:
          attrs.diffBaseline !== undefined ? DIFF_NEUTRAL : BUCKETS[t].color,
        diff: attrs.diffNodes?.get(pid),
        tooltip: this.nodeTooltip(fullName(p), pid),
      };
    });

    const edges: GraphEdge[] = this.edges.map((e) => ({
      id: edgeKey(e.from, e.to, e.kind),
      from: e.from,
      to: e.to,
      color:
        e.diff === 'added'
          ? DIFF_ADDED
          : e.diff === 'removed'
            ? DIFF_REMOVED
            : e.kind === 'provider'
              ? EDGE_PROVIDER
              : e.fg
                ? EDGE_FG
                : EDGE_SERVICE,
      dashed: e.kind === 'provider' || e.diff === 'removed',
      diff:
        e.diff === 'added'
          ? 'added'
          : e.diff === 'removed'
            ? 'removed'
            : undefined,
      tooltip: this.edgeTooltip(e, nm),
    }));

    const legend =
      attrs.diffBaseline !== undefined
        ? [
            m('span.pf-lgraph-diff-leg.pf-lgraph-diff-added', '● added'),
            m('span.pf-lgraph-diff-leg.pf-lgraph-diff-changed', '● changed'),
            m('span.pf-lgraph-diff-leg.pf-lgraph-diff-removed', '╌ removed'),
          ]
        : [
            m('span', {style: `color:${EDGE_SERVICE}`}, '── service'),
            m('span', {style: `color:${EDGE_FG}`}, '── foreground'),
            m('span', {style: `color:${EDGE_PROVIDER}`}, '╌╌ provider'),
          ];

    return m(LayeredGraph, {
      nodes,
      edges,
      layerLabels: BUCKET_LABELS,
      selectedIds: attrs.selectedPids,
      selectedEdgeId: attrs.selectedEdge
        ? edgeKey(
            attrs.selectedEdge.from,
            attrs.selectedEdge.to,
            attrs.selectedEdge.kind,
          )
        : undefined,
      legend,
      onSelect: (id, additive) => attrs.onSelect(id, additive),
      onEdgeSelect: (edgeId) => {
        const e = this.edges.find(
          (x) => edgeKey(x.from, x.to, x.kind) === edgeId,
        );
        if (e !== undefined) {
          attrs.onEdgeSelect?.({
            from: e.from,
            to: e.to,
            kind: e.kind,
            count: e.count,
            names: e.names,
            fg: e.fg,
          });
        }
      },
      onDeselect: () => attrs.onDeselect?.(),
    });
  }
}
