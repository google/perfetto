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
 * Shared utilities for declarative chart components.
 *
 * This module provides pure functions for common chart operations like
 * tick generation, value formatting, and coordinate transformations.
 * These are extracted from the legacy BaseRenderer to support the new
 * declarative, stateless chart architecture.
 */

import * as d3 from 'd3';

export interface ChartMargin {
  readonly top: number;
  readonly right: number;
  readonly bottom: number;
  readonly left: number;
}

export interface TickInfo {
  readonly value: number;
  readonly label: string;
}

/**
 * Default margin for charts.
 */
export const DEFAULT_MARGIN: ChartMargin = {
  top: 5,
  right: 0,
  bottom: 45,
  left: 8,
};

/**
 * Standard viewBox width for responsive SVG charts.
 * Height is determined by the height prop.
 */
export const VIEWBOX_WIDTH = 600;

/**
 * Additional width for legend area (when legends are shown).
 */
export const LEGEND_WIDTH = 100;

/**
 * Generate evenly-spaced tick values for a linear scale.
 *
 * @param min Minimum value of the domain
 * @param max Maximum value of the domain
 * @param count Desired number of ticks
 * @returns Array of tick values
 */
export function generateTicks(
  min: number,
  max: number,
  count: number,
): number[] {
  if (min === max) return [min];
  const range = max - min;
  const step = range / (count - 1);
  const ticks: number[] = [];
  for (let i = 0; i < count; i++) {
    ticks.push(min + i * step);
  }
  return ticks;
}

/**
 * Generate tick values for a logarithmic scale (powers of 10).
 *
 * @param max Maximum value of the domain
 * @returns Array of tick values (1, 10, 100, 1000, ...)
 */
export function generateLogTicks(max: number): number[] {
  if (max <= 1) return [1];
  const ticks: number[] = [1];
  let power = 1;
  while (Math.pow(10, power) <= max) {
    ticks.push(Math.pow(10, power));
    power++;
  }
  return ticks;
}

/**
 * Format a number for display in charts.
 *
 * Applies appropriate formatting based on magnitude:
 * - Integers: 1,234
 * - Large numbers: 1.2k, 1.2M
 * - Decimals: 12.34
 * - Small numbers: 0.001
 *
 * @param value Number to format
 * @returns Formatted string
 */
export function formatNumber(value: number): string {
  if (Number.isInteger(value)) {
    return value.toLocaleString();
  }
  // For decimals, show up to 2 decimal places
  if (Math.abs(value) >= 1000) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 0});
  }
  if (Math.abs(value) >= 1) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 2});
  }
  // For very small numbers, use more precision
  return value.toPrecision(3);
}

/**
 * Format a value using a D3 format specifier.
 *
 * @param value Number to format
 * @param format D3 format specifier (e.g., '.2s', '.0f', '.2%')
 * @returns Formatted string
 */
export function formatValueWithSpec(value: number, format: string): string {
  return d3.format(format)(value);
}

/**
 * Convert client coordinates to SVG viewBox coordinates.
 *
 * Handles the transformation from screen space to SVG coordinate space,
 * accounting for viewBox scaling and preserveAspectRatio.
 *
 * @param svg SVG element
 * @param clientX Client X coordinate (from mouse event)
 * @param clientY Client Y coordinate (from mouse event)
 * @returns Point in SVG viewBox coordinates
 */
export function clientToSVGCoords(
  svg: SVGSVGElement,
  clientX: number,
  clientY: number,
): {x: number; y: number} {
  const point = svg.createSVGPoint();
  point.x = clientX;
  point.y = clientY;
  const ctm = svg.getScreenCTM();
  if (!ctm) return {x: 0, y: 0};
  const svgPoint = point.matrixTransform(ctm.inverse());
  return {x: svgPoint.x, y: svgPoint.y};
}

/**
 * Convert a client X coordinate to a data value using a scale.
 *
 * Helper for brushing interactions.
 *
 * @param event Pointer event
 * @param margin Chart margin
 * @param scale D3 scale to invert
 * @param chartWidth Width of the chart area (excluding margins)
 * @returns Data value
 */
export function clientXToValue(
  event: PointerEvent,
  margin: ChartMargin,
  scale: d3.ScaleLinear<number, number>,
  chartWidth: number,
): number {
  const group = event.currentTarget as SVGGElement;
  const svg = group.ownerSVGElement!;
  const coords = clientToSVGCoords(svg, event.clientX, event.clientY);
  const chartX = coords.x - margin.left;
  const ratio = Math.max(0, Math.min(1, chartX / chartWidth));
  const domain = scale.domain();
  return domain[0] + ratio * (domain[1] - domain[0]);
}

/**
 * Convert a client Y coordinate to a data value using a scale.
 *
 * Helper for brushing interactions in scatter plots.
 *
 * @param event Pointer event
 * @param margin Chart margin
 * @param scale D3 scale to invert
 * @param chartHeight Height of the chart area (excluding margins)
 * @returns Data value
 */
export function clientYToValue(
  event: PointerEvent,
  margin: ChartMargin,
  scale: d3.ScaleLinear<number, number>,
  chartHeight: number,
): number {
  const group = event.currentTarget as SVGGElement;
  const svg = group.ownerSVGElement!;
  const coords = clientToSVGCoords(svg, event.clientX, event.clientY);
  const chartY = coords.y - margin.top;
  const ratio = Math.max(0, Math.min(1, chartY / chartHeight));
  const domain = scale.domain();
  // Y scale is inverted (0 at top, max at bottom)
  return domain[1] - ratio * (domain[1] - domain[0]);
}

/**
 * Calculate correlation statistics for scatter plot.
 *
 * @param xValues Array of X values
 * @param yValues Array of Y values
 * @returns Correlation coefficient and regression line parameters
 */
export function calculateCorrelation(
  xValues: number[],
  yValues: number[],
): {r: number; slope: number; intercept: number} | undefined {
  if (xValues.length !== yValues.length || xValues.length < 2) {
    return undefined;
  }

  const n = xValues.length;
  const sumX = xValues.reduce((a, b) => a + b, 0);
  const sumY = yValues.reduce((a, b) => a + b, 0);
  const sumXY = xValues.reduce((sum, x, i) => sum + x * yValues[i], 0);
  const sumX2 = xValues.reduce((sum, x) => sum + x * x, 0);
  const sumY2 = yValues.reduce((sum, y) => sum + y * y, 0);

  const meanX = sumX / n;
  const meanY = sumY / n;

  const numerator = n * sumXY - sumX * sumY;
  const denominator = Math.sqrt(
    (n * sumX2 - sumX * sumX) * (n * sumY2 - sumY * sumY),
  );

  if (denominator === 0) return undefined;

  const r = numerator / denominator;
  const slope = (n * sumXY - sumX * sumY) / (n * sumX2 - sumX * sumX);
  const intercept = meanY - slope * meanX;

  return {r, slope, intercept};
}

/**
 * Clamp a value between min and max.
 */
export function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}

/**
 * Check if a value is a valid number (not NaN, not Infinity).
 */
export function isValidNumber(value: unknown): value is number {
  return typeof value === 'number' && !isNaN(value) && isFinite(value);
}

/**
 * Safe conversion to number, returning undefined for invalid values.
 */
export function toNumber(value: unknown): number | undefined {
  if (typeof value === 'number') {
    return isValidNumber(value) ? value : undefined;
  }
  if (typeof value === 'string') {
    const num = Number(value);
    return isValidNumber(num) ? num : undefined;
  }
  return undefined;
}

/**
 * Format a duration in nanoseconds to human-readable string.
 *
 * @param ns Duration in nanoseconds
 * @returns Formatted string (e.g., "1.2ms", "3.4s")
 */
export function formatDuration(ns: number): string {
  if (ns < 1000) return `${ns.toFixed(0)}ns`;
  if (ns < 1000000) return `${(ns / 1000).toFixed(1)}μs`;
  if (ns < 1000000000) return `${(ns / 1000000).toFixed(1)}ms`;
  return `${(ns / 1000000000).toFixed(2)}s`;
}

/**
 * Calculate nice domain bounds for a scale.
 *
 * Extends the domain to include round numbers for better-looking axes.
 *
 * @param min Minimum value in data
 * @param max Maximum value in data
 * @returns Nice domain bounds
 */
export function niceDomain(min: number, max: number): [number, number] {
  if (min === max) {
    // Handle degenerate case
    if (min === 0) return [0, 1];
    const padding = Math.abs(min) * 0.1;
    return [min - padding, min + padding];
  }

  const scale = d3.scaleLinear().domain([min, max]).nice();
  return scale.domain() as [number, number];
}

/**
 * Generate a color scale for categorical data.
 *
 * @param categories Array of category names
 * @param scheme D3 color scheme (default: schemeCategory10)
 * @returns Function mapping category to color
 */
export function createCategoricalColorScale(
  categories: string[],
  scheme?: readonly string[],
): (category: string) => string {
  const scale = d3
    .scaleOrdinal<string>()
    .domain(categories)
    .range(scheme ?? d3.schemeCategory10);
  return (category: string) => scale(category);
}

/**
 * Truncate label to fit character limit.
 */
export function truncateLabel(label: string, maxChars: number): string {
  if (label.length <= maxChars) return label;
  return label.substring(0, maxChars - 1) + '\u2026';
}

/**
 * Truncate label to fit pixel width (~6px/char).
 */
export function truncateLabelToWidth(label: string, maxWidth: number): string {
  return truncateLabel(label, Math.max(3, Math.floor(maxWidth / 6)));
}
