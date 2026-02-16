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
import {formatNumber} from './chart_utils';
import {
  type ThemeColors,
  EChartView,
  EChartEventHandler,
  EChartClickParams,
} from './echart_view';
import {buildTooltipOption} from './chart_option_builder';

export interface SankeyNode {
  readonly name: string;
  readonly color?: string;
  readonly depth?: number;
}

export interface SankeyLink {
  readonly source: string;
  readonly target: string;
  readonly value: number;
}

export interface SankeyData {
  readonly nodes: readonly SankeyNode[];
  readonly links: readonly SankeyLink[];
}

export interface SankeyChartAttrs {
  readonly data: SankeyData | undefined;
  readonly height?: number;
  readonly fillParent?: boolean;
  readonly className?: string;
  readonly formatValue?: (value: number) => string;
  readonly onNodeClick?: (node: SankeyNode) => void;
}

export class Sankey implements m.ClassComponent<SankeyChartAttrs> {
  view({attrs}: m.Vnode<SankeyChartAttrs>) {
    const {data, height, fillParent, className} = attrs;

    const isEmpty =
      data !== undefined &&
      (data.nodes.length === 0 || data.links.length === 0);
    const option =
      data !== undefined && !isEmpty
        ? buildSankeyOption(attrs, data)
        : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildSankeyEventHandlers(attrs, data),
      resolveOption: applySankeyTheme,
    });
  }
}

function buildSankeyOption(
  attrs: SankeyChartAttrs,
  data: SankeyData,
): EChartsCoreOption {
  const {formatValue = (v: number) => formatNumber(v)} = attrs;

  // Compute total outflow per source node for percentage in tooltips.
  const outflow = new Map<string, number>();
  for (const link of data.links) {
    outflow.set(link.source, (outflow.get(link.source) ?? 0) + link.value);
  }

  return {
    animation: false,
    tooltip: buildTooltipOption({
      trigger: 'item' as const,
      formatter: (params: {
        dataType?: string;
        name?: string;
        value?: number;
        data?: {source?: string; target?: string; value?: number};
      }) => {
        if (params.dataType === 'edge') {
          const d = params.data;
          if (d === undefined) return '';
          const total = outflow.get(d.source ?? '') ?? 0;
          const pct =
            total > 0 ? (((d.value ?? 0) / total) * 100).toFixed(1) : '0';
          return [
            `${d.source} → ${d.target}`,
            `${formatValue(d.value ?? 0)}`,
            `${pct}% of ${d.source}`,
          ].join('<br>');
        }
        // Node tooltip.
        const name = params.name ?? '';
        const value = params.value ?? 0;
        return `${name}<br>${formatValue(value)}`;
      },
    }),
    series: [
      {
        type: 'sankey',
        orient: 'horizontal',
        data: data.nodes.map((n) => ({
          name: n.name,
          ...(n.depth !== undefined ? {depth: n.depth} : {}),
        })),
        links: data.links.map((l) => ({
          source: l.source,
          target: l.target,
          value: l.value,
        })),
        label: {
          show: true,
          fontSize: 11,
        },
        lineStyle: {
          color: 'gradient',
          opacity: 0.4,
        },
        emphasis: {
          focus: 'adjacency',
        },
        draggable: false,
        nodeWidth: 24,
        nodeGap: 16,
        layoutIterations: 0,
        left: 20,
        right: 160,
        top: 10,
        bottom: 10,
      },
    ],
  };
}

interface SankeySeries {
  type?: string;
  label?: {color?: unknown};
}

function applySankeyTheme(
  option: EChartsCoreOption,
  colors: ThemeColors,
): EChartsCoreOption {
  const series = (option as {series?: unknown[]}).series;
  if (!Array.isArray(series) || series.length === 0) return option;

  const sankey = series[0] as SankeySeries;
  if (sankey.type !== 'sankey') return option;

  if (sankey.label) {
    sankey.label.color = colors.textColor;
  }

  return option;
}

function buildSankeyEventHandlers(
  attrs: SankeyChartAttrs,
  data: SankeyData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onNodeClick || data === undefined) return [];

  const nodeMap = new Map<string, SankeyNode>();
  for (const node of data.nodes) {
    nodeMap.set(node.name, node);
  }

  const onNodeClick = attrs.onNodeClick;

  return [
    {
      eventName: 'click',
      handler: (params) => {
        const p = params as EChartClickParams;
        if (p.name !== undefined) {
          const node = nodeMap.get(p.name);
          if (node !== undefined) {
            onNodeClick(node);
          }
        }
      },
    },
  ];
}
