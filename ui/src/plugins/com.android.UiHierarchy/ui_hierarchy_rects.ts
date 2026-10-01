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
import {Button} from '../../widgets/button';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {CanvasPanZoom, type Point, type Rect} from './canvas_pan_zoom';
import {kindToString, type UiHierarchyNode} from './ui_hierarchy_data';
import {displayName} from './ui_hierarchy_props';
import type {UiHierarchyViewOptions} from './ui_hierarchy_session';
import {
  WM_KIND_ACTIVITY,
  WM_KIND_DISPLAY,
  WM_KIND_DISPLAY_AREA,
  WM_KIND_TASK,
  WM_KIND_TASK_FRAGMENT,
  WM_KIND_TOKEN,
  WM_KIND_WINDOW,
} from './ui_hierarchy_wm';

// A node projected into base (zoom=1, pan=0) coordinates, in paint order
// (back to front).
interface ProjectedNode {
  readonly n: UiHierarchyNode;
  readonly base: Point[];
}

// A node as painted in the current frame, in screen coordinates.
interface PaintedNode {
  readonly n: UiHierarchyNode;
  readonly q: Point[];
  readonly bbox: Rect;
  readonly order: number;
  readonly area: number;
  readonly selected: boolean;
}

interface Theme {
  readonly text: string;
  readonly textMuted: string;
  readonly accent: string;
  readonly font: string;
}

export interface UiHierarchyRectsAttrs {
  readonly nodes: UiHierarchyNode[];
  readonly byNodeId: ReadonlyMap<string, UiHierarchyNode>;
  readonly selectedNodeId?: string;
  readonly onSelect: (nodeId: string) => void;
  // Double-click on a node. Return true if handled (otherwise the view zooms
  // to the node).
  readonly onActivate?: (nodeId: string) => boolean;
  // Drawn as faint, non-interactive outlines behind `nodes` (2D only), and
  // included in the fitted area so `nodes` keep their place on screen.
  readonly context?: UiHierarchyNode[];
  // 2D: the view is fitted to this rect (content coordinates) and nothing
  // is drawn outside it.
  readonly clip?: Rect;
  // 2D: nodes listed here are filled only inside these rects (content
  // coordinates), e.g. the touchable region of an input window; an empty
  // list leaves the node unfilled.
  readonly fillRegions?: ReadonlyMap<string, ReadonlyArray<Rect>>;
  readonly options: UiHierarchyViewOptions;
}

const SCENE_MARGIN = 20;
const INNER_LABEL_FONT_PX = 10;
const INNER_LABEL_PAD = 3;

const GESTURE_HELP =
  'Scroll: zoom at cursor\n' +
  'Ctrl/Cmd + scroll or pinch: smooth zoom\n' +
  'Shift + scroll: pan horizontally\n' +
  'Drag (left or middle button): pan\n' +
  'Double-click node: zoom to node; empty area: reset\n' +
  'Keys (canvas focused): +/- zoom, 0 reset, arrows pan, F fit selection';

export class UiHierarchyRectsView implements m.ClassComponent<UiHierarchyRectsAttrs> {
  private attrs?: UiHierarchyRectsAttrs;
  private canvas?: HTMLCanvasElement;
  private resizeObserver?: ResizeObserver;
  private rafId?: number;

  private readonly pz = new CanvasPanZoom({
    onChange: () => this.scheduleDraw(),
    onClick: (p) => this.select(this.hitTest(p)),
    onHover: (p, e) => this.hover(this.hitTest(p), e),
    onHoverEnd: () => this.hover(undefined),
    onDoubleClick: (p) => {
      const id = this.hitTest(p);
      if (id === undefined) this.pz.reset();
      else if (this.attrs?.onActivate?.(id) !== true) this.fitNode(id);
    },
    onFitSelection: () => {
      const id = this.attrs?.selectedNodeId;
      if (id !== undefined) this.fitNode(id);
    },
  });

  // Per-frame geometry, used for hit testing and fit-to-node.
  private painted: PaintedNode[] = [];
  // The clip rect in screen coordinates, if any.
  private clipQ?: Point[];
  // 2D: content to screen coordinates for this frame.
  private toScreen?: (p: Point) => Point;

  private hoveredNode?: UiHierarchyNode;
  private hoverClient?: Point;

  // State last rendered into the DOM overlay (see scheduleDraw).
  private renderedZoom = 1;
  private renderedDragging = false;

  oncreate(vnode: m.VnodeDOM<UiHierarchyRectsAttrs>): void {
    this.attrs = vnode.attrs;
    const canvas = vnode.dom.querySelector<HTMLCanvasElement>(
      'canvas.pf-uih-rects-canvas',
    );
    if (canvas === null) return;
    this.canvas = canvas;
    this.pz.attach(canvas);
    this.resizeObserver = new ResizeObserver(() => this.scheduleDraw());
    this.resizeObserver.observe(canvas);
    this.scheduleDraw();
  }

  onupdate(vnode: m.VnodeDOM<UiHierarchyRectsAttrs>): void {
    this.attrs = vnode.attrs;
    this.scheduleDraw();
  }

  onremove(): void {
    this.pz.detach();
    this.resizeObserver?.disconnect();
    this.resizeObserver = undefined;
    if (this.rafId !== undefined) cancelAnimationFrame(this.rafId);
    this.rafId = undefined;
    this.canvas = undefined;
  }

  view(vnode: m.Vnode<UiHierarchyRectsAttrs>): m.Children {
    this.attrs = vnode.attrs;
    this.renderedZoom = this.pz.zoom;
    this.renderedDragging = this.pz.isDragging;
    return m('.pf-uih-rects', [
      m('canvas.pf-uih-rects-canvas', {
        'class': this.pz.isDragging ? 'pf-uih-rects-canvas--dragging' : '',
        'aria-label': 'UI hierarchy layout. ' + GESTURE_HELP,
      }),
      this.renderZoomControls(),
      this.renderTooltip(),
    ]);
  }

  // ---------------------------------------------------------------------------
  // Interaction

  private select(nodeId: string | undefined): void {
    if (nodeId !== undefined) this.attrs?.onSelect(nodeId);
  }

  private hover(nodeId: string | undefined, e?: PointerEvent): void {
    const node =
      nodeId !== undefined ? this.attrs?.byNodeId.get(nodeId) : undefined;
    const client = node && e ? {x: e.clientX, y: e.clientY} : undefined;
    const changed =
      node !== this.hoveredNode ||
      client?.x !== this.hoverClient?.x ||
      client?.y !== this.hoverClient?.y;
    this.hoveredNode = node;
    this.hoverClient = client;
    if (changed) m.redraw();
  }

  // Returns the node at `p` (element coordinates): the smallest (innermost)
  // one containing it, ties broken by paint order.
  private hitTest(p: Point): string | undefined {
    if (this.clipQ !== undefined && !pointInPoly(p, this.clipQ)) {
      return undefined;
    }
    let best: PaintedNode | undefined;
    for (const d of this.painted) {
      if (!inRect(p, d.bbox) || !pointInPoly(p, d.q)) continue;
      if (
        best === undefined ||
        d.area < best.area ||
        (d.area === best.area && d.order > best.order)
      ) {
        best = d;
      }
    }
    return best?.n.nodeId;
  }

  private fitNode(nodeId: string): void {
    const d = this.painted.find((it) => it.n.nodeId === nodeId);
    if (d === undefined || this.canvas === undefined) return;
    const a = this.pz.toBase({x: d.bbox.left, y: d.bbox.top});
    const b = this.pz.toBase({x: d.bbox.right, y: d.bbox.bottom});
    this.pz.fitRect(
      {left: a.x, top: a.y, right: b.x, bottom: b.y},
      {
        left: 0,
        top: 0,
        right: this.canvas.clientWidth,
        bottom: this.canvas.clientHeight,
      },
      48,
    );
  }

  // ---------------------------------------------------------------------------
  // DOM overlays

  private renderZoomControls(): m.Children {
    return m('.pf-uih-zoom-controls', [
      m(Button, {
        icon: 'zoom_out',
        compact: true,
        title: 'Zoom out (-)',
        onclick: () => this.pz.zoomCentered(1 / 1.25),
      }),
      m(
        'button.pf-uih-zoom-controls__level',
        {
          title: 'Reset zoom (0)',
          onclick: () => this.pz.reset(),
        },
        `${Math.round(this.pz.zoom * 100)}%`,
      ),
      m(Button, {
        icon: 'zoom_in',
        compact: true,
        title: 'Zoom in (+)',
        onclick: () => this.pz.zoomCentered(1.25),
      }),
      m(Button, {
        icon: 'center_focus_strong',
        compact: true,
        title: 'Zoom to selected node (F)',
        disabled: this.attrs?.selectedNodeId === undefined,
        onclick: () => {
          const id = this.attrs?.selectedNodeId;
          if (id !== undefined) this.fitNode(id);
        },
      }),
      m(Button, {
        icon: 'fit_screen',
        compact: true,
        title: 'Fit all (0)',
        onclick: () => this.pz.reset(),
      }),
      m(Button, {
        icon: 'help_outline',
        compact: true,
        title: GESTURE_HELP,
      }),
    ]);
  }

  private renderTooltip(): m.Children {
    const node = this.hoveredNode;
    const at = this.hoverClient;
    if (node === undefined || at === undefined || this.pz.isDragging) {
      return null;
    }
    const w = node.boundsRight - node.boundsLeft;
    const h = node.boundsBottom - node.boundsTop;
    const text = node.isTextRedacted ? '<redacted>' : node.text;

    const row = (label: string, value: string, cls = '') =>
      m('.pf-uih-tooltip__row', [
        m('span.pf-uih-tooltip__key', label),
        m(`span.pf-uih-tooltip__val${cls}`, value),
      ]);

    return m(
      '.pf-uih-tooltip',
      {
        // Positioned after layout so it can be flipped to stay on screen.
        style: {left: `${at.x}px`, top: `${at.y}px`},
        oncreate: (v: m.VnodeDOM) => placeTooltip(v.dom as HTMLElement, at),
        onupdate: (v: m.VnodeDOM) => placeTooltip(v.dom as HTMLElement, at),
      },
      [
        m('.pf-uih-tooltip__header', [
          m(Chip, {
            label: kindToString(node.kind),
            intent: intentForKind(node.kind),
            compact: true,
          }),
          m('span.pf-uih-tooltip__name', node.name || `Node ${node.nodeId}`),
        ]),
        text !== undefined && text !== '' && row('Text', text, '.pf-uih-code'),
        node.contentDescription !== undefined &&
          row('Description', node.contentDescription),
        node.role !== undefined && row('Role', node.role),
        node.testTag !== undefined &&
          row('Test tag', node.testTag, '.pf-uih-code'),
        row(
          'Bounds',
          `[${node.boundsLeft}, ${node.boundsTop}, ${node.boundsRight}, ` +
            `${node.boundsBottom}]  ${w} x ${h} px`,
          '.pf-uih-code',
        ),
        (node.isClickable || !node.isVisible || node.isFocused) &&
          row(
            'State',
            [
              node.isClickable ? 'clickable' : '',
              node.isFocused ? 'focused' : '',
              node.isVisible ? '' : 'hidden',
            ]
              .filter((s) => s !== '')
              .join(', '),
          ),
      ],
    );
  }

  // ---------------------------------------------------------------------------
  // Painting

  private scheduleDraw(): void {
    if (this.rafId !== undefined) return;
    this.rafId = requestAnimationFrame(() => {
      this.rafId = undefined;
      this.draw();
      // The DOM overlay shows the zoom level and the drag cursor; only ask
      // Mithril to re-render when those changed (a redraw calls onupdate,
      // which schedules a paint, so an unconditional redraw would loop).
      if (
        this.renderedZoom !== this.pz.zoom ||
        this.renderedDragging !== this.pz.isDragging
      ) {
        m.redraw();
      }
    });
  }

  private draw(): void {
    const c = this.canvas;
    const attrs = this.attrs;
    if (c === undefined || attrs === undefined) return;
    const dpr = window.devicePixelRatio || 1;
    const cssW = c.clientWidth;
    const cssH = c.clientHeight;
    if (cssW === 0 || cssH === 0) return;
    const pxW = Math.round(cssW * dpr);
    const pxH = Math.round(cssH * dpr);
    if (c.width !== pxW || c.height !== pxH) {
      c.width = pxW;
      c.height = pxH;
    }
    const ctx = c.getContext('2d');
    if (ctx === null) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    const theme = readTheme(c);
    const hasArea = (n: UiHierarchyNode) =>
      n.boundsRight > n.boundsLeft && n.boundsBottom > n.boundsTop;
    const nodes = attrs.nodes.filter(hasArea);
    this.painted = [];

    if (nodes.length === 0) {
      ctx.fillStyle = theme.textMuted;
      ctx.font = `12px ${theme.font}`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('No nodes match the current filters.', cssW / 2, cssH / 2);
      return;
    }

    const context = attrs.options.projection3d
      ? []
      : (attrs.context ?? []).filter(hasArea);
    let projected: ProjectedNode[];
    this.clipQ = undefined;
    this.toScreen = undefined;
    if (attrs.options.projection3d) {
      projected = project3d(nodes, attrs.options, cssW, cssH);
    } else {
      const p = project2d([...nodes, ...context], cssW, cssH, attrs.clip);
      projected = p.items;
      this.toScreen = (pt) => this.pz.toScreen(p.fit(pt));
      if (attrs.clip !== undefined) {
        this.clipQ = rectQuad(attrs.clip).map((pt) =>
          this.pz.toScreen(p.fit(pt)),
        );
      }
    }
    const contextProjected = projected.splice(nodes.length);
    this.paintContext(ctx, contextProjected, theme);

    this.painted = projected.map((p, order) => {
      const q = p.base.map((pt) => this.pz.toScreen(pt));
      const bbox = boundsOf(q);
      return {
        n: p.n,
        q,
        bbox,
        order,
        area: polygonArea(q),
        selected: p.n.nodeId === attrs.selectedNodeId,
      };
    });

    ctx.save();
    if (this.clipQ !== undefined) {
      tracePoly(ctx, this.clipQ);
      ctx.clip();
    }
    const shading = attrs.options.shading;
    // In 2D every node overlaps its ancestors, so fills accumulate; keep them
    // faint there. In 3D layers are separated and can be more opaque.
    const fillAlpha = attrs.options.projection3d
      ? 0.3
      : layerAlpha(Math.max(1, ...nestingDepths(nodes).values()), 0.45);
    const total = this.painted.length;
    for (const d of this.painted) {
      this.paintRect(ctx, d, shading, fillAlpha, total, theme);
    }
    // The selection outline is painted last so it is never covered, but
    // without a fill so it never hides the nodes it contains.
    const sel = this.painted.find((d) => d.selected);
    if (sel !== undefined) {
      tracePoly(ctx, sel.q);
      ctx.lineWidth = 2;
      ctx.strokeStyle = theme.accent;
      ctx.stroke();
    }
    ctx.restore();
    const view = {left: 0, top: 0, right: cssW, bottom: cssH};
    this.paintInnerLabels(
      ctx,
      this.clipQ !== undefined
        ? intersectRects(view, boundsOf(this.clipQ))
        : view,
      theme,
    );
  }

  // Outlines only: labels would clash with the open window's own labels.
  private paintContext(
    ctx: CanvasRenderingContext2D,
    items: ProjectedNode[],
    theme: Theme,
  ): void {
    if (items.length === 0) return;
    ctx.save();
    ctx.setLineDash([4, 3]);
    ctx.lineWidth = 1;
    ctx.strokeStyle = withAlpha(theme.textMuted, 0.45);
    for (const it of items) {
      tracePoly(
        ctx,
        it.base.map((pt) => this.pz.toScreen(pt)),
      );
      ctx.stroke();
    }
    ctx.restore();
  }

  private paintRect(
    ctx: CanvasRenderingContext2D,
    d: PaintedNode,
    shading: UiHierarchyViewOptions['shading'],
    fillAlpha: number,
    total: number,
    theme: Theme,
  ): void {
    const base = colorForNode(d.n);
    if (shading !== 'wireframe') {
      if (d.selected) {
        ctx.fillStyle = withAlpha(theme.accent, Math.max(0.15, fillAlpha));
      } else {
        const k =
          shading === 'gradient'
            ? 0.55 + 0.45 * (d.order / Math.max(1, total - 1))
            : 1;
        ctx.fillStyle = shade(
          base,
          k,
          d.n.isVisible ? fillAlpha : fillAlpha / 3,
        );
      }
      const regions = this.attrs?.fillRegions?.get(d.n.nodeId);
      const toScreen = this.toScreen;
      if (regions !== undefined && toScreen !== undefined) {
        for (const r of regions) {
          tracePoly(ctx, rectQuad(r).map(toScreen));
          ctx.fill();
        }
      } else {
        tracePoly(ctx, d.q);
        ctx.fill();
      }
    }
    tracePoly(ctx, d.q);
    ctx.lineWidth = 1;
    ctx.strokeStyle = shade(base, 1, d.n.isVisible ? 0.8 : 0.3);
    ctx.stroke();
  }

  // Labels inside rects: the node's text, else its short name. Placed
  // front-to-back (innermost first) and skipped whenever they would overlap
  // an already placed label or not fit inside their rect, so text never
  // collides or spills out. Only the selected and hovered rects get
  // shortened labels. `view` is the visible area.
  private paintInnerLabels(
    ctx: CanvasRenderingContext2D,
    view: Rect,
    theme: Theme,
  ): void {
    ctx.font = `${INNER_LABEL_FONT_PX}px ${theme.font}`;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    const lineH = INNER_LABEL_FONT_PX + 2;
    const minW = 40;
    const placed: Rect[] = [];
    const hoveredId = this.hoveredNode?.nodeId;
    const focused = (d: PaintedNode) => d.selected || d.n.nodeId === hoveredId;
    const order = [...this.painted].sort(
      (a, b) =>
        Number(b.selected) - Number(a.selected) ||
        Number(focused(b)) - Number(focused(a)) ||
        a.area - b.area ||
        b.order - a.order,
    );
    for (const d of order) {
      const {bbox} = d;
      const w = bbox.right - bbox.left;
      const h = bbox.bottom - bbox.top;
      if (w < minW || h < lineH + 2 * INNER_LABEL_PAD) continue;
      if (bbox.right < view.left || bbox.left > view.right) continue;
      if (bbox.bottom < view.top || bbox.top > view.bottom) continue;
      // Keep labels of partially visible rects on screen.
      const x = Math.max(bbox.left, view.left) + INNER_LABEL_PAD;
      const y = Math.max(bbox.top, view.top) + INNER_LABEL_PAD;
      const maxW = Math.min(bbox.right, view.right) - x - INNER_LABEL_PAD;
      if (maxW < minW - 2 * INNER_LABEL_PAD) continue;
      if (y + lineH > Math.min(bbox.bottom, view.bottom)) continue;
      const raw =
        d.n.text !== undefined && d.n.text !== '' && !d.n.isTextRedacted
          ? d.n.text
          : displayName(d.n.name);
      if (!raw) continue;
      const label = fitText(ctx, raw, maxW);
      if (label === '') continue;
      if (label !== raw && !focused(d)) continue;
      const box = {
        left: x - 1,
        top: y - 1,
        right: x + ctx.measureText(label).width + 1,
        bottom: y + lineH,
      };
      if (placed.some((r) => rectsIntersect(r, box))) continue;
      placed.push(box);
      ctx.fillStyle = d.selected
        ? theme.text
        : d.n.isVisible
          ? withAlpha(theme.text, 0.85)
          : theme.textMuted;
      ctx.fillText(label, x, y);
    }
  }
}

// -----------------------------------------------------------------------------
// Projection

// Projects `nodes` flat, fitted to `fitTo` (default: all nodes). Also
// returns the transform, to project other content the same way.
function project2d(
  nodes: UiHierarchyNode[],
  sceneW: number,
  sceneH: number,
  fitTo?: Rect,
): {items: ProjectedNode[]; fit: (p: Point) => Point} {
  const quads = nodes.map((n) => ({
    n,
    q: rectQuad({
      left: n.boundsLeft,
      top: n.boundsTop,
      right: n.boundsRight,
      bottom: n.boundsBottom,
    }),
  }));
  const fit = fitTransform(
    fitTo ?? boundsOf(quads.flatMap((it) => it.q)),
    sceneW,
    sceneH,
  );
  return {items: quads.map((it) => ({n: it.n, base: it.q.map(fit)})), fit};
}

function project3d(
  nodes: UiHierarchyNode[],
  o: UiHierarchyViewOptions,
  sceneW: number,
  sceneH: number,
): ProjectedNode[] {
  const b = boundsOf(
    nodes.flatMap((n) => [
      {x: n.boundsLeft, y: n.boundsTop},
      {x: n.boundsRight, y: n.boundsBottom},
    ]),
  );
  const mx = (b.left + b.right) / 2;
  const my = (b.top + b.bottom) / 2;
  // Layers are stacked by nesting depth, so siblings share a plane and each
  // level of the tree sits in front of its parent.
  const depths = nestingDepths(nodes);
  const maxDepth = Math.max(1, ...depths.values());
  // Spacing scales with the content so the stack looks the same for any
  // screen size.
  const size = Math.hypot(b.right - b.left, b.bottom - b.top);
  const zStep = size * (0.005 + o.explode * 0.06);
  const mz = ((maxDepth - 1) * zStep) / 2;
  const pitch = (o.rotation * Math.PI) / 4;
  const yaw = (o.rotation * Math.PI) / 3;
  const cx = Math.cos(pitch);
  const sx = Math.sin(pitch);
  const cy = Math.cos(yaw);
  const sy = Math.sin(yaw);
  const project = (x: number, y: number, z: number): Point => {
    const X = x - mx;
    const Y = y - my;
    const Z = z - mz;
    const X1 = X * cy + Z * sy;
    const Z1 = -X * sy + Z * cy;
    return {x: X1, y: Y * cx - Z1 * sx};
  };

  // The camera never rotates past 90 degrees, so painting back to front is
  // simply ascending z, keeping the input (tree) order within a layer.
  const proj = nodes
    .map((n, i) => {
      const level = (depths.get(n.nodeId) ?? 1) - 1;
      const z = level * zStep;
      const ps = [
        project(n.boundsLeft, n.boundsTop, z),
        project(n.boundsRight, n.boundsTop, z),
        project(n.boundsRight, n.boundsBottom, z),
        project(n.boundsLeft, n.boundsBottom, z),
      ];
      return {n, ps, level, i};
    })
    .sort((a, b) => a.level - b.level || a.i - b.i);
  const fit = fitTransform(boundsOf(proj.flatMap((p) => p.ps)), sceneW, sceneH);
  return proj.map((p) => ({n: p.n, base: p.ps.map(fit)}));
}

// Uniformly scales and centres `content` into the scene with a margin.
function fitTransform(
  content: Rect,
  sceneW: number,
  sceneH: number,
): (p: Point) => Point {
  const cw = Math.max(1, content.right - content.left);
  const ch = Math.max(1, content.bottom - content.top);
  const scale = Math.max(
    1e-6,
    Math.min(
      (sceneW - 2 * SCENE_MARGIN) / cw,
      (sceneH - 2 * SCENE_MARGIN) / ch,
    ),
  );
  const offX = (sceneW - cw * scale) / 2 - content.left * scale;
  const offY = (sceneH - ch * scale) / 2 - content.top * scale;
  return (p) => ({x: offX + p.x * scale, y: offY + p.y * scale});
}

// -----------------------------------------------------------------------------
// Helpers

// Per-layer alpha such that `depth` stacked layers composite to `total`.
function layerAlpha(depth: number, total: number): number {
  return 1 - Math.pow(1 - total, 1 / Math.max(1, depth));
}

// Nesting depth (1 = root) of every node, counting only ancestors that are
// in `nodes`, so filtered-out nodes do not leave empty layers.
function nestingDepths(nodes: UiHierarchyNode[]): Map<string, number> {
  const byId = new Map(nodes.map((n) => [n.nodeId, n]));
  const depth = new Map<string, number>();
  const depthOf = (n: UiHierarchyNode): number => {
    const known = depth.get(n.nodeId);
    if (known !== undefined) return known;
    // Mark before recursing so malformed cycles terminate.
    depth.set(n.nodeId, 1);
    const parent =
      n.parentNodeId !== undefined ? byId.get(n.parentNodeId) : undefined;
    const d = parent !== undefined ? depthOf(parent) + 1 : 1;
    depth.set(n.nodeId, d);
    return d;
  };
  for (const n of nodes) depthOf(n);
  return depth;
}

function placeTooltip(el: HTMLElement, at: Point): void {
  const offset = 14;
  const margin = 8;
  const w = el.offsetWidth;
  const h = el.offsetHeight;
  let x = at.x + offset;
  let y = at.y + offset;
  if (x + w > window.innerWidth - margin) x = at.x - offset - w;
  if (y + h > window.innerHeight - margin) y = at.y - offset - h;
  el.style.left = `${Math.max(margin, x)}px`;
  el.style.top = `${Math.max(margin, y)}px`;
}

function readTheme(el: Element): Theme {
  const s = getComputedStyle(el);
  const v = (name: string, fallback: string) =>
    s.getPropertyValue(name).trim() || fallback;
  return {
    text: v('--pf-color-text', '#202124'),
    textMuted: v('--pf-color-text-muted', '#5f6368'),
    accent: v('--pf-color-accent', '#1a73e8'),
    font: v('--pf-font-compact', 'sans-serif'),
  };
}

export function intentForKind(kind: number): Intent {
  switch (kind) {
    case 1:
      return Intent.Primary;
    case 2:
      return Intent.Warning;
    case 3:
      return Intent.Success;
    default:
      return Intent.None;
  }
}

// Distinct colours for WM windows, which all share one kind.
const WINDOW_PALETTE = [
  '#3f51b5',
  '#00897b',
  '#8e24aa',
  '#f4511e',
  '#039be5',
  '#7cb342',
  '#d81b60',
  '#6d4c41',
];

// Node colour: per kind, except WM windows, which get a colour that is
// stable across snapshots (from their token).
export function colorForNode(n: UiHierarchyNode): string {
  if (n.kind === WM_KIND_WINDOW && n.wm !== undefined) {
    return WINDOW_PALETTE[Math.abs(n.wm.token) % WINDOW_PALETTE.length];
  }
  return colorForKind(n.kind);
}

// Kind colours, shared by the canvas and the hierarchy tree.
export function colorForKind(kind: number): string {
  switch (kind) {
    case 1:
      return '#00acc1'; // View
    case 2:
      return '#8e24aa'; // ComposeView
    case 3:
      return '#43a047'; // Compose node
    case 4:
      return '#fb8c00'; // Composable
    case WM_KIND_DISPLAY:
      return '#546e7a';
    case WM_KIND_DISPLAY_AREA:
      return '#90a4ae';
    case WM_KIND_TASK:
      return '#00897b';
    case WM_KIND_TASK_FRAGMENT:
      return '#4db6ac';
    case WM_KIND_ACTIVITY:
      return '#fb8c00';
    case WM_KIND_TOKEN:
      return '#9e9e9e';
    case WM_KIND_WINDOW:
      return '#3f51b5';
    default:
      return '#607d8b';
  }
}

function shade(hex: string, factor: number, alpha: number): string {
  const r = Math.round(parseInt(hex.slice(1, 3), 16) * factor);
  const g = Math.round(parseInt(hex.slice(3, 5), 16) * factor);
  const b = Math.round(parseInt(hex.slice(5, 7), 16) * factor);
  return `rgba(${r},${g},${b},${alpha.toFixed(3)})`;
}

// Applies an alpha to any CSS colour using color-mix, which the canvas accepts.
function withAlpha(color: string, alpha: number): string {
  return `color-mix(in srgb, ${color} ${Math.round(alpha * 100)}%, transparent)`;
}

function tracePoly(ctx: CanvasRenderingContext2D, q: Point[]): void {
  ctx.beginPath();
  ctx.moveTo(q[0].x, q[0].y);
  for (let i = 1; i < q.length; i++) ctx.lineTo(q[i].x, q[i].y);
  ctx.closePath();
}

export function boundsOf(pts: Point[]): Rect {
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (const p of pts) {
    left = Math.min(left, p.x);
    top = Math.min(top, p.y);
    right = Math.max(right, p.x);
    bottom = Math.max(bottom, p.y);
  }
  return {left, top, right, bottom};
}

function inRect(p: Point, r: Rect): boolean {
  return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

export function rectsIntersect(a: Rect, b: Rect): boolean {
  return (
    a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom
  );
}

function intersectRects(a: Rect, b: Rect): Rect {
  return {
    left: Math.max(a.left, b.left),
    top: Math.max(a.top, b.top),
    right: Math.min(a.right, b.right),
    bottom: Math.min(a.bottom, b.bottom),
  };
}

// Corners of `r`, clockwise from the top left.
function rectQuad(r: Rect): Point[] {
  return [
    {x: r.left, y: r.top},
    {x: r.right, y: r.top},
    {x: r.right, y: r.bottom},
    {x: r.left, y: r.bottom},
  ];
}

export function pointInPoly(p: Point, poly: Point[]): boolean {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const xi = poly[i].x;
    const yi = poly[i].y;
    const xj = poly[j].x;
    const yj = poly[j].y;
    const intersect =
      yi > p.y !== yj > p.y &&
      p.x < ((xj - xi) * (p.y - yi)) / (yj - yi || 1) + xi;
    if (intersect) inside = !inside;
  }
  return inside;
}

export function polygonArea(poly: Point[]): number {
  let area = 0;
  for (let i = 0; i < poly.length; i++) {
    const j = (i + 1) % poly.length;
    area += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
  }
  return Math.abs(area / 2);
}

// Truncates `text` with an ellipsis to fit in `maxW` using the current font.
// Returns '' if not even one character plus ellipsis fits.
export function fitText(
  ctx: CanvasRenderingContext2D,
  text: string,
  maxW: number,
): string {
  if (ctx.measureText(text).width <= maxW) return text;
  // Binary search the longest prefix that fits.
  let lo = 0;
  let hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (ctx.measureText(text.slice(0, mid) + '\u2026').width <= maxW) lo = mid;
    else hi = mid - 1;
  }
  return lo === 0 ? '' : text.slice(0, lo) + '\u2026';
}
