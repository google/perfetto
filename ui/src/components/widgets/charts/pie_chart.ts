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
import {Spinner} from '../../../widgets/spinner';
import {CHART_COLORS, formatNumber, truncateLabel} from './chart_utils';

/**
 * A single slice in the pie chart.
 */
export interface PieChartSlice {
  /** Label for this slice */
  readonly label: string;
  /** Numeric value for this slice */
  readonly value: number;
  /** Optional custom color for this slice */
  readonly color?: string;
}

/**
 * Data provided to a PieChart.
 */
export interface PieChartData {
  /** The slices to display */
  readonly slices: readonly PieChartSlice[];
}

export interface PieChartAttrs {
  /**
   * Pie chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: PieChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for values in tooltips.
   */
  readonly formatValue?: (value: number) => string;

  /**
   * Show legend. Defaults to true.
   */
  readonly showLegend?: boolean;

  /**
   * Show percentage labels on slices. Defaults to false.
   */
  readonly showLabels?: boolean;

  /**
   * Inner radius ratio for donut chart (0-1). 0 = pie, >0 = donut.
   * Defaults to 0 (pie chart).
   */
  readonly innerRadiusRatio?: number;

  /**
   * Callback when a slice is clicked.
   */
  readonly onSliceClick?: (slice: PieChartSlice) => void;
}

const DEFAULT_HEIGHT = 200;
const VIEWBOX_SIZE = 200;
const CENTER = VIEWBOX_SIZE / 2;
const LEGEND_WIDTH = 120;

export class PieChart implements m.ClassComponent<PieChartAttrs> {
  private hoveredSlice?: PieChartSlice;

  view({attrs}: m.Vnode<PieChartAttrs>) {
    const {
      data,
      height = DEFAULT_HEIGHT,
      fillParent,
      className,
      formatValue = (v) => formatNumber(v),
      showLegend = true,
      showLabels = false,
      innerRadiusRatio = 0,
      onSliceClick,
    } = attrs;

    if (data === undefined) {
      return m(
        '.pf-pie-chart',
        {
          className: classNames(
            fillParent && 'pf-pie-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-pie-chart__loading', m(Spinner)),
      );
    }

    const validSlices = data.slices.filter((s) => s.value > 0);
    if (validSlices.length === 0) {
      return m(
        '.pf-pie-chart',
        {
          className: classNames(
            fillParent && 'pf-pie-chart--fill-parent',
            className,
          ),
          style: {height: `${height}px`},
        },
        m('.pf-pie-chart__empty', 'No data to display'),
      );
    }

    const total = validSlices.reduce((sum, s) => sum + s.value, 0);
    const outerRadius = Math.min(height, VIEWBOX_SIZE) / 2 - 10;
    const innerRadius = outerRadius * innerRadiusRatio;

    // Calculate viewBox width based on whether legend is shown
    const viewBoxWidth = showLegend
      ? VIEWBOX_SIZE + LEGEND_WIDTH
      : VIEWBOX_SIZE;

    // Build slice paths
    let currentAngle = -Math.PI / 2; // Start at top
    const sliceElements = validSlices.map((slice, idx) => {
      const sliceAngle = (slice.value / total) * 2 * Math.PI;
      const startAngle = currentAngle;
      const endAngle = currentAngle + sliceAngle;
      currentAngle = endAngle;

      const color = slice.color ?? CHART_COLORS[idx % CHART_COLORS.length];
      const isHovered = this.hoveredSlice === slice;

      // Calculate arc path
      const path = describeArc(
        CENTER,
        CENTER,
        isHovered ? outerRadius + 5 : outerRadius,
        innerRadius,
        startAngle,
        endAngle,
      );

      // Calculate label position (middle of slice)
      const labelAngle = startAngle + sliceAngle / 2;
      const labelRadius = (outerRadius + innerRadius) / 2;
      const labelX = CENTER + Math.cos(labelAngle) * labelRadius;
      const labelY = CENTER + Math.sin(labelAngle) * labelRadius;
      const percentage = ((slice.value / total) * 100).toFixed(1);

      return m('g.pf-pie-chart__slice-group', [
        m('path.pf-pie-chart__slice', {
          'd': path,
          'fill': color,
          'stroke': 'var(--pf-color-background)',
          'stroke-width': 2,
          'className': classNames(isHovered && 'pf-pie-chart__slice--hover'),
          'onmouseenter': () => {
            this.hoveredSlice = slice;
          },
          'onmouseleave': () => {
            this.hoveredSlice = undefined;
          },
          'onclick': onSliceClick
            ? () => {
                onSliceClick(slice);
              }
            : undefined,
          'style': {
            cursor: onSliceClick ? 'pointer' : 'default',
          },
        }),
        showLabels &&
          sliceAngle > 0.3 && // Only show label if slice is large enough
          m(
            'text.pf-pie-chart__slice-label',
            {
              'x': labelX,
              'y': labelY,
              'text-anchor': 'middle',
              'dominant-baseline': 'middle',
              'pointer-events': 'none',
            },
            `${percentage}%`,
          ),
      ]);
    });

    // Build legend
    const legendElements = showLegend
      ? validSlices.map((slice, idx) => {
          const color = slice.color ?? CHART_COLORS[idx % CHART_COLORS.length];
          const percentage = ((slice.value / total) * 100).toFixed(1);
          const yOffset = idx * 18;
          const isHovered = this.hoveredSlice === slice;
          return m(
            'g.pf-pie-chart__legend-item',
            {
              transform: `translate(${VIEWBOX_SIZE + 10}, ${20 + yOffset})`,
              onmouseenter: () => {
                this.hoveredSlice = slice;
              },
              onmouseleave: () => {
                this.hoveredSlice = undefined;
              },
            },
            [
              m('rect', {
                x: 0,
                y: -6,
                width: 12,
                height: 12,
                fill: color,
                rx: 2,
              }),
              m(
                'text.pf-pie-chart__legend-label',
                {
                  'x': 18,
                  'y': 0,
                  'dominant-baseline': 'middle',
                  'className': classNames(
                    isHovered && 'pf-pie-chart__legend-label--hover',
                  ),
                },
                `${truncateLabel(slice.label, 12)} `,
                m('tspan.pf-pie-chart__legend-value', `(${percentage}%)`),
              ),
            ],
          );
        })
      : null;

    const style: Record<string, string> = {height: `${height}px`};

    return m(
      '.pf-pie-chart',
      {
        className: classNames(
          fillParent && 'pf-pie-chart--fill-parent',
          className,
        ),
        style,
      },
      [
        m(
          'svg.pf-pie-chart__svg',
          {
            viewBox: `0 0 ${viewBoxWidth} ${VIEWBOX_SIZE}`,
            preserveAspectRatio: 'xMidYMid meet',
          },
          [
            // Slices
            m('g.pf-pie-chart__slices', sliceElements),
            // Legend
            legendElements,
          ],
        ),
        // Tooltip
        this.hoveredSlice &&
          m(
            '.pf-pie-chart__tooltip',
            m('.pf-pie-chart__tooltip-content', [
              m('.pf-pie-chart__tooltip-row', this.hoveredSlice.label),
              m(
                '.pf-pie-chart__tooltip-row',
                `Value: ${formatValue(this.hoveredSlice.value)}`,
              ),
              m(
                '.pf-pie-chart__tooltip-row',
                `${((this.hoveredSlice.value / total) * 100).toFixed(1)}%`,
              ),
            ]),
          ),
      ],
    );
  }
}

/**
 * Generate an SVG arc path for a slice.
 */
function describeArc(
  cx: number,
  cy: number,
  outerRadius: number,
  innerRadius: number,
  startAngle: number,
  endAngle: number,
): string {
  const startOuter = polarToCartesian(cx, cy, outerRadius, startAngle);
  const endOuter = polarToCartesian(cx, cy, outerRadius, endAngle);
  const startInner = polarToCartesian(cx, cy, innerRadius, startAngle);
  const endInner = polarToCartesian(cx, cy, innerRadius, endAngle);

  const largeArcFlag = endAngle - startAngle > Math.PI ? 1 : 0;

  if (innerRadius === 0) {
    // Pie slice (no hole)
    return [
      `M ${cx} ${cy}`,
      `L ${startOuter.x} ${startOuter.y}`,
      `A ${outerRadius} ${outerRadius} 0 ${largeArcFlag} 1 ${endOuter.x} ${endOuter.y}`,
      'Z',
    ].join(' ');
  }

  // Donut slice (with hole)
  return [
    `M ${startOuter.x} ${startOuter.y}`,
    `A ${outerRadius} ${outerRadius} 0 ${largeArcFlag} 1 ${endOuter.x} ${endOuter.y}`,
    `L ${endInner.x} ${endInner.y}`,
    `A ${innerRadius} ${innerRadius} 0 ${largeArcFlag} 0 ${startInner.x} ${startInner.y}`,
    'Z',
  ].join(' ');
}

function polarToCartesian(
  cx: number,
  cy: number,
  radius: number,
  angle: number,
): {x: number; y: number} {
  return {
    x: cx + radius * Math.cos(angle),
    y: cy + radius * Math.sin(angle),
  };
}
