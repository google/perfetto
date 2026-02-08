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
 * Reusable legend rendering for multi-series charts.
 *
 * Pure function that generates Mithril vnodes for chart legends,
 * used by bar charts (grouped/stacked) and CDF charts with multiple lines.
 */

import m from 'mithril';

export interface LegendItem {
  readonly name: string;
  readonly color: string;
}

export interface LegendConfig {
  readonly items: readonly LegendItem[];
  readonly position?: 'top-right' | 'top-left' | 'bottom-right' | 'bottom-left';
  readonly itemHeight?: number;
  readonly swatchSize?: number;
  readonly className?: string;
  readonly chartWidth?: number; // Required for right-aligned legends
}

/**
 * Renders a legend for multi-series charts.
 *
 * @param config Legend configuration
 * @returns Mithril vnode for the legend group
 *
 * @example
 * ```typescript
 * renderLegend({
 *   items: [
 *     {name: 'Series A', color: 'steelblue'},
 *     {name: 'Series B', color: 'orange'},
 *   ],
 *   position: 'top-right',
 * })
 * ```
 */
export function renderLegend(config: LegendConfig): m.Children {
  const {
    items,
    position = 'top-right',
    itemHeight = 20,
    swatchSize = 12,
    className,
    chartWidth,
  } = config;

  if (items.length === 0) return null;

  // Calculate position transform and text anchor
  let transform = '';
  let textAnchor: 'start' | 'end' = 'start';
  let swatchX = 0;
  let textX = swatchSize + 6;

  const isRight = position.includes('right');
  const isBottom = position.includes('bottom');

  if (isRight) {
    textAnchor = 'end';
    // Position legend at right edge, items aligned right
    const x =
      chartWidth !== undefined && chartWidth !== null ? chartWidth - 10 : -10;
    const y = isBottom ? -10 : 10;
    transform = `translate(${x}, ${y})`;
    // For right-aligned, text comes first (negative x), then swatch
    textX = -(swatchSize + 6);
    swatchX = -swatchSize;
  } else {
    textAnchor = 'start';
    const y = isBottom ? -10 : 10;
    transform = `translate(10, ${y})`;
  }

  const children: m.ChildArray = items.map((item, i) => {
    return m(
      'g.pf-legend__item',
      {
        key: item.name,
        transform: `translate(0, ${i * itemHeight})`,
      },
      [
        // Color swatch
        m('rect.pf-legend__swatch', {
          x: swatchX,
          y: 0,
          width: swatchSize,
          height: swatchSize,
          fill: item.color,
        }),
        // Label
        m(
          'text.pf-legend__label',
          {
            'x': textX,
            'y': swatchSize / 2,
            'dominant-baseline': 'middle',
            'text-anchor': textAnchor,
          },
          item.name,
        ),
      ],
    );
  });

  return m(
    'g.pf-legend',
    {
      class: className,
      transform,
    },
    children,
  );
}
