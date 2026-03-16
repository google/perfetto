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
import type {EChartsCoreOption} from 'echarts/core';
import {type ThemeColors, EChartView} from './echart_view';

/** Data returned by the stat card: a single aggregated value. */
export interface StatCardData {
  readonly value: number;
}

export interface StatCardAttrs {
  /** The aggregated data to display, or undefined if loading. */
  readonly data: StatCardData | undefined;
  /** Whether the data is still being fetched. */
  readonly isPending: boolean;
  /** Label to display below the value. */
  readonly label: string;
  /** Height of the chart in pixels. Defaults to 120 (use ~300 with gauge). */
  readonly height?: number;
  /** Fill parent container. Defaults to false. */
  readonly fillParent?: boolean;
  /** Minimum value of the gauge scale. Defaults to 0. */
  readonly min?: number;
  /** Maximum value of the gauge scale. Defaults to 100. */
  readonly max?: number;
  /**
   * Show the gauge dial (arc, progress, and pointer). Defaults to false.
   * When false, only the value and label are displayed.
   */
  readonly showGauge?: boolean;
  /**
   * Gauge diameter as a CSS-style percentage string (e.g. '80%').
   * Controls the radius of the gauge arc relative to the container.
   * Defaults to '75%'. Only used when showGauge is true.
   */
  readonly diameter?: string;
  /** Custom formatter for the displayed value. */
  readonly formatValue?: (value: number) => string;
}

/**
 * A stat card widget that displays a single aggregated value with a label,
 * optionally rendered as an ECharts gauge with arc, progress, and pointer.
 */
export class StatCard implements m.ClassComponent<StatCardAttrs> {
  view({attrs}: m.CVnode<StatCardAttrs>) {
    const {data, isPending} = attrs;
    const height = attrs.height ?? 120;
    const fillParent = attrs.fillParent ?? false;

    const option = buildGaugeOption(attrs, data);
    return m(EChartView, {
      option: isPending && data === undefined ? undefined : option,
      resolveOption: applyGaugeTheme,
      height,
      fillParent,
      empty: !isPending && data === undefined,
    });
  }
}

/**
 * Applies theme-aware colors to gauge axis line track.
 * The ECharts theme system does not cover gauge-specific properties like
 * axisLine.lineStyle.color, so we resolve them from the Perfetto theme.
 */
interface GaugeSeries {
  type?: string;
  axisLine?: {lineStyle?: {color?: unknown}};
  splitLine?: {lineStyle?: {color?: unknown}};
  axisLabel?: {color?: unknown};
  anchor?: {itemStyle?: {borderColor?: unknown}};
  title?: {color?: unknown};
  detail?: {color?: unknown};
}

function applyGaugeTheme(
  option: EChartsCoreOption,
  colors: ThemeColors,
): EChartsCoreOption {
  const series = (option as {series?: unknown[]}).series;
  if (!Array.isArray(series) || series.length === 0) return option;

  // Mutate in-place: the option is freshly built by buildGaugeOption() on
  // every render, so there is no shared-reference risk. structuredClone is
  // not viable here because the option contains function values (formatter).
  const gauge = series[0] as GaugeSeries;
  if (gauge.type !== 'gauge') return option;

  if (gauge.axisLine?.lineStyle) {
    gauge.axisLine.lineStyle.color = [[1, colors.borderColor]];
  }
  if (gauge.splitLine?.lineStyle) {
    gauge.splitLine.lineStyle.color = colors.textColor;
  }
  if (gauge.axisLabel) {
    gauge.axisLabel.color = colors.textColor;
  }
  if (gauge.anchor?.itemStyle) {
    gauge.anchor.itemStyle.borderColor = colors.accentColor;
  }
  if (gauge.title) {
    gauge.title.color = colors.textColor;
  }
  if (gauge.detail) {
    gauge.detail.color = colors.textColor;
  }
  return option;
}

function buildGaugeOption(
  attrs: StatCardAttrs,
  data: StatCardData | undefined,
): EChartsCoreOption {
  const value = data?.value ?? 0;
  const min = attrs.min ?? 0;
  const max = attrs.max ?? 100;
  const formatter = attrs.formatValue ?? formatStatValue;
  const formatted = formatter(value);
  const showGauge = attrs.showGauge ?? false;

  if (!showGauge) {
    // Simple "big number" display — no arc, no pointer.
    return {
      series: [
        {
          type: 'gauge',
          startAngle: 0,
          endAngle: 0,
          min,
          max,
          axisLine: {show: false},
          axisTick: {show: false},
          splitLine: {show: false},
          axisLabel: {show: false},
          pointer: {show: false},
          detail: {
            show: true,
            offsetCenter: [0, '-15%'],
            formatter: () => formatted,
            fontWeight: 700,
          },
          title: {
            show: true,
            offsetCenter: [0, '20%'],
          },
          data: [{value, name: attrs.label}],
        },
      ],
    };
  }

  const diameter = attrs.diameter ?? '75%';

  return {
    series: [
      {
        type: 'gauge',
        radius: diameter,
        // Shifted up to visually center the arc + value/label text below.
        // The 270° arc has a gap at the bottom, while value (70%) and
        // label (90%) text extend below the center point.
        center: ['50%', '45%'],
        min,
        max,
        progress: {
          show: true,
          width: 18,
        },
        axisLine: {
          lineStyle: {
            width: 18,
            // Placeholder — replaced by applyGaugeTheme with the actual
            // theme border color.
            color: [[1, '#e6ebf8']],
          },
        },
        axisTick: {show: false},
        splitLine: {
          length: 15,
          lineStyle: {
            width: 2,
            // Placeholder — replaced by applyGaugeTheme.
            color: '#999',
          },
        },
        axisLabel: {
          distance: 25,
          // Placeholder — replaced by applyGaugeTheme.
          color: '#999',
          fontSize: 14,
        },
        anchor: {
          show: true,
          showAbove: true,
          size: 25,
          itemStyle: {
            borderWidth: 10,
            borderColor: '#999',
          },
        },
        pointer: {show: true},
        title: {
          show: true,
          offsetCenter: [0, '90%'],
        },
        detail: {
          valueAnimation: true,
          fontSize: 40,
          offsetCenter: [0, '70%'],
          formatter: () => formatted,
          fontWeight: 700,
        },
        data: [{value, name: attrs.label}],
      },
    ],
  };
}

function formatStatValue(value: number): string {
  if (Number.isInteger(value)) return value.toLocaleString();
  return value.toLocaleString(undefined, {
    minimumFractionDigits: 0,
    maximumFractionDigits: 2,
  });
}
