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
import {assertUnreachable} from '../../base/assert';
import {type time, Time, Timecode} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {
  type AxisRange,
  logRange,
} from '../../components/widgets/charts_svg/common';
import {CursorTooltip} from '../../widgets/cursor_tooltip';
import {PopupPosition} from '../../widgets/popup';
// A local copy of the timeline's tick generator, so our sync-mode gridlines
// land on the exact same times/pixels as the timeline's own axis.
import {generateTicks, getMaxMajorTicks, TickType} from './gridlines';
import {
  type CounterSeries,
  formatMetric,
  type LineStyle,
  type SeriesWindow,
  swatchClassFor,
} from './counter_index';
import type {ViewerModel} from './model';
import {timeTickLabel} from './time_axis';
import {WasdNavigation} from './wasd';

// Width below which the left gutter is too small to also show a counter name
// (page-style narrow y-axis); the drawer's track-shell-width gutter is wider.
const PAD_LEFT_DEFAULT = 72;
const TOP_AXIS = 26; // time axis + labels
const BOTTOM = 6;
const LANE_GAP = 10;
const AXIS_FONT = '10px Roboto, Arial, sans-serif';
const BUCKETS_PER_PX = 2;
// Greyed (non-focused) line opacity, and the per-frame easing fraction toward
// the target opacity so hover greying/un-greying fades instead of snapping
// (~0.2 settles in ~12 frames / 200ms — the same order as the UI's own hovers).
const DIM_ALPHA = 0.22;
const DIM_EASE = 0.2;
// Target total drawn points across all visible lines. Above this, per-line
// resolution is coarsened so hundreds/thousands of overlaid lines stay
// interactive (the mipmap makes coarser cheaper, and fewer segments render
// faster).
const TARGET_TOTAL_POINTS = 320_000;
const FETCH_DEBOUNCE_MS = 40;
// Re-resolve theme colours at most once every this many draws.
const REPALETTE_FRAMES = 30;
// Breathing room inside each lane so the top value/line and the min/max tick
// labels are never flush against (or clipped by) the lane edges.
const LANE_PAD_TOP = 10;
const LANE_PAD_BOTTOM = 6;
// Roughly how many vertical pixels each y-axis tick wants; used to pick the
// tick count from the lane height (fewer ticks on short lanes, never cramped).
const TICK_SPACING_PX = 34;
// Max lines drawn/fetched at once in overlay mode. Overlaying more than this is
// not legible, so we cap it (with a note) to stay fast even if the user selects
// every counter. Stacked mode has no such cap — it only ever touches the lanes
// currently on screen.
const MAX_OVERLAY_SERIES = 256;

// A value axis with an adjustable target tick count (charts_svg's niceRange is
// fixed at ~5, too many for short lanes). `min`/`max` are the *raw* bounds so
// mapY scales the curve continuously while zooming; only `ticks` are rounded to
// nice numbers (renderers cull ticks outside min..max), so labels stay legible
// while the plot never snaps.
function niceValueTicks(
  rawMin: number,
  rawMax: number,
  approx: number,
): AxisRange {
  if (!isFinite(rawMin) || !isFinite(rawMax) || rawMin === rawMax) {
    const v = isFinite(rawMin) ? rawMin : 0;
    return {min: v - 1, max: v + 1, ticks: [v - 1, v, v + 1]};
  }
  const rough = (rawMax - rawMin) / Math.max(1, approx);
  const mag = Math.pow(10, Math.floor(Math.log10(rough)));
  const norm = rough / mag;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
  const niceMin = Math.floor(rawMin / step) * step;
  const niceMax = Math.ceil(rawMax / step) * step;
  const decimals = Math.max(0, -Math.floor(Math.log10(step))) + 2;
  const ticks: number[] = [];
  for (let t = niceMin; t <= niceMax + step / 2; t += step) {
    ticks.push(Number(t.toFixed(decimals)));
  }
  return {min: rawMin, max: rawMax, ticks};
}

// Log-scale counterpart: raw bounds for continuous drawing, power-of-10 tick
// labels within them (reuses logRange's decade ticks, but keeps the raw bounds
// so a log zoom doesn't jump at decade boundaries either).
function logDrawRange(rawMin: number, rawMax: number): AxisRange {
  return {min: rawMin, max: rawMax, ticks: logRange(rawMin, rawMax).ticks};
}

// Subsamples an axis' ticks (keeping the endpoints) so at most `maxTicks`
// remain — used to thin log-scale power-of-10 ticks on short lanes.
function thinTicks(r: AxisRange, maxTicks: number): AxisRange {
  if (r.ticks.length <= maxTicks || maxTicks < 2) return r;
  const stride = Math.ceil((r.ticks.length - 1) / (maxTicks - 1));
  const ticks = r.ticks.filter((_, i) => i % stride === 0);
  const last = r.ticks[r.ticks.length - 1];
  if (ticks[ticks.length - 1] !== last) ticks.push(last);
  return {min: r.min, max: r.max, ticks};
}

// First index i in a[0..n) with a[i] >= target, assuming a is ascending; n if
// none. Used to clip a series' samples to the viewport in O(log n).
function lowerBound(a: Float64Array, target: number, n: number): number {
  let lo = 0;
  let hi = n;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (a[mid] < target) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

interface Palette {
  readonly text: string;
  readonly muted: string;
  readonly border: string;
  readonly grid: string;
  readonly bg: string;
  readonly laneBg: string;
  readonly accent: string;
  readonly series: ReadonlyArray<string>;
}

interface LaneLayout {
  readonly top: number;
  readonly bottom: number;
  readonly height: number;
}

// One row of the hover tooltip: a counter (with its swatch), or the summary
// "Total" row (no swatch).
interface TooltipRow {
  readonly name: string;
  readonly value: string;
  readonly swatch: string;
  readonly dim: boolean;
  readonly isTotal?: boolean;
}

// Adds an alpha channel to a hex colour (palette and override colours are both
// hex). Used to build the translucent area-fill gradient.
function toRgba(color: string, alpha: number): string {
  let h = color.trim();
  if (!h.startsWith('#')) return color;
  h = h.slice(1);
  if (h.length === 3) {
    h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
  }
  if (h.length !== 6) return color;
  const r = parseInt(h.slice(0, 2), 16);
  const g = parseInt(h.slice(2, 4), 16);
  const b = parseInt(h.slice(4, 6), 16);
  if (Number.isNaN(r) || Number.isNaN(g) || Number.isNaN(b)) return color;
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

export interface TimeseriesChartAttrs {
  readonly model: ViewerModel;
}

// A GPU-friendly Canvas2D viewer: one shared time axis over a vertical
// stack of per-counter lanes, each auto-scaled to the visible window so the
// magnitude of every metric is legible at once (Battery-Historian style).
export class TimeseriesChart implements m.ClassComponent<TimeseriesChartAttrs> {
  private model!: ViewerModel;
  private container!: HTMLElement;
  private canvas!: HTMLCanvasElement;
  private ctx!: CanvasRenderingContext2D;
  private ro?: ResizeObserver;
  // Registration with the shared canvas-redraw scheduler; disposed on unmount.
  private rafDisposable?: Disposable;
  private fetchInFlight = 0;
  private disposed = false;
  // Cached theme palette: getComputedStyle is expensive, so we resolve it once
  // and refresh only every REPALETTE_FRAMES draws (picks up theme toggles).
  private palCache?: Palette;
  private palAge = 0;

  private cssW = 0;
  private cssH = 0;
  private dpr = 1;
  // Plot insets for the current frame: the y-axis gutter (track-shell width) on
  // the left, none on the right, so the plot lines up with the tracks above.
  private padLeft = PAD_LEFT_DEFAULT;
  private padRight = 0;
  // Time<->px mapping for the current frame (the timeline's own scale), rebuilt
  // each draw so data, gridlines and crosshair align exactly with the tracks.
  private ts?: TimeScale;
  // Per-frame linear map from ns-since-traceStart to px (the TimeScale is
  // linear), so xOfRel() needs no per-point BigInt/Time allocation.
  private relToPxBase = 0;
  private relToPxSlope = 0;
  // A pinned crosshair time (sync mode): click the chart to park a reference
  // line that persists after the pointer leaves; click it again to clear.
  private pinnedTime?: time;
  // The counter under the cursor (its info.id): its line is emphasised and the
  // others greyed while hovering.
  private focusId?: number;
  // Per-line current opacity (info.id -> alpha), eased toward its target each
  // frame so the greying/un-greying fades in smoothly instead of snapping.
  private readonly dimAlpha = new Map<number, number>();
  // True while any line's alpha is still easing toward its target, so the
  // per-frame early-out keeps redrawing until the fade settles.
  private dimAnimating = false;
  // The one combined range used by every lane/line in 'shared' y-range mode,
  // recomputed at the start of each draw (undefined in other modes).
  private sharedRange?: {min: number; max: number};
  // Whether any lane is emphasised (multi-select), cached per draw so the
  // per-line dimmed() check stays O(1).
  private emphasisActive = false;
  // Last value this chart wrote to the timeline's shared hover cursor, so we
  // dedupe writes and only clear it on unmount if we still own it (another
  // chart sharing the model may have taken it over).
  private publishedHover?: time;
  // Distinguishes a single click (isolate / pin) from a double click (hide /
  // bookmark): the single action fires only if no double-click follows.
  private clickTimer?: ReturnType<typeof setTimeout>;
  // Vertical scroll (stacked mode, when the lane stack exceeds the viewport).
  private scrollInner?: HTMLElement;
  private scrollTop = 0;
  // On-screen lane rects for this draw, with the counter's index in
  // model.selected (`i`) and its position among the visible lanes (`k`), so
  // hit-testing (crosshair, reorder, resize) maps back exactly. Lane heights can
  // differ per lane, so positions come from here, not a uniform step.
  private laneRects: Array<LaneLayout & {i: number; k: number}> = [];

  // Interaction state.
  private mouseX = -1;
  private mouseY = -1;
  private mouseInside = false;
  private drag?: {
    mode: 'pan' | 'zoombox';
    startX: number;
    curX: number;
    startView: {s: time; e: time};
  };
  private readonly pointers = new Map<number, number>(); // id -> clientX
  private pinch?: {
    startDist: number;
    startView: {s: time; e: time};
    centerX: number;
  };
  // Active height-resize drag (overlay plot height, or stacked lane height).
  private resize?:
    | {startY: number; startH: number; kind: 'overlay'}
    | {startY: number; startH: number; kind: 'lane'; series: CounterSeries};
  // Active stacked-lane reorder drag (grab a lane by its shell to move it).
  private laneReorder?: {series: CounterSeries; targetIdx: number};
  // Bottom edge (px) of the overlay plot, for the resize hit-test.
  private overlayBottom = 0;
  // Max stacked total from the last stacked-area draw, so hover hit-testing maps
  // to the same value axis.
  private overlayStackMax = 1;

  private fetchTimer?: ReturnType<typeof setTimeout>;
  private lastFetchSig = '';
  private lastTipSig = '';
  private wasd?: WasdNavigation;
  private removeInvalidateListener?: () => void;

  oncreate(vnode: m.CVnodeDOM<TimeseriesChartAttrs>) {
    this.model = vnode.attrs.model;
    this.container = vnode.dom as HTMLElement;
    this.canvas = this.container.querySelector('canvas') as HTMLCanvasElement;
    this.scrollInner = this.container.querySelector(
      '.pf-tsv__scroll-inner',
    ) as HTMLElement;
    const ctx = this.canvas.getContext('2d');
    if (ctx === null) throw new Error('2d context unavailable');
    this.ctx = ctx;

    this.ro = new ResizeObserver(() => this.wake());
    this.ro.observe(this.container);
    this.container.addEventListener('scroll', () => {
      this.scrollTop = this.container.scrollTop;
      this.wake();
    });

    // External (toolbar/legend) mutations wake the render loop.
    this.removeInvalidateListener = this.model.addInvalidateListener(() =>
      this.wake(),
    );

    // WASD navigation, matching the main timeline: A/D pan, W/S zoom at cursor.
    this.wasd = new WasdNavigation({
      element: this.canvas,
      onPanned: (px) => {
        this.model.panBy(
          (px / this.plotWidth()) * Number(this.model.viewDuration),
        );
        this.wake();
      },
      onZoomed: (posPx, ratio) => {
        if (posPx < this.padLeft) return;
        this.model.zoomAt(this.xToTime(posPx), 1 - ratio);
        this.wake();
      },
    });

    // Redraw whenever the shared scheduler paints (covers the timeline's own
    // zoom/pan/hover); our own changes call wake() to request a paint.
    this.rafDisposable = this.model.trace.raf.addCanvasRedrawCallback(() =>
      this.render(),
    );

    this.attachListeners();
    this.wake();
  }

  onremove() {
    this.disposed = true;
    this.rafDisposable?.[Symbol.dispose]();
    this.ro?.disconnect();
    this.wasd?.[Symbol.dispose]();
    this.removeInvalidateListener?.();
    if (this.fetchTimer !== undefined) clearTimeout(this.fetchTimer);
    if (this.clickTimer !== undefined) clearTimeout(this.clickTimer);
    // Don't leave a stale hover pointer behind — but only clear it if we still
    // own it (another chart sharing this model may have set it since).
    const timeline = this.model.trace.timeline;
    if (timeline.hoverCursorTimestamp === this.publishedHover) {
      timeline.hoverCursorTimestamp = undefined;
    }
  }

  view() {
    return m(
      '.pf-tsv__chart',
      m('.pf-tsv__scroll-inner', m('canvas.pf-tsv__canvas')),
      this.renderTooltip(),
    );
  }

  // A Perfetto cursor-tooltip listing the counter name(s) and value(s) at the
  // hovered time. Replaces on-chart labels (the legend chips are the legend).
  private renderTooltip(): m.Children {
    if (this.disposed || !this.mouseInside) return undefined;
    if (this.drag !== undefined || this.resize !== undefined) return undefined;
    if (this.mouseX < this.padLeft || this.mouseX > this.cssW - this.padRight) {
      return undefined;
    }
    const rows = this.tooltipRows();
    if (rows.length === 0) return undefined;
    const t = this.xToTime(this.mouseX);
    return m(
      CursorTooltip,
      {position: PopupPosition.Right},
      m(
        '.pf-tsv__tip',
        m('.pf-tsv__tip-time', this.timeLabel(t)),
        rows.map((r) =>
          m(
            '.pf-tsv__tip-row',
            {
              className: [
                r.dim && 'pf-tsv__tip-row--dim',
                r.isTotal && 'pf-tsv__tip-row--total',
              ]
                .filter(Boolean)
                .join(' '),
            },
            r.isTotal
              ? m('span.pf-tsv__tip-dot.pf-tsv__tip-dot--none')
              : m(`span.pf-tsv__tip-dot.${r.swatch}`),
            m('span.pf-tsv__tip-name', r.name),
            m('span.pf-tsv__tip-val', r.value),
          ),
        ),
      ),
    );
  }

  // A cheap signature of what the tooltip currently shows, so pointermove only
  // triggers a Mithril redraw when the content would actually change (the
  // CursorTooltip repositions itself independently of Mithril).
  private tooltipSig(): string {
    if (this.disposed || !this.mouseInside) return 'off';
    if (this.drag !== undefined || this.resize !== undefined) return 'off';
    if (this.mouseX < this.padLeft || this.mouseX > this.cssW - this.padRight) {
      return 'off';
    }
    const lane =
      this.model.layout === 'overlay' ? 'ov' : this.stackedLaneAt(this.mouseY);
    return `${this.model.layout}|${lane}|${Math.round(this.mouseX)}|${this.focusId}`;
  }

  private tooltipRows(): ReadonlyArray<TooltipRow> {
    const row = (s: CounterSeries, i: number, v: number): TooltipRow => ({
      name: s.label(this.model.nameBy),
      value: this.fmt(s, v),
      swatch: swatchClassFor(s.colorOverride, i),
      dim: false,
    });
    if (this.model.layout === 'overlay') {
      // Only the hovered line plus the total across visible lines — listing
      // every counter is unusable with hundreds/thousands selected.
      const visible = this.overlaySeries();
      const out: TooltipRow[] = [];
      const focused = visible.find(({s}) => s.info.id === this.focusId);
      if (focused !== undefined) {
        const v = this.valueAtCursor(focused.s);
        if (v !== undefined) out.push(row(focused.s, focused.i, v));
      }
      let total = 0;
      let any = false;
      for (const {s} of visible) {
        const v = this.valueAtCursor(s);
        if (v !== undefined) {
          total += v;
          any = true;
        }
      }
      if (any) {
        out.push({
          name: `Total (${visible.length})`,
          value: formatMetric(total, this.overlayUnit(visible)),
          swatch: '',
          dim: false,
          isTotal: true,
        });
      }
      return out;
    }
    // Stacked: just the lane under the cursor (computed from the current scroll
    // position, so it stays correct even if a draw hasn't happened yet).
    const i = this.stackedLaneAt(this.mouseY);
    if (i < 0) return [];
    const s = this.model.selected[i];
    const v = this.valueAtCursor(s);
    return v === undefined ? [] : [row(s, i, v)];
  }

  // A lane's height: its own override if set, else the global height. Lanes can
  // differ, so all layout math is cumulative (see the stacked draw).
  private laneHeightOf(series: CounterSeries): number {
    return series.laneHeightOverride ?? this.model.laneHeight;
  }

  // Original selection index of the stacked lane at canvas-y `y`, or -1 if over
  // a gap/axis. Uses the rects drawn this frame (which carry per-lane geometry),
  // so it composes with per-lane heights and scroll.
  private stackedLaneAt(y: number): number {
    if (y < TOP_AXIS) return -1;
    for (const r of this.laneRects) {
      if (y >= r.top && y <= r.bottom) return r.i;
    }
    return -1;
  }

  // The position among the visible lanes nearest canvas-y `y`, for
  // drag-to-reorder — uses lane centres so the drop target flips at midpoints.
  // Considers only the on-screen lanes (reorder happens within the view).
  private visibleLaneIndexAt(y: number): number {
    if (this.laneRects.length === 0) return -1;
    let bestK = this.laneRects[0].k;
    let bestDist = Infinity;
    for (const r of this.laneRects) {
      const dist = Math.abs(y - (r.top + r.bottom) / 2);
      if (dist < bestDist) {
        bestDist = dist;
        bestK = r.k;
      }
    }
    return bestK;
  }

  // ---- rendering ----------------------------------------------------------

  // Requests a canvas redraw via the shared scheduler (the conventional path).
  // Called on any local change (pointer, scroll, resize, model mutation, fetch
  // completion). External timeline zoom/pan/hover already schedule redraws,
  // which reach render() through the callback registered in oncreate — so we
  // follow the timeline without polling.
  private wake() {
    if (this.disposed) return;
    this.model.trace.raf.scheduleCanvasRedraw();
  }

  // Paints the chart for one frame: (re)fetch when what we need changed, update
  // hover focus, advance the greying animation, draw. Runs on every shared
  // canvas redraw (so timeline zoom/pan/hover are followed for free); while the
  // greying is still easing it asks for the next frame.
  private render() {
    // No work when unmounted or when the drawer tab is hidden (zero-width).
    if (this.disposed || this.container.clientWidth === 0) return;
    // Read the viewport up front so the fetch signature below reflects the
    // current width (draw() reads it too, but runs after we've keyed the fetch).
    this.cssW = this.container.clientWidth;
    this.cssH = this.container.clientHeight;
    const m = this.model;
    // Refetch when what/where/how-much data we need changes: window, layout,
    // selection, and — because fetching is virtualized to on-screen lanes — the
    // scroll position, lane height and width.
    const fetchSig =
      `${m.layout}|${m.viewStart}|${m.viewEnd}|${m.selectionGeneration}` +
      `|${Math.round(this.scrollTop)}|${m.laneHeight}|${this.cssW}|${m.yRangeMode}`;
    if (fetchSig !== this.lastFetchSig) {
      this.lastFetchSig = fetchSig;
      this.scheduleFetch();
    }
    // Focus (hover): the line under the cursor is emphasised, the rest greyed.
    this.focusId =
      this.mouseInside && this.drag === undefined && this.resize === undefined
        ? this.lineAt(this.mouseX, this.mouseY)?.s.info.id
        : undefined;
    this.advanceDim();
    this.draw();
    // Keep animating the greying fade until it settles.
    if (this.dimAnimating) m.trace.raf.scheduleCanvasRedraw();
  }

  // Eases every line's opacity one step toward its target (1 when focused or
  // nothing is hovered, DIM_ALPHA when greyed) and flags whether any is still in
  // transit, so hover/un-hover cross-fades smoothly rather than snapping.
  private advanceDim() {
    let animating = false;
    for (const s of this.model.selected) {
      const target = this.dimmed(s) ? DIM_ALPHA : 1;
      // Unseen lines start fully visible so the very first greying still fades.
      const cur = this.dimAlpha.get(s.info.id) ?? 1;
      const next = cur + (target - cur) * DIM_EASE;
      if (Math.abs(target - next) < 0.005) {
        this.dimAlpha.set(s.info.id, target);
      } else {
        this.dimAlpha.set(s.info.id, next);
        animating = true;
      }
    }
    this.dimAnimating = animating;
  }

  // The current (eased) opacity for a line — DIM_ALPHA..1.
  private alphaFor(series: CounterSeries): number {
    return (
      this.dimAlpha.get(series.info.id) ?? (this.dimmed(series) ? DIM_ALPHA : 1)
    );
  }

  private plotWidth(): number {
    return Math.max(1, this.cssW - this.padLeft - this.padRight);
  }

  private seriesColor(series: CounterSeries, i: number, pal: Palette): string {
    return series.colorOverride ?? pal.series[i % pal.series.length];
  }

  // Formats a value with the track's unit, adjusted for the line's mode:
  // delta gets a Δ prefix, rate a /s suffix.
  private fmt(series: CounterSeries, v: number): string {
    const u = series.info.unit;
    if (series.mode === 'delta') return `Δ${formatMetric(v, u)}`;
    if (series.mode === 'rate') return `${formatMetric(v, u)}/s`;
    return formatMetric(v, u);
  }

  private localY(clientY: number): number {
    return clientY - this.canvas.getBoundingClientRect().top;
  }

  // If the given y is on a height-resize edge, returns what would be resized.
  // For stacked lanes this targets the specific lane under the edge, so each
  // lane resizes independently.
  private hitResize(
    y: number,
  ):
    | {kind: 'overlay'; startH: number}
    | {kind: 'lane'; startH: number; series: CounterSeries}
    | undefined {
    if (this.model.layout === 'overlay') {
      if (Math.abs(y - this.overlayBottom) <= 5) {
        return {kind: 'overlay', startH: this.overlayBottom - TOP_AXIS};
      }
      return undefined;
    }
    for (const r of this.laneRects) {
      if (Math.abs(y - r.bottom) <= 4 && r.bottom >= TOP_AXIS) {
        const series = this.model.selected[r.i];
        return {kind: 'lane', startH: r.height, series};
      }
    }
    return undefined;
  }

  // A grip drawn on the resize edge (overlay plot bottom).
  private drawResizeHandle(pal: Palette, y: number) {
    const ctx = this.ctx;
    const cx = (this.padLeft + (this.cssW - this.padRight)) / 2;
    ctx.save();
    ctx.fillStyle = pal.laneBg;
    ctx.strokeStyle = pal.border;
    ctx.lineWidth = 1;
    ctx.fillRect(cx - 22, y - 3, 44, 7);
    ctx.strokeRect(cx - 22 + 0.5, y - 3 + 0.5, 43, 6);
    ctx.strokeStyle = pal.muted;
    ctx.beginPath();
    ctx.moveTo(cx - 10, y - 1);
    ctx.lineTo(cx + 10, y - 1);
    ctx.moveTo(cx - 10, y + 1.5);
    ctx.lineTo(cx + 10, y + 1.5);
    ctx.stroke();
    ctx.restore();
  }

  private dashFor(style: LineStyle): number[] {
    switch (style) {
      case 'solid':
        return [];
      case 'dashed':
        return [6, 4];
      case 'dotted':
        return [2, 3];
      default:
        return assertUnreachable(style);
    }
  }

  private scheduleFetch() {
    if (this.fetchTimer !== undefined) clearTimeout(this.fetchTimer);
    this.fetchTimer = setTimeout(() => this.doFetch(), FETCH_DEBOUNCE_MS);
  }

  // Which lanes/lines actually need data right now, and at what bucket
  // resolution. Stacked fetches only the lanes on screen (so any number of
  // selected counters stays fast); overlay fetches the visible lines, capped.
  private fetchList(): {series: CounterSeries[]; bpp: number} {
    const {model} = this;
    if (model.layout === 'overlay') {
      const series = model.selected
        .filter((s) => !s.hidden)
        .slice(0, MAX_OVERLAY_SERIES);
      // Coarsen per-line resolution so the total drawn points stay bounded.
      const bpp = Math.max(
        0.25,
        Math.min(
          BUCKETS_PER_PX,
          TARGET_TOTAL_POINTS / (Math.max(1, series.length) * this.plotWidth()),
        ),
      );
      return {series, bpp};
    }
    // Stacked: each lane is an independent full-width plot, so it gets full
    // resolution; we fetch only the lanes drawn on screen last frame (draw runs
    // synchronously before this debounced fetch, so laneRects is current).
    return {
      series: this.laneRects.map((r) => this.model.selected[r.i]),
      bpp: BUCKETS_PER_PX,
    };
  }

  private async doFetch() {
    const {series, bpp} = this.fetchList();
    const dur = this.model.viewDuration;
    let step = dur / BigInt(Math.max(1, Math.floor(this.plotWidth() * bpp)));
    if (step < 1n) step = 1n;
    const start = this.model.viewStart;
    const end = this.model.viewEnd;
    this.fetchInFlight++;
    const mode = this.model.yRangeMode;
    const global = mode === 'global';
    const traceEnd = this.model.traceEnd;
    const jobs = series.map((s) =>
      s
        .fetch(start, end, step)
        // In 'global' mode also make sure the whole-trace range is available so
        // the axis can lock to it (cheap + cached per mode).
        .then(() => (global ? s.ensureGlobalRange(traceEnd) : undefined))
        .catch(() => {}),
    );
    // 'shared' mode needs the whole-trace range of *every* visible line (not
    // just the on-screen lanes) so the one combined scale is complete.
    if (mode === 'shared') {
      for (const s of this.model.selected) {
        if (s.hidden) continue;
        jobs.push(s.ensureGlobalRange(traceEnd).catch(() => {}));
      }
    }
    try {
      await Promise.all(jobs);
    } finally {
      this.fetchInFlight--;
    }
    this.wake();
  }

  // ---- coordinate mapping -------------------------------------------------

  // The timeline's track-shell width, read from the CSS var the timeline sets
  // (--track-shell-width on <body>); the drawer matches it so its plot lines up
  // with the track content above. Falls back to the timeline's default.
  private trackShellWidth(): number {
    const v = getComputedStyle(document.body).getPropertyValue(
      '--track-shell-width',
    );
    const n = parseInt(v, 10);
    return Number.isFinite(n) && n > 0 ? n : 100;
  }

  // Time<->px scale for the current frame: the timeline's own visibleWindow over
  // [track-shell-width, width], so a time maps to the identical pixel as the
  // tracks above.
  private makeTimeScale(): TimeScale {
    return new TimeScale(this.model.trace.timeline.visibleWindow, {
      left: this.padLeft,
      right: this.cssW - this.padRight,
    });
  }

  // The frame's cached scale (draw() sets this.ts); falls back to a fresh one
  // for pointer maths that run between frames.
  private scale(): TimeScale {
    return this.ts ?? this.makeTimeScale();
  }

  // x for a time expressed as ns relative to traceStart (series samples are
  // stored this way). Uses the per-frame linear map (base + rel*slope) computed
  // in draw(), avoiding a BigInt/Time allocation for every drawn point.
  private xOfRel(rel: number): number {
    return this.relToPxBase + rel * this.relToPxSlope;
  }

  private xToTime(x: number): time {
    return this.makeTimeScale().pxToHpTime(x).toTime('round');
  }

  // ---- drawing ------------------------------------------------------------

  private draw() {
    // cssW/cssH are set by render() before this runs (from the scroller's client
    // box, which excludes any scrollbar); the sticky canvas always covers exactly
    // the viewport, and .pf-tsv__scroll-inner provides the scroll range.
    this.dpr = window.devicePixelRatio || 1;
    // Use the timeline's exact plot bounds (left = track-shell width, right =
    // full width) so the drawer's time scale matches the timeline's — crosshairs
    // and gridlines line up pixel-for-pixel. The left column isn't wasted: it's
    // drawn as a shell (counter name + axis), mirroring the track shells above.
    this.padLeft = this.trackShellWidth();
    this.padRight = 0;
    // In 'shared' mode, resolve the one common range for this frame up front so
    // every lane/line agrees (computed from each series' whole-trace range).
    this.sharedRange =
      this.model.yRangeMode === 'shared'
        ? this.computeSharedRange()
        : undefined;
    this.emphasisActive = this.model.hasEmphasis();
    this.ts = this.makeTimeScale();
    // Precompute the linear rel->px map (base + rel*slope) from two points 1s
    // apart, so per-point drawing does pure float math with no allocation.
    const px0 = this.ts.timeToPx(this.model.traceStart);
    const px1s = this.ts.timeToPx(
      Time.fromRaw(this.model.traceStart + 1_000_000_000n),
    );
    this.relToPxBase = px0;
    this.relToPxSlope = (px1s - px0) / 1e9;
    if (this.canvas.width !== this.cssW * this.dpr) {
      this.canvas.width = Math.max(1, Math.floor(this.cssW * this.dpr));
    }
    if (this.canvas.height !== this.cssH * this.dpr) {
      this.canvas.height = Math.max(1, Math.floor(this.cssH * this.dpr));
    }
    // Canvas display size tracks the viewport (computed per frame from the
    // container + DPR), so it must be set imperatively rather than in CSS.
    this.canvas.style.width = `${this.cssW}px`;
    this.canvas.style.height = `${this.cssH}px`;

    const ctx = this.ctx;
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    const pal = this.getPalette();
    ctx.clearRect(0, 0, this.cssW, this.cssH);
    ctx.fillStyle = pal.bg;
    ctx.fillRect(0, 0, this.cssW, this.cssH);

    const lanesTop = TOP_AXIS;
    const lanesBottom = this.cssH - BOTTOM;

    // The empty state is a DOM EmptyState in the page, so the chart only ever
    // mounts with a selection; guard the transient case anyway.
    if (this.model.selected.length === 0) {
      this.setScrollHeight(0);
      return;
    }

    if (this.model.layout === 'overlay') {
      this.setScrollHeight(0);
      this.scrollTop = 0;
      const full = lanesBottom - lanesTop;
      const oh = Math.max(60, Math.min(this.model.overlayHeight ?? full, full));
      const overlayBottom = lanesTop + oh;
      this.overlayBottom = overlayBottom;
      this.drawShellGutter(pal, lanesTop, overlayBottom);
      this.drawOverlay(pal, lanesTop, overlayBottom);
      this.drawTimeAxis(pal, lanesTop, overlayBottom);
      this.drawOverlayCrosshair(pal, lanesTop, overlayBottom);
      this.drawResizeHandle(pal, overlayBottom);
      this.drawZoomBox(pal, lanesTop, overlayBottom);
      return;
    }

    // Stacked: one lane per *visible* (non-hidden) counter, fixed height;
    // scroll vertically when the stack is taller than the viewport. Colours are
    // keyed to the original selection index so they stay stable as lanes are
    // hidden/shown.
    const lanes = this.visibleLanes();
    const n = lanes.length;
    const viewportH = lanesBottom - lanesTop;
    this.laneRects = [];
    if (n > 0) {
      // Each lane can have its own height (dragged bottom edge); the rest fall
      // back to the global height. Positions are cumulative rather than a
      // uniform step so per-lane resize composes cleanly.
      const tops = new Array<number>(n);
      let acc = 0;
      for (let k = 0; k < n; k++) {
        tops[k] = acc;
        acc += this.laneHeightOf(lanes[k].s) + LANE_GAP;
      }
      const contentH = acc - LANE_GAP;
      this.setScrollHeight(
        contentH > viewportH ? TOP_AXIS + contentH + BOTTOM : 0,
      );
      const maxScroll = Math.max(0, contentH - viewportH);
      if (this.scrollTop > maxScroll) this.scrollTop = maxScroll;

      this.drawShellGutter(pal, lanesTop, lanesBottom);
      ctx.save();
      ctx.beginPath();
      ctx.rect(0, lanesTop, this.cssW, viewportH);
      ctx.clip();
      // Only draw the lanes on screen, so any number of selected counters stays
      // fast (the rest exist as scroll range only).
      for (let k = 0; k < n; k++) {
        const h = this.laneHeightOf(lanes[k].s);
        const top = lanesTop + tops[k] - this.scrollTop;
        if (top + h < lanesTop || top > lanesBottom) continue;
        const rect: LaneLayout = {top, bottom: top + h, height: h};
        this.laneRects.push({i: lanes[k].i, k, ...rect});
        this.drawLane(pal, lanes[k].s, lanes[k].i, rect);
      }
      ctx.restore();
    } else {
      this.setScrollHeight(0);
    }

    // Draw the time axis last so it stays crisp over any scrolled lane.
    this.drawTimeAxis(pal, lanesTop, lanesBottom);
    this.drawCrosshair(pal, lanesTop, lanesBottom);
    this.drawZoomBox(pal, lanesTop, lanesBottom);
    this.drawLaneReorder(pal, lanesTop);
  }

  // While dragging a lane to reorder: outline the grabbed lane and show a drop
  // indicator at the slot boundary where it will land.
  private drawLaneReorder(pal: Palette, lanesTop: number) {
    const drag = this.laneReorder;
    if (drag === undefined) return;
    const ctx = this.ctx;
    const lanes = this.visibleLanes();
    const from = lanes.findIndex((l) => l.s === drag.series);
    if (from < 0) return;
    const to = drag.targetIdx;
    const step = this.model.laneHeight + LANE_GAP;
    ctx.save();
    ctx.strokeStyle = pal.accent;
    // Outline the grabbed lane at its current position.
    const dragged = this.laneRects.find((r) => r.i === lanes[from].i);
    if (dragged !== undefined) {
      ctx.lineWidth = 2;
      ctx.strokeRect(0.5, dragged.top + 0.5, this.cssW - 1, dragged.height - 1);
    }
    // Drop indicator at the boundary the lane will land on.
    const boundary = to > from ? to + 1 : to;
    const y = lanesTop + boundary * step - this.scrollTop - LANE_GAP / 2;
    ctx.lineWidth = 3;
    ctx.beginPath();
    ctx.moveTo(0, Math.round(y) + 0.5);
    ctx.lineTo(this.cssW, Math.round(y) + 0.5);
    ctx.stroke();
    ctx.restore();
  }

  // Selected counters that are currently shown (not hidden via the legend),
  // each with its original selection index (used for a stable colour).
  private visibleLanes(): Array<{s: CounterSeries; i: number}> {
    const out: Array<{s: CounterSeries; i: number}> = [];
    const sel = this.model.selected;
    for (let i = 0; i < sel.length; i++) {
      if (!sel[i].hidden) out.push({s: sel[i], i});
    }
    // Emphasised lanes rise to the top, greyed ones sink to the bottom (stable,
    // so order within each group is preserved). `i` stays the selection index,
    // so colours don't shift.
    if (this.model.hasEmphasis()) {
      out.sort((a, b) => Number(b.s.emphasized) - Number(a.s.emphasized));
    }
    return out;
  }

  // Sizes the scroll spacer: 0/undefined => fit the viewport (no scrollbar),
  // otherwise the total content height so the container can scroll.
  private setScrollHeight(totalPx: number): void {
    if (this.scrollInner === undefined) return;
    this.scrollInner.style.height = totalPx > 0 ? `${totalPx}px` : '100%';
  }

  // Normalized y for overlay % mode: maps a value into [top,bottom] using the
  // series' own visible range (log-aware), so mixed magnitudes are shape-
  // comparable on one 0-100% plot. Used for the crosshair dots.
  private overlayY(
    series: CounterSeries,
    v: number,
    top: number,
    height: number,
  ): number {
    const w = series.window;
    const plotTop = top + LANE_PAD_TOP;
    const plotH = Math.max(1, height - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    if (w === undefined) return plotTop + plotH;
    const r = this.visibleRange(w);
    return this.mapY(
      v,
      {min: r.min, max: r.max, ticks: []},
      plotTop,
      plotH,
      this.model.yScale === 'log',
    );
  }

  // Common unit across the visible overlay series, or undefined if they differ
  // (then the shared value axis shows plain numbers).
  private overlayUnit(
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
  ): string | undefined {
    let unit: string | undefined;
    let first = true;
    for (const {s} of visible) {
      if (first) {
        unit = s.info.unit;
        first = false;
      } else if (s.info.unit !== unit) {
        return undefined;
      }
    }
    return unit;
  }

  // Combined on-screen value range across all visible overlay series, for the
  // shared absolute value axis.
  private overlayCombinedRange(
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
  ): {min: number; max: number} {
    let min = Infinity;
    let max = -Infinity;
    for (const {s} of visible) {
      const r = this.valueRange(s);
      if (r === undefined) continue;
      if (r.min < min) min = r.min;
      if (r.max > max) max = r.max;
    }
    if (min === Infinity) return {min: 0, max: 1};
    return {min, max};
  }

  // The shared absolute value axis (a nice-tick range) for overlay Value mode.
  private overlayAbsRange(
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
    heightPx: number,
  ): AxisRange {
    const c = this.overlayCombinedRange(visible);
    const approx = this.tickTarget(heightPx);
    if (this.model.yScale === 'log' && c.max > 0) {
      return thinTicks(
        logDrawRange(c.min > 0 ? c.min : c.max / 1e3, c.max),
        approx + 1,
      );
    }
    return niceValueTicks(Math.min(0, c.min), c.max, approx);
  }

  private drawOverlay(pal: Palette, top: number, bottom: number) {
    const ctx = this.ctx;
    const left = this.padLeft;
    const right = this.cssW - this.padRight;
    const height = bottom - top;

    ctx.fillStyle = pal.laneBg;
    ctx.fillRect(left, top, right - left, height);
    ctx.strokeStyle = pal.border;
    ctx.lineWidth = 1;
    ctx.strokeRect(left + 0.5, top + 0.5, right - left - 1, height - 1);

    const visible = this.overlaySeries();
    if (this.model.overlayStacked) {
      this.drawOverlayStacked(pal, top, bottom, visible);
    } else if (this.model.overlayNormalize) {
      this.drawOverlayNormalized(pal, top, bottom, visible);
    } else {
      this.drawOverlayAbsolute(pal, top, bottom, visible);
    }
  }

  // Horizontal gridlines + value labels for a shared absolute (unit) axis.
  private drawOverlayValueAxis(
    pal: Palette,
    top: number,
    bottom: number,
    range: AxisRange,
    unit: string | undefined,
    log: boolean,
  ) {
    const ctx = this.ctx;
    const left = this.padLeft;
    const right = this.cssW - this.padRight;
    const plotTop = top + LANE_PAD_TOP;
    const plotH = Math.max(1, bottom - top - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const tick of range.ticks) {
      const y = this.mapY(tick, range, plotTop, plotH, log);
      if (y < top - 0.5 || y > bottom + 0.5) continue;
      ctx.strokeStyle = pal.grid;
      ctx.beginPath();
      ctx.moveTo(left, Math.round(y) + 0.5);
      ctx.lineTo(right, Math.round(y) + 0.5);
      ctx.stroke();
      ctx.fillStyle = pal.muted;
      ctx.fillText(formatMetric(tick, unit), left - 6, y);
    }
  }

  private drawOverlayNormalized(
    pal: Palette,
    top: number,
    bottom: number,
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
  ) {
    const ctx = this.ctx;
    const left = this.padLeft;
    const right = this.cssW - this.padRight;
    const height = bottom - top;
    // Normalized (%) gridlines — each series is scaled to its own window.
    const gridTop = top + LANE_PAD_TOP;
    const gridH = Math.max(1, height - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    const fracs =
      height < 150
        ? [0, 0.5, 1]
        : height < 300
          ? [0, 0.25, 0.5, 0.75, 1]
          : [0, 0.2, 0.4, 0.6, 0.8, 1];
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const p of fracs) {
      const y = gridTop + gridH * (1 - p);
      ctx.strokeStyle = pal.grid;
      ctx.beginPath();
      ctx.moveTo(left, Math.round(y) + 0.5);
      ctx.lineTo(right, Math.round(y) + 0.5);
      ctx.stroke();
      ctx.fillStyle = pal.muted;
      ctx.fillText(`${Math.round(p * 100)}%`, left - 6, y);
    }
    const plotTop = top + LANE_PAD_TOP;
    const plotH = gridH;
    const log = this.model.yScale === 'log';
    // Each series normalised to its own on-screen range (computed once).
    this.drawOverlayLines(pal, top, bottom, visible, (s) => {
      const r = this.valueRange(s) ?? {min: 0, max: 1};
      const range = {min: r.min, max: r.max, ticks: []};
      return (v: number) => this.mapY(v, range, plotTop, plotH, log);
    });
  }

  private drawOverlayAbsolute(
    pal: Palette,
    top: number,
    bottom: number,
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
  ) {
    const log = this.model.yScale === 'log';
    const range = this.overlayAbsRange(visible, bottom - top);
    this.drawOverlayValueAxis(
      pal,
      top,
      bottom,
      range,
      this.overlayUnit(visible),
      log,
    );
    const plotTop = top + LANE_PAD_TOP;
    const plotH = Math.max(1, bottom - top - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    const yFn = (v: number) => this.mapY(v, range, plotTop, plotH, log);
    this.drawOverlayLines(pal, top, bottom, visible, () => yFn);
  }

  // Shared stepped-line drawing for overlay Lines modes. `mkY(series)` returns
  // the value->y mapper for that series (computed once per series, not per
  // point).
  private drawOverlayLines(
    pal: Palette,
    top: number,
    bottom: number,
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
    mkY: (s: CounterSeries) => (v: number) => number,
  ) {
    const ctx = this.ctx;
    const left = this.padLeft;
    const right = this.cssW - this.padRight;
    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, right - left, bottom - top);
    ctx.clip();
    for (const {s: series, i} of visible) {
      const w = series.window;
      if (w === undefined || w.count === 0) continue;
      const yOfV = mkY(series);
      const stride = this.drawStride(w.count);
      const last = w.count - 1;
      ctx.beginPath();
      for (let k = 0; k <= last; k += stride) {
        const x = this.xOfRel(w.tsRel[k]);
        const y = yOfV(w.lastV[k]);
        if (k === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
        const xn =
          k + stride <= last ? this.xOfRel(w.tsRel[k + stride]) : right;
        ctx.lineTo(xn, y);
      }
      ctx.strokeStyle = this.seriesColor(series, i, pal);
      ctx.globalAlpha = this.alphaFor(series); // eased grey for non-focused
      ctx.lineWidth = 1.5;
      ctx.lineJoin = 'miter';
      ctx.setLineDash(this.dashFor(series.lineStyle));
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
    ctx.setLineDash([]);
    ctx.restore();
  }

  // Stacked-area overlay: series stacked cumulatively on a shared value axis.
  // Series have independent sample times, so we resample onto a common pixel
  // grid (stepped, last value <= x). Stacking sums values, so it is inherently
  // linear (log scale is ignored here). Negative samples are clamped to 0.
  private drawOverlayStacked(
    pal: Palette,
    top: number,
    bottom: number,
    visible: ReadonlyArray<{s: CounterSeries; i: number}>,
  ) {
    const ctx = this.ctx;
    const left = this.padLeft;
    const right = this.cssW - this.padRight;
    const plotTop = top + LANE_PAD_TOP;
    const plotH = Math.max(1, bottom - top - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    const COL = 3;
    const xs: number[] = [];
    for (let x = left; x <= right; x += COL) xs.push(x);
    if (xs.length === 0 || xs[xs.length - 1] !== right) xs.push(right);
    const vals = visible.map(({s}) =>
      xs.map((x) => Math.max(0, this.valueAtX(s, x) ?? 0)),
    );
    let maxTotal = 0;
    if (this.model.yRangeMode !== 'fit') {
      // Lock the axis to the largest the stack can ever reach (sum of each
      // series' whole-trace max), so it doesn't rescale while zooming. (A single
      // shared axis is what a cumulative stacked-area already is, so 'global'
      // and 'shared' behave the same here.)
      for (const {s} of visible) {
        maxTotal += Math.max(0, this.ownRange(s)?.max ?? 0);
      }
    } else {
      for (let c = 0; c < xs.length; c++) {
        let sum = 0;
        for (let j = 0; j < visible.length; j++) sum += vals[j][c];
        if (sum > maxTotal) maxTotal = sum;
      }
    }
    this.overlayStackMax = maxTotal || 1;
    const range = niceValueTicks(
      0,
      maxTotal || 1,
      this.tickTarget(bottom - top),
    );
    this.drawOverlayValueAxis(
      pal,
      top,
      bottom,
      range,
      this.overlayUnit(visible),
      false,
    );

    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, right - left, bottom - top);
    ctx.clip();
    const base = new Float64Array(xs.length);
    for (let j = 0; j < visible.length; j++) {
      const {s, i} = visible[j];
      const color = this.seriesColor(s, i, pal);
      const a = this.alphaFor(s); // eased grey for non-focused bands
      // Filled band between the running base and base+value.
      ctx.beginPath();
      for (let c = 0; c < xs.length; c++) {
        const y = this.mapY(base[c] + vals[j][c], range, plotTop, plotH, false);
        if (c === 0) ctx.moveTo(xs[c], y);
        else ctx.lineTo(xs[c], y);
      }
      for (let c = xs.length - 1; c >= 0; c--) {
        ctx.lineTo(xs[c], this.mapY(base[c], range, plotTop, plotH, false));
      }
      ctx.closePath();
      ctx.globalAlpha = a;
      ctx.fillStyle = toRgba(color, 0.55);
      ctx.fill();
      // Top edge stroke.
      ctx.beginPath();
      for (let c = 0; c < xs.length; c++) {
        const y = this.mapY(base[c] + vals[j][c], range, plotTop, plotH, false);
        if (c === 0) ctx.moveTo(xs[c], y);
        else ctx.lineTo(xs[c], y);
      }
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.2;
      ctx.lineJoin = 'miter';
      ctx.stroke();
      for (let c = 0; c < xs.length; c++) base[c] += vals[j][c];
    }
    ctx.globalAlpha = 1;
    ctx.restore();
  }

  private drawOverlayCrosshair(pal: Palette, top: number, bottom: number) {
    const cross = this.crosshair();
    if (cross === undefined) return;
    const ctx = this.ctx;
    const height = bottom - top;
    const x = Math.round(cross.x) + 0.5;
    ctx.save();
    ctx.setLineDash([4, 3]);
    ctx.strokeStyle = pal.accent;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, bottom);
    ctx.stroke();
    ctx.setLineDash([]);

    const label = this.timeLabel(cross.t);
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'center';
    const tw = ctx.measureText(label).width + 8;
    ctx.fillStyle = pal.accent;
    ctx.fillRect(cross.x - tw / 2, 1, tw, 13);
    ctx.fillStyle = pal.bg;
    ctx.fillText(label, cross.x, 11);

    // A single dot at the hovered (focused) line's sample — dotting every line
    // is noise (and slow) with a big selection; the tooltip has the details.
    // Skipped in stacked-area mode (the bands already show the split).
    const focused =
      this.focusId === undefined || this.model.overlayStacked
        ? undefined
        : this.overlaySeries().find(({s}) => s.info.id === this.focusId);
    if (focused !== undefined) {
      const v = this.valueAtX(focused.s, cross.x);
      if (v !== undefined) {
        const log = this.model.yScale === 'log';
        const plotTop = top + LANE_PAD_TOP;
        const plotH = Math.max(1, height - LANE_PAD_TOP - LANE_PAD_BOTTOM);
        const y = this.model.overlayNormalize
          ? this.overlayY(focused.s, v, top, height)
          : this.mapY(
              v,
              this.overlayAbsRange(this.overlaySeries(), height),
              plotTop,
              plotH,
              log,
            );
        ctx.beginPath();
        ctx.fillStyle = this.seriesColor(focused.s, focused.i, pal);
        ctx.arc(cross.x, y, 3, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = pal.bg;
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    }
    ctx.restore();
  }

  // Visible (non-hidden) overlay lines with their selection index, capped so
  // overlaying a huge selection stays fast and legible.
  private overlaySeries(): ReadonlyArray<{s: CounterSeries; i: number}> {
    // In value/stacked overlay a unit filter (when the selection mixes units)
    // scopes the shared axis to one unit; % mode normalises each line so it
    // ignores the filter.
    const unit = !this.model.overlayNormalize
      ? this.model.overlayUnitFilter
      : undefined;
    const out: Array<{s: CounterSeries; i: number}> = [];
    const sel = this.model.selected;
    for (let i = 0; i < sel.length && out.length < MAX_OVERLAY_SERIES; i++) {
      if (sel[i].hidden) continue;
      if (unit !== undefined && sel[i].info.unit !== unit) continue;
      out.push({s: sel[i], i});
    }
    return out;
  }

  // The series under the cursor: the lane in stacked mode, the band in
  // stacked-area, or the nearest line in overlay lines modes. `maxDist` caps how
  // far (px) a line may be for overlay lines — Infinity (default, for hover
  // focus) always picks the nearest; a small value (for clicks) leaves empty
  // space unmatched so a click there pins/bookmarks instead of isolating.
  private lineAt(
    x: number,
    y: number,
    maxDist = Infinity,
  ): {s: CounterSeries; i: number} | undefined {
    if (x < this.padLeft || x > this.cssW - this.padRight) return undefined;
    if (this.model.layout === 'stacked') {
      const i = this.stackedLaneAt(y);
      return i < 0 ? undefined : {s: this.model.selected[i], i};
    }
    const top = TOP_AXIS;
    const bottom = this.overlayBottom;
    if (y < top || y > bottom) return undefined;
    const height = bottom - top;
    const plotTop = top + LANE_PAD_TOP;
    const plotH = Math.max(1, height - LANE_PAD_TOP - LANE_PAD_BOTTOM);
    const log = this.model.yScale === 'log';
    const visible = this.overlaySeries();
    if (this.model.overlayStacked) {
      // The cumulative band that contains y (uses the same range as the draw).
      const range = niceValueTicks(
        0,
        this.overlayStackMax || 1,
        this.tickTarget(height),
      );
      let base = 0;
      for (const {s, i} of visible) {
        const v = Math.max(0, this.valueAtX(s, x) ?? 0);
        const yTop = this.mapY(base + v, range, plotTop, plotH, false);
        const yBot = this.mapY(base, range, plotTop, plotH, false);
        if (y >= yTop && y <= yBot) return {s, i};
        base += v;
      }
      return undefined;
    }
    const absRange = this.model.overlayNormalize
      ? undefined
      : this.overlayAbsRange(visible, height);
    let best: {s: CounterSeries; i: number} | undefined;
    let bestDist = maxDist;
    for (const {s, i} of visible) {
      const v = this.valueAtX(s, x);
      if (v === undefined) continue;
      const yv = absRange
        ? this.mapY(v, absRange, plotTop, plotH, log)
        : this.overlayY(s, v, top, height);
      const d = Math.abs(y - yv);
      if (d < bestDist) {
        bestDist = d;
        best = {s, i};
      }
    }
    return best;
  }

  // True if `series` should be greyed because another line is focused (hovered).
  private dimmed(series: CounterSeries): boolean {
    // A persistent multi-select (emphasis) greys everything not selected; absent
    // that, hovering greys everything but the line under the cursor.
    if (this.emphasisActive) return !series.emphasized;
    return this.focusId !== undefined && series.info.id !== this.focusId;
  }

  private drawTimeAxis(pal: Palette, top: number, bottom: number) {
    const ctx = this.ctx;
    ctx.font = AXIS_FONT;
    ctx.textBaseline = 'alphabetic';
    this.drawSyncedGridlines(pal, top, bottom);
    // Axis rule.
    ctx.strokeStyle = pal.border;
    ctx.beginPath();
    ctx.moveTo(this.padLeft, top + 0.5);
    ctx.lineTo(this.cssW - this.padRight, top + 0.5);
    ctx.stroke();
  }

  // Shades the left shell column (synced drawer) and draws its right divider, so
  // the (aligned) plot's left offset reads as an intentional shell — like the
  // track shells above — rather than an empty gap.
  private drawShellGutter(pal: Palette, top: number, bottom: number) {
    const ctx = this.ctx;
    ctx.fillStyle = pal.laneBg;
    ctx.fillRect(0, top, this.padLeft, bottom - top);
    ctx.strokeStyle = pal.border;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(this.padLeft + 0.5, top);
    ctx.lineTo(this.padLeft + 0.5, bottom);
    ctx.stroke();
  }

  // Time label for the crosshair / gridlines: mirrors the timeline's own
  // timecode (domain time, [d]HH:MM:SS.mmm) so the markings match exactly.
  private timeLabel(t: time): string {
    const tc = new Timecode(this.model.trace.timeline.toDomainTime(t));
    return `${tc.dhhmmss}.${tc.millis}`;
  }

  // Vertical gridlines that coincide pixel-for-pixel with the timeline's own,
  // by reusing its tick generator over the identical time scale, origin and
  // major-tick spacing. Keeps the drawer visually locked to the tracks above.
  private drawSyncedGridlines(pal: Palette, top: number, bottom: number) {
    const ctx = this.ctx;
    const scale = this.scale();
    const span = scale.timeSpan.toTimeSpan();
    if (span.duration <= 0n) return;
    const offset = this.model.trace.timeline.getTimeAxisOrigin();
    const maxMajorTicks = getMaxMajorTicks(this.plotWidth());
    for (const {type, time: t} of generateTicks(span, maxMajorTicks, offset)) {
      if (type !== TickType.MAJOR) continue;
      const x = Math.floor(scale.timeToPx(t));
      ctx.strokeStyle = pal.grid;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, top);
      ctx.lineTo(x + 0.5, bottom);
      ctx.stroke();
      ctx.fillStyle = pal.muted;
      ctx.textAlign = 'center';
      ctx.fillText(this.timeLabel(t), x, TOP_AXIS - 9);
    }
  }

  // Target number of y-axis ticks for a lane of the given height. Worst case is
  // ~2 (min + max); more are added as vertical space permits.
  private tickTarget(heightPx: number): number {
    const avail = heightPx - LANE_PAD_TOP - LANE_PAD_BOTTOM;
    return Math.max(1, Math.min(6, Math.round(avail / TICK_SPACING_PX)));
  }

  // Value range over the samples currently within the viewport, so the y-axis
  // fits what's on screen and rescales *continuously* as you zoom (no snap) —
  // and stays correct when the fetched window is the whole series (small-counter
  // full cache) rather than exactly the viewport.
  private visibleRange(w: SeriesWindow): {min: number; max: number} {
    const n = w.count;
    if (n === 0) return {min: 0, max: 1};
    const sRel = Number(this.model.viewStart - this.model.traceStart);
    const eRel = Number(this.model.viewEnd - this.model.traceStart);
    let i = lowerBound(w.tsRel, sRel, n);
    if (i > 0) i--; // include the sample carrying the value at the left edge
    let min = Infinity;
    let max = -Infinity;
    for (; i < n && w.tsRel[i] <= eRel; i++) {
      if (w.minV[i] < min) min = w.minV[i];
      if (w.maxV[i] > max) max = w.maxV[i];
    }
    if (min === Infinity) {
      // Viewport is entirely before the first sample; use it as the flat carry.
      min = w.minV[0];
      max = w.maxV[0];
    }
    return {min, max};
  }

  // One series' own value range: its whole-trace range in 'global'/'shared'
  // modes (when computed for the current y-mode), else the viewport range.
  // Undefined when no data is available yet (caller falls back).
  private ownRange(
    series: CounterSeries,
  ): {min: number; max: number} | undefined {
    if (
      this.model.yRangeMode !== 'fit' &&
      series.globalMode === series.mode &&
      series.globalMin !== undefined &&
      series.globalMax !== undefined
    ) {
      return {min: series.globalMin, max: series.globalMax};
    }
    const w = series.window;
    return w === undefined || w.count === 0 ? undefined : this.visibleRange(w);
  }

  // The value range a series' *axis* should use. In 'shared' mode every visible
  // lane/line uses the one combined range (so heights are comparable); otherwise
  // it's the series' own range (viewport for 'fit', whole-trace for 'global').
  private valueRange(
    series: CounterSeries,
  ): {min: number; max: number} | undefined {
    if (this.model.yRangeMode === 'shared') {
      return this.sharedRange ?? this.ownRange(series);
    }
    return this.ownRange(series);
  }

  // The combined range across all visible lanes/lines, for 'shared' mode.
  // Recomputed once per draw (see draw()) from each series' own whole-trace
  // range, so all axes agree.
  private computeSharedRange(): {min: number; max: number} | undefined {
    let min = Infinity;
    let max = -Infinity;
    for (const s of this.model.selected) {
      if (s.hidden) continue;
      const r = this.ownRange(s);
      if (r === undefined) continue;
      if (r.min < min) min = r.min;
      if (r.max > max) max = r.max;
    }
    return min === Infinity ? undefined : {min, max};
  }

  private laneRange(series: CounterSeries, heightPx: number): AxisRange {
    const approx = this.tickTarget(heightPx);
    const r = this.valueRange(series);
    if (r === undefined) return niceValueTicks(0, 1, approx);
    if (this.model.yScale === 'log' && r.max > 0) {
      const lo = r.min > 0 ? r.min : r.max / 1e3;
      return thinTicks(logDrawRange(lo, r.max), approx + 1);
    }
    return niceValueTicks(Math.min(0, r.min), r.max, approx);
  }

  // Maps a value to a y-pixel within [plotTop, plotTop+plotH], linear or log.
  private mapY(
    v: number,
    range: AxisRange,
    plotTop: number,
    plotH: number,
    log: boolean,
  ): number {
    const h = Math.max(1, plotH);
    if (log) {
      const lo = Math.log10(Math.max(range.min, 1e-9));
      const hi = Math.log10(Math.max(range.max, 1e-9));
      const vv = Math.log10(Math.max(v, 1e-9));
      const f = hi === lo ? 0 : (vv - lo) / (hi - lo);
      return plotTop + h - f * h;
    }
    const span = range.max - range.min || 1;
    return plotTop + h - ((v - range.min) / span) * h;
  }

  private yOf(v: number, range: AxisRange, lane: LaneLayout): number {
    const top = lane.top + LANE_PAD_TOP;
    const h = lane.bottom - LANE_PAD_BOTTOM - top;
    return this.mapY(v, range, top, h, this.model.yScale === 'log');
  }

  // Point-drawing stride so a line never draws more than ~2 samples per pixel —
  // beyond that is invisible but costs render time. Keeps fully-cached counters
  // (and dense overlays) fast without changing the on-screen result.
  private drawStride(count: number): number {
    const budget = 2 * this.plotWidth();
    return count > budget ? Math.ceil(count / budget) : 1;
  }

  private drawLane(
    pal: Palette,
    series: CounterSeries,
    idx: number,
    lane: LaneLayout,
  ) {
    const ctx = this.ctx;
    const color = this.seriesColor(series, idx, pal);
    const range = this.laneRange(series, lane.height);
    const left = this.padLeft;
    const right = this.cssW - this.padRight;

    // Lane background + frame.
    ctx.fillStyle = pal.laneBg;
    ctx.fillRect(left, lane.top, right - left, lane.height);
    ctx.strokeStyle = pal.border;
    ctx.lineWidth = 1;
    ctx.strokeRect(
      left + 0.5,
      lane.top + 0.5,
      right - left - 1,
      lane.height - 1,
    );

    // The wide left column is a shell (like the track shells above): show the
    // counter name, left-aligned and clipped so it never runs into the y-axis
    // labels on the shell's right edge (only when the gutter is wide enough).
    if (left > PAD_LEFT_DEFAULT) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(4, lane.top, Math.max(1, left - 52), lane.height);
      ctx.clip();
      ctx.fillStyle = pal.text;
      ctx.font = AXIS_FONT;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'alphabetic';
      ctx.fillText(series.label(this.model.nameBy), 8, lane.top + 14);
      ctx.restore();
    }

    // Horizontal gridlines + y tick labels.
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const tick of range.ticks) {
      const y = this.yOf(tick, range, lane);
      if (y < lane.top - 0.5 || y > lane.bottom + 0.5) continue;
      ctx.strokeStyle = pal.grid;
      ctx.beginPath();
      ctx.moveTo(left, Math.round(y) + 0.5);
      ctx.lineTo(right, Math.round(y) + 0.5);
      ctx.stroke();
      ctx.fillStyle = pal.muted;
      ctx.fillText(this.fmt(series, tick), left - 6, y);
    }

    this.drawSeries(series, color, range, lane);

    // The counter name lives in the legend chips / hover tooltip, not on the
    // chart. We keep only a small latest-value badge (no name) at top-right.
    const w = series.window;
    if (w !== undefined && w.count > 0) {
      ctx.textBaseline = 'alphabetic';
      ctx.textAlign = 'right';
      ctx.fillStyle = pal.muted;
      ctx.font = AXIS_FONT;
      ctx.fillText(
        this.fmt(series, w.lastV[w.count - 1]),
        right - 6,
        lane.top + 13,
      );
    }
  }

  private drawSeries(
    series: CounterSeries,
    color: string,
    range: AxisRange,
    lane: LaneLayout,
  ) {
    const w = series.window;
    if (w === undefined || w.count === 0) return;
    const ctx = this.ctx;
    const n = w.count;
    const baseY = lane.bottom - LANE_PAD_BOTTOM;
    // Grey this series out when another line is focused (hovered); eased.
    const a = this.alphaFor(series);
    const stride = this.drawStride(n);
    const last = n - 1;
    // x of the sample `stride` ahead of i, or the right edge past the end.
    const nextX = (i: number) =>
      i + stride <= last
        ? this.xOfRel(w.tsRel[i + stride])
        : this.cssW - this.padRight;

    ctx.save();
    ctx.beginPath();
    ctx.rect(
      this.padLeft,
      lane.top,
      this.cssW - this.padRight - this.padLeft,
      lane.height,
    );
    ctx.clip();

    // Gradient area under the (stepped) last value — lighter than the line,
    // fading towards the baseline.
    if (this.model.fill) {
      ctx.beginPath();
      ctx.moveTo(this.xOfRel(w.tsRel[0]), baseY);
      for (let i = 0; i <= last; i += stride) {
        const x = this.xOfRel(w.tsRel[i]);
        const y = this.yOf(w.lastV[i], range, lane);
        ctx.lineTo(x, y);
        ctx.lineTo(nextX(i), y);
      }
      // Drop to the baseline at the right edge (where the stepped line ends),
      // not at the last sample, so the area under the final flat segment fills
      // completely instead of leaving a diagonal gap.
      ctx.lineTo(this.cssW - this.padRight, baseY);
      ctx.closePath();
      const grad = ctx.createLinearGradient(0, lane.top, 0, baseY);
      grad.addColorStop(0, toRgba(color, 0.32));
      grad.addColorStop(1, toRgba(color, 0.02));
      ctx.globalAlpha = a;
      ctx.fillStyle = grad;
      ctx.fill();
    }

    // Min/max band (shows the values coalesced into each bucket).
    ctx.beginPath();
    for (let i = 0; i <= last; i += stride) {
      const x = this.xOfRel(w.tsRel[i]);
      const xn = nextX(i);
      const yMax = this.yOf(w.maxV[i], range, lane);
      ctx.rect(
        x,
        yMax,
        Math.max(1, xn - x),
        this.yOf(w.minV[i], range, lane) - yMax,
      );
    }
    ctx.globalAlpha = 0.18 * a;
    ctx.fillStyle = color;
    ctx.fill();

    // Stepped last-value line.
    ctx.globalAlpha = a;
    ctx.beginPath();
    for (let i = 0; i <= last; i += stride) {
      const x = this.xOfRel(w.tsRel[i]);
      const y = this.yOf(w.lastV[i], range, lane);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
      ctx.lineTo(nextX(i), y);
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = 'miter';
    ctx.setLineDash(this.dashFor(series.lineStyle));
    ctx.stroke();
    ctx.restore();
  }

  // Value of a series at plot-x `x` (the last sample at or before that time).
  private valueAtX(series: CounterSeries, x: number): number | undefined {
    const w = series.window;
    if (w === undefined || w.count === 0) return undefined;
    const rel = Number(this.xToTime(x) - this.model.traceStart);
    // Last sample with tsRel <= rel (binary search).
    let lo = 0;
    let hi = w.count - 1;
    let ans = 0;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (w.tsRel[mid] <= rel) {
        ans = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    return w.lastV[ans];
  }

  // Value at the local mouse cursor (used by the DOM tooltip, which only shows
  // while the pointer is over the chart).
  private valueAtCursor(series: CounterSeries): number | undefined {
    if (!this.mouseInside || this.mouseX < this.padLeft) return undefined;
    return this.valueAtX(series, this.mouseX);
  }

  // Where the vertical crosshair sits this frame (and the time under it), or
  // undefined when nothing is hovered / an interaction is in progress. It
  // follows the timeline's shared hover cursor, so hovering the tracks above
  // moves our pointer and hovering here moves theirs.
  private crosshair(): {x: number; t: time} | undefined {
    if (this.drag !== undefined || this.resize !== undefined) return undefined;
    const t = this.model.trace.timeline.hoverCursorTimestamp;
    if (t === undefined) return undefined;
    const x = this.scale().timeToPx(t);
    if (x < this.padLeft || x > this.cssW - this.padRight) return undefined;
    return {x, t};
  }

  private drawCrosshair(pal: Palette, top: number, bottom: number) {
    const cross = this.crosshair();
    if (cross === undefined) return;
    const ctx = this.ctx;
    const x = Math.round(cross.x) + 0.5;
    ctx.save();
    ctx.setLineDash([4, 3]);
    ctx.strokeStyle = pal.accent;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, bottom);
    ctx.stroke();
    ctx.setLineDash([]);

    // Time label pinned to the top axis.
    const label = this.timeLabel(cross.t);
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'alphabetic';
    const tw = ctx.measureText(label).width + 8;
    ctx.fillStyle = pal.accent;
    ctx.fillRect(cross.x - tw / 2, 1, tw, 13);
    ctx.fillStyle = pal.bg;
    ctx.fillText(label, cross.x, 11);

    // Per-lane dots at the hovered sample, using the drawn (scrolled) rects.
    for (const lane of this.laneRects) {
      if (lane.bottom < top || lane.top > bottom) continue; // off-screen lane
      const series = this.model.selected[lane.i];
      const v = this.valueAtX(series, cross.x);
      if (v === undefined) continue;
      const y = this.yOf(v, this.laneRange(series, lane.height), lane);
      if (y < top || y > bottom) continue;
      ctx.beginPath();
      ctx.fillStyle = this.seriesColor(series, lane.i, pal);
      ctx.arc(cross.x, y, 3, 0, Math.PI * 2);
      ctx.fill();
      ctx.strokeStyle = pal.bg;
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
    ctx.restore();
  }

  private drawZoomBox(pal: Palette, top: number, bottom: number) {
    if (this.drag === undefined || this.drag.mode !== 'zoombox') return;
    const ctx = this.ctx;
    const x0 = Math.min(this.drag.startX, this.drag.curX);
    const x1 = Math.max(this.drag.startX, this.drag.curX);
    ctx.save();
    ctx.fillStyle = pal.accent;
    ctx.globalAlpha = 0.15;
    ctx.fillRect(x0, top, x1 - x0, bottom - top);
    ctx.globalAlpha = 1;
    ctx.strokeStyle = pal.accent;
    ctx.lineWidth = 1;
    ctx.strokeRect(x0 + 0.5, top + 0.5, x1 - x0, bottom - top);
    const t0 = this.xToTime(x0);
    const t1 = this.xToTime(x1);
    const label = timeTickLabel(t1, t0);
    ctx.font = AXIS_FONT;
    ctx.textAlign = 'center';
    ctx.fillStyle = pal.text;
    ctx.fillText(label, (x0 + x1) / 2, top + 14);
    ctx.restore();
  }

  // Returns the theme palette, recomputing (via getComputedStyle) only every
  // REPALETTE_FRAMES draws so it's not a per-frame layout cost, while still
  // picking up light/dark theme toggles within a fraction of a second.
  private getPalette(): Palette {
    if (this.palCache === undefined || this.palAge <= 0) {
      this.palCache = this.palette();
      this.palAge = REPALETTE_FRAMES;
    }
    this.palAge--;
    return this.palCache;
  }

  private palette(): Palette {
    const cs = getComputedStyle(this.container);
    const get = (name: string, fallback: string) => {
      const v = cs.getPropertyValue(name).trim();
      return v.length > 0 ? v : fallback;
    };
    const series: string[] = [];
    for (let i = 1; i <= 8; i++) {
      series.push(get(`--pf-chart-color-${i}`, '#4285f4'));
    }
    return {
      text: get('--pf-color-text', '#202124'),
      muted: get('--pf-color-text-muted', '#5f6368'),
      border: get('--pf-color-border', '#dadce0'),
      grid: get('--pf-color-border-secondary', 'rgba(0,0,0,0.06)'),
      bg: get('--pf-color-background', '#ffffff'),
      laneBg: get('--pf-color-background-secondary', 'rgba(0,0,0,0.02)'),
      accent: get('--pf-color-accent', '#1a73e8'),
      series,
    };
  }

  // ---- interaction --------------------------------------------------------

  private localX(clientX: number): number {
    return clientX - this.canvas.getBoundingClientRect().left;
  }

  private attachListeners() {
    const c = this.canvas;

    // Note: the mouse wheel is intentionally NOT captured for zoom — it scrolls
    // the stacked lanes (and the page) normally. Zoom is via WASD, drag-select,
    // or two-finger pinch.

    c.addEventListener('pointerdown', (e: PointerEvent) => {
      c.setPointerCapture(e.pointerId);
      const ly = this.localY(e.clientY);
      // Height-resize takes priority over zoom/pan.
      const rz = this.hitResize(ly);
      if (rz !== undefined) {
        this.resize = {startY: ly, ...rz};
        this.wake();
        return;
      }
      // Grabbing a stacked lane's shell (name gutter) starts a reorder drag.
      if (
        this.model.layout === 'stacked' &&
        e.button === 0 &&
        this.localX(e.clientX) < this.padLeft
      ) {
        const k = this.visibleLaneIndexAt(ly);
        const lane = k >= 0 ? this.visibleLanes()[k] : undefined;
        if (lane !== undefined) {
          this.laneReorder = {series: lane.s, targetIdx: k};
          this.canvas.style.cursor = 'grabbing';
          this.wake();
          return;
        }
      }
      this.pointers.set(e.pointerId, e.clientX);
      if (this.pointers.size === 2) {
        this.beginPinch();
        this.drag = undefined;
        return;
      }
      const x = this.localX(e.clientX);
      const pan = e.shiftKey || e.button === 1 || e.button === 2;
      this.drag = {
        mode: pan ? 'pan' : 'zoombox',
        startX: x,
        curX: x,
        startView: {s: this.model.viewStart, e: this.model.viewEnd},
      };
      this.wake();
    });

    c.addEventListener('pointermove', (e: PointerEvent) => {
      if (this.pointers.has(e.pointerId)) {
        this.pointers.set(e.pointerId, e.clientX);
      }
      this.mouseX = this.localX(e.clientX);
      const ly = this.localY(e.clientY);
      this.mouseY = ly;
      this.mouseInside = true;
      // Active lane reorder: track which slot we'd drop into.
      if (this.laneReorder !== undefined) {
        this.laneReorder.targetIdx = this.visibleLaneIndexAt(ly);
        this.wake();
        return;
      }
      // Move the timeline's shared hover pointer with our cursor (sync mode).
      this.syncHoverCursor();
      // Redraw the DOM tooltip only when its content changes (out-of-band
      // handler, so Mithril won't auto-redraw).
      const sig = this.tooltipSig();
      if (sig !== this.lastTipSig) {
        this.lastTipSig = sig;
        m.redraw();
      }

      // Active height resize.
      if (this.resize !== undefined) {
        const dy = ly - this.resize.startY;
        if (this.resize.kind === 'overlay') {
          const full = this.cssH - BOTTOM - TOP_AXIS;
          this.model.setOverlayHeight(
            Math.max(60, Math.min(this.resize.startH + dy, full)),
          );
        } else {
          this.model.setSeriesLaneHeight(
            this.resize.series,
            Math.max(40, Math.min(this.resize.startH + dy, 400)),
          );
        }
        this.wake();
        return;
      }
      // Cursor feedback: resize edge, a grabbable stacked-lane shell, else the
      // crosshair.
      const overShell =
        this.model.layout === 'stacked' &&
        this.mouseX < this.padLeft &&
        ly > TOP_AXIS;
      this.canvas.style.cursor =
        this.drag === undefined && this.hitResize(ly) !== undefined
          ? 'ns-resize'
          : overShell
            ? 'grab'
            : 'crosshair';

      if (this.pinch !== undefined && this.pointers.size >= 2) {
        this.updatePinch();
        this.wake();
        return;
      }
      if (this.drag !== undefined) {
        this.drag.curX = this.mouseX;
        if (this.drag.mode === 'pan') {
          const dxPx = this.drag.curX - this.drag.startX;
          const dxNs =
            -(dxPx / this.plotWidth()) *
            Number(this.drag.startView.e - this.drag.startView.s);
          this.model.setView(this.drag.startView.s, this.drag.startView.e);
          this.model.panBy(dxNs);
        }
      }
      this.wake();
    });

    const end = (e: PointerEvent) => {
      this.pointers.delete(e.pointerId);
      if (this.pointers.size < 2) this.pinch = undefined;
      this.resize = undefined;
      // Finish a lane reorder.
      if (this.laneReorder !== undefined) {
        this.model.reorder(this.laneReorder.series, this.laneReorder.targetIdx);
        this.laneReorder = undefined;
        this.canvas.style.cursor = 'crosshair';
        this.wake();
        m.redraw();
        return;
      }
      if (this.drag !== undefined && this.drag.mode === 'zoombox') {
        const x0 = Math.min(this.drag.startX, this.drag.curX);
        const x1 = Math.max(this.drag.startX, this.drag.curX);
        if (x1 - x0 > 3) {
          this.model.setView(this.xToTime(x0), this.xToTime(x1));
        } else {
          // A click (no drag). Resolve single- vs double-click before acting.
          this.scheduleClick(this.drag.startX, this.mouseY);
        }
      }
      this.drag = undefined;
      this.wake();
      m.redraw();
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);

    c.addEventListener('pointerleave', () => {
      this.mouseInside = false;
      // Reverts the shared hover cursor to the pinned time (or clears it).
      this.syncHoverCursor();
      this.wake();
      m.redraw(); // hide the tooltip
    });
    c.addEventListener('dblclick', (e: MouseEvent) => {
      this.onDoubleClick(this.localX(e.clientX), this.localY(e.clientY));
    });
    c.addEventListener('contextmenu', (e) => e.preventDefault());
  }

  // A single click isolates the line under the cursor (show only it), or — on
  // empty space — parks/clears the reference crosshair. Deferred briefly so a
  // double-click can pre-empt it.
  private scheduleClick(x: number, y: number) {
    if (this.clickTimer !== undefined) clearTimeout(this.clickTimer);
    this.clickTimer = setTimeout(() => {
      this.clickTimer = undefined;
      const hit = this.lineAt(x, y, 12);
      const stacked = this.model.layout === 'stacked';
      if (hit !== undefined) {
        // Stacked: click greys the others and multi-selects (toggle). Overlay:
        // click isolates the line (show only it).
        if (stacked) this.model.toggleEmphasis(hit.s);
        else this.model.toggleOnly(hit.s);
      } else if (stacked && this.model.hasEmphasis()) {
        this.model.clearEmphasis(); // click empty space to clear the selection
      } else if (this.model.isIsolated()) {
        // Clicking away from the single isolated line brings them all back, so
        // the isolate doesn't get stuck when the click lands in empty space
        // (e.g. below a lone overlay-isolated line).
        this.model.setAllHidden(false);
      } else {
        this.togglePinAt(x);
      }
      this.wake();
      m.redraw();
    }, 220);
  }

  // A double click hides the line under the cursor (keeping the others), or —
  // on empty space — drops a persistent bookmark note on the timeline.
  private onDoubleClick(x: number, y: number) {
    if (this.clickTimer !== undefined) {
      clearTimeout(this.clickTimer);
      this.clickTimer = undefined;
    }
    const hit = this.lineAt(x, y, 12);
    if (hit !== undefined) {
      if (!hit.s.hidden) this.model.toggleHidden(hit.s);
    } else {
      this.model.trace.notes.addNote({timestamp: this.xToTime(x)});
    }
    this.wake();
    m.redraw();
  }

  // Publishes the timeline's shared hover cursor so the vertical hover pointer
  // over the tracks tracks our cursor — and vice-versa via crosshair(). While
  // the pointer is over the plot it follows the cursor; otherwise it falls back
  // to the pinned time (a parked reference line) or clears. Deduped so an
  // unchanged value doesn't schedule a redundant timeline redraw.
  private syncHoverCursor(): void {
    const inPlot =
      this.mouseInside &&
      this.drag === undefined &&
      this.resize === undefined &&
      this.mouseX >= this.padLeft &&
      this.mouseX <= this.cssW - this.padRight;
    const next = inPlot ? this.xToTime(this.mouseX) : this.pinnedTime;
    if (next !== this.publishedHover) {
      this.publishedHover = next;
      this.model.trace.timeline.hoverCursorTimestamp = next;
    }
  }

  // Click-to-pin: park a persistent reference crosshair at plot-x `x` (so it
  // survives the pointer leaving); clicking on/near the existing pin clears it.
  private togglePinAt(x: number): void {
    if (this.pinnedTime !== undefined) {
      const px = this.scale().timeToPx(this.pinnedTime);
      if (Math.abs(px - x) < 6) {
        this.pinnedTime = undefined;
        this.syncHoverCursor();
        return;
      }
    }
    this.pinnedTime = this.xToTime(x);
    this.model.trace.timeline.hoverCursorTimestamp = this.pinnedTime;
  }

  private beginPinch() {
    const xs = [...this.pointers.values()];
    if (xs.length < 2) return;
    const dist = Math.abs(xs[0] - xs[1]);
    const midClient = (xs[0] + xs[1]) / 2;
    this.pinch = {
      startDist: Math.max(1, dist),
      startView: {s: this.model.viewStart, e: this.model.viewEnd},
      centerX: this.localX(midClient),
    };
  }

  private updatePinch() {
    if (this.pinch === undefined) return;
    const xs = [...this.pointers.values()];
    if (xs.length < 2) return;
    const dist = Math.max(1, Math.abs(xs[0] - xs[1]));
    const factor = this.pinch.startDist / dist; // fingers apart => zoom in
    this.model.setView(this.pinch.startView.s, this.pinch.startView.e);
    const centerTime = this.xToTime(this.pinch.centerX);
    this.model.zoomAt(centerTime, factor);
  }
}
