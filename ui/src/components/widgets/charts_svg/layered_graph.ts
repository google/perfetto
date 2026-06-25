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
import {ensureExists} from '../../../base/assert';
import './layered_graph.scss';
import {CursorTooltip} from '../../../widgets/cursor_tooltip';

// A generic, data-driven SVG node-graph chart, following the same
// data-in / callbacks-out convention as the other charts_svg widgets: a
// ClassComponent with a `readonly` Attrs interface, fed `nodes` / `edges` /
// `layerLabels` (like DataGrid is fed `data` / `schema`) and reporting selection
// back through callbacks. Nodes are laid out left-to-right in ordered "layers"
// (columns) with directed edges between them. It owns only presentation +
// interaction (deterministic layout, zoom / pan, hover, select, degree-based
// node sizing, edges-revealed-on-focus, diff styling), has no knowledge of what
// the nodes are, and is self-contained (its styles live in layered_graph.scss),
// so it can be reused outside any one plugin.

// Diff status of a node/edge vs a baseline. Absent = unchanged (no styling).
export type GraphDiff = 'added' | 'removed' | 'changed';

export interface GraphNode {
  readonly id: number;
  // Short display name; the widget truncates it and appends the id.
  readonly label: string;
  readonly layer: number; // column index into layerLabels (the layer)
  readonly fill: string; // circle fill colour
  readonly diff?: GraphDiff;
  readonly tooltip?: m.Children; // hover tooltip body (full name, etc.)
}

export interface GraphEdge {
  readonly id: string; // stable identity for selection / hover
  readonly from: number; // source node id
  readonly to: number; // target node id
  readonly color: string;
  readonly dashed?: boolean;
  readonly diff?: 'added' | 'removed'; // drawn prominently at rest in diff mode
  readonly tooltip?: m.Children;
}

export interface LayeredGraphAttrs {
  readonly nodes: ReadonlyArray<GraphNode>;
  readonly edges: ReadonlyArray<GraphEdge>;
  readonly layerLabels: ReadonlyArray<string>; // column headers, left→right
  readonly selectedIds?: ReadonlySet<number>;
  readonly selectedEdgeId?: string;
  // Legend content shown left of the Fit button (caller-specific swatches).
  readonly legend?: m.Children;
  // additive (shift-click) asks the caller to toggle the id in a multi-select.
  readonly onSelect?: (id: number, additive: boolean) => void;
  readonly onEdgeSelect?: (edgeId: string) => void;
  readonly onDeselect?: () => void;
}

export class LayeredGraph implements m.ClassComponent<LayeredGraphAttrs> {
  private hoverId?: number;
  private hoverEdgeId?: string;
  // CursorTooltip reads a module-level mouse position only set after the first
  // document mousemove; a mouseenter under a stationary cursor before any move
  // crashes it. Gate on having actually seen pointer movement over the graph.
  private sawPointer = false;
  // viewBox for zoom/pan. While userAdjusted is false the graph auto-fits to
  // content on every render; once the user zooms/pans we preserve their
  // viewport. Fit resets userAdjusted.
  private vb?: {x: number; y: number; w: number; h: number};
  private userAdjusted = false;
  private vw = 0; // svg viewport px (for aspect-matched fit)
  private vh = 0;
  private cw = 0; // content bounds (for pan clamping)
  private ch = 0;
  // Pending pointer-down (not yet a drag) + whether we crossed the threshold.
  // We only capture the pointer once a real drag starts, so plain clicks still
  // reach nodes/edges.
  private down?: {
    px: number;
    py: number;
    vx: number;
    vy: number;
    id: number;
    el: Element;
  };
  private dragging = false;
  // True once a real pan happens, so the click that ends a drag isn't treated
  // as an empty-space click (which would clear the selection).
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
    // Allow deep zoom-in (min viewBox 20px) so edges that bunch together can be
    // spread far enough apart to click individually.
    const nw = Math.max(20, Math.min(this.vb.w * 8, this.vb.w * k));
    const nh = this.vb.h * (nw / this.vb.w);
    this.vb = {
      x: this.vb.x + (this.vb.w - nw) * fx,
      y: this.vb.y + (this.vb.h - nh) * fy,
      w: nw,
      h: nh,
    };
  }
  private onDown(ev: PointerEvent) {
    if (!this.vb) return;
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
    this.sawPointer = true;
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
    const M = 60; // keep at least a margin of content on screen
    nx = Math.max(-this.vb.w + M, Math.min(this.cw - M, nx));
    ny = Math.max(-this.vb.h + M, Math.min(this.ch - M, ny));
    this.vb.x = nx;
    this.vb.y = ny;
  }
  private onUp() {
    this.down = undefined;
    this.dragging = false;
  }

  view({attrs}: m.Vnode<LayeredGraphAttrs>) {
    const colW = 250;
    const labelDx = 9;
    const rowH = 26;
    const top = 34;
    const left = 24;
    const r = 5;

    // Lay nodes out in their layer columns, left→right. Empty layers are
    // skipped so the layout stays compact (no blank gutter columns).
    const nLayers = attrs.layerLabels.length;
    const cols: number[][] = Array.from({length: nLayers}, () => []);
    const byId = new Map<number, GraphNode>();
    for (const n of attrs.nodes) {
      const b = Math.max(0, Math.min(nLayers - 1, n.layer));
      cols[b].push(n.id);
      byId.set(n.id, n);
    }
    // Connection count (in + out edges) per node; drives node size below and
    // the in-column ordering here.
    const degree = new Map<number, number>();
    for (const e of attrs.edges) {
      degree.set(e.from, (degree.get(e.from) ?? 0) + 1);
      degree.set(e.to, (degree.get(e.to) ?? 0) + 1);
    }
    // Within each layer, order nodes by size (degree) descending so the biggest
    // dots sit at the top of the column; tie-break by id for a stable layout.
    for (const col of cols) {
      col.sort((a, b) => (degree.get(b) ?? 0) - (degree.get(a) ?? 0) || a - b);
    }

    const activeLayers = cols
      .map((_, i) => i)
      .filter((i) => cols[i].length > 0);
    const pos = new Map<number, {x: number; y: number}>();
    activeLayers.forEach((b, ci) => {
      cols[b].forEach((id, row) => {
        pos.set(id, {x: left + ci * colW, y: top + row * rowH});
      });
    });
    const height = top + 10 + Math.max(...cols.map((c) => c.length), 1) * rowH;
    const width = left + Math.max(1, activeLayers.length) * colW;

    // Node size = connection count (in + out edges): hubs read as big dots the
    // moment the graph opens, so the overall shape is visible without drawing
    // any edges. `degree` is computed above (it also orders each column).
    const radiusOf = (id: number) =>
      r + Math.min(8, Math.sqrt(degree.get(id) ?? 0) * 1.7);

    const edgesById = new Map<string, GraphEdge>(
      attrs.edges.map((e) => [e.id, e]),
    );
    const sel = attrs.selectedIds;
    const selEdge =
      attrs.selectedEdgeId !== undefined
        ? edgesById.get(attrs.selectedEdgeId)
        : undefined;
    // Edges are hidden by default (so labels are never crossed) and revealed
    // only for the focused node(s): the hovered node (transient), or the
    // selected node(s) (pinned bold). A selected edge keeps its client end's
    // edges on screen too.
    const focusIds =
      this.hoverId !== undefined
        ? new Set<number>([this.hoverId])
        : new Set<number>(sel ?? []);
    if (selEdge) focusIds.add(selEdge.from);
    const bold =
      this.hoverId === undefined || (sel?.has(this.hoverId) ?? false);
    const neighbours = new Set<number>();

    const path = (e: GraphEdge) => {
      const a = ensureExists(pos.get(e.from));
      const b = ensureExists(pos.get(e.to));
      const ax = a.x + radiusOf(e.from);
      const bx = b.x - radiusOf(e.to);
      const mx = (ax + bx) / 2;
      return `M ${ax} ${a.y} C ${mx} ${a.y}, ${mx} ${b.y}, ${bx} ${b.y}`;
    };
    const edgeHandlers = (e: GraphEdge) => ({
      onmouseenter: () => {
        this.hoverEdgeId = e.id;
      },
      onmouseleave: () => {
        if (this.hoverEdgeId === e.id) this.hoverEdgeId = undefined;
      },
      onclick: (ev: MouseEvent) => {
        ev.stopPropagation();
        attrs.onEdgeSelect?.(e.id);
      },
    });

    // Focused (bold) edges for the hovered/selected node(s).
    const edgeEls: m.Children[] = [];
    if (focusIds.size > 0) {
      for (const e of attrs.edges) {
        if (!focusIds.has(e.from) && !focusIds.has(e.to)) continue;
        if (!pos.has(e.from) || !pos.has(e.to)) continue;
        neighbours.add(e.from);
        neighbours.add(e.to);
        const d = path(e);
        edgeEls.push(
          m('g.pf-lgraph-edge', edgeHandlers(e), [
            m('path', {
              'd': d,
              'fill': 'none',
              'stroke': 'transparent',
              // Constant ~12px clickable band at every zoom (non-scaling), so
              // selection tracks the pointer instead of ballooning when zoomed
              // in or vanishing when zoomed out.
              'stroke-width': 12,
              'vector-effect': 'non-scaling-stroke',
            }),
            m('path', {
              'd': d,
              'fill': 'none',
              'stroke': e.color,
              'stroke-opacity': bold ? 0.95 : 0.5,
              'stroke-width': bold ? 1.4 : 0.9,
              'stroke-dasharray': e.dashed ? '4,3' : undefined,
              'marker-end': 'url(#pf-ng-arrow)',
              'vector-effect': 'non-scaling-stroke',
            }),
          ]),
        );
      }
    }

    // Super-faint baseline: every edge at very low opacity so the wiring shape
    // reads at rest. Diff edges (added/removed) are drawn prominently here so a
    // diff is visible without focusing; unchanged edges stay faint (no diff
    // artifact). Each carries a fat invisible hit area so even a faint edge is
    // hoverable/clickable.
    const baseEls: m.Children[] = [];
    for (const e of attrs.edges) {
      if (!pos.has(e.from) || !pos.has(e.to)) continue;
      const d = path(e);
      const hovered = this.hoverEdgeId === e.id;
      const restOpacity = e.diff !== undefined ? 0.6 : 0.08;
      const restWidth = e.diff !== undefined ? 1.5 : 1;
      baseEls.push(
        m('g.pf-lgraph-edge', edgeHandlers(e), [
          m('path', {
            d,
            'fill': 'none',
            'stroke': 'transparent',
            'stroke-width': 12,
          }),
          m('path', {
            'd': d,
            'fill': 'none',
            'stroke': e.color,
            'stroke-opacity': hovered ? 0.9 : restOpacity,
            'stroke-width': hovered ? 2 : restWidth,
            'stroke-dasharray': e.dashed ? '5,3' : undefined,
            'pointer-events': 'none',
            'vector-effect': 'non-scaling-stroke',
          }),
        ]),
      );
    }

    const diffCls = (d?: GraphDiff) =>
      d === 'added'
        ? '.pf-lgraph-diff-added'
        : d === 'removed'
          ? '.pf-lgraph-diff-removed'
          : d === 'changed'
            ? '.pf-lgraph-diff-changed'
            : '';
    const nodeEls: m.Children[] = [];
    for (const [id, p] of pos) {
      const n = ensureExists(byId.get(id));
      const isSel = sel?.has(id) ?? false;
      const dim = focusIds.size > 0 && !focusIds.has(id) && !neighbours.has(id);
      const label = `${n.label.length > 15 ? n.label.slice(0, 14) + '…' : n.label} ${id}`;
      const nodeR = radiusOf(id);
      nodeEls.push(
        m(
          'g.pf-lgraph-node' +
            (isSel ? '.pf-lgraph-node--sel' : '') +
            diffCls(n.diff),
          {
            onclick: (ev: MouseEvent) => {
              ev.stopPropagation();
              attrs.onSelect?.(id, ev.shiftKey);
            },
            onmouseenter: () => {
              this.hoverId = id;
            },
            onmouseleave: () => {
              if (this.hoverId === id && !this.dragging) {
                this.hoverId = undefined;
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
              fill: n.fill,
            }),
            m(
              'text.pf-lgraph-nlabel',
              {
                'x': p.x + nodeR + labelDx,
                'y': p.y + 3,
                'font-size': 10,
                'font-weight': isSel || focusIds.has(id) ? 'bold' : 'normal',
              },
              label,
            ),
          ],
        ),
      );
    }

    const headers = activeLayers.map((b, ci) =>
      m(
        'text.pf-lgraph-col',
        {
          'x': left + ci * colW,
          'y': 18,
          'font-size': 11,
          'font-weight': 'bold',
        },
        attrs.layerLabels[b],
      ),
    );

    // Auto-fit to content until the user manually zooms/pans.
    if (!this.userAdjusted || this.vb === undefined) {
      const pad = 12;
      let w = width;
      let h = height;
      if (this.vw > 0 && this.vh > 0) {
        const ar = this.vw / this.vh;
        if (w / h < ar) h = w / ar;
        else w = h * ar;
      }
      this.vb = {x: -pad, y: -pad, w: w + pad, h: h + pad};
    }
    this.cw = width;
    this.ch = height;
    const vb = this.vb;

    const tip =
      this.dragging || !this.sawPointer
        ? undefined
        : this.hoverEdgeId !== undefined
          ? edgesById.get(this.hoverEdgeId)?.tooltip
          : this.hoverId !== undefined
            ? byId.get(this.hoverId)?.tooltip
            : undefined;

    return m('.pf-lgraph-graph', [
      tip !== undefined && tip !== null
        ? m(CursorTooltip, {className: 'pf-lgraph-tip'}, tip)
        : undefined,
      m('.pf-lgraph-legend', [
        m(
          'span.pf-lgraph-hint',
          'click a node or edge for details · hover to peek · scroll = zoom · drag = pan',
        ),
        m('span.pf-lgraph-grow'),
        attrs.legend,
        m(
          'button.pf-lgraph-fit',
          {
            onclick: () => {
              this.userAdjusted = false;
            },
          },
          'Fit',
        ),
      ]),
      m(
        'svg.pf-lgraph-svg',
        {
          width: '100%',
          height: '100%',
          viewBox: `${vb.x} ${vb.y} ${vb.w} ${vb.h}`,
          preserveAspectRatio: 'xMidYMid meet',
          oncreate: (v: m.VnodeDOM) => {
            const rc = (v.dom as Element).getBoundingClientRect();
            this.vw = rc.width;
            this.vh = rc.height;
          },
          onupdate: (v: m.VnodeDOM) => {
            const rc = (v.dom as Element).getBoundingClientRect();
            if (
              rc.width > 0 &&
              (Math.abs(rc.width - this.vw) > 1 ||
                Math.abs(rc.height - this.vh) > 1)
            ) {
              this.vw = rc.width;
              this.vh = rc.height;
              if (!this.userAdjusted) m.redraw();
            }
          },
          onwheel: (e: WheelEvent) => this.onWheel(e),
          onpointerdown: (e: PointerEvent) => this.onDown(e),
          onpointermove: (e: PointerEvent) => this.onMove(e),
          onpointerup: () => this.onUp(),
          onpointerleave: () => this.onUp(),
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
                id: 'pf-ng-arrow',
                viewBox: '0 0 10 10',
                refX: 9,
                refY: 5,
                markerWidth: 1.6,
                markerHeight: 1.6,
                orient: 'auto-start-reverse',
              },
              m('path', {d: 'M 0 0 L 10 5 L 0 10 z', fill: 'context-stroke'}),
            ),
          ),
          // Each layer in its own <g> so the node layer's child count is stable
          // when focus edges appear/disappear (otherwise unkeyed nodes shift and
          // the one under the cursor is recreated — hover flicker).
          m('g.pf-lgraph-layer-headers', headers),
          m('g.pf-lgraph-layer-base', baseEls),
          m('g.pf-lgraph-layer-edges', edgeEls),
          m('g.pf-lgraph-layer-nodes', nodeEls),
        ],
      ),
    ]);
  }
}
