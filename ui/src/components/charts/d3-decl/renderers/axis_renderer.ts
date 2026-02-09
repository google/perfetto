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
 * Reusable axis rendering functions for declarative charts.
 *
 * These pure functions generate Mithril vnodes for chart axes,
 * eliminating duplication across chart components.
 *
 * Design principles:
 * - Pure functions (no side effects)
 * - Return Mithril vnodes (m.Children)
 * - Use D3 scales for calculations only
 * - Support both linear and categorical axes
 */

import m from 'mithril';
import * as d3 from 'd3';
import {formatNumber, truncateLabelToWidth} from '../chart_utils';

export interface LinearAxisConfig {
  readonly scale:
    | d3.ScaleLinear<number, number>
    | d3.ScaleLogarithmic<number, number>;
  readonly orientation: 'top' | 'bottom' | 'left' | 'right';
  readonly length: number;
  readonly label?: string;
  readonly tickCount?: number;
  readonly tickFormatter?: (value: number) => string;
  readonly className?: string;
}

/**
 * Renders a linear axis with ticks and optional label.
 *
 * @param config Axis configuration
 * @returns Mithril vnode for the axis group
 *
 * @example
 * ```typescript
 * m('g', {transform: `translate(0, ${chartHeight})`},
 *   renderLinearAxis({
 *     scale: xScale,
 *     orientation: 'bottom',
 *     length: chartWidth,
 *     label: 'Duration (ms)',
 *     tickFormatter: formatDuration,
 *   })
 * )
 * ```
 */
export function renderLinearAxis(config: LinearAxisConfig): m.Children {
  const {
    scale,
    orientation,
    length,
    label,
    tickCount = 5,
    tickFormatter = formatNumber,
    className,
  } = config;

  const isVertical = orientation === 'left' || orientation === 'right';
  const tickLength = 6;
  const tickPadding = 3;
  const labelOffset = isVertical ? -40 : 35;

  // Generate tick values using D3's smart algorithm
  const ticks = scale.ticks(tickCount);

  const children: m.ChildArray = [];

  // Axis line
  children.push(
    m('line.pf-axis__line', {
      key: 'axis-line',
      x1: 0,
      y1: 0,
      x2: isVertical ? 0 : length,
      y2: isVertical ? length : 0,
    }),
  );

  // Ticks and labels
  for (const tick of ticks) {
    const pos = scale(tick);
    if (pos === undefined || !isFinite(pos)) continue;

    const tickOffset =
      orientation === 'left' || orientation === 'top'
        ? -tickLength
        : tickLength;
    const textOffset =
      tickOffset + (tickOffset < 0 ? -tickPadding : tickPadding);

    children.push(
      m(
        'g.pf-axis__tick-group',
        {
          key: `tick-${tick}`,
          transform: isVertical
            ? `translate(0, ${pos})`
            : `translate(${pos}, 0)`,
        },
        [
          // Tick mark
          m('line.pf-axis__tick', {
            x1: 0,
            y1: 0,
            x2: isVertical ? tickOffset : 0,
            y2: isVertical ? 0 : tickOffset,
          }),
          // Tick label
          m(
            'text.pf-axis__tick-label',
            {
              'x': isVertical ? textOffset : 0,
              'y': isVertical ? 0 : textOffset,
              'text-anchor': isVertical
                ? orientation === 'left'
                  ? 'end'
                  : 'start'
                : 'middle',
              'dominant-baseline': isVertical ? 'middle' : 'hanging',
            },
            tickFormatter(tick),
          ),
        ],
      ),
    );
  }

  // Axis label
  if (label) {
    children.push(
      m(
        'text.pf-axis__label',
        {
          'key': 'axis-label',
          'transform': isVertical
            ? `translate(${labelOffset}, ${length / 2}) rotate(-90)`
            : `translate(${length / 2}, ${labelOffset})`,
          'text-anchor': 'middle',
          'dominant-baseline': 'auto',
        },
        label,
      ),
    );
  }

  return m(`g.pf-axis.pf-axis--${orientation}`, {class: className}, children);
}

export interface BandAxisConfig {
  readonly scale: d3.ScaleBand<string>;
  readonly orientation: 'top' | 'bottom';
  readonly length: number;
  readonly label?: string;
  readonly tickFormatter?: (value: string) => string;
  readonly maxTicks?: number;
  readonly className?: string;
}

/**
 * Renders a band (categorical) axis with category labels.
 *
 * Automatically handles overcrowding by skipping labels when there are too many categories.
 *
 * @param config Axis configuration
 * @returns Mithril vnode for the axis group
 *
 * @example
 * ```typescript
 * m('g', {transform: `translate(0, ${chartHeight})`},
 *   renderBandAxis({
 *     scale: xScale,
 *     orientation: 'bottom',
 *     length: chartWidth,
 *     label: 'Process Name',
 *     maxTicks: 20,
 *   })
 * )
 * ```
 */
export function renderBandAxis(config: BandAxisConfig): m.Children {
  const {
    scale,
    orientation,
    length,
    label,
    tickFormatter = (v) => v,
    maxTicks = 20,
    className,
  } = config;

  const tickLength = 6;
  const tickPadding = 3;
  const labelOffset = 35;

  const domain = scale.domain();
  const tickStep =
    domain.length > maxTicks ? Math.ceil(domain.length / maxTicks) : 1;
  const tickValues =
    tickStep > 1 ? domain.filter((_, i) => i % tickStep === 0) : domain;

  const children: m.ChildArray = [];

  // Axis line
  children.push(
    m('line.pf-axis__line', {
      key: 'axis-line',
      x1: 0,
      y1: 0,
      x2: length,
      y2: 0,
    }),
  );

  // Ticks and labels
  for (const tick of tickValues) {
    const bandPos = scale(tick);
    if (bandPos === undefined) continue;

    const x = bandPos + scale.bandwidth() / 2;

    const tickOffset = orientation === 'top' ? -tickLength : tickLength;
    const textOffset =
      tickOffset + (tickOffset < 0 ? -tickPadding : tickPadding);

    children.push(
      m(
        'g.pf-axis__tick-group',
        {
          key: `tick-${tick}`,
          transform: `translate(${x}, 0)`,
        },
        [
          // Tick mark
          m('line.pf-axis__tick', {
            x1: 0,
            y1: 0,
            x2: 0,
            y2: tickOffset,
          }),
          // Tick label
          m(
            'text.pf-axis__tick-label',
            {
              'x': 0,
              'y': textOffset,
              'text-anchor': 'middle',
              'dominant-baseline': 'hanging',
            },
            truncateLabelToWidth(tickFormatter(tick), scale.bandwidth()),
          ),
        ],
      ),
    );
  }

  // Axis label
  if (label) {
    children.push(
      m(
        'text.pf-axis__label',
        {
          'key': 'axis-label',
          'transform': `translate(${length / 2}, ${labelOffset})`,
          'text-anchor': 'middle',
          'dominant-baseline': 'auto',
        },
        label,
      ),
    );
  }

  return m(`g.pf-axis.pf-axis--${orientation}`, {class: className}, children);
}

export interface GridLinesConfig {
  readonly scale:
    | d3.ScaleLinear<number, number>
    | d3.ScaleLogarithmic<number, number>;
  readonly orientation: 'horizontal' | 'vertical';
  readonly length: number;
  readonly tickCount?: number;
  readonly className?: string;
}

/**
 * Renders grid lines for better readability.
 *
 * @param config Grid line configuration
 * @returns Mithril vnode for grid lines group
 *
 * @example
 * ```typescript
 * // Horizontal grid lines from Y axis
 * renderGridLines({
 *   scale: yScale,
 *   orientation: 'horizontal',
 *   length: chartWidth,
 *   tickCount: 5,
 * })
 * ```
 */
export function renderGridLines(config: GridLinesConfig): m.Children {
  const {scale, orientation, length, tickCount = 5, className} = config;

  const ticks = scale.ticks(tickCount);
  const isHorizontal = orientation === 'horizontal';

  const children: m.ChildArray = ticks
    .map((tick) => {
      const pos = scale(tick);
      if (pos === undefined || !isFinite(pos)) return null;

      return m('line.pf-grid-line', {
        key: `grid-${tick}`,
        x1: isHorizontal ? 0 : pos,
        y1: isHorizontal ? pos : 0,
        x2: isHorizontal ? length : pos,
        y2: isHorizontal ? pos : length,
      });
    })
    .filter((x): x is m.Vnode => x !== null);

  return m(`g.pf-grid.pf-grid--${orientation}`, {class: className}, children);
}
