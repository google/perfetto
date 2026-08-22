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

// The "Surface" view: a rotatable, stacked 3D view of the layer rectangles,
// rendered with pure 2D-canvas projection (no WebGL/Three.js, so it works
// headless).
// Each layer's bounds (layer space + affine transform) become a quad; quads are
// stacked along Z by draw depth, rotated (yaw + pitch) and orthographically
// projected. Painter-ordered by projected depth, coloured by visibility, with
// selection/pin highlight, labels, and click-to-select.

import m from 'mithril';
import {assertExists, assertIsInstance} from '../../base/assert';
import type {SfLayer, SfTransform} from './surfaceflinger_data';

interface Pt {
  x: number;
  y: number;
}
interface P3 {
  x: number;
  y: number;
  d: number;
}

function apply(t: SfTransform, x: number, y: number): Pt {
  return {x: t.dsdx * x + t.dtdx * y + t.tx, y: t.dtdy * x + t.dsdy * y + t.ty};
}

function quad(layer: SfLayer): Pt[] | undefined {
  const r = layer.rect;
  if (r === undefined || r.w <= 0 || r.h <= 0) return undefined;
  const t = layer.transform;
  return [
    apply(t, r.x, r.y),
    apply(t, r.x + r.w, r.y),
    apply(t, r.x + r.w, r.y + r.h),
    apply(t, r.x, r.y + r.h),
  ];
}

function pointInPoly(p: Pt, q: Pt[]): boolean {
  let inside = false;
  for (let i = 0, j = q.length - 1; i < q.length; j = i++) {
    const a = q[i];
    const b = q[j];
    if (
      a.y > p.y !== b.y > p.y &&
      p.x < ((b.x - a.x) * (p.y - a.y)) / (b.y - a.y) + a.x
    ) {
      inside = !inside;
    }
  }
  return inside;
}

export interface SfRectsOptions {
  showOnlyVisible: boolean;
  explode: number; // 0..1 Z separation (spacing)
  rotation: number; // 0..1 yaw/pitch
  shading: 'gradient' | 'opacity' | 'wireframe';
}

export interface SfRectsAttrs {
  layers: SfLayer[];
  selectedRowId?: number;
  hiddenLayerIds: Set<number>;
  pinnedLayerIds: Set<number>;
  onSelect: (rowId: number) => void;
  options: SfRectsOptions;
}

// Colours are read from the Perfetto theme variables at draw time (a 2D canvas
// context can't use CSS variables), so the view follows light/dark mode.
interface RectColors {
  visibleFill: string;
  invisibleFill: string;
  selectedFill: string;
  border: string;
  selectedBorder: string;
  pinnedBorder: string;
  label: string;
  halo: string;
}

function readColors(el: HTMLElement): RectColors {
  const cs = getComputedStyle(el);
  const v = (name: string, fallback: string) =>
    cs.getPropertyValue(name).trim() || fallback;
  return {
    visibleFill: v('--pf-color-success', 'rgb(0,128,0)'),
    invisibleFill: v('--pf-color-text-muted', 'rgb(150,150,150)'),
    selectedFill: v('--pf-color-accent', 'rgb(38,103,231)'),
    border: v('--pf-color-border', 'rgb(120,120,120)'),
    selectedBorder: v('--pf-color-accent', 'rgb(38,103,231)'),
    pinnedBorder: v('--pf-color-warning', 'rgb(232,158,0)'),
    label: v('--pf-color-text', 'rgb(40,40,40)'),
    halo: v('--pf-color-background', 'rgb(255,255,255)'),
  };
}

export class SfRectsView implements m.ClassComponent<SfRectsAttrs> {
  private canvas?: HTMLCanvasElement;
  private attrs?: SfRectsAttrs;
  private hit: Array<{rowId: number; q: Pt[]; order: number}> = [];
  // Clickable region (in the gutter) for each rendered label.
  private labelHit: Array<{rowId: number; x0: number; y0: number; y1: number}> =
    [];

  view(vnode: m.Vnode<SfRectsAttrs>) {
    this.attrs = vnode.attrs;
    return m('.pf-sf-rects', [
      m('canvas.pf-sf-rects__canvas', {
        onclick: (e: MouseEvent) => this.onClick(e),
        oncreate: (v: m.VnodeDOM) => {
          this.canvas = assertIsInstance(v.dom, HTMLCanvasElement);
          this.draw();
        },
        onupdate: () => this.draw(),
      }),
    ]);
  }

  private onClick(e: MouseEvent) {
    if (!this.canvas || !this.attrs) return;
    const r = this.canvas.getBoundingClientRect();
    const p = {x: e.clientX - r.left, y: e.clientY - r.top};
    // Clicking a label (in the gutter) selects its rect. Mithril redraws after
    // this handler, and selectLayer() redraws again once its args load.
    for (const lh of this.labelHit) {
      if (p.x >= lh.x0 && p.y >= lh.y0 && p.y <= lh.y1) {
        this.attrs.onSelect(lh.rowId);
        return;
      }
    }
    let best: {rowId: number; order: number} | undefined;
    for (const h of this.hit) {
      if (pointInPoly(p, h.q) && (best === undefined || h.order > best.order)) {
        best = {rowId: h.rowId, order: h.order};
      }
    }
    if (best) {
      this.attrs.onSelect(best.rowId);
    }
  }

  private draw() {
    const canvas = this.canvas;
    const attrs = this.attrs;
    if (!canvas || !attrs) return;
    const cssW = canvas.clientWidth || 600;
    const cssH = canvas.clientHeight || 420;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.floor(cssW * dpr);
    canvas.height = Math.floor(cssH * dpr);
    const ctx = assertExists(canvas.getContext('2d'));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);
    const col = readColors(canvas);

    const bounded = attrs.layers
      .map((l) => ({l, q: quad(l)}))
      .filter((d): d is {l: SfLayer; q: Pt[]} => d.q !== undefined)
      .filter((d) => !attrs.hiddenLayerIds.has(d.l.layerId));
    const items = bounded
      .filter((d) => !attrs.options.showOnlyVisible || d.l.isVisible)
      .sort((a, b) => (a.l.drawDepth ?? 0) - (b.l.drawDepth ?? 0)); // back->front

    if (items.length === 0) {
      // Distinguish "nothing here" from "everything here is hidden by the
      // only-visible filter" (e.g. an encoder display whose only layer is an
      // invisible mirror) so the empty state is actionable.
      const hiddenByVis = attrs.options.showOnlyVisible
        ? bounded.filter((d) => !d.l.isVisible).length
        : 0;
      const msg =
        hiddenByVis > 0
          ? `No visible layers — ${hiddenByVis} hidden. Uncheck “Only visible” to show.`
          : 'No layers with bounds in this snapshot.';
      ctx.fillStyle = col.label;
      ctx.globalAlpha = 0.6;
      ctx.font = '13px sans-serif';
      ctx.fillText(msg, 12, 24);
      ctx.globalAlpha = 1;
      this.hit = [];
      this.labelHit = [];
      return;
    }

    // Reserve a right-hand gutter for the leader-line labels, so text lives
    // beside the scene and never overlaps the rectangles.
    const gutterW = Math.min(220, Math.max(132, Math.floor(cssW * 0.32)));
    const sceneW = cssW - gutterW;

    const n = items.length;
    const zStep = attrs.options.explode * 160; // world Z per layer
    // Pitch (angleX) ramps to 22.5° and yaw (angleY) is 1.5x pitch, both smooth
    // from 0 so rotation=0 is a flat, straight-on 2D view.
    const pitch = attrs.options.rotation * (Math.PI / 8);
    const yaw = pitch * 1.5;
    const cy = Math.cos(yaw);
    const sy = Math.sin(yaw);
    const cx = Math.cos(pitch);
    const sx = Math.sin(pitch);

    // Scene centre (2D centroid of all corners; mid Z).
    let mx = 0;
    let my = 0;
    let cnt = 0;
    for (const d of items) {
      for (const p of d.q) {
        mx += p.x;
        my += p.y;
        cnt++;
      }
    }
    mx /= cnt;
    my /= cnt;
    const mz = ((n - 1) * zStep) / 2;

    const project = (x: number, y: number, z: number): P3 => {
      const X = x - mx;
      const Y = y - my;
      const Z = z - mz;
      // yaw around Y, then pitch around X.
      const X1 = X * cy + Z * sy;
      const Z1 = -X * sy + Z * cy;
      const Y2 = Y * cx - Z1 * sx;
      const Z2 = Y * sx + Z1 * cx;
      return {x: X1, y: Y2, d: Z2};
    };

    const proj = items.map((d, i) => {
      const z = i * zStep;
      const ps = d.q.map((p) => project(p.x, p.y, z));
      const depth = ps.reduce((s, p) => s + p.d, 0) / ps.length;
      return {l: d.l, ps, depth};
    });

    // Fit projected points into the canvas.
    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    for (const d of proj) {
      for (const p of d.ps) {
        minX = Math.min(minX, p.x);
        minY = Math.min(minY, p.y);
        maxX = Math.max(maxX, p.x);
        maxY = Math.max(maxY, p.y);
      }
    }
    // Fit the projected scene into the area left of the label gutter.
    const margin = 18;
    const sw = maxX - minX || 1;
    const sh = maxY - minY || 1;
    const scale = Math.min(
      (sceneW - 2 * margin) / sw,
      (cssH - 2 * margin) / sh,
    );
    const offX = margin + (sceneW - 2 * margin - sw * scale) / 2 - minX * scale;
    const offY = margin + (cssH - 2 * margin - sh * scale) / 2 - minY * scale;
    const toScreen = (p: P3): Pt => ({
      x: offX + p.x * scale,
      y: offY + p.y * scale,
    });

    // Painter order: far (small projected depth) first.
    proj.sort((a, b) => a.depth - b.depth);

    // Resolve the three fill colours to RGB once so gradient mode can darken by
    // depth (solid) and opacity mode can apply the layer's own alpha.
    const rgbVisible = parseRgb(ctx, col.visibleFill);
    const rgbInvisible = parseRgb(ctx, col.invisibleFill);
    const rgbSelected = parseRgb(ctx, col.selectedFill);
    const mode = attrs.options.shading;

    this.hit = [];
    const drawn = proj.map((d, order) => {
      const sq = d.ps.map(toScreen);
      this.hit.push({rowId: d.l.rowId, q: sq, order});
      return {
        l: d.l,
        sq,
        order,
        selected: d.l.rowId === attrs.selectedRowId,
        pinned: attrs.pinnedLayerIds.has(d.l.layerId),
      };
    });

    // Fills + borders.
    for (const d of drawn) {
      const rgb = d.selected
        ? rgbSelected
        : d.l.isVisible
          ? rgbVisible
          : rgbInvisible;
      ctx.beginPath();
      ctx.moveTo(d.sq[0].x, d.sq[0].y);
      for (let i = 1; i < d.sq.length; i++) ctx.lineTo(d.sq[i].x, d.sq[i].y);
      ctx.closePath();
      if (mode === 'opacity') {
        const op = d.l.opacity ?? (d.l.isVisible ? 0.72 : 0.28);
        ctx.fillStyle = `rgba(${rgb.r},${rgb.g},${rgb.b},${op.toFixed(3)})`;
        ctx.fill();
      } else if (mode === 'gradient') {
        // Front layers brighter, back layers darker (solid fill).
        const k = 0.55 + 0.45 * (d.order / Math.max(1, n - 1));
        ctx.fillStyle = `rgb(${Math.round(rgb.r * k)},${Math.round(
          rgb.g * k,
        )},${Math.round(rgb.b * k)})`;
        ctx.fill();
      } // wireframe: no fill, borders only.
      ctx.lineWidth = d.pinned ? 3 : d.selected ? 2.5 : 1;
      ctx.strokeStyle = d.pinned
        ? col.pinnedBorder
        : d.selected
          ? col.selectedBorder
          : col.border;
      ctx.stroke();
    }

    this.drawLabels(ctx, col, drawn, sceneW, cssH, gutterW);
  }

  // Leader-line labels in the right gutter: every label sits in its own vertical
  // slot (never overlapping another label or a rect) and a thin line connects it
  // to the rect. Labels every rect when there are few; above a cap of 30, only
  // the selected/pinned rects are labelled.
  private drawLabels(
    ctx: CanvasRenderingContext2D,
    col: RectColors,
    drawn: Array<{
      l: SfLayer;
      sq: Pt[];
      order: number;
      selected: boolean;
      pinned: boolean;
    }>,
    sceneW: number,
    cssH: number,
    gutterW: number,
  ) {
    this.labelHit = [];
    const labelAll = drawn.length <= 30;
    let cands = drawn.filter((d) =>
      labelAll
        ? d.selected || d.pinned || d.l.isVisible
        : d.selected || d.pinned,
    );
    // Priority: selected/pinned first, then front-most.
    cands.sort((a, b) => {
      const pa = a.selected || a.pinned ? 1 : 0;
      const pb = b.selected || b.pinned ? 1 : 0;
      return pa !== pb ? pb - pa : b.order - a.order;
    });

    const lineH = 16;
    const pad = 8;
    const maxLabels = Math.max(0, Math.floor((cssH - 2 * pad) / lineH));
    cands = cands.slice(0, maxLabels);
    if (cands.length === 0) return;

    // Anchor each label to its rect's rightmost corner (closest to the gutter)
    // to keep leader lines short, then stack labels top-to-bottom with at least
    // lineH between them, always leaving room for the labels below.
    const anchorOf = (sq: Pt[]) =>
      sq.reduce((b, p) => (p.x > b.x ? p : b), sq[0]);
    const placed = cands
      .map((d) => {
        const a = anchorOf(d.sq);
        return {d, ax: a.x, ay: a.y, y: 0};
      })
      .sort((a, b) => a.ay - b.ay);
    let nextMin = pad;
    for (let i = 0; i < placed.length; i++) {
      const remainingAfter = placed.length - 1 - i;
      // Leave a few px of descender headroom so the bottom label isn't clipped.
      const maxY = cssH - pad - 3 - remainingAfter * lineH;
      let y = Math.max(placed[i].ay, nextMin);
      if (y > maxY) y = maxY;
      placed[i].y = y;
      nextMin = y + lineH;
    }

    const labelX = sceneW + 10;
    const maxTextW = gutterW - 18;
    for (const it of placed) {
      // The whole gutter row for this label is clickable (selects the rect).
      this.labelHit.push({
        rowId: it.d.l.rowId,
        x0: sceneW,
        y0: it.y - lineH + 3,
        y1: it.y + 4,
      });
      const strong = it.d.selected || it.d.pinned;
      const lineColor = it.d.pinned
        ? col.pinnedBorder
        : it.d.selected
          ? col.selectedBorder
          : col.border;
      // Leader line.
      ctx.strokeStyle = lineColor;
      ctx.lineWidth = strong ? 1.5 : 1;
      ctx.globalAlpha = strong ? 0.95 : 0.5;
      ctx.beginPath();
      ctx.moveTo(it.ax, it.ay);
      ctx.lineTo(labelX - 4, it.y - 4);
      ctx.stroke();
      ctx.globalAlpha = 1;
      // Anchor dot.
      ctx.beginPath();
      ctx.arc(it.ax, it.ay, 2.5, 0, Math.PI * 2);
      ctx.fillStyle = lineColor;
      ctx.fill();
      // Text with a halo so it stays legible over any leader line.
      ctx.font = `${strong ? 'bold ' : ''}11px sans-serif`;
      const text = fitText(ctx, it.d.l.name, maxTextW);
      ctx.lineJoin = 'round';
      ctx.lineWidth = 3;
      ctx.strokeStyle = col.halo;
      ctx.strokeText(text, labelX, it.y);
      ctx.fillStyle = strong ? lineColor : col.label;
      ctx.fillText(text, labelX, it.y);
    }
  }
}

// Normalises any CSS colour string to RGB components by round-tripping it
// through the canvas (which returns #rrggbb or rgba(...)).
function parseRgb(
  ctx: CanvasRenderingContext2D,
  color: string,
): {r: number; g: number; b: number} {
  ctx.fillStyle = color;
  const s = ctx.fillStyle as string;
  if (s.startsWith('#')) {
    return {
      r: parseInt(s.slice(1, 3), 16),
      g: parseInt(s.slice(3, 5), 16),
      b: parseInt(s.slice(5, 7), 16),
    };
  }
  const m = s.match(/rgba?\(([^)]+)\)/);
  if (m) {
    const [r, g, b] = m[1].split(',').map((x) => parseFloat(x));
    return {r, g, b};
  }
  return {r: 128, g: 128, b: 128};
}

// Truncates text with an ellipsis to fit maxW px (font must already be set).
function fitText(
  ctx: CanvasRenderingContext2D,
  name: string,
  maxW: number,
): string {
  if (ctx.measureText(name).width <= maxW) return name;
  let s = name;
  while (s.length > 1 && ctx.measureText(s + '…').width > maxW) {
    s = s.slice(0, -1);
  }
  return s + '…';
}
