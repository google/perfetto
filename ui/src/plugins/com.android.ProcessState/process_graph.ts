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
import {assertExists} from '../../base/assert';
import type {Trace} from '../../public/trace';
import type {Row} from '../../trace_processor/query_result';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {CursorTooltip} from '../../widgets/cursor_tooltip';

export interface EdgeSel {
  readonly from: number;
  readonly to: number;
  readonly kind: 'service' | 'provider';
  readonly count: number;
  readonly names: string;
  readonly fg: boolean;
}

export interface ProcessGraphAttrs {
  readonly trace: Trace;
  readonly processes: ReadonlyArray<Row>;
  readonly bindingsQuery: number; // snapshot id
  readonly selectedPids?: ReadonlySet<number>;
  // additive (shift-click) toggles the pid in the selection instead of replacing.
  readonly onSelect: (pid: number, additive: boolean) => void;
  // Clicking an edge reports the binding up so the host can show its details.
  readonly onEdgeSelect?: (e: EdgeSel) => void;
  // Clicking empty canvas space clears the current selection.
  readonly onDeselect?: () => void;
}

interface Edge {
  readonly from: number; // client pid
  readonly to: number; // server pid
  readonly fg: boolean;
  readonly kind: 'service' | 'provider';
  readonly count: number; // number of underlying bindings
  readonly names: string; // service short-names / provider authorities
}

// Map an oom-adj value to a tier index (column) and colour, so the graph is
// laid out left-to-right by importance (lower adj = more important = left).
function tier(adj: number): number {
  if (adj < 0) return 0; // persistent / system
  if (adj <= 100) return 1; // foreground / visible
  if (adj <= 200) return 2; // perceptible
  if (adj <= 250) return 3; // service
  if (adj < 900) return 4; // background / cached-ish
  return 5; // cached empty
}
const TIER_LABEL = [
  'persistent',
  'foreground & visible',
  'perceptible',
  'service',
  'background',
  'cached',
];
// Tableau 10 categorical palette — muted, even-weight colours.
const TIER_COLOR = [
  '#e15759', // persist  — red
  '#59a14f', // fg/vis   — green
  '#4e79a7', // percept  — blue
  '#b07aa1', // service  — purple
  '#f28e2b', // bg       — orange
  '#bab0ac', // cached   — grey
];
const EDGE_SERVICE = '#bab0ac';
const EDGE_PROVIDER = '#4e79a7';
const EDGE_FG = '#e15759';

// A hand-rolled, deterministic SVG node-graph: processes are nodes laid out in
// columns by oom-adj tier; service/content-provider bindings are directed
// edges (client -> server). Clicking a node selects it and drives the grids.
// Deterministic layout (vs a force sim) so positions stay stable while you
// scrub snapshots.
export class ProcessGraph implements m.ClassComponent<ProcessGraphAttrs> {
  private edges: Edge[] = [];
  private loadedFor = -1;
  private hoverPid?: number;
  private hoverEdge?: Edge;
  // CursorTooltip reads a module-level mouse position that's only set after the
  // first document mousemove. A node/edge mouseenter can fire under a stationary
  // cursor (element appears beneath it) before any move, which crashes the
  // tooltip. Gate on having actually seen pointer movement over the graph.
  private sawPointer = false;
  // The edge the user clicked: highlighted + its bindings kept on screen.
  private selectedEdge?: {from: number; to: number};
  // viewBox for zoom/pan. While userAdjusted is false the graph auto-fits to
  // content on every render (so scrubbing snapshots reframes to fit); once the
  // user zooms or pans we preserve their viewport. Fit resets userAdjusted.
  private vb?: {x: number; y: number; w: number; h: number};
  private userAdjusted = false;
  private vw = 0; // svg viewport px (for aspect-matched fit)
  private vh = 0;
  private cw = 0; // content bounds (for pan clamping)
  private ch = 0;
  // Pending pointer-down (not yet a drag) + whether we've crossed the drag
  // threshold. We only capture the pointer once a real drag starts, so plain
  // clicks still reach nodes/edges.
  private down?: {
    px: number;
    py: number;
    vx: number;
    vy: number;
    id: number;
    el: Element;
  };
  private dragging = false;
  // Set true once a real pan happens, so the click that ends a drag doesn't get
  // treated as an empty-space click (which would clear the selection).
  private didDrag = false;

  private onWheel(ev: WheelEvent) {
    if (!this.vb) return;
    this.userAdjusted = true;
    ev.preventDefault();
    const svg = ev.currentTarget as SVGSVGElement;
    const rect = svg.getBoundingClientRect();
    const fx = (ev.clientX - rect.left) / rect.width;
    const fy = (ev.clientY - rect.top) / rect.height;
    const k = ev.deltaY > 0 ? 1.12 : 1 / 1.12; // wheel down = zoom out
    const nw = Math.max(60, Math.min(this.vb.w * 8, this.vb.w * k));
    const nh = this.vb.h * (nw / this.vb.w);
    // keep the point under the cursor fixed
    this.vb = {
      x: this.vb.x + (this.vb.w - nw) * fx,
      y: this.vb.y + (this.vb.h - nh) * fy,
      w: nw,
      h: nh,
    };
  }
  private onDown(ev: PointerEvent) {
    if (!this.vb) return;
    // Record the start but DON'T capture yet — capturing here would steal the
    // click from nodes/edges. We promote to a drag only once the pointer moves.
    this.down = {
      px: ev.clientX,
      py: ev.clientY,
      vx: this.vb.x,
      vy: this.vb.y,
      id: ev.pointerId,
      el: ev.currentTarget as Element,
    };
    this.dragging = false;
  }
  private onMove(ev: PointerEvent) {
    this.sawPointer = true; // safe now to show the cursor-following tooltip
    if (!this.down || !this.vb) return;
    const dx = ev.clientX - this.down.px;
    const dy = ev.clientY - this.down.py;
    if (!this.dragging) {
      if (Math.hypot(dx, dy) < 4) return; // still a click, not a drag
      this.dragging = true;
      this.didDrag = true;
      this.userAdjusted = true;
      this.down.el.setPointerCapture(this.down.id);
    }
    const rect = (ev.currentTarget as Element).getBoundingClientRect();
    let nx = this.down.vx - (dx / rect.width) * this.vb.w;
    let ny = this.down.vy - (dy / rect.height) * this.vb.h;
    // Clamp so at least a margin of content stays on screen (can't lose it).
    const M = 60;
    nx = Math.max(-this.vb.w + M, Math.min(this.cw - M, nx));
    ny = Math.max(-this.vb.h + M, Math.min(this.ch - M, ny));
    this.vb.x = nx;
    this.vb.y = ny;
  }
  private onUp() {
    this.down = undefined;
    this.dragging = false;
  }

  // Tooltips are intentionally light: they only surface what the graph itself
  // can't show — the full (un-truncated) name and the binding kind. Everything
  // else lives in the detail tables you get on click.

  // Node: just the full process name (the label in the graph is truncated).
  private nodeTooltip(pid: number, byPid: Map<number, Row>): m.Children {
    const p = byPid.get(pid);
    return [
      m('.pf-ps-tip-name', String(p?.['name'] ?? pid)),
      m('.pf-ps-tip-sub', `pid ${pid}`),
    ];
  }

  // Edge: the binding kind (service / foreground service / content provider),
  // which the colour/dash hints at but doesn't spell out, plus client → host.
  private edgeTooltip(e: Edge, byPid: Map<number, Row>): m.Children {
    const nm = (pid: number) => String(byPid.get(pid)?.['name'] ?? pid);
    const kind =
      e.kind === 'provider'
        ? 'content-provider binding'
        : e.fg
          ? 'foreground service binding'
          : 'service binding';
    return [
      m('.pf-ps-tip-name', kind),
      m('.pf-ps-tip-sub', `${nm(e.from)} → ${nm(e.to)}`),
    ];
  }

  private async load(attrs: ProcessGraphAttrs) {
    const snap = attrs.bindingsQuery;
    const q = await attrs.trace.engine.query(`
      SELECT b.client_pid AS f, s.owning_pid AS t,
             max(b.flag_foreground_service) AS fg, count(*) AS cnt,
             group_concat(DISTINCT s.short_name) AS names
      FROM android_process_state_binding b
      JOIN android_process_state_service s
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
    // Content-provider edges: client -> provider-owning process.
    const pq = await attrs.trace.engine.query(`
      SELECT pb.client_pid AS f, p.owning_pid AS t, count(*) AS cnt,
             group_concat(DISTINCT p.authority) AS names
      FROM android_process_state_provider_binding pb
      JOIN android_process_state_provider p
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
    this.edges = edges;
    this.loadedFor = snap;
    m.redraw();
  }

  view({attrs}: m.Vnode<ProcessGraphAttrs>) {
    if (this.loadedFor !== attrs.bindingsQuery) {
      this.load(attrs).catch((e) => console.error('ProcessGraph', e));
    }

    // Two layouts share the same node-rendering / zoom-pan machinery:
    //  * tiers: oom-adj importance columns (left = more important).
    //  * tree:  nest each process under its adj_source_pid — the "alive because
    //           of" hierarchy; killing a node implicates its whole subtree.
    const colW = 250; // column pitch (tiers) / label gutter (tree)
    const labelDx = 9; // label starts this far right of the node
    const rowH = 26;
    const top = 34;
    const left = 24;
    const r = 5;
    const nameOf = (p: Row) => String(p['name'] ?? p['pid']);
    const adjOfRow = (p: Row) => Number(p['cur_adj'] ?? 999);

    const pos = new Map<
      number,
      {x: number; y: number; adj: number; name: string}
    >();
    let width = 0;
    let height = 0;

    // Lay processes out in oom-adj importance columns (left = most important).
    // Tier columns actually used this snapshot, left→right; empty tiers are
    // skipped so the layout stays compact (no blank gutter columns).
    const cols: number[][] = [[], [], [], [], [], []];
    const meta = new Map<number, {adj: number; name: string}>();
    for (const p of attrs.processes) {
      const pid = Number(p['pid']);
      const adj = adjOfRow(p);
      cols[tier(adj)].push(pid);
      meta.set(pid, {adj, name: nameOf(p)});
    }
    const activeTiers = cols.map((_, i) => i).filter((i) => cols[i].length > 0);
    activeTiers.forEach((t, ci) => {
      cols[t].forEach((pid, row) => {
        const mt = assertExists(meta.get(pid));
        pos.set(pid, {x: left + ci * colW, y: top + row * rowH, ...mt});
      });
    });
    height = top + 10 + Math.max(...cols.map((c) => c.length), 1) * rowH;
    width = left + Math.max(1, activeTiers.length) * colW;

    // The "big picture" at rest: size each node by how many bindings touch it,
    // so hubs (system_server, etc.) read as big dots the moment the graph opens
    // — the overall shape is visible WITHOUT drawing any edges (which would
    // re-clutter the labels). Connection count = in + out edges.
    const degree = new Map<number, number>();
    for (const e of this.edges) {
      degree.set(e.from, (degree.get(e.from) ?? 0) + 1);
      degree.set(e.to, (degree.get(e.to) ?? 0) + 1);
    }
    const radiusOf = (pid: number) =>
      r + Math.min(8, Math.sqrt(degree.get(pid) ?? 0) * 1.7);

    // Edges are HIDDEN by default (so labels are never crossed) and revealed
    // only for the focused node — the one you hover, or the selected one. A
    // hover is a light, transient preview; a click selects and draws the same
    // edges BOLD and persistent (they stay after the pointer leaves). This
    // turns the graph into a "point to peek, click to pin" tool.
    // A clicked edge persists its node's edges (focus falls back to the edge's
    // client end), so the wiring stays on screen after the pointer leaves.
    // The set of processes whose edges are revealed bold: the hovered node (a
    // transient preview) OR, when nothing is hovered, every selected node (so a
    // multi-selection shows the union of its wiring). A selected edge keeps its
    // client end's edges on screen too.
    const sel = attrs.selectedPids;
    const focusPids =
      this.hoverPid !== undefined
        ? new Set<number>([this.hoverPid])
        : new Set<number>(sel ?? []);
    if (this.selectedEdge) focusPids.add(this.selectedEdge.from);
    // Bold when the shown edges belong to the selection (not a transient hover):
    // either nothing is hovered, or we're hovering an already-selected node.
    const bold =
      this.hoverPid === undefined || (sel?.has(this.hoverPid) ?? false);
    const neighbours = new Set<number>();
    const edgeEls: m.Children[] = [];

    if (focusPids.size > 0) {
      // Edges HIDDEN by default (so labels are never crossed); revealed only for
      // the focused node(s) — hover = light preview, click = pinned bold.
      for (const e of this.edges) {
        if (!focusPids.has(e.from) && !focusPids.has(e.to)) continue;
        const a = pos.get(e.from);
        const b = pos.get(e.to);
        if (!a || !b) continue;
        neighbours.add(e.from);
        neighbours.add(e.to);
        const base =
          e.kind === 'provider' ? EDGE_PROVIDER : e.fg ? EDGE_FG : EDGE_SERVICE;
        const ax = a.x + radiusOf(e.from);
        const bx = b.x - radiusOf(e.to);
        const mx = (ax + bx) / 2;
        const d = `M ${ax} ${a.y} C ${mx} ${a.y}, ${mx} ${b.y}, ${bx} ${b.y}`;
        edgeEls.push(
          m(
            'g.pf-ps-edge',
            {
              onmouseenter: () => {
                this.hoverEdge = e;
              },
              onmouseleave: () => {
                if (this.hoverEdge === e) this.hoverEdge = undefined;
              },
              onclick: (ev: MouseEvent) => {
                ev.stopPropagation();
                this.selectedEdge = {from: e.from, to: e.to};
                attrs.onEdgeSelect?.({
                  from: e.from,
                  to: e.to,
                  kind: e.kind,
                  count: e.count,
                  names: e.names,
                  fg: e.fg,
                });
              },
            },
            [
              // fat transparent hit area so a thin curve is easy to click
              m('path', {
                d,
                'fill': 'none',
                'stroke': 'transparent',
                'stroke-width': 8,
              }),
              m('path', {
                d,
                'fill': 'none',
                // A clicked edge looks exactly like a node-highlighted edge (same
                // colour/weight); the binding it points to is shown in the details
                // panel, so it needs no extra emphasis of its own.
                'stroke': base,
                'stroke-opacity': bold ? 0.95 : 0.5,
                'stroke-width': bold ? 1.4 : 0.9,
                'stroke-dasharray': e.kind === 'provider' ? '4,3' : undefined,
                'marker-end': 'url(#pf-ps-arrow)',
              }),
            ],
          ),
        );
      }
    }

    // Super-faint baseline: ALL edges at very low opacity so the overall wiring
    // shape reads at rest, before anything is focused. Drawn UNDER the focused
    // bold edges; each carries an invisible fat hit area so even a faint edge can
    // be hovered (tooltip) and clicked (select). The hovered one brightens.
    const baseEls: m.Children[] = [];
    {
      for (const e of this.edges) {
        const a = pos.get(e.from);
        const b = pos.get(e.to);
        if (!a || !b) continue;
        const ax = a.x + radiusOf(e.from);
        const bx = b.x - radiusOf(e.to);
        const mx = (ax + bx) / 2;
        const d = `M ${ax} ${a.y} C ${mx} ${a.y}, ${mx} ${b.y}, ${bx} ${b.y}`;
        const hovered = this.hoverEdge === e;
        baseEls.push(
          m(
            'g.pf-ps-edge',
            {
              onmouseenter: () => {
                this.hoverEdge = e;
              },
              onmouseleave: () => {
                if (this.hoverEdge === e) this.hoverEdge = undefined;
              },
              onclick: (ev: MouseEvent) => {
                ev.stopPropagation();
                this.selectedEdge = {from: e.from, to: e.to};
                attrs.onEdgeSelect?.({
                  from: e.from,
                  to: e.to,
                  kind: e.kind,
                  count: e.count,
                  names: e.names,
                  fg: e.fg,
                });
              },
            },
            [
              m('path', {
                d,
                'fill': 'none',
                'stroke': 'transparent',
                'stroke-width': 7,
              }),
              m('path', {
                d,
                'fill': 'none',
                'stroke':
                  e.kind === 'provider'
                    ? EDGE_PROVIDER
                    : e.fg
                      ? EDGE_FG
                      : EDGE_SERVICE,
                'stroke-opacity': hovered ? 0.9 : 0.08,
                'stroke-width': hovered ? 2 : 1,
                'stroke-dasharray': e.kind === 'provider' ? '5,3' : undefined,
                'pointer-events': 'none',
              }),
            ],
          ),
        );
      }
    }

    const nodeEls: m.Children[] = [];
    for (const [pid, p] of pos) {
      const isSel = sel?.has(pid) ?? false;
      // When something is focused, dim everything that isn't focused or a neighbour.
      const dim =
        focusPids.size > 0 && !focusPids.has(pid) && !neighbours.has(pid);
      // Chop hard (full name is in the hover tooltip); show … when truncated.
      const shortName = p.name.replace(/^.*\//, '');
      const label = `${shortName.length > 15 ? shortName.slice(0, 14) + '…' : shortName} ${pid}`;
      const nodeR = radiusOf(pid);
      nodeEls.push(
        m(
          'g.pf-ps-node' + (isSel ? '.pf-ps-node--sel' : ''),
          {
            onclick: (ev: MouseEvent) => {
              ev.stopPropagation();
              attrs.onSelect(pid, ev.shiftKey);
            },
            onmouseenter: () => {
              this.hoverPid = pid;
            },
            onmouseleave: () => {
              // Keep the focus (and its edges) while panning — the pointer
              // naturally leaves the node during a drag.
              if (this.hoverPid === pid && !this.dragging) {
                this.hoverPid = undefined;
              }
            },
            style: dim ? 'opacity:0.25' : undefined,
          },
          [
            // stroke/stroke-width come from CSS (theme-aware); fill + radius here.
            m('circle', {
              cx: p.x,
              cy: p.y,
              r: isSel ? nodeR + 2 : nodeR,
              fill: TIER_COLOR[tier(p.adj)],
            }),
            // White halo (paint-order: stroke) keeps the label legible.
            m(
              'text.pf-ps-nlabel',
              {
                'x': p.x + nodeR + labelDx,
                'y': p.y + 3,
                'font-size': 10,
                'font-weight': isSel || focusPids.has(pid) ? 'bold' : 'normal',
              },
              label,
            ),
          ],
        ),
      );
    }

    const headers = activeTiers.map((t, ci) =>
      m(
        'text.pf-ps-col',
        {
          'x': left + ci * colW,
          'y': 18,
          'font-size': 11,
          'font-weight': 'bold',
        },
        TIER_LABEL[t],
      ),
    );

    // Auto-fit to content until the user manually zooms/pans (see userAdjusted).
    if (!this.userAdjusted || this.vb === undefined) {
      // Fill the viewport with no dead side-margin: match the viewBox aspect to
      // the viewport by SHRINKING the over-long viewBox axis (so the content
      // fills the constraining dimension and the overflow is just a pan away),
      // rather than padding the content into the centre of an expanded box —
      // which left big empty margins when the area was wide and short.
      const pad = 12;
      let w = width;
      let h = height;
      if (this.vw > 0 && this.vh > 0) {
        const ar = this.vw / this.vh;
        if (w / h < ar) {
          h = w / ar;
        } // wide viewport: fill width, pan vertically
        else w = h * ar; // tall viewport: fill height, pan horizontally
      }
      this.vb = {x: -pad, y: -pad, w: w + pad, h: h + pad};
    }
    this.cw = width;
    this.ch = height;
    const vb = this.vb;

    // Mouse-following tooltip (Perfetto standard): real data only, full names.
    const byPid = new Map<number, Row>(
      attrs.processes.map((p) => [Number(p['pid']), p]),
    );
    const tip =
      this.dragging || !this.sawPointer
        ? undefined
        : this.hoverEdge
          ? this.edgeTooltip(this.hoverEdge, byPid)
          : this.hoverPid !== undefined
            ? this.nodeTooltip(this.hoverPid, byPid)
            : undefined;

    return m('.pf-ps-graph', [
      tip !== undefined && m(CursorTooltip, {className: 'pf-ps-tip'}, tip),
      m('.pf-ps-legend', [
        m(
          'span.pf-ps-hint',
          'click a node or edge for details · hover to peek · scroll = zoom · drag = pan',
        ),
        m('span.pf-ps-grow'),
        m('span.sv', '── service'),
        m('span.fg', '── foreground'),
        m('span.pv', '╌╌ provider'),
        m(
          'button.pf-ps-fit',
          {
            onclick: () => {
              this.userAdjusted = false;
            },
          },
          'Fit',
        ),
      ]),
      m(
        'svg.pf-ps-svg',
        {
          width: '100%',
          height: '100%',
          viewBox: `${vb.x} ${vb.y} ${vb.w} ${vb.h}`,
          preserveAspectRatio: 'xMidYMid meet',
          oncreate: (v: m.VnodeDOM) => {
            const r = (v.dom as Element).getBoundingClientRect();
            this.vw = r.width;
            this.vh = r.height;
          },
          onupdate: (v: m.VnodeDOM) => {
            const r = (v.dom as Element).getBoundingClientRect();
            // When the viewport actually changes size (e.g. the details-panel
            // divider or the graph splitter is dragged), re-fit so the content
            // fills the new shape instead of leaving letterbox margin. The auto-fit
            // reads vw/vh, so it lags one frame behind a resize without this redraw.
            if (
              r.width > 0 &&
              (Math.abs(r.width - this.vw) > 1 ||
                Math.abs(r.height - this.vh) > 1)
            ) {
              this.vw = r.width;
              this.vh = r.height;
              if (!this.userAdjusted) m.redraw();
            }
          },
          onwheel: (e: WheelEvent) => this.onWheel(e),
          onpointerdown: (e: PointerEvent) => this.onDown(e),
          onpointermove: (e: PointerEvent) => this.onMove(e),
          onpointerup: () => this.onUp(),
          onpointerleave: () => this.onUp(),
          // A click on empty canvas (nodes/edges stopPropagation their own clicks)
          // clears the selection — unless it was the click that ended a pan.
          onclick: () => {
            if (this.didDrag) {
              this.didDrag = false;
              return;
            }
            attrs.onDeselect?.();
          },
        },
        [
          m(
            'defs',
            m(
              'marker',
              {
                id: 'pf-ps-arrow',
                viewBox: '0 0 10 10',
                refX: 9,
                refY: 5,
                // ~5x smaller than before (markerUnits defaults to strokeWidth, so
                // the head scales with the line; 1.6 keeps it a small neat tip).
                markerWidth: 1.6,
                markerHeight: 1.6,
                orient: 'auto-start-reverse',
                // inherit each edge's stroke colour so provider (blue) / fg (red) /
                // service (grey) arrows are coloured to match their line.
              },
              m('path', {d: 'M 0 0 L 10 5 L 0 10 z', fill: 'context-stroke'}),
            ),
          ),
          // Each layer in its own <g> so the node layer's child count is stable
          // when focus edges appear/disappear — otherwise the unkeyed nodes shift
          // and the one under the cursor gets recreated (hover flicker).
          m('g.pf-ps-layer-headers', headers),
          m('g.pf-ps-layer-base', baseEls),
          m('g.pf-ps-layer-edges', edgeEls),
          m('g.pf-ps-layer-nodes', nodeEls),
        ],
      ),
    ]);
  }
}
