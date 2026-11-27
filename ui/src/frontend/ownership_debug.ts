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

import './ownership_debug.scss';
import m from 'mithril';
import {hash} from '../base/hash';
import {findAttributionAt, UNATTRIBUTED} from '../core/ownership';
import {
  type PointComparison,
  ScreenSnapshot,
} from '../core/screen_share/screen_snapshot';

const REFRESH_MS = 1000;

// The comparison looks up a point every 8 px, about 32,000 points on a large
// screen, so it runs less often.
const COMPARE_REFRESH_MS = 3000;
const COMPARE_STEP = 8;
const COMPARE_EDGE_BAND = 2;

// Unattributed areas use a fixed, bright color so gaps stand out.
const UNATTRIBUTED_COLOR = 'rgba(255, 0, 255, 0.6)';
const DISAGREEMENT_COLOR = 'rgb(230, 0, 0)';

// Boxes smaller than this get no owner name.
const MIN_LABELED_WIDTH = 80;
const MIN_LABELED_HEIGHT = 14;

function fillFor(owner: string): string {
  if (owner === UNATTRIBUTED) return UNATTRIBUTED_COLOR;
  return `hsla(${hash(owner, 360)}, 75%, 50%, 0.35)`;
}

function outlineFor(owner: string): string {
  if (owner === UNATTRIBUTED) return 'rgba(255, 0, 255, 0.9)';
  return `hsla(${hash(owner, 360)}, 75%, 35%, 0.9)`;
}

// A debug overlay that shows who owns the screen:
// - each owner's area, painted from the labeled boxes, with each box
//   outlined and named;
// - a red dot wherever the browser draws something whose owner differs from
//   what the boxes say;
// - a legend with each owner's share of the screen and the disagreements;
// - a readout next to the pointer with the owner under it.
//
// Everything has `pointer-events: none` and no owner label, so the UI stays
// usable and neither the boxes nor the point lookup see the overlay.
class OwnershipOverlay {
  private root?: HTMLElement;
  private canvas?: HTMLCanvasElement;
  private legend?: HTMLElement;
  private cursor?: HTMLElement;
  private timer?: ReturnType<typeof setInterval>;
  private snapshot?: ScreenSnapshot;
  private comparison?: PointComparison;
  private comparedAt = 0;

  toggle(): void {
    this.root ? this.hide() : this.show();
  }

  private show(): void {
    const root = document.createElement('div');
    root.className = 'pf-ownership-overlay';
    const canvas = document.createElement('canvas');
    canvas.className = 'pf-ownership-overlay__canvas';
    const legend = document.createElement('div');
    legend.className = 'pf-ownership-overlay__legend';
    const cursor = document.createElement('div');
    cursor.className = 'pf-ownership-overlay__cursor';
    root.append(canvas, legend, cursor);
    document.body.appendChild(root);
    this.root = root;
    this.canvas = canvas;
    this.legend = legend;
    this.cursor = cursor;
    window.addEventListener('pointermove', this.onPointerMove);
    this.refresh();
    this.timer = setInterval(() => this.refresh(), REFRESH_MS);
  }

  private hide(): void {
    clearInterval(this.timer);
    this.timer = undefined;
    window.removeEventListener('pointermove', this.onPointerMove);
    if (this.legend) m.render(this.legend, null);
    if (this.cursor) m.render(this.cursor, null);
    this.root?.remove();
    this.root = undefined;
    this.canvas = undefined;
    this.legend = undefined;
    this.cursor = undefined;
    this.snapshot = undefined;
    this.comparison = undefined;
    this.comparedAt = 0;
  }

  private readonly onPointerMove = (e: PointerEvent) => {
    this.fadeLegendUnder(e.clientX, e.clientY);
    this.updateCursor(e.clientX, e.clientY);
  };

  // Fades the legend while the pointer is over it, so the UI below can be
  // seen. The legend can't use :hover, because it ignores the pointer.
  private fadeLegendUnder(x: number, y: number): void {
    const legend = this.legend;
    if (!legend) return;
    const r = legend.getBoundingClientRect();
    const over = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    legend.classList.toggle('pf-ownership-overlay__legend--faded', over);
  }

  // Shows the owner under the pointer. When the boxes and the point lookup
  // disagree, shows both, in red.
  private updateCursor(x: number, y: number): void {
    const cursor = this.cursor;
    if (!cursor || !this.snapshot) return;
    const snapshotOwner = this.snapshot.findOwnerAt(x, y);
    const pageOwner = findAttributionAt(x, y).owner;
    m.render(
      cursor,
      snapshotOwner === pageOwner
        ? m('div', snapshotOwner)
        : [
            m(
              '.pf-ownership-overlay__cursor-diff',
              `snapshot: ${snapshotOwner}`,
            ),
            m('.pf-ownership-overlay__cursor-diff', `page: ${pageOwner}`),
          ],
    );
    // Keep the readout inside the window.
    const w = cursor.offsetWidth;
    const h = cursor.offsetHeight;
    const left = x + 14 + w > window.innerWidth ? x - 10 - w : x + 14;
    const top = y + 14 + h > window.innerHeight ? y - 10 - h : y + 14;
    cursor.style.transform = `translate(${left}px, ${top}px)`;
  }

  private refresh(): void {
    const canvas = this.canvas;
    const legend = this.legend;
    if (!canvas || !legend) return;

    const width = window.innerWidth;
    const height = window.innerHeight;
    const snapshot = ScreenSnapshot.take();
    this.snapshot = snapshot;
    const now = performance.now();
    if (!this.comparison || now - this.comparedAt > COMPARE_REFRESH_MS) {
      this.comparison = snapshot.compareWithPage(
        COMPARE_STEP,
        COMPARE_EDGE_BAND,
      );
      this.comparedAt = now;
    }
    const comparison: PointComparison | undefined = this.comparison;

    m.render(legend, renderLegend(snapshot, comparison));

    const dpr = window.devicePixelRatio;
    canvas.width = Math.round(width * dpr);
    canvas.height = Math.round(height * dpr);
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.scale(dpr, dpr);
    drawSnapshot(ctx, snapshot);
    ctx.fillStyle = DISAGREEMENT_COLOR;
    for (const d of comparison.disagreements) {
      ctx.beginPath();
      ctx.arc(d.x, d.y, 2.5, 0, 2 * Math.PI);
      ctx.fill();
    }
  }
}

// Draws each owner's area, an outline around each labeled box, and the owner's
// name in the corner of each box large enough for it, where the box is on top.
function drawSnapshot(
  ctx: CanvasRenderingContext2D,
  snapshot: ScreenSnapshot,
): void {
  for (const {bounds: r, owner} of snapshot.listRegions()) {
    ctx.fillStyle = fillFor(owner);
    ctx.fillRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
  }
  ctx.lineWidth = 1;
  for (const {bounds: b, owner} of snapshot.boxes) {
    ctx.strokeStyle = outlineFor(owner);
    ctx.strokeRect(
      b.left + 0.5,
      b.top + 0.5,
      b.right - b.left - 1,
      b.bottom - b.top - 1,
    );
  }
  ctx.font = '10px monospace';
  ctx.textBaseline = 'top';
  for (const {bounds: b, owner} of snapshot.boxes) {
    if (b.right - b.left < MIN_LABELED_WIDTH) continue;
    if (b.bottom - b.top < MIN_LABELED_HEIGHT) continue;
    const x = b.left + 3;
    const y = b.top + 3;
    if (snapshot.findOwnerAt(x, y) !== owner) continue;
    const w = ctx.measureText(owner).width;
    ctx.fillStyle = 'rgba(255, 255, 255, 0.3)';
    ctx.fillRect(x - 1, y - 1, w + 2, 12);
    ctx.fillStyle = 'rgba(0, 0, 0, 0.45)';
    ctx.fillText(owner, x, y);
  }
}

// Each owner's share of the screen, then the disagreements grouped by element
// and owners.
function renderLegend(
  snapshot: ScreenSnapshot,
  comparison: PointComparison,
): m.Children {
  const shares = Array.from(snapshot.computeShares().entries()).sort(
    (a, b) => b[1] - a[1],
  );
  const groups = new Map<string, number>();
  for (const d of comparison.disagreements) {
    const key = `${d.element}: page ${d.pageOwner}, snapshot ${d.snapshotOwner}`;
    groups.set(key, (groups.get(key) ?? 0) + 1);
  }
  const topGroups = Array.from(groups.entries())
    .sort((a, b) => b[1] - a[1])
    .slice(0, 6);
  return [
    m('.pf-ownership-overlay__title', 'Share of the screen (thousandths)'),
    shares.map(([owner, share]) => legendRow(fillFor(owner), owner, share)),
    m(
      '.pf-ownership-overlay__title.pf-ownership-overlay__title--diff',
      `Disagreements: ${comparison.disagreements.length} of ` +
        `${comparison.compared} points`,
    ),
    topGroups.map(([key, count]) => legendRow(DISAGREEMENT_COLOR, key, count)),
  ];
}

function legendRow(color: string, label: string, value: number): m.Children {
  return m(
    '.pf-ownership-overlay__row',
    m('span.pf-ownership-overlay__swatch', {style: {background: color}}),
    m('span.pf-ownership-overlay__label', label),
    m('span.pf-ownership-overlay__value', value),
  );
}

export const ownershipOverlay = new OwnershipOverlay();
