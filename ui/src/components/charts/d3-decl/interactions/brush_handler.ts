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

/**
 * Brush interaction handlers for declarative charts.
 *
 * These classes manage pointer events for brush selection in charts,
 * handling both click and drag interactions cleanly without race conditions.
 *
 * Key design principles:
 * - Single source of truth for pointer state
 * - No timing-based flags (justBrushed, etc.)
 * - Distinguish click vs drag based on actual movement distance
 * - Proper pointer capture for reliable event handling
 */

import * as d3 from 'd3';
import {ChartMargin} from '../chart_utils';

/**
 * Handles 1D brush interactions for histograms and CDFs.
 * Manages horizontal brush selection with click vs drag distinction.
 */
export class BrushHandler1D {
  private startValue?: number;
  private currentValue?: number;
  private scale: d3.ScaleLinear<number, number>;
  private cachedEventHandlers?: {
    onpointerdown: (e: PointerEvent) => void;
    onpointermove: (e: PointerEvent) => void;
    onpointerup: (e: PointerEvent) => void;
    onpointercancel: (e: PointerEvent) => void;
  };

  constructor(
    private readonly svg: SVGSVGElement,
    scale: d3.ScaleLinear<number, number>,
    private readonly margin: ChartMargin,
    private readonly chartWidth: number,
    private readonly onBrushEnd: (start: number, end: number) => void,
    private readonly onClickEmpty: () => void,
  ) {
    this.scale = scale;
  }

  /**
   * Update the scale when the chart re-renders with new data/domain.
   * This is necessary when filtering changes the data range.
   */
  updateScale(scale: d3.ScaleLinear<number, number>): void {
    const oldDomain = this.scale.domain();
    const newDomain = scale.domain();

    // Only update if domain actually changed (prevents infinite render loops)
    if (oldDomain[0] === newDomain[0] && oldDomain[1] === newDomain[1]) {
      return;
    }

    this.scale = scale;
  }

  /**
   * Returns Mithril event handlers to spread into chart group element.
   * Usage: m('g', ...this.getEventHandlers(), [...children])
   */
  getEventHandlers() {
    // Cache handler object - if recreated every render, Mithril detaches/reattaches
    // handlers which breaks in-progress pointer interactions.
    if (!this.cachedEventHandlers) {
      this.cachedEventHandlers = {
        onpointerdown: (e: PointerEvent) => {
          this.handlePointerDown(e);
        },
        onpointermove: (e: PointerEvent) => {
          this.handlePointerMove(e);
        },
        onpointerup: (e: PointerEvent) => {
          this.handlePointerUp(e);
        },
        onpointercancel: (e: PointerEvent) => {
          this.handlePointerCancel(e);
        },
      };
    }
    return this.cachedEventHandlers;
  }

  private handlePointerDown(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.setPointerCapture(e.pointerId);

    this.startValue = this.clientXToValue(e);
    this.currentValue = this.startValue;
  }

  private handlePointerMove(e: PointerEvent): void {
    if (this.startValue === undefined) {
      return;
    }

    this.currentValue = this.clientXToValue(e);
  }

  private handlePointerUp(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.releasePointerCapture(e.pointerId);

    if (this.startValue === undefined || this.currentValue === undefined) {
      this.reset();
      return;
    }

    const start = Math.min(this.startValue, this.currentValue);
    const end = Math.max(this.startValue, this.currentValue);

    // Distinguish click from drag based on distance moved
    const domain = this.scale.domain();
    const domainWidth = domain[1] - domain[0];
    const threshold = domainWidth * 0.01; // 1% of domain
    const isDrag = Math.abs(end - start) > threshold;

    if (isDrag) {
      this.onBrushEnd(start, end);
    } else {
      this.onClickEmpty();
    }

    this.reset();
  }

  private handlePointerCancel(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    try {
      target.releasePointerCapture(e.pointerId);
    } catch (err) {
      // Silently handle error
    }
    this.reset();
  }

  /**
   * Returns current brush selection if active (during drag).
   */
  getCurrentBrush(): {start: number; end: number} | undefined {
    if (this.startValue === undefined || this.currentValue === undefined) {
      return undefined;
    }

    return {
      start: Math.min(this.startValue, this.currentValue),
      end: Math.max(this.startValue, this.currentValue),
    };
  }

  /**
   * Converts client X coordinate to data value.
   * Clamps to chart bounds to prevent values outside the visible range.
   */
  private clientXToValue(e: PointerEvent): number {
    // Get fresh SVG reference from event target to avoid stale reference after re-render
    const svg = (e.currentTarget as SVGElement).closest('svg') || this.svg;

    // Use SVG coordinate transformation to properly handle all transforms
    const point = svg.createSVGPoint();
    point.x = e.clientX;
    point.y = e.clientY;

    const ctm = svg.getScreenCTM();
    if (!ctm) return this.scale.domain()[0];

    const svgPoint = point.matrixTransform(ctm.inverse());
    const chartX = svgPoint.x - this.margin.left;
    const clampedX = Math.max(0, Math.min(this.chartWidth, chartX));
    const value = this.scale.invert(clampedX);

    return value;
  }

  private reset(): void {
    this.startValue = undefined;
    this.currentValue = undefined;
  }
}

/**
 * Handles 2D brush interactions for scatter plots.
 * Manages rectangular brush selection.
 */
export class BrushHandler2D {
  private startX?: number;
  private startY?: number;
  private currentX?: number;
  private currentY?: number;
  private xScale: d3.ScaleLinear<number, number>;
  private yScale: d3.ScaleLinear<number, number>;

  constructor(
    private readonly svg: SVGSVGElement,
    xScale: d3.ScaleLinear<number, number>,
    yScale: d3.ScaleLinear<number, number>,
    private readonly margin: ChartMargin,
    private readonly chartWidth: number,
    private readonly chartHeight: number,
    private readonly onBrushEnd: (rect: {
      xMin: number;
      xMax: number;
      yMin: number;
      yMax: number;
    }) => void,
    private readonly onClickEmpty: () => void,
  ) {
    this.xScale = xScale;
    this.yScale = yScale;
  }

  /**
   * Update scales on re-render (e.g., after filtering changes domains).
   */
  updateScales(
    xScale: d3.ScaleLinear<number, number>,
    yScale: d3.ScaleLinear<number, number>,
  ): void {
    const oldXDomain = this.xScale.domain();
    const newXDomain = xScale.domain();
    const oldYDomain = this.yScale.domain();
    const newYDomain = yScale.domain();

    // Only update if domains actually changed (prevents infinite render loops)
    const xChanged =
      oldXDomain[0] !== newXDomain[0] || oldXDomain[1] !== newXDomain[1];
    const yChanged =
      oldYDomain[0] !== newYDomain[0] || oldYDomain[1] !== newYDomain[1];

    if (!xChanged && !yChanged) {
      return;
    }

    this.xScale = xScale;
    this.yScale = yScale;
  }

  getEventHandlers() {
    return {
      onpointerdown: (e: PointerEvent) => this.handlePointerDown(e),
      onpointermove: (e: PointerEvent) => this.handlePointerMove(e),
      onpointerup: (e: PointerEvent) => this.handlePointerUp(e),
      onpointercancel: (e: PointerEvent) => this.handlePointerCancel(e),
    };
  }

  private handlePointerDown(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.setPointerCapture(e.pointerId);

    const point = this.clientToValue(e);
    this.startX = point.x;
    this.startY = point.y;
    this.currentX = point.x;
    this.currentY = point.y;
  }

  private handlePointerMove(e: PointerEvent): void {
    if (this.startX === undefined) return;

    const point = this.clientToValue(e);
    this.currentX = point.x;
    this.currentY = point.y;
  }

  private handlePointerUp(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.releasePointerCapture(e.pointerId);

    if (
      this.startX === undefined ||
      this.startY === undefined ||
      this.currentX === undefined ||
      this.currentY === undefined
    ) {
      this.reset();
      return;
    }

    const xMin = Math.min(this.startX, this.currentX);
    const xMax = Math.max(this.startX, this.currentX);
    const yMin = Math.min(this.startY, this.currentY);
    const yMax = Math.max(this.startY, this.currentY);

    // Distinguish click from drag
    const xDomain = this.xScale.domain();
    const yDomain = this.yScale.domain();
    const xThreshold = (xDomain[1] - xDomain[0]) * 0.01;
    const yThreshold = (yDomain[1] - yDomain[0]) * 0.01;

    const isDrag =
      Math.abs(xMax - xMin) > xThreshold || Math.abs(yMax - yMin) > yThreshold;

    if (isDrag) {
      this.onBrushEnd({xMin, xMax, yMin, yMax});
    } else {
      this.onClickEmpty();
    }

    this.reset();
  }

  private handlePointerCancel(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.releasePointerCapture(e.pointerId);
    this.reset();
  }

  getCurrentBrush():
    | {xMin: number; xMax: number; yMin: number; yMax: number}
    | undefined {
    if (
      this.startX === undefined ||
      this.startY === undefined ||
      this.currentX === undefined ||
      this.currentY === undefined
    ) {
      return undefined;
    }

    return {
      xMin: Math.min(this.startX, this.currentX),
      xMax: Math.max(this.startX, this.currentX),
      yMin: Math.min(this.startY, this.currentY),
      yMax: Math.max(this.startY, this.currentY),
    };
  }

  private clientToValue(e: PointerEvent): {x: number; y: number} {
    // Get fresh SVG reference from event target to avoid stale reference after re-render
    const svg = (e.currentTarget as SVGElement).closest('svg') || this.svg;

    // Use SVG coordinate transformation to properly handle all transforms
    const point = svg.createSVGPoint();
    point.x = e.clientX;
    point.y = e.clientY;

    const ctm = svg.getScreenCTM();
    if (!ctm) {
      return {
        x: this.xScale.domain()[0],
        y: this.yScale.domain()[0],
      };
    }

    const svgPoint = point.matrixTransform(ctm.inverse());
    const chartX = svgPoint.x - this.margin.left;
    const chartY = svgPoint.y - this.margin.top;

    // Clamp coordinates to chart bounds
    const clampedX = Math.max(0, Math.min(this.chartWidth, chartX));
    const clampedY = Math.max(0, Math.min(this.chartHeight, chartY));

    return {
      x: this.xScale.invert(clampedX),
      y: this.yScale.invert(clampedY),
    };
  }

  private reset(): void {
    this.startX = undefined;
    this.startY = undefined;
    this.currentX = undefined;
    this.currentY = undefined;
  }
}

/**
 * Handles categorical brush interactions for bar charts.
 * Manages selection of category ranges with proper click vs drag distinction.
 */
export class BrushHandlerCategorical {
  private startCategory?: string;
  private currentCategory?: string;
  private scale: d3.ScaleBand<string>;
  private categories: readonly string[];
  private cachedEventHandlers?: {
    onpointerdown: (e: PointerEvent) => void;
    onpointermove: (e: PointerEvent) => void;
    onpointerup: (e: PointerEvent) => void;
    onpointercancel: (e: PointerEvent) => void;
  };

  constructor(
    private readonly svg: SVGSVGElement,
    scale: d3.ScaleBand<string>,
    private readonly margin: ChartMargin,
    categories: readonly string[],
    private readonly onBrushEnd: (categories: readonly string[]) => void,
    private readonly onClickCategory: (category: string) => void,
    private readonly onClickEmpty: () => void,
  ) {
    this.scale = scale;
    this.categories = categories;
  }

  /**
   * Update scale and categories on re-render (e.g., after filtering changes domain).
   */
  updateScaleAndCategories(
    scale: d3.ScaleBand<string>,
    categories: readonly string[],
  ): void {
    // Only update if categories actually changed (prevents infinite render loops)
    if (
      this.categories.length === categories.length &&
      this.categories.every((cat, i) => cat === categories[i])
    ) {
      return;
    }

    this.scale = scale;
    this.categories = categories;
  }

  getEventHandlers() {
    // Cache handler object - if recreated every render, Mithril detaches/reattaches
    // handlers which breaks in-progress pointer interactions.
    if (!this.cachedEventHandlers) {
      this.cachedEventHandlers = {
        onpointerdown: (e: PointerEvent) => this.handlePointerDown(e),
        onpointermove: (e: PointerEvent) => this.handlePointerMove(e),
        onpointerup: (e: PointerEvent) => this.handlePointerUp(e),
        onpointercancel: (e: PointerEvent) => this.handlePointerCancel(e),
      };
    }
    return this.cachedEventHandlers;
  }

  private handlePointerDown(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.setPointerCapture(e.pointerId);

    this.startCategory = this.clientXToCategory(e);
    this.currentCategory = this.startCategory;
  }

  private handlePointerMove(e: PointerEvent): void {
    if (this.startCategory === undefined) return;

    const category = this.clientXToCategory(e);
    // Keep tracking even when cursor moves over gaps between bars.
    // Only update currentCategory if we found a valid category.
    if (category !== undefined) {
      this.currentCategory = category;
    }
  }

  private handlePointerUp(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.releasePointerCapture(e.pointerId);

    if (
      this.startCategory === undefined ||
      this.currentCategory === undefined
    ) {
      // Click on empty space (outside all category bands)
      this.onClickEmpty();
      this.reset();
      return;
    }

    const isDrag = this.startCategory !== this.currentCategory;

    if (isDrag) {
      // Multi-category brush selection (dragged across categories)
      const selectedCategories = this.getCategoriesInRange(
        this.startCategory,
        this.currentCategory,
      );
      this.onBrushEnd(selectedCategories);
    } else {
      // Single click within a category band
      // onClickCategory handles the logic for clear vs select
      this.onClickCategory(this.startCategory);
    }

    this.reset();
  }

  private handlePointerCancel(e: PointerEvent): void {
    const target = e.currentTarget as Element;
    target.releasePointerCapture(e.pointerId);
    this.reset();
  }

  getCurrentBrush(): readonly string[] | undefined {
    if (
      this.startCategory === undefined ||
      this.currentCategory === undefined
    ) {
      return undefined;
    }

    return this.getCategoriesInRange(this.startCategory, this.currentCategory);
  }

  private clientXToCategory(e: PointerEvent): string | undefined {
    // Get fresh SVG reference from event target to avoid stale reference after re-render
    const svg = (e.currentTarget as SVGElement).closest('svg') || this.svg;

    // Use SVG coordinate transformation to properly handle all transforms
    const point = svg.createSVGPoint();
    point.x = e.clientX;
    point.y = e.clientY;

    const ctm = svg.getScreenCTM();
    if (!ctm) return undefined;

    const svgPoint = point.matrixTransform(ctm.inverse());
    const chartX = svgPoint.x - this.margin.left;

    // Check if in left/right margins
    const viewBox = svg.viewBox.baseVal;
    const chartWidth = viewBox.width - this.margin.left - this.margin.right;
    if (chartX < 0 || chartX > chartWidth) {
      return undefined;
    }

    // Find which band the click is in horizontally (Y doesn't matter)
    for (const category of this.categories) {
      const bandStart = this.scale(category) ?? 0;
      const bandEnd = bandStart + this.scale.bandwidth();
      if (chartX >= bandStart && chartX < bandEnd) {
        return category;
      }
    }

    // Click is between bars (horizontal gaps)
    return undefined;
  }

  private getCategoriesInRange(start: string, end: string): readonly string[] {
    const startIdx = this.categories.indexOf(start);
    const endIdx = this.categories.indexOf(end);

    if (startIdx === -1 || endIdx === -1) return [];

    const minIdx = Math.min(startIdx, endIdx);
    const maxIdx = Math.max(startIdx, endIdx);

    return this.categories.slice(minIdx, maxIdx + 1);
  }

  private reset(): void {
    this.startCategory = undefined;
    this.currentCategory = undefined;
  }
}
