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

export type BrushDirection = 'horizontal' | 'vertical';

/**
 * Reusable SVG brush controller for chart components.
 *
 * Manages brush state (start/end positions in chart-pixel space),
 * provides pointer event handlers for the chart area <g> element,
 * and renders the visual selection rectangle.
 *
 * Supports both horizontal (X-axis) and vertical (Y-axis) brushing.
 * Each chart maps the resulting pixel range to its own domain
 * (e.g. continuous data values for Histogram, bar labels for BarChart).
 */
export class SvgBrush {
  private start?: number;
  private end?: number;

  /**
   * Whether a brush drag is currently in progress.
   */
  get isActive(): boolean {
    return this.start !== undefined && this.end !== undefined;
  }

  /**
   * Returns the current brush range in chart-pixel coordinates,
   * with start <= end. Returns undefined if no brush is active.
   */
  get range(): {start: number; end: number} | undefined {
    if (this.start === undefined || this.end === undefined) {
      return undefined;
    }
    return {
      start: Math.min(this.start, this.end),
      end: Math.max(this.start, this.end),
    };
  }

  /**
   * Returns mithril attrs to apply to the chart area <g> element.
   * Handles pointer capture, tracking, and completion.
   *
   * @param margin The margin offset of the chart area within the SVG viewBox.
   * @param margin.left Left margin in viewBox units.
   * @param margin.top Top margin in viewBox units.
   * @param direction Which axis the brush operates on.
   * @param onComplete Called with (start, end) in chart-pixel space when the
   *   brush drag completes. start <= end is guaranteed.
   */
  chartAreaAttrs(
    margin: {left: number; top: number},
    direction: BrushDirection,
    onComplete: (start: number, end: number) => void,
  ): object {
    const toChartCoord = (e: PointerEvent) =>
      clientToChartCoord(e, margin, direction);
    return {
      style: {
        cursor: direction === 'horizontal' ? 'col-resize' : 'row-resize',
      },
      onpointerdown: (e: PointerEvent) => {
        (e.currentTarget as Element).setPointerCapture(e.pointerId);
        const coord = toChartCoord(e);
        this.start = coord;
        this.end = coord;
      },
      onpointermove: (e: PointerEvent) => {
        if (this.start === undefined) return;
        this.end = toChartCoord(e);
      },
      onpointerup: (e: PointerEvent) => {
        (e.currentTarget as Element).releasePointerCapture(e.pointerId);
        if (this.start !== undefined && this.end !== undefined) {
          const lo = Math.min(this.start, this.end);
          const hi = Math.max(this.start, this.end);
          onComplete(lo, hi);
        }
        this.start = undefined;
        this.end = undefined;
      },
    };
  }

  /**
   * Renders the brush selection rectangle overlay.
   * Returns null if no brush is active.
   *
   * @param chartWidth Width of the chart area in viewBox units.
   * @param chartHeight Height of the chart area in viewBox units.
   * @param direction Which axis the brush operates on.
   * @param cssClass CSS class for the rectangle.
   */
  renderSelection(
    chartWidth: number,
    chartHeight: number,
    direction: BrushDirection,
    cssClass: string,
  ): m.Children {
    const r = this.range;
    if (r === undefined) return null;
    if (direction === 'horizontal') {
      return m(`rect.${cssClass}`, {
        x: r.start,
        y: 0,
        width: Math.max(0, r.end - r.start),
        height: chartHeight,
      });
    }
    return m(`rect.${cssClass}`, {
      x: 0,
      y: r.start,
      width: chartWidth,
      height: Math.max(0, r.end - r.start),
    });
  }
}

/**
 * Convert a pointer event to a chart-relative coordinate.
 * Uses SVG's coordinate transformation to handle viewBox scaling.
 */
function clientToChartCoord(
  e: PointerEvent,
  margin: {left: number; top: number},
  direction: BrushDirection,
): number {
  const group = e.currentTarget as SVGGElement;
  const svg = group.ownerSVGElement;
  if (svg === null) return 0;
  const point = svg.createSVGPoint();
  point.x = e.clientX;
  point.y = e.clientY;
  const ctm = svg.getScreenCTM();
  if (ctm === null) return 0;
  const svgPoint = point.matrixTransform(ctm.inverse());
  return direction === 'horizontal'
    ? svgPoint.x - margin.left
    : svgPoint.y - margin.top;
}
