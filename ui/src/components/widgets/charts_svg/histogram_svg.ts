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
import {classNames} from '../../../base/classnames';
import type {HistogramAttrs, HistogramData} from '../charts/histogram';
import {clamp} from '../../../base/math_utils';
import {shortUuid} from '../../../base/uuid';
import {
  AXIS_LABEL_FONT_SIZE,
  BORDER_COLOR,
  TEXT_COLOR,
  TICK_LABEL_GAP,
  TICK_LENGTH,
  computePlotLayout,
  logRange,
  niceRange,
  renderPlotFrame,
} from './common';
import {formatNumber} from '../charts/chart_utils';
import {SvgChartFrame} from './svg_chart_frame';
import {ChartTooltip} from './tooltip';

export type {
  HistogramAttrs,
  HistogramData,
  HistogramBucket,
} from '../charts/histogram';

const DEFAULT_BAR_COLOR = 'var(--pf-chart-color-1)';
const SELECTION_COLOR = 'rgba(0, 120, 212, 0.45)';
// Hover tint deliberately matches the selection colour: hovering a bucket
// previews what selecting it would look like.
const DEFAULT_BAR_HOVER_COLOR = SELECTION_COLOR;
// Visual gap (px) between the last regular bucket and the appended NULL bar.
const NULL_GAP_PX = 8;

interface HoverState {
  readonly index: number; // slot index: 0..n-1 buckets, n = NULL bar
}

export class HistogramSvg implements m.ClassComponent<HistogramAttrs> {
  private hover?: HoverState;
  // Active brush drag state, in *slot index space* so a resize during the
  // drag doesn't mangle the selection.
  private brushing?: {start: number; current: number; pointerId: number};
  private readonly clipId = `pf-chart-clip-${shortUuid()}`;

  view({attrs}: m.Vnode<HistogramAttrs>) {
    const {data} = attrs;
    const nullCount = data?.nullCount ?? 0;
    const isLoading = data === undefined;
    const isEmpty =
      data !== undefined && data.buckets.length === 0 && nullCount === 0;

    const tooltip = (() => {
      if (this.hover === undefined || data === undefined) return false;
      const rawFmtX = attrs.formatXValue ?? ((v: number) => formatNumber(v));
      const fmtX =
        attrs.integerDimension ?? false
          ? (v: number) => rawFmtX(Math.round(v))
          : rawFmtX;
      const fmtY = attrs.formatYValue ?? formatNumber;
      const idx = this.hover.index;
      const isNull = idx >= data.buckets.length;
      const nCount = data.nullCount ?? 0;
      const totalWithNull = data.totalCount + nCount;
      const count = isNull ? nCount : data.buckets[idx].count;
      const header = isNull
        ? 'NULL'
        : `${fmtX(data.buckets[idx].start)} – ${fmtX(data.buckets[idx].end)}`;
      const pct =
        totalWithNull > 0 ? ((count / totalWithNull) * 100).toFixed(1) : '0';
      return m(ChartTooltip, [
        m(ChartTooltip.Header, header),
        m(ChartTooltip.Row, {name: 'Count', value: fmtY(count)}),
        m(ChartTooltip.Row, {name: '%', value: `${pct}%`}),
      ]);
    })();

    return m(
      '.pf-chart-svg',
      {
        className: classNames(
          attrs.fillParent && 'pf-chart-svg--fill-parent',
          'pf-chart-svg--legend-top',
          attrs.className,
        ),
        style: attrs.fillParent
          ? undefined
          : {height: `${attrs.height ?? 200}px`},
      },
      m(SvgChartFrame, {
        isLoading,
        isEmpty,
        renderChart: (w, h) => this.renderChart(attrs, data!, w, h),
      }),
      tooltip,
    );
  }

  private renderChart(
    attrs: HistogramAttrs,
    data: HistogramData,
    width: number,
    height: number,
  ) {
    if (width === 0 || height === 0) {
      return m('svg', {width: '100%', height: '100%'});
    }

    // When integerDimension is true, X-axis labels snap to integers — useful
    // for integer-valued data where bucket bounds may be non-integer floats
    // (e.g. the loader returns 10.4 when the underlying values are 10–11).
    const integerDimension = attrs.integerDimension ?? false;
    const rawFmtX = attrs.formatXValue ?? ((v: number) => formatNumber(v));
    const fmtX = integerDimension
      ? (v: number) => rawFmtX(Math.round(v))
      : rawFmtX;
    const fmtY = attrs.formatYValue ?? formatNumber;
    const logScale = attrs.logScale ?? false;
    const barColor = attrs.barColor ?? DEFAULT_BAR_COLOR;
    const barHoverColor = attrs.barHoverColor ?? DEFAULT_BAR_HOVER_COLOR;

    const buckets = data.buckets;
    const nullCount = data.nullCount ?? 0;
    const hasNull = nullCount > 0;
    const totalSlots = buckets.length + (hasNull ? 1 : 0);

    // Y-axis range over all bar counts (including NULL).
    const counts: number[] = buckets.map((b) => b.count);
    if (hasNull) counts.push(nullCount);
    const yMaxRaw = counts.length > 0 ? Math.max(...counts) : 1;
    const yRange = logScale
      ? logRange(1, Math.max(yMaxRaw, 1))
      : niceRange(0, yMaxRaw, {integer: true, minInterval: 1});

    // Layout
    const xName = attrs.xAxisLabel;
    const yName = attrs.yAxisLabel ?? 'Count';
    const layout = computePlotLayout({
      width,
      height,
      yLabels: yRange.ticks.map(fmtY),
      xName,
      yName,
    });
    const {padLeft, padTop, plotW, plotH} = layout;

    // NULL bar is given the same width as a regular bar, plus a small gap.
    const nullExtraSlots = hasNull ? 1 : 0;
    const slotWidth = totalSlots > 0 ? plotW / totalSlots : 0;
    const nullGapPx = hasNull ? Math.min(NULL_GAP_PX, slotWidth * 0.5) : 0;
    // Adjust so bars + gap exactly fit.
    const dataSlots = totalSlots - nullExtraSlots;
    const adjBarWidth =
      totalSlots === 0
        ? 0
        : hasNull
          ? (plotW - nullGapPx) / totalSlots
          : plotW / totalSlots;

    const slotX = (i: number) => {
      if (!hasNull || i < dataSlots) return padLeft + i * adjBarWidth;
      return padLeft + dataSlots * adjBarWidth + nullGapPx;
    };
    // Width of slot i, computed as the gap to the next slot's left edge so
    // adjacent bars share a pixel boundary (avoids inconsistent subpixel
    // gaps between bars).
    const slotWidthAt = (i: number) => {
      if (i === dataSlots - 1 && !hasNull) {
        return padLeft + plotW - slotX(i);
      }
      return (
        slotX(i + 1) -
        slotX(i) -
        (hasNull && i === dataSlots - 1 ? nullGapPx : 0)
      );
    };

    const yToPx = logScale
      ? (y: number) => {
          const v = y > 0 ? y : yRange.min;
          const num = Math.log10(v) - Math.log10(yRange.min);
          const den = Math.log10(yRange.max) - Math.log10(yRange.min) || 1;
          return padTop + plotH - (num / den) * plotH;
        }
      : (y: number) =>
          padTop +
          plotH -
          ((y - yRange.min) / (yRange.max - yRange.min || 1)) * plotH;

    // Decide which slots get an x-axis tick label. Show bucket start values;
    // skip every k-th to keep them from overlapping.
    const charBudget = 6; // ~px per char as in estimateLabelWidth
    const slotLabelMax = Math.max(
      1,
      ...buckets.map((b) => fmtX(b.start).length),
      hasNull ? 4 : 0,
    );
    const labelPx = slotLabelMax * charBudget + 6;
    const labelStep = Math.max(
      1,
      Math.ceil(labelPx / Math.max(1, adjBarWidth)),
    );

    // Hit-test: convert client x to slot index.
    const xToSlot = (clientX: number, rectLeft: number): number => {
      const px = clientX - rectLeft - padLeft;
      if (px < 0) return -1;
      if (!hasNull) {
        const i = Math.floor(px / adjBarWidth);
        if (i < 0 || i >= totalSlots) return -1;
        return i;
      }
      // Data area
      if (px <= dataSlots * adjBarWidth) {
        const i = Math.floor(px / adjBarWidth);
        return clamp(i, 0, dataSlots - 1);
      }
      // Gap or NULL bar
      const nullStart = dataSlots * adjBarWidth + nullGapPx;
      if (px >= nullStart && px <= nullStart + adjBarWidth) {
        return dataSlots; // NULL slot
      }
      return -1;
    };

    // Selection overlay: highlight buckets overlapping the selection range.
    const sel = attrs.selection;
    const isSelected = (i: number): boolean => {
      if (sel === undefined || i >= buckets.length) return false;
      const b = buckets[i];
      // Point selection (start == end): highlight the bucket containing it.
      // Buckets are half-open [start, end) so a value on a boundary belongs
      // to the next bucket — except the last bucket, which must be inclusive
      // on its upper edge or a value equal to the data max lands nowhere.
      if (sel.start === sel.end) {
        const isLast = i === buckets.length - 1;
        return (
          sel.start >= b.start &&
          (isLast ? sel.start <= b.end : sel.start < b.end)
        );
      }
      return b.end > sel.start && b.start < sel.end;
    };

    return m(
      'svg.pf-chart-svg__svg',
      {
        width,
        height,
        viewBox: `0 0 ${width} ${height}`,
        style: attrs.onBrush && {cursor: 'crosshair'},
        oncontextmenu: (e: Event) => e.preventDefault(),
        onpointerdown:
          attrs.onBrush &&
          ((e: PointerEvent) => this.handleBrushDown(e, xToSlot)),
        onpointermove: (e: PointerEvent) =>
          this.handlePointerMove(e, xToSlot, totalSlots),
        onpointerup:
          attrs.onBrush &&
          ((e: PointerEvent) =>
            this.handleBrushUp(e, attrs.onBrush!, buckets, dataSlots)),
        onpointerleave: () => {
          if (this.brushing) return;
          if (this.hover !== undefined) this.hover = undefined;
        },
        onlostpointercapture: () => {
          this.brushing = undefined;
        },
      },
      m(
        'defs',
        m(
          'clipPath',
          {id: this.clipId},
          m('rect', {x: padLeft, y: padTop, width: plotW, height: plotH}),
        ),
      ),
      renderPlotFrame({
        layout,
        height,
        xName,
        yName,
        yTicks: yRange.ticks.map((t) => ({label: fmtY(t), y: yToPx(t)})),
      }),
      // Selected- and hovered-bar column backgrounds (full plot height).
      m(
        'g',
        {
          'clip-path': `url(#${this.clipId})`,
          'pointer-events': 'none',
          'shape-rendering': 'crispEdges',
        },
        // Selection backgrounds (regular buckets only).
        buckets.map((_, i) => {
          if (!isSelected(i)) return undefined;
          return m('rect', {
            x: slotX(i),
            y: padTop,
            width: Math.max(0, slotWidthAt(i)),
            height: plotH,
            fill: 'rgba(0, 120, 212, 0.12)',
          });
        }),
        // Hovered-slot background. Drawn on top of the selection background
        // so the hovered column reads clearly even when it's also selected.
        // Includes the NULL slot.
        this.hover !== undefined &&
          this.hover.index >= 0 &&
          this.hover.index < totalSlots &&
          m('rect', {
            x: slotX(this.hover.index),
            y: padTop,
            width: Math.max(
              0,
              this.hover.index < dataSlots
                ? slotWidthAt(this.hover.index)
                : adjBarWidth,
            ),
            height: plotH,
            fill: 'rgba(0, 120, 212, 0.18)',
          }),
      ),
      // Bars
      m(
        'g',
        {'clip-path': `url(#${this.clipId})`, 'shape-rendering': 'crispEdges'},
        buckets.map((b, i) => {
          const x = slotX(i);
          const y = yToPx(b.count);
          const h = Math.max(0, padTop + plotH - y);
          const hovered = this.hover?.index === i;
          const selected = isSelected(i);
          const fill = selected
            ? SELECTION_COLOR
            : hovered
              ? barHoverColor
              : barColor;
          return m('rect', {
            x,
            y,
            'width': Math.max(0, slotWidthAt(i)),
            'height': h,
            fill,
            'pointer-events': 'none',
          });
        }),
        hasNull &&
          (() => {
            const i = dataSlots;
            const x = slotX(i);
            const y = yToPx(nullCount);
            const h = Math.max(0, padTop + plotH - y);
            const hovered = this.hover?.index === i;
            const fill = hovered ? barHoverColor : barColor;
            return m('rect', {
              x,
              y,
              'width': Math.max(0, adjBarWidth),
              'height': h,
              fill,
              'opacity': 0.7,
              'pointer-events': 'none',
            });
          })(),
      ),
      // X ticks + labels (per-slot, thinned to fit).
      buckets.map((b, i) => {
        if (i % labelStep !== 0) return undefined;
        const x = slotX(i);
        return m('g', [
          m('line', {
            x1: x,
            y1: padTop + plotH,
            x2: x,
            y2: padTop + plotH + TICK_LENGTH,
            stroke: BORDER_COLOR,
          }),
          m(
            'text',
            {
              'x': x,
              'y': padTop + plotH + TICK_LENGTH + TICK_LABEL_GAP,
              'fill': TEXT_COLOR,
              'font-size': AXIS_LABEL_FONT_SIZE,
              'text-anchor': 'middle',
              'dominant-baseline': 'hanging',
            },
            fmtX(b.start),
          ),
        ]);
      }),
      // Final right-edge tick at the last bucket end.
      buckets.length > 0 &&
        (() => {
          const last = buckets[buckets.length - 1];
          const x = slotX(dataSlots - 1) + adjBarWidth;
          return m('g', [
            m('line', {
              x1: x,
              y1: padTop + plotH,
              x2: x,
              y2: padTop + plotH + TICK_LENGTH,
              stroke: BORDER_COLOR,
            }),
            m(
              'text',
              {
                'x': x,
                'y': padTop + plotH + TICK_LENGTH + TICK_LABEL_GAP,
                'fill': TEXT_COLOR,
                'font-size': AXIS_LABEL_FONT_SIZE,
                'text-anchor': 'middle',
                'dominant-baseline': 'hanging',
              },
              fmtX(last.end),
            ),
          ]);
        })(),
      // NULL label under the NULL bar.
      hasNull &&
        m(
          'text',
          {
            'x': slotX(dataSlots) + adjBarWidth / 2,
            'y': padTop + plotH + TICK_LENGTH + TICK_LABEL_GAP,
            'fill': TEXT_COLOR,
            'font-size': AXIS_LABEL_FONT_SIZE,
            'text-anchor': 'middle',
            'dominant-baseline': 'hanging',
          },
          'NULL',
        ),
      // Brush selection rectangle (live drag).
      this.brushing &&
        (() => {
          const a = clamp(this.brushing!.start, 0, dataSlots - 1);
          const b = clamp(this.brushing!.current, 0, dataSlots - 1);
          const lo = Math.min(a, b);
          const hi = Math.max(a, b);
          const x = slotX(lo);
          const w = slotX(hi) + adjBarWidth - x;
          return m('rect', {
            'x': x,
            'y': padTop,
            'width': Math.max(0, w),
            'height': plotH,
            'fill': 'rgba(0, 120, 212, 0.15)',
            'stroke': 'rgba(0, 120, 212, 0.5)',
            'stroke-width': 1,
            'pointer-events': 'none',
          });
        })(),
    );
  }

  private handleBrushDown(
    e: PointerEvent,
    xToSlot: (clientX: number, rectLeft: number) => number,
  ) {
    const svg = e.currentTarget as SVGSVGElement;
    const rect = svg.getBoundingClientRect();
    const slot = xToSlot(e.clientX, rect.left);
    if (slot < 0) return;
    svg.setPointerCapture(e.pointerId);
    this.brushing = {start: slot, current: slot, pointerId: e.pointerId};
    this.hover = undefined;
    e.preventDefault();
  }

  private handleBrushUp(
    e: PointerEvent,
    onBrush: (range: {start: number; end: number}) => void,
    buckets: HistogramData['buckets'],
    dataSlots: number,
  ) {
    if (!this.brushing || this.brushing.pointerId !== e.pointerId) return;
    const {start, current} = this.brushing;
    this.brushing = undefined;
    if (buckets.length === 0) return;
    const lo = clamp(Math.min(start, current), 0, dataSlots - 1);
    const hi = clamp(Math.max(start, current), 0, dataSlots - 1);
    onBrush({start: buckets[lo].start, end: buckets[hi].end});
  }

  private handlePointerMove(
    e: PointerEvent,
    xToSlot: (clientX: number, rectLeft: number) => number,
    totalSlots: number,
  ) {
    const svg = e.currentTarget as SVGSVGElement;
    const rect = svg.getBoundingClientRect();
    if (this.brushing && this.brushing.pointerId === e.pointerId) {
      const slot = xToSlot(e.clientX, rect.left);
      if (slot >= 0) {
        this.brushing = {
          start: this.brushing.start,
          current: slot,
          pointerId: e.pointerId,
        };
      }
      this.hover = undefined;
      return;
    }
    const slot = xToSlot(e.clientX, rect.left);
    if (slot < 0 || slot >= totalSlots) {
      if (this.hover !== undefined) this.hover = undefined;
      return;
    }
    if (this.hover?.index !== slot) this.hover = {index: slot};
  }
}
