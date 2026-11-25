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
import {SimpleResizeObserver} from '../../../base/resize_observer';
import './live_strip_chart.scss';

export interface StripSeries {
  readonly name: string;
  readonly color: string;
  // Points keyed by monotonic trace seconds (t) and numeric value (v).
  readonly points: ReadonlyArray<{readonly t: number; readonly v: number}>;
}

export interface StripMarker {
  readonly t: number;
  readonly label: string;
  readonly color: string;
}

export interface LiveStripChartAttrs {
  readonly series: ReadonlyArray<StripSeries>;
  // Latest monotonic trace timestamp (seconds) in the current poll.
  readonly latestSec: number;
  // Rolling window width in seconds (default 60).
  readonly windowSec?: number;
  // Target jitter-buffer lag behind latestSec in seconds (default 2.0).
  // Kept slightly above the poll cadence so real samples continuously feed
  // the 60fps playhead at steady 1.0x real time without stalling.
  readonly lagSec?: number;
  // When true, advances the viewport smoothly at 60fps via a jitter buffer.
  // When false (paused or replay), snaps cleanly to latestSec with no rAF loop.
  readonly isLive: boolean;
  readonly stacked?: boolean;
  readonly fillArea?: boolean;
  readonly scaleAxes?: boolean;
  readonly height?: number;
  readonly yMaxFixed?: number;
  readonly yMaxFloor?: number;
  readonly formatY: (v: number) => string;
  readonly markers?: ReadonlyArray<StripMarker>;
  readonly showLegend?: boolean;
  readonly onSeriesClick?: (seriesName: string) => void;
  // Optional discriminator; changing it clears accumulated rolling history
  // (e.g. when switching metric or drill-down scope) without remounting DOM.
  readonly resetKey?: string;
}

const LEFT_PAD = 56;
const RIGHT_PAD = 10;
const TOP_PAD = 10;
const BOTTOM_PAD = 20;
const DEFAULT_LAG_S = 2.0;

function niceCeiling(raw: number): number {
  if (!isFinite(raw) || raw <= 0) return 1;
  const exp = Math.floor(Math.log10(raw));
  const base = Math.pow(10, exp);
  const f = raw / base;
  const step =
    f <= 1 ? 1 : f <= 1.5 ? 1.5 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10;
  return step * base;
}

function resolveColor(raw: string, style: CSSStyleDeclaration): string {
  let cur = raw.trim();
  if (!cur) return '#2563eb';
  for (let pass = 0; pass < 4 && cur.includes('var('); pass++) {
    const next = cur.replace(
      /var\(\s*(--[a-zA-Z0-9_-]+)(?:\s*,\s*([^)]+))?\s*\)/g,
      (_match, varName: string, fallback?: string) => {
        const val = style.getPropertyValue(varName).trim();
        return val !== '' ? val : (fallback?.trim() ?? '');
      },
    );
    if (next === cur) break;
    cur = next;
  }
  return cur || '#2563eb';
}

function colorWithAlpha(color: string, alpha: number): string {
  const c = color.trim();
  if (c.startsWith('#') && (c.length === 7 || c.length === 4)) {
    const full =
      c.length === 4 ? `#${c[1]}${c[1]}${c[2]}${c[2]}${c[3]}${c[3]}` : c;
    const r = parseInt(full.slice(1, 3), 16);
    const g = parseInt(full.slice(3, 5), 16);
    const b = parseInt(full.slice(5, 7), 16);
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }
  const rgbMatch = c.match(/^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/i);
  if (rgbMatch) {
    return `rgba(${rgbMatch[1]}, ${rgbMatch[2]}, ${rgbMatch[3]}, ${alpha})`;
  }
  const pct = Math.round(Math.max(0, Math.min(1, alpha)) * 100);
  return `color-mix(in srgb, ${c} ${pct}%, transparent)`;
}

export interface StripHoverItem {
  readonly name: string;
  readonly color: string;
  readonly value: string;
  readonly rawValue: number;
}

export interface StripHoverInfo {
  readonly relS: number;
  readonly timeLabel: string;
  readonly px: number;
  readonly items: ReadonlyArray<StripHoverItem>;
  readonly totalValue?: string;
  readonly signature: string;
}

interface PlotSnapshot {
  readonly xMin: number;
  readonly xMax: number;
  readonly winSec: number;
  readonly plotW: number;
  readonly plotH: number;
  readonly yMin: number;
  readonly yMax: number;
  readonly resolvedSeries: ReadonlyArray<MergedSeries>;
}

export function computeStripHoverInfo(opts: {
  readonly series: ReadonlyArray<MergedSeries>;
  readonly hoverX: number;
  readonly plotW: number;
  readonly nowSec: number;
  readonly winSec: number;
  readonly stacked: boolean;
  readonly atRightEdge: boolean;
  readonly formatY: (v: number) => string;
}): StripHoverInfo | undefined {
  const rightX = LEFT_PAD + opts.plotW;
  const px = opts.atRightEdge
    ? rightX
    : Math.max(LEFT_PAD, Math.min(rightX, opts.hoverX));
  const t = opts.atRightEdge
    ? opts.nowSec
    : opts.nowSec - opts.winSec + ((px - LEFT_PAD) / opts.plotW) * opts.winSec;
  const relS = opts.atRightEdge ? 0 : t - opts.nowSec;
  const timeLabel = Math.abs(relS) < 0.5 ? 'Now' : `${relS.toFixed(0)}s`;

  const sampled: StripHoverItem[] = [];
  let total = 0;
  for (const s of opts.series) {
    const v = sampleAt(s.pts, t);
    if (v === undefined) continue;
    if (opts.stacked) total += Math.max(0, v);
    sampled.push({
      name: s.name,
      color: s.color,
      value: opts.formatY(v),
      rawValue: v,
    });
  }
  if (sampled.length === 0) return undefined;

  let items: StripHoverItem[] = opts.stacked ? [...sampled].reverse() : sampled;
  if (opts.stacked && items.length > 6) {
    const nonZero = items.filter((it) => it.rawValue > 0);
    if (nonZero.length > 0) items = nonZero;
  }
  const totalValue =
    opts.stacked && sampled.length > 1 ? opts.formatY(total) : undefined;
  const signature = `${timeLabel}|${totalValue ?? ''}|${items
    .map((it) => `${it.name}:${it.value}`)
    .join('|')}`;
  return {relS, timeLabel, px, items, totalValue, signature};
}

function hitTestStackedBand(
  plot: PlotSnapshot,
  x: number,
  y: number,
): string | undefined {
  if (
    x < LEFT_PAD ||
    x > LEFT_PAD + plot.plotW ||
    y < TOP_PAD ||
    y > TOP_PAD + plot.plotH
  ) {
    return undefined;
  }
  const t = plot.xMin + ((x - LEFT_PAD) / plot.plotW) * plot.winSec;
  const yFrac = 1 - (y - TOP_PAD) / plot.plotH;
  const val = plot.yMin + yFrac * Math.max(1e-6, plot.yMax - plot.yMin);
  let baseline = 0;
  for (const s of plot.resolvedSeries) {
    const v = Math.max(0, sampleAt(s.pts, t) ?? 0);
    const top = baseline + v;
    if (v > 0 && val >= baseline && val <= top) {
      return s.name;
    }
    baseline = top;
  }
  return undefined;
}

interface MergedSeries {
  readonly name: string;
  readonly color: string;
  // Sorted ascending by t.
  readonly pts: Array<{readonly t: number; readonly v: number}>;
}

// Computes the next smooth playhead timestamp given current playhead, target
// latest sample timestamp, frame delta (seconds), and jitter-buffer lag.
export function stepLivePlayhead(
  cur: number,
  target: number,
  dt: number,
  lagSec: number,
): number {
  if (dt <= 0) return cur;
  const lead = target - cur;
  let rate = 1.0;
  if (lead > lagSec + 0.35) {
    rate = 1.0 + Math.min(1.0, (lead - lagSec) * 0.45);
  } else if (lead < 0.45) {
    rate = lead <= 0.01 ? 0 : Math.max(0.08, (lead / 0.45) * 0.9);
  }
  return Math.min(target, cur + dt * rate);
}

// High-DPI 60fps Canvas rolling strip-chart for smooth Live monitoring.
// Eliminates discrete 1-3s jumps by:
//  1) Anchoring a fixed [-windowSec, Now] X-axis frame on the right edge.
//  2) Gliding a jitter-buffered playhead at steady 1.0x real time (60fps)
//     across real buffered samples so curves flow smoothly across both edges.
//  3) Smoothly damping Y-axis scale transitions with downward hysteresis and
//     a fixed left gutter so axes and layout never pump or jitter.
export class LiveStripChart implements m.ClassComponent<LiveStripChartAttrs> {
  private canvas?: HTMLCanvasElement;
  private wrap?: HTMLElement;
  private resizeObs?: SimpleResizeObserver;
  private rafId?: number;
  private lastFrameMs = 0;

  // Per-series merged history so points glide cleanly off the left edge.
  private history = new Map<string, Map<number, number>>();
  private merged: MergedSeries[] = [];
  private latestAttrs?: LiveStripChartAttrs;
  private prevLatestSec?: number;
  private prevResetKey?: string;

  // Smooth animation state.
  private displayNowSec?: number;
  private displayYMin?: number;
  private displayYMax?: number;

  // Interactive hover & click hit-testing state.
  private hoverX?: number;
  private hoverY?: number;
  private hoverAtRightEdge = false;
  private hoveredBandName?: string;
  private hoverInfo?: StripHoverInfo;
  private lastHoverSig = '';
  private lastHoverRedrawMs = 0;
  private lastPlot?: PlotSnapshot;

  oncreate({dom, attrs}: m.CVnodeDOM<LiveStripChartAttrs>) {
    this.wrap = dom as HTMLElement;
    this.canvas = dom.querySelector('canvas') ?? undefined;
    this.ingestAttrs(attrs);
    this.draw(0);
    this.syncLoop();
    this.resizeObs = new SimpleResizeObserver(dom, () => {
      this.draw(0);
    });
  }

  onupdate({attrs}: m.CVnodeDOM<LiveStripChartAttrs>) {
    this.ingestAttrs(attrs);
    this.syncLoop();
    if (!attrs.isLive) {
      this.draw(0);
    }
  }

  onremove() {
    if (this.rafId !== undefined) {
      cancelAnimationFrame(this.rafId);
      this.rafId = undefined;
    }
    this.resizeObs?.[Symbol.dispose]();
    this.resizeObs = undefined;
  }

  private ingestAttrs(attrs: LiveStripChartAttrs): void {
    this.latestAttrs = attrs;
    const winSec = attrs.windowSec ?? 60;
    const lagSec = attrs.lagSec ?? DEFAULT_LAG_S;

    // Reset history if the user switched devices/traces (time jumped backwards)
    // or switched metric/grouping scope (resetKey changed).
    if (
      (this.prevLatestSec !== undefined &&
        attrs.latestSec < this.prevLatestSec - 5) ||
      (this.prevResetKey !== undefined && attrs.resetKey !== this.prevResetKey)
    ) {
      this.history.clear();
      this.displayNowSec = undefined;
      this.displayYMin = undefined;
      this.displayYMax = undefined;
      this.hoverInfo = undefined;
      this.lastHoverSig = '';
    }
    this.prevLatestSec = attrs.latestSec;
    this.prevResetKey = attrs.resetKey;

    const cutoff = attrs.latestSec - winSec - 15;
    const activeNames = new Set<string>();
    const nextMerged: MergedSeries[] = [];

    for (const s of attrs.series) {
      activeNames.add(s.name);
      let bucket = this.history.get(s.name);
      if (!bucket) {
        bucket = new Map();
        this.history.set(s.name, bucket);
      }
      for (const p of s.points) {
        if (!Number.isFinite(p.t) || !Number.isFinite(p.v)) continue;
        // Round timestamp to milliseconds to avoid float-noise duplicates across polls.
        const tKey = Math.round(p.t * 1000) / 1000;
        bucket.set(tKey, p.v);
      }
      for (const t of bucket.keys()) {
        if (t < cutoff) bucket.delete(t);
      }
      const pts = [...bucket.entries()]
        .sort((a, b) => a[0] - b[0])
        .map(([t, v]) => ({t, v}));
      if (pts.length > 0) {
        nextMerged.push({name: s.name, color: s.color, pts});
      }
    }
    for (const name of this.history.keys()) {
      if (!activeNames.has(name)) this.history.delete(name);
    }
    this.merged = nextMerged;

    if (!attrs.isLive) {
      this.displayNowSec = attrs.latestSec;
    } else if (
      this.displayNowSec === undefined ||
      Math.abs(attrs.latestSec - this.displayNowSec) > 12
    ) {
      // First frame or huge resume gap: seed cleanly at target jitter lag.
      const minT = this.minSampleT();
      const span = minT !== undefined ? attrs.latestSec - minT : 0;
      const initialLag = span > 3 ? Math.min(lagSec, span * 0.25) : 0;
      this.displayNowSec = attrs.latestSec - initialLag;
    }
  }

  private minSampleT(): number | undefined {
    let min: number | undefined;
    for (const s of this.merged) {
      const t = s.pts[0]?.t;
      if (t !== undefined && (min === undefined || t < min)) min = t;
    }
    return min;
  }

  private syncLoop(): void {
    const shouldRun = Boolean(this.latestAttrs?.isLive);
    if (shouldRun && this.rafId === undefined) {
      this.lastFrameMs = performance.now();
      const step = (nowMs: number) => {
        const dt = Math.min(
          0.05,
          Math.max(0, (nowMs - this.lastFrameMs) / 1000),
        );
        this.lastFrameMs = nowMs;
        this.draw(dt);
        if (this.latestAttrs?.isLive) {
          this.rafId = requestAnimationFrame(step);
        } else {
          this.rafId = undefined;
        }
      };
      this.rafId = requestAnimationFrame(step);
    } else if (!shouldRun && this.rafId !== undefined) {
      cancelAnimationFrame(this.rafId);
      this.rafId = undefined;
    }
  }

  private advancePlayhead(dt: number, attrs: LiveStripChartAttrs): number {
    const target = attrs.latestSec;
    if (!attrs.isLive || dt <= 0) {
      this.displayNowSec = attrs.isLive
        ? (this.displayNowSec ?? target)
        : target;
      return this.displayNowSec;
    }
    const lag = attrs.lagSec ?? DEFAULT_LAG_S;
    const cur = this.displayNowSec ?? target;
    this.displayNowSec = stepLivePlayhead(cur, target, dt, lag);
    return this.displayNowSec;
  }

  private computeVisibleYRange(
    attrs: LiveStripChartAttrs,
    xMin: number,
    xMax: number,
    dt: number,
  ): {readonly yMin: number; readonly yMax: number} {
    if (attrs.yMaxFixed !== undefined && attrs.yMaxFixed > 0) {
      this.displayYMin = 0;
      this.displayYMax = attrs.yMaxFixed;
      return {yMin: 0, yMax: attrs.yMaxFixed};
    }

    let visMax = 0;
    let visMin = Infinity;

    if (attrs.stacked) {
      const sumByT = new Map<number, number>();
      for (const s of this.merged) {
        for (const p of s.pts) {
          if (p.t >= xMin - 2 && p.t <= xMax + 2) {
            sumByT.set(p.t, (sumByT.get(p.t) ?? 0) + Math.max(0, p.v));
          }
        }
      }
      for (const v of sumByT.values()) {
        if (v > visMax) visMax = v;
      }
      visMin = 0;
    } else {
      for (const s of this.merged) {
        for (const p of s.pts) {
          if (p.t >= xMin - 2 && p.t <= xMax + 2) {
            if (p.v > visMax) visMax = p.v;
            if (p.v < visMin) visMin = p.v;
          }
        }
      }
    }

    const floor = attrs.yMaxFloor ?? 1;
    let targetMin = 0;
    let targetMax = niceCeiling(Math.max(floor, visMax * 1.12));

    if (
      attrs.scaleAxes &&
      !attrs.stacked &&
      isFinite(visMin) &&
      visMax > 0 &&
      visMin > visMax * 0.35
    ) {
      const pad = Math.max((visMax - visMin) * 0.25, visMax * 0.06);
      targetMin = Math.max(0, Math.floor(visMin - pad));
      targetMax = Math.max(targetMin + 1, Math.ceil(visMax + pad));
    }

    if (this.displayYMax === undefined || !attrs.isLive || dt <= 0) {
      this.displayYMin = targetMin;
      this.displayYMax = targetMax;
    } else {
      if (targetMax > this.displayYMax) {
        const k = 1 - Math.exp(-dt * 8);
        this.displayYMax += (targetMax - this.displayYMax) * k;
      } else if (targetMax < this.displayYMax * 0.72) {
        // Gentle downward ease with hysteresis to prevent nervous Y-axis pumping.
        const k = 1 - Math.exp(-dt * 2.2);
        this.displayYMax += (targetMax - this.displayYMax) * k;
      }
      const curMin = this.displayYMin ?? 0;
      const kMin = 1 - Math.exp(-dt * 4);
      this.displayYMin = curMin + (targetMin - curMin) * kMin;
    }

    const yMin = this.displayYMin ?? 0;
    const yMax = Math.max(yMin + floor, this.displayYMax);
    return {yMin, yMax};
  }

  private draw(dt: number): void {
    const attrs = this.latestAttrs;
    const canvas = this.canvas;
    const wrap = this.wrap;
    if (!attrs || !canvas || !wrap) return;

    const cssW = canvas.clientWidth;
    const cssH = canvas.clientHeight;
    if (cssW <= 0 || cssH <= 0) return;

    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const pxW = Math.round(cssW * dpr);
    const pxH = Math.round(cssH * dpr);
    if (canvas.width !== pxW || canvas.height !== pxH) {
      canvas.width = pxW;
      canvas.height = pxH;
    }

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const style = getComputedStyle(wrap);
    const textMuted =
      style.getPropertyValue('--pf-color-text-muted').trim() || '#64748b';
    const borderColor =
      style.getPropertyValue('--pf-color-border').trim() || '#e2e8f0';
    const bgColor =
      style.getPropertyValue('--pf-color-background').trim() || '#ffffff';

    const resolvedSeries = this.merged.map((s) => ({
      ...s,
      color: resolveColor(s.color, style),
    }));

    const winSec = attrs.windowSec ?? 60;
    const nowSec = this.advancePlayhead(dt, attrs);
    const xMin = nowSec - winSec;
    const xMax = nowSec;
    const {yMin, yMax} = this.computeVisibleYRange(attrs, xMin, xMax, dt);
    const ySpan = Math.max(1e-6, yMax - yMin);

    const plotW = Math.max(1, cssW - LEFT_PAD - RIGHT_PAD);
    const plotH = Math.max(1, cssH - TOP_PAD - BOTTOM_PAD);

    this.lastPlot = {
      xMin,
      xMax,
      winSec,
      plotW,
      plotH,
      yMin,
      yMax,
      resolvedSeries,
    };

    // Keep hover tooltip labels and stacked-band highlight live as data
    // scrolls smoothly underneath a stationary cursor (including right edge).
    if (this.hoverX !== undefined) {
      const rightX = LEFT_PAD + plotW;
      const clampedX = this.hoverAtRightEdge
        ? rightX
        : Math.max(LEFT_PAD, Math.min(rightX, this.hoverX));
      this.hoverX = clampedX;
      const nextInfo = computeStripHoverInfo({
        series: resolvedSeries,
        hoverX: clampedX,
        plotW,
        nowSec,
        winSec,
        stacked: Boolean(attrs.stacked),
        atRightEdge: this.hoverAtRightEdge,
        formatY: attrs.formatY,
      });
      this.hoverInfo = nextInfo;
      if (attrs.onSeriesClick && attrs.stacked && this.hoverY !== undefined) {
        this.hoveredBandName = hitTestStackedBand(
          this.lastPlot,
          clampedX,
          this.hoverY,
        );
      } else {
        this.hoveredBandName = undefined;
      }
      const nextSig = nextInfo?.signature ?? '';
      if (nextSig !== this.lastHoverSig) {
        const nowMs = performance.now();
        if (nowMs - this.lastHoverRedrawMs >= 100 || this.lastHoverSig === '') {
          this.lastHoverSig = nextSig;
          this.lastHoverRedrawMs = nowMs;
          m.redraw();
        }
      }
    }

    const toX = (t: number) => LEFT_PAD + ((t - xMin) / winSec) * plotW;
    const toY = (v: number) =>
      TOP_PAD + plotH - Math.max(0, Math.min(1, (v - yMin) / ySpan)) * plotH;

    ctx.save();
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, cssW, cssH);

    // 1. Horizontal gridlines & fixed-gutter Y-axis labels
    const yTicks = plotH >= 150 ? 4 : 2;
    ctx.font = '10px system-ui, -apple-system, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= yTicks; i++) {
      const frac = i / yTicks;
      const val = yMin + ySpan * frac;
      const y = Math.round(TOP_PAD + plotH * (1 - frac)) + 0.5;
      ctx.strokeStyle = borderColor;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(LEFT_PAD, y);
      ctx.lineTo(LEFT_PAD + plotW, y);
      ctx.stroke();

      ctx.fillStyle = textMuted;
      ctx.fillText(attrs.formatY(val), LEFT_PAD - 6, y);
    }

    // 2. Fixed X-axis labels: -60s, -45s, -30s, -15s, Now
    ctx.textBaseline = 'top';
    const xMarks = [0, 0.25, 0.5, 0.75, 1];
    for (const frac of xMarks) {
      const x = LEFT_PAD + frac * plotW;
      const age = Math.round((1 - frac) * winSec);
      const label = age === 0 ? 'Now' : `-${age}s`;
      ctx.textAlign = frac === 0 ? 'left' : frac === 1 ? 'right' : 'center';
      ctx.fillStyle = textMuted;
      ctx.fillText(label, x, TOP_PAD + plotH + 5);
    }

    // 3. Clipped series plot region so curves glide smoothly across both edges
    ctx.save();
    ctx.beginPath();
    ctx.rect(LEFT_PAD, TOP_PAD - 2, plotW, plotH + 4);
    ctx.clip();

    if (attrs.stacked && resolvedSeries.length > 0) {
      this.drawStackedSeries(
        ctx,
        resolvedSeries,
        toX,
        toY,
        xMin,
        xMax,
        attrs.onSeriesClick ? this.hoveredBandName : undefined,
      );
    } else {
      this.drawLineSeries(
        ctx,
        resolvedSeries,
        toX,
        toY,
        xMin,
        xMax,
        plotH,
        attrs.fillArea,
      );
    }

    // 4. Vertical event markers (e.g. LMK kills)
    if (attrs.markers && attrs.markers.length > 0) {
      for (const mk of attrs.markers) {
        if (mk.t < xMin || mk.t > xMax) continue;
        const x = toX(mk.t);
        const mkColor = resolveColor(mk.color, style);
        ctx.strokeStyle = mkColor;
        ctx.lineWidth = 1.25;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(x, TOP_PAD);
        ctx.lineTo(x, TOP_PAD + plotH);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = mkColor;
        ctx.beginPath();
        ctx.arc(x, TOP_PAD + 3, 2.5, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // 5. Hover crosshair
    if (
      this.hoverX !== undefined &&
      this.hoverX >= LEFT_PAD &&
      this.hoverX <= LEFT_PAD + plotW
    ) {
      const hoverT = this.hoverAtRightEdge
        ? nowSec
        : xMin + ((this.hoverX - LEFT_PAD) / plotW) * winSec;
      ctx.strokeStyle = textMuted;
      ctx.lineWidth = 1;
      ctx.setLineDash([2, 2]);
      ctx.beginPath();
      ctx.moveTo(this.hoverX, TOP_PAD);
      ctx.lineTo(this.hoverX, TOP_PAD + plotH);
      ctx.stroke();
      ctx.setLineDash([]);

      if (attrs.stacked) {
        let acc = 0;
        for (const s of resolvedSeries) {
          const val = sampleAt(s.pts, hoverT);
          if (val === undefined) continue;
          acc += Math.max(0, val);
          if (resolvedSeries.length <= 12 && val > 0) {
            ctx.fillStyle = s.color;
            ctx.beginPath();
            ctx.arc(this.hoverX, toY(acc), 2.5, 0, Math.PI * 2);
            ctx.fill();
          }
        }
      } else {
        for (const s of resolvedSeries) {
          const val = sampleAt(s.pts, hoverT);
          if (val !== undefined) {
            ctx.fillStyle = s.color;
            ctx.beginPath();
            ctx.arc(this.hoverX, toY(val), 3, 0, Math.PI * 2);
            ctx.fill();
          }
        }
      }
    }

    ctx.restore();

    // 6. Crisp leading-edge live dot at Now (outside clip so it never crops)
    if (attrs.isLive && !attrs.stacked && resolvedSeries.length <= 4) {
      const rightX = LEFT_PAD + plotW;
      for (const s of resolvedSeries) {
        const val = sampleAt(s.pts, nowSec);
        if (val === undefined) continue;
        const y = toY(val);
        ctx.fillStyle = colorWithAlpha(s.color, 0.24);
        ctx.beginPath();
        ctx.arc(rightX, y, 5, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = s.color;
        ctx.strokeStyle = bgColor;
        ctx.lineWidth = 1.25;
        ctx.beginPath();
        ctx.arc(rightX, y, 2.6, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
      }
    }

    ctx.restore();
  }

  private drawLineSeries(
    ctx: CanvasRenderingContext2D,
    series: ReadonlyArray<MergedSeries>,
    toX: (t: number) => number,
    toY: (v: number) => number,
    xMin: number,
    xMax: number,
    plotH: number,
    fillArea?: boolean,
  ): void {
    const useFill = fillArea ?? series.length <= 2;
    for (const s of series) {
      const visible = sliceWithBoundary(s.pts, xMin, xMax);
      if (visible.length < 2) continue;

      if (useFill) {
        const grad = ctx.createLinearGradient(0, TOP_PAD, 0, TOP_PAD + plotH);
        grad.addColorStop(0, colorWithAlpha(s.color, 0.18));
        grad.addColorStop(1, colorWithAlpha(s.color, 0.01));
        ctx.fillStyle = grad;
        ctx.beginPath();
        ctx.moveTo(toX(visible[0]!.t), TOP_PAD + plotH);
        for (const p of visible) {
          ctx.lineTo(toX(p.t), toY(p.v));
        }
        ctx.lineTo(toX(visible[visible.length - 1]!.t), TOP_PAD + plotH);
        ctx.closePath();
        ctx.fill();
      }

      ctx.strokeStyle = s.color;
      ctx.lineWidth = 1.75;
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      ctx.beginPath();
      visible.forEach((p, idx) => {
        const x = toX(p.t);
        const y = toY(p.v);
        if (idx === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();
    }
  }

  private drawStackedSeries(
    ctx: CanvasRenderingContext2D,
    series: ReadonlyArray<MergedSeries>,
    toX: (t: number) => number,
    toY: (v: number) => number,
    xMin: number,
    xMax: number,
    highlightName?: string,
  ): void {
    const tSet = new Set<number>();
    for (const s of series) {
      const vis = sliceWithBoundary(s.pts, xMin, xMax);
      for (const p of vis) tSet.add(p.t);
    }
    const ts = [...tSet].sort((a, b) => a - b);
    if (ts.length < 2) return;

    const baseline = new Float64Array(ts.length);
    for (const s of series) {
      const top = new Float64Array(ts.length);
      for (let i = 0; i < ts.length; i++) {
        const v = sampleAt(s.pts, ts[i]!) ?? 0;
        top[i] = baseline[i]! + Math.max(0, v);
      }

      const alpha =
        highlightName === undefined
          ? 0.52
          : s.name === highlightName
            ? 0.76
            : 0.36;
      ctx.fillStyle = colorWithAlpha(s.color, alpha);
      ctx.beginPath();
      for (let i = 0; i < ts.length; i++) {
        const x = toX(ts[i]!);
        const y = toY(top[i]!);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      for (let i = ts.length - 1; i >= 0; i--) {
        ctx.lineTo(toX(ts[i]!), toY(baseline[i]!));
      }
      ctx.closePath();
      ctx.fill();

      ctx.strokeStyle = s.color;
      ctx.lineWidth = s.name === highlightName ? 1.75 : 1.25;
      ctx.lineJoin = 'round';
      ctx.beginPath();
      for (let i = 0; i < ts.length; i++) {
        const x = toX(ts[i]!);
        const y = toY(top[i]!);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      baseline.set(top);
    }
  }

  private handleMouseMove(e: MouseEvent, attrs: LiveStripChartAttrs): void {
    const canvas = this.canvas;
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect();
    const rawX = e.clientX - rect.left;
    const rawY = e.clientY - rect.top;
    const plotW = Math.max(1, rect.width - LEFT_PAD - RIGHT_PAD);
    const rightX = LEFT_PAD + plotW;
    if (rawX < LEFT_PAD - 4 || rawX > rightX + RIGHT_PAD) {
      this.hoverX = undefined;
      this.hoverY = undefined;
      this.hoverAtRightEdge = false;
      this.hoveredBandName = undefined;
      this.hoverInfo = undefined;
      this.lastHoverSig = '';
      if (!attrs.isLive) this.draw(0);
      return;
    }
    const atRightEdge = rawX >= rightX - 4;
    const x = atRightEdge ? rightX : Math.max(LEFT_PAD, Math.min(rightX, rawX));
    this.hoverX = x;
    this.hoverY = rawY;
    this.hoverAtRightEdge = atRightEdge;

    const winSec = attrs.windowSec ?? 60;
    const nowSec = this.displayNowSec ?? attrs.latestSec;
    const style = this.wrap ? getComputedStyle(this.wrap) : undefined;
    const resolvedSeries = this.merged.map((s) => ({
      ...s,
      color: style ? resolveColor(s.color, style) : s.color,
    }));
    const nextInfo = computeStripHoverInfo({
      series: resolvedSeries,
      hoverX: x,
      plotW,
      nowSec,
      winSec,
      stacked: Boolean(attrs.stacked),
      atRightEdge,
      formatY: attrs.formatY,
    });
    this.hoverInfo = nextInfo;
    this.lastHoverSig = nextInfo?.signature ?? '';
    this.lastHoverRedrawMs = performance.now();

    if (attrs.onSeriesClick && attrs.stacked && this.lastPlot) {
      this.hoveredBandName = hitTestStackedBand(this.lastPlot, x, rawY);
    } else {
      this.hoveredBandName = undefined;
    }
    if (!attrs.isLive) this.draw(0);
  }

  private handleClick(e: MouseEvent, attrs: LiveStripChartAttrs): void {
    if (
      !attrs.onSeriesClick ||
      !attrs.stacked ||
      !this.canvas ||
      !this.lastPlot
    ) {
      return;
    }
    const rect = this.canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    const hit = hitTestStackedBand(this.lastPlot, x, y);
    if (hit !== undefined) {
      attrs.onSeriesClick(hit);
    }
  }

  view({attrs}: m.CVnode<LiveStripChartAttrs>) {
    const h = attrs.height ?? 145;
    const showLegend = attrs.showLegend ?? attrs.series.length > 1;
    const legendItems = attrs.series.filter((s) => s.points.length > 0);
    const tipRight =
      this.hoverInfo &&
      this.canvas &&
      this.hoverInfo.px > this.canvas.clientWidth * 0.6;
    const clickable = attrs.onSeriesClick !== undefined;

    return m('.pf-live-strip', [
      m('.pf-live-strip__canvas-wrap', {style: {height: `${h}px`}}, [
        m('canvas.pf-live-strip__canvas', {
          style:
            clickable && this.hoveredBandName !== undefined
              ? {cursor: 'pointer'}
              : undefined,
          onmousemove: (e: MouseEvent) => this.handleMouseMove(e, attrs),
          onclick: clickable
            ? (e: MouseEvent) => this.handleClick(e, attrs)
            : undefined,
          onmouseleave: () => {
            this.hoverX = undefined;
            this.hoverY = undefined;
            this.hoverAtRightEdge = false;
            this.hoveredBandName = undefined;
            this.hoverInfo = undefined;
            this.lastHoverSig = '';
            if (!attrs.isLive) this.draw(0);
          },
        }),
        this.hoverInfo &&
          m(
            '.pf-live-strip__tip',
            {
              style: tipRight
                ? {
                    right: `${Math.max(8, (this.canvas?.clientWidth ?? 200) - this.hoverInfo.px + 10)}px`,
                  }
                : {left: `${this.hoverInfo.px + 10}px`},
            },
            [
              m('.pf-live-strip__tip-time', this.hoverInfo.timeLabel),
              this.hoverInfo.items.map((it) =>
                m('.pf-live-strip__tip-row', [
                  m('span.pf-live-strip__dot', {style: {background: it.color}}),
                  m('span', it.name),
                  m('strong', it.value),
                ]),
              ),
              this.hoverInfo.totalValue !== undefined &&
                m('.pf-live-strip__tip-total', [
                  m('span', 'Total'),
                  m('strong', this.hoverInfo.totalValue),
                ]),
            ],
          ),
      ]),
      showLegend &&
        legendItems.length > 0 &&
        m(
          '.pf-live-strip__legend',
          legendItems.map((s) =>
            m(
              'span.pf-live-strip__legend-item',
              {
                className: clickable
                  ? 'pf-live-strip__legend-item--clickable'
                  : undefined,
                onclick: clickable
                  ? () => attrs.onSeriesClick?.(s.name)
                  : undefined,
                title: clickable ? `Click to drill into ${s.name}` : undefined,
              },
              [
                m('span.pf-live-strip__dot', {style: {background: s.color}}),
                s.name,
              ],
            ),
          ),
        ),
    ]);
  }
}

// Returns all points inside [xMin, xMax] plus one neighbor outside each edge
// so lines enter and exit the clipped viewport seamlessly.
function sliceWithBoundary(
  pts: ReadonlyArray<{readonly t: number; readonly v: number}>,
  xMin: number,
  xMax: number,
): Array<{readonly t: number; readonly v: number}> {
  if (pts.length === 0) return [];
  let lo = 0;
  while (lo < pts.length && pts[lo]!.t < xMin) lo++;
  if (lo > 0) lo--;
  let hi = pts.length - 1;
  while (hi >= 0 && pts[hi]!.t > xMax) hi--;
  if (hi < pts.length - 1) hi++;
  if (lo > hi) return [];
  return pts.slice(lo, hi + 1);
}

// Linear interpolation between adjacent samples at trace timestamp t.
export function sampleAt(
  pts: ReadonlyArray<{readonly t: number; readonly v: number}>,
  t: number,
): number | undefined {
  if (pts.length === 0) return undefined;
  if (t < pts[0]!.t - 0.5 || t > pts[pts.length - 1]!.t + 3.0) return undefined;
  if (t <= pts[0]!.t) return pts[0]!.v;
  if (t >= pts[pts.length - 1]!.t) return pts[pts.length - 1]!.v;
  let lo = 0;
  let hi = pts.length - 1;
  while (lo + 1 < hi) {
    const mid = (lo + hi) >> 1;
    if (pts[mid]!.t <= t) lo = mid;
    else hi = mid;
  }
  const a = pts[lo]!;
  const b = pts[hi]!;
  const span = b.t - a.t;
  if (span <= 0) return a.v;
  if (span > 6) return Math.abs(t - a.t) < Math.abs(b.t - t) ? a.v : b.v;
  const f = (t - a.t) / span;
  return a.v + (b.v - a.v) * f;
}
