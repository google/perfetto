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
import {EChartView, EChartEventHandler, EChartClickParams} from './echart_view';
import {buildTooltipOption} from './chart_option_builder';

/**
 * A node in the treemap hierarchy.
 */
export interface TreemapNode {
  /** Display name for this node */
  readonly name: string;
  /** Size value (determines rectangle area) */
  readonly value: number;
  /** Optional children for hierarchical treemaps */
  readonly children?: readonly TreemapNode[];
}

export function countTreemapLeaves(
  nodes: readonly TreemapNode[] | undefined,
): number {
  if (nodes === undefined) return 0;
  let count = 0;
  for (const node of nodes) {
    if (node.children !== undefined && node.children.length > 0) {
      count += countTreemapLeaves(node.children);
    } else {
      count += 1;
    }
  }
  return count;
}

/**
 * Data provided to a TreemapChart.
 */
export interface TreemapData {
  /** Top-level nodes (can have nested children) */
  readonly nodes: readonly TreemapNode[];
}

export interface TreemapChartAttrs {
  /**
   * Treemap data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: TreemapData | undefined;

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
   * Callback when a node is clicked.
   */
  readonly onNodeClick?: (node: TreemapNode) => void;

  /**
   * Minimum visible rectangle size. Nodes smaller than this are hidden.
   * Defaults to 10.
   */
  readonly visibleMin?: number;

  /**
   * Show labels on rectangles. Defaults to true.
   */
  readonly showLabels?: boolean;

  /**
   * Enable drill-down on click. Defaults to false.
   * When true, clicking a parent node zooms into it.
   */
  readonly enableDrillDown?: boolean;
}

export class Treemap implements m.ClassComponent<TreemapChartAttrs> {
  view({attrs}: m.Vnode<TreemapChartAttrs>) {
    const {data, height, fillParent, className} = attrs;

    const isEmpty = data !== undefined && data.nodes.length === 0;
    const option =
      data !== undefined && !isEmpty
        ? buildTreemapOption(attrs, data)
        : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildTreemapEventHandlers(attrs, data),
    });
  }
}

function buildTreemapOption(
  attrs: TreemapChartAttrs,
  data: TreemapData,
): EChartsCoreOption {
  const {
    formatValue = (v: number) => formatNumber(v),
    visibleMin = 10,
    showLabels = true,
    enableDrillDown = false,
  } = attrs;

  const total = data.nodes.reduce((sum, n) => sum + computeTotal(n), 0);

  return {
    animation: false,
    tooltip: buildTooltipOption({
      trigger: 'item' as const,
      formatter: (params: {name?: string; value?: number}) => {
        const name = params.name ?? '';
        const value = params.value ?? 0;
        const pct = total > 0 ? ((value / total) * 100).toFixed(1) : '0';
        return [name, `Value: ${formatValue(value)}`, `${pct}%`].join('<br>');
      },
    }),
    series: [
      {
        type: 'treemap',
        data: convertNodes(data.nodes),
        roam: enableDrillDown ? 'move' : false,
        nodeClick: enableDrillDown ? 'zoomToNode' : false,
        visibleMin,
        label: {
          show: showLabels,
          formatter: '{b}',
          fontSize: 11,
        },
        itemStyle: {
          borderWidth: 2,
          gapWidth: 2,
        },
        breadcrumb: enableDrillDown
          ? {
              show: true,
            }
          : {show: false},
        levels: [
          {
            // Level 0: parent groups
            itemStyle: {
              borderWidth: 3,
              gapWidth: 3,
            },
            upperLabel: {
              show: true,
              height: 20,
              fontSize: 12,
              fontWeight: 'bold' as const,
            },
          },
          {
            // Level 1: children
            colorSaturation: [0.35, 0.65],
            itemStyle: {
              borderWidth: 1,
              gapWidth: 1,
            },
          },
          {
            // Level 2+: deeper children (if any)
            colorSaturation: [0.25, 0.55],
            itemStyle: {
              borderWidth: 1,
              gapWidth: 1,
            },
          },
        ],
      },
    ],
  };
}

/**
 * Convert TreemapNode tree to ECharts data format.
 */
function convertNodes(
  nodes: readonly TreemapNode[],
): Array<Record<string, unknown>> {
  return nodes.map((node) => {
    const children = node.children;
    const hasChildren = children !== undefined && children.length > 0;
    // For nodes with children, use computed value from children if value is 0
    const value =
      hasChildren && node.value === 0 ? computeTotal(node) : node.value;
    const result: Record<string, unknown> = {
      name: node.name,
      value,
    };
    if (hasChildren) {
      result.children = convertNodes(children);
    }
    return result;
  });
}

/**
 * Compute total value including children.
 */
function computeTotal(node: TreemapNode): number {
  if (node.children !== undefined && node.children.length > 0) {
    return node.children.reduce((sum, c) => sum + computeTotal(c), 0);
  }
  return node.value;
}

function buildTreemapEventHandlers(
  attrs: TreemapChartAttrs,
  data: TreemapData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onNodeClick || data === undefined) return [];

  // Build name-to-node mapping for click handler
  const nodeMap = new Map<string, TreemapNode>();
  buildNodeMap(data.nodes, nodeMap);

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

/**
 * Recursively build a name-to-node map for click handler lookups.
 * First occurrence wins when names collide across tree levels.
 */
function buildNodeMap(
  nodes: readonly TreemapNode[],
  nodeMap: Map<string, TreemapNode>,
): void {
  for (const node of nodes) {
    if (!nodeMap.has(node.name)) {
      nodeMap.set(node.name, node);
    }
    if (node.children !== undefined) {
      buildNodeMap(node.children, nodeMap);
    }
  }
}
