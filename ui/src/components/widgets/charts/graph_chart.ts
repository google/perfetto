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

import type {EChartsCoreOption} from 'echarts/core';
import m from 'mithril';
import {classNames} from '../../../base/classnames';
import {
  bindEventListener,
  currentTargetOffset,
  elementIsEditable,
  toHTMLElement,
} from '../../../base/dom_utils';
import type {ChartThemeColors} from './chart_theme';
import './common';
import {
  type EChartApi,
  type EChartClickParams,
  EChartView,
} from './echart_view';

export const DEFAULT_GRAPH_CANVAS_W = 335;
export const DEFAULT_GRAPH_LEFT_MARGIN_X = 230;
export const DEFAULT_GRAPH_ROW_LABEL_X = 215;
export const ROAM_MIN_ZOOM = 0.5;
export const ROAM_MAX_ZOOM = 15;

const NODE_SCALE_SLOPE = 0.25;
const MAX_NODE_GROWTH = 0.5; // Caps screen-space node & label growth at 1.5x.
const KEYBOARD_PAN_STEP_PX = 48;
const KEYBOARD_ZOOM_FACTOR = 1.18;

export type GraphChartLayout = 'layered' | 'force' | 'circular' | 'none';
export type GraphNodeHighlight = 'added' | 'removed' | 'changed' | 'ghost';
export type GraphEdgeHighlight = 'added' | 'removed';

export interface GraphTooltipRow {
  readonly label?: string;
  readonly value: string;
  readonly bold?: boolean;
}

export type GraphTooltip = string | ReadonlyArray<string | GraphTooltipRow>;

export interface GraphLayer {
  readonly id: string | number;
  readonly label: string;
  readonly y?: number;
  readonly color?: string;
  readonly lightTextColor?: string;
  readonly dimmed?: boolean;
}

export interface GraphBounds {
  readonly minX: number;
  readonly maxX: number;
  readonly minY: number;
  readonly maxY: number;
}

export interface GraphNode {
  readonly id: string;
  readonly label: string;
  readonly layer?: string | number;
  readonly x?: number;
  readonly y?: number;
  readonly size?: number;
  readonly color?: string;
  readonly opacity?: number;
  readonly showLabel?: boolean;
  readonly draggable?: boolean;
  readonly selected?: boolean;
  readonly highlight?: GraphNodeHighlight;
  readonly tooltip?: GraphTooltip;
}

export interface GraphEdge {
  readonly id: string;
  readonly source: string;
  readonly target: string;
  readonly weight?: number;
  readonly color?: string;
  readonly lineType?: 'solid' | 'dashed' | 'dotted';
  readonly width?: number;
  readonly opacity?: number;
  readonly curveness?: number;
  readonly selected?: boolean;
  readonly highlight?: GraphEdgeHighlight;
  readonly tooltip?: GraphTooltip;
}

export interface GraphChartData {
  readonly nodes: ReadonlyArray<GraphNode>;
  readonly edges: ReadonlyArray<GraphEdge>;
  readonly layers?: ReadonlyArray<GraphLayer>;
  readonly bounds?: GraphBounds;
}

export interface GraphChartAttrs {
  readonly data: GraphChartData | undefined;
  readonly layout?: GraphChartLayout;
  readonly height?: number;
  readonly fillParent?: boolean;
  readonly className?: string;
  readonly ariaLabel?: string;
  /**
   * Increment this counter to reset zoom and pan back to the initial view.
   */
  readonly resetToken?: number;
  /**
   * Animate node transitions across updates (default: true).
   */
  readonly animateUpdates?: boolean;
  /**
   * Append `↻N` self-loop count suffixes onto node labels when self-loop edges
   * exist on a node (default: true).
   */
  readonly showSelfLoopCountOnLabel?: boolean;
  /**
   * Enable WASD pan and Shift+W/S / +/- zoom when hovered or keyboard-focused
   * (default: true).
   */
  readonly keyboardNavigation?: boolean;
  readonly onNodeClick?: (nodeId: string) => void;
  readonly onEdgeClick?: (edgeId: string) => void;
  readonly onCanvasEmptyClick?: () => void;
}

export type KeyboardRoamCommand =
  | {readonly kind: 'pan'; readonly dx: number; readonly dy: number}
  | {readonly kind: 'zoom'; readonly zoom: number};

/**
 * Maps physical WASD / zoom keys to ECharts `graphRoam` commands, matching
 * Perfetto's 2D graph convention (`WASD` pans in 4 directions, `Shift+W/S` or
 * `+/-` zooms in/out).
 */
export function computeKeyboardRoam(
  code: string,
  key: string,
  shiftKey: boolean,
): KeyboardRoamCommand | undefined {
  if (shiftKey) {
    if (code === 'KeyW') {
      return {kind: 'zoom', zoom: KEYBOARD_ZOOM_FACTOR};
    }
    if (code === 'KeyS') {
      return {kind: 'zoom', zoom: 1 / KEYBOARD_ZOOM_FACTOR};
    }
  } else {
    switch (code) {
      case 'KeyW':
        return {kind: 'pan', dx: 0, dy: KEYBOARD_PAN_STEP_PX};
      case 'KeyA':
        return {kind: 'pan', dx: KEYBOARD_PAN_STEP_PX, dy: 0};
      case 'KeyS':
        return {kind: 'pan', dx: 0, dy: -KEYBOARD_PAN_STEP_PX};
      case 'KeyD':
        return {kind: 'pan', dx: -KEYBOARD_PAN_STEP_PX, dy: 0};
      default:
        break;
    }
  }
  if (key === '+' || key === '=') {
    return {kind: 'zoom', zoom: KEYBOARD_ZOOM_FACTOR};
  }
  if (key === '-' || key === '_') {
    return {kind: 'zoom', zoom: 1 / KEYBOARD_ZOOM_FACTOR};
  }
  return undefined;
}

export function effectiveNodeScaleRatio(zoom: number): number {
  const capZoom = 1 + MAX_NODE_GROWTH / NODE_SCALE_SLOPE;
  if (zoom <= capZoom) {
    return NODE_SCALE_SLOPE;
  }
  return Number((MAX_NODE_GROWTH / (zoom - 1)).toFixed(4));
}

/**
 * Escapes untrusted strings for safe embedding in ECharts HTML tooltips,
 * including `{` and `}` so ECharts template placeholders (`{a}`, `{b}`) never
 * expand inside user or trace text.
 */
export function escapeGraphTooltipHtml(raw: string): string {
  return raw
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
    .replace(/\{/g, '&#123;')
    .replace(/\}/g, '&#125;');
}

export function formatGraphTooltip(
  tooltip: GraphTooltip | undefined,
  fallbackText: string,
): string {
  if (tooltip === undefined) {
    return escapeGraphTooltipHtml(fallbackText);
  }
  if (typeof tooltip === 'string') {
    return escapeGraphTooltipHtml(tooltip);
  }
  return tooltip
    .map((entry) => {
      if (typeof entry === 'string') {
        return escapeGraphTooltipHtml(entry);
      }
      const valHtml = entry.bold
        ? `<b>${escapeGraphTooltipHtml(entry.value)}</b>`
        : escapeGraphTooltipHtml(entry.value);
      return entry.label !== undefined && entry.label !== ''
        ? `${escapeGraphTooltipHtml(entry.label)}: ${valHtml}`
        : valHtml;
    })
    .join('<br>');
}

export interface BarycenterInputNode {
  readonly id: string;
  readonly layer: number;
  readonly tieBreak?: string;
}

/**
 * Computes Sugiyama-style horizontal ordering across layered rows via
 * alternating barycenter sweeps to reduce edge crossings.
 */
export function computeBarycenterOrder(
  nodes: ReadonlyArray<BarycenterInputNode>,
  edges: ReadonlyArray<{readonly source: string; readonly target: string}>,
  sweeps = 12,
): {
  readonly normX: ReadonlyMap<string, number>;
  readonly layers: ReadonlyArray<ReadonlyArray<string>>;
} {
  const adj = new Map<string, string[]>();
  const deg = new Map<string, number>();
  for (const {source, target} of edges) {
    if (source === target) continue;
    let srcList = adj.get(source);
    if (srcList === undefined) {
      srcList = [];
      adj.set(source, srcList);
    }
    srcList.push(target);

    let dstList = adj.get(target);
    if (dstList === undefined) {
      dstList = [];
      adj.set(target, dstList);
    }
    dstList.push(source);

    deg.set(source, (deg.get(source) ?? 0) + 1);
    deg.set(target, (deg.get(target) ?? 0) + 1);
  }

  const tieBreakById = new Map<string, string>();
  const byLayer = new Map<number, string[]>();
  for (const n of nodes) {
    tieBreakById.set(n.id, n.tieBreak ?? n.id);
    let bucket = byLayer.get(n.layer);
    if (bucket === undefined) {
      bucket = [];
      byLayer.set(n.layer, bucket);
    }
    bucket.push(n.id);
  }

  const layerNums = [...byLayer.keys()].sort((a, b) => a - b);
  const layers = layerNums.map((l) => {
    const arr = [...(byLayer.get(l) ?? [])];
    arr.sort((a, b) => {
      const degDiff = (deg.get(b) ?? 0) - (deg.get(a) ?? 0);
      if (degDiff !== 0) return degDiff;
      const tbA = tieBreakById.get(a) ?? a;
      const tbB = tieBreakById.get(b) ?? b;
      return tbA.localeCompare(tbB) || a.localeCompare(b);
    });
    return arr;
  });

  const normX = new Map<string, number>();
  for (const layer of layers) {
    const len = Math.max(1, layer.length);
    for (let i = 0; i < layer.length; i++) {
      const id = layer[i];
      if (id !== undefined) {
        normX.set(id, (i + 0.5) / len);
      }
    }
  }

  for (let sweep = 0; sweep < sweeps; sweep++) {
    const indices = layers.map((_, idx) => idx);
    if (sweep % 2 === 1) {
      indices.reverse();
    }
    for (const li of indices) {
      const layer = layers[li];
      if (layer === undefined) continue;
      const barycenter = (id: string): number => {
        const neighbors = (adj.get(id) ?? []).filter((v) => normX.has(v));
        if (neighbors.length === 0) {
          return normX.get(id) ?? 0.5;
        }
        let sum = 0;
        for (const v of neighbors) {
          sum += normX.get(v) ?? 0;
        }
        return sum / neighbors.length;
      };
      layer.sort((a, b) => barycenter(a) - barycenter(b) || a.localeCompare(b));
      const len = Math.max(1, layer.length);
      for (let i = 0; i < layer.length; i++) {
        const id = layer[i];
        if (id !== undefined) {
          normX.set(id, (i + 0.5) / len);
        }
      }
    }
  }

  return {normX, layers};
}

function isDarkBackground(bgColor: string): boolean {
  const hex = bgColor.trim();
  if (hex.startsWith('#')) {
    const clean = hex.slice(1);
    const full =
      clean.length === 3
        ? clean
            .split('')
            .map((ch) => ch + ch)
            .join('')
        : clean;
    if (full.length >= 6) {
      const r = Number.parseInt(full.slice(0, 2), 16);
      const g = Number.parseInt(full.slice(2, 4), 16);
      const b = Number.parseInt(full.slice(4, 6), 16);
      if (!Number.isNaN(r) && !Number.isNaN(g) && !Number.isNaN(b)) {
        return (0.299 * r + 0.587 * g + 0.114 * b) / 255 < 0.5;
      }
    }
  }
  const rgbMatch = /rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/.exec(hex);
  if (rgbMatch !== null) {
    const r = Number(rgbMatch[1]);
    const g = Number(rgbMatch[2]);
    const b = Number(rgbMatch[3]);
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255 < 0.5;
  }
  return false;
}

export function parseGraphClickData(
  data: unknown,
): {nodeId?: string; edgeId?: string; isAnchor?: boolean} | undefined {
  if (typeof data !== 'object' || !Boolean(data)) {
    return undefined;
  }
  const rec = data as Record<string, unknown>;
  return {
    nodeId: typeof rec.nodeId === 'string' ? rec.nodeId : undefined,
    edgeId: typeof rec.edgeId === 'string' ? rec.edgeId : undefined,
    isAnchor: rec.isAnchor === true,
  };
}

export function parseRoamZoomDelta(raw: unknown): number | undefined {
  if (typeof raw !== 'object' || !Boolean(raw)) {
    return undefined;
  }
  const rec = raw as Record<string, unknown>;
  return typeof rec.zoom === 'number' && Number.isFinite(rec.zoom)
    ? rec.zoom
    : undefined;
}

export interface BuildGraphOptionConfig {
  readonly layout?: GraphChartLayout;
  readonly nodeScaleRatio?: number;
  readonly animateUpdates?: boolean;
  readonly showSelfLoopCountOnLabel?: boolean;
}

function resolveEffectiveLayout(
  data: GraphChartData,
  requested: GraphChartLayout | undefined,
): GraphChartLayout {
  if (requested !== undefined) {
    return requested;
  }
  if (data.layers !== undefined && data.layers.length > 0) {
    return 'layered';
  }
  if (
    data.nodes.length > 0 &&
    data.nodes.every((n) => n.x !== undefined && n.y !== undefined)
  ) {
    return 'none';
  }
  return 'force';
}

export function buildGraphChartOption(
  data: GraphChartData,
  colors: ChartThemeColors,
  config: BuildGraphOptionConfig = {},
): EChartsCoreOption {
  const effectiveLayout = resolveEffectiveLayout(data, config.layout);
  const nodeScaleRatio = config.nodeScaleRatio ?? NODE_SCALE_SLOPE;
  const animateUpdates = config.animateUpdates ?? true;
  const showSelfLoopCount = config.showSelfLoopCountOnLabel ?? true;

  const isDark = isDarkBackground(colors.backgroundColor);
  const addedColor = isDark ? '#66bb6a' : '#15803d';
  const removedColor = isDark ? '#ef5350' : '#dc2626';
  const changedColor = isDark ? '#ffee58' : '#b45309';
  const defaultBorder = isDark
    ? 'rgba(255, 255, 255, 0.28)'
    : 'rgba(15, 23, 42, 0.35)';
  const selectedBorder = isDark ? '#ffffff' : '#0f172a';
  const defaultNodeColor = colors.chartColors[0] ?? '#5c6bc0';

  const deg = new Map<string, number>();
  const selfLoopWeightById = new Map<string, number>();
  const parallelCount = new Map<string, number>();
  for (const e of data.edges) {
    if (e.highlight === 'removed') continue;
    deg.set(e.source, (deg.get(e.source) ?? 0) + 1);
    if (e.source !== e.target) {
      deg.set(e.target, (deg.get(e.target) ?? 0) + 1);
    } else {
      selfLoopWeightById.set(
        e.source,
        (selfLoopWeightById.get(e.source) ?? 0) + (e.weight ?? 1),
      );
    }
    const pairKey = `${e.source}\0${e.target}`;
    parallelCount.set(pairKey, (parallelCount.get(pairKey) ?? 0) + 1);
  }

  // Resolve coordinates for positioned layouts ('layered', 'circular', 'none').
  const isPositioned = effectiveLayout !== 'force';
  const resolvedX = new Map<string, number>();
  const resolvedY = new Map<string, number>();
  const resolvedSize = new Map<string, number>();

  const layers = data.layers ?? [];
  const layerIndexById = new Map<string | number, number>();
  const layerYById = new Map<string | number, number>();
  const defaultCanvasH = Math.max(360, (layers.length - 1) * 40);
  const layerDenom = Math.max(1, layers.length - 1);
  for (let i = 0; i < layers.length; i++) {
    const layer = layers[i];
    if (layer !== undefined) {
      layerIndexById.set(layer.id, i);
      layerYById.set(
        layer.id,
        layer.y ?? 30 + (i / layerDenom) * defaultCanvasH,
      );
    }
  }

  if (effectiveLayout === 'layered') {
    const needsAutoX = data.nodes.some((n) => n.x === undefined);
    const autoOrder = needsAutoX
      ? computeBarycenterOrder(
          data.nodes.map((n) => ({
            id: n.id,
            layer:
              n.layer !== undefined ? (layerIndexById.get(n.layer) ?? 0) : 0,
            tieBreak: n.label,
          })),
          data.edges,
        )
      : undefined;
    for (const n of data.nodes) {
      const autoNorm = autoOrder?.normX.get(n.id) ?? 0.5;
      resolvedX.set(
        n.id,
        n.x ?? DEFAULT_GRAPH_LEFT_MARGIN_X + autoNorm * DEFAULT_GRAPH_CANVAS_W,
      );
      const fallbackY =
        n.layer !== undefined
          ? (layerYById.get(n.layer) ?? defaultCanvasH + 55)
          : 30;
      resolvedY.set(n.id, n.y ?? fallbackY);
    }
  } else if (effectiveLayout === 'circular') {
    const count = Math.max(1, data.nodes.length);
    const radius = Math.max(80, Math.min(220, count * 14));
    const cx = 240;
    const cy = 240;
    for (let i = 0; i < data.nodes.length; i++) {
      const n = data.nodes[i];
      if (n === undefined) continue;
      const angle = (2 * Math.PI * i) / count - Math.PI / 2;
      resolvedX.set(n.id, n.x ?? cx + radius * Math.cos(angle));
      resolvedY.set(n.id, n.y ?? cy + radius * Math.sin(angle));
    }
  } else if (effectiveLayout === 'none') {
    for (const n of data.nodes) {
      resolvedX.set(n.id, n.x ?? 0);
      resolvedY.set(n.id, n.y ?? 0);
    }
  }

  let minX = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY;
  let minY = Number.POSITIVE_INFINITY;
  let maxY = Number.NEGATIVE_INFINITY;
  for (const x of resolvedX.values()) {
    minX = Math.min(minX, x);
    maxX = Math.max(maxX, x);
  }
  for (const y of resolvedY.values()) {
    minY = Math.min(minY, y);
    maxY = Math.max(maxY, y);
  }
  for (const y of layerYById.values()) {
    minY = Math.min(minY, y);
    maxY = Math.max(maxY, y);
  }

  const effectiveBounds: GraphBounds | undefined =
    data.bounds ??
    (effectiveLayout === 'layered' && layers.length > 0
      ? {
          minX: DEFAULT_GRAPH_ROW_LABEL_X,
          maxX: Math.max(
            DEFAULT_GRAPH_LEFT_MARGIN_X + DEFAULT_GRAPH_CANVAS_W,
            Number.isFinite(maxX) ? maxX : 0,
          ),
          minY: 0,
          maxY: (Number.isFinite(maxY) ? maxY : defaultCanvasH) + 60,
        }
      : undefined);

  const rowLabelX = effectiveBounds?.minX ?? DEFAULT_GRAPH_ROW_LABEL_X;
  const maxBoundX =
    effectiveBounds?.maxX ??
    (Number.isFinite(maxX)
      ? maxX + 40
      : DEFAULT_GRAPH_LEFT_MARGIN_X + DEFAULT_GRAPH_CANVAS_W);

  const pinNodes =
    isPositioned && effectiveBounds !== undefined
      ? [
          {
            id: 'pin:tl',
            name: '',
            x: effectiveBounds.minX,
            y: effectiveBounds.minY,
            symbolSize: 0.1,
            draggable: false,
            itemStyle: {color: 'transparent'},
            tooltip: {show: false},
            emphasis: {disabled: true},
            isAnchor: true,
          },
          {
            id: 'pin:br',
            name: '',
            x: effectiveBounds.maxX,
            y: effectiveBounds.maxY,
            symbolSize: 0.1,
            draggable: false,
            itemStyle: {color: 'transparent'},
            tooltip: {show: false},
            emphasis: {disabled: true},
            isAnchor: true,
          },
        ]
      : [];

  const labelAnchors =
    effectiveLayout === 'layered'
      ? layers.map((layer) => ({
          id: `row-label:${layer.id}`,
          name: layer.label,
          x: rowLabelX,
          y: layerYById.get(layer.id) ?? 30,
          symbol: 'circle',
          symbolSize: 0.1,
          draggable: false,
          itemStyle: {color: 'transparent', borderColor: 'transparent'},
          tooltip: {show: false},
          emphasis: {disabled: true},
          label: {
            show: true,
            position: 'left',
            formatter: '{b}',
            color: isDark
              ? (layer.color ?? colors.textColor)
              : (layer.lightTextColor ?? layer.color ?? colors.textColor),
            opacity: layer.dimmed === true ? 0.38 : 1,
            fontWeight: 'bold',
            fontSize: 9.5,
          },
          isAnchor: true,
        }))
      : [];

  const nodeLabelById = new Map<string, string>();
  const graphNodes = data.nodes.map((n) => {
    nodeLabelById.set(n.id, n.label);
    const d = deg.get(n.id) ?? 0;
    const isGhost = n.highlight === 'ghost' || n.highlight === 'removed';
    const size = n.size ?? (isGhost ? 8 : Math.max(10.5, 8 + 3 * Math.sqrt(d)));
    resolvedSize.set(n.id, size);

    const selfCount = selfLoopWeightById.get(n.id) ?? 0;
    const displayLabel =
      showSelfLoopCount && selfCount > 0 && !isGhost
        ? `${n.label} ↻${selfCount}`
        : n.label;

    let borderColor = defaultBorder;
    let borderWidth = 1.5;
    let borderType: 'solid' | 'dashed' = 'solid';
    if (isGhost) {
      borderColor = removedColor;
      borderWidth = 2;
      borderType = 'dashed';
    } else if (n.highlight === 'changed') {
      borderColor = changedColor;
      borderWidth = 3;
    } else if (n.highlight === 'added') {
      borderColor = addedColor;
      borderWidth = 3;
    }
    if (n.selected === true) {
      borderColor = selectedBorder;
      borderWidth = 3;
    }

    const labelColor = isGhost
      ? removedColor
      : n.highlight === 'changed'
        ? changedColor
        : colors.textColor;

    const showLabel =
      n.showLabel ??
      (d > 0 ||
        n.highlight !== undefined ||
        n.selected === true ||
        !isPositioned);

    return {
      id: n.id,
      nodeId: n.id,
      name: displayLabel,
      ...(isPositioned
        ? {x: resolvedX.get(n.id) ?? 0, y: resolvedY.get(n.id) ?? 0}
        : {}),
      draggable: n.draggable ?? !isGhost,
      symbolSize: size,
      tooltip: {
        formatter: formatGraphTooltip(n.tooltip, n.label),
      },
      label: {
        show: showLabel,
        position: 'top',
        fontSize: 9.5,
        padding: [1, 3],
        color: labelColor,
        formatter: '{b}',
      },
      itemStyle: {
        color: isGhost ? 'transparent' : (n.color ?? defaultNodeColor),
        opacity: n.opacity ?? (isGhost ? 0.78 : 1),
        borderColor,
        borderWidth,
        borderType,
      },
    };
  });

  // Circular SVG arc + chevron badges keep self-loops crisp and locked to the
  // 1.5x zoom cap in positioned layouts ('layered', 'circular', 'none').
  const selfLoopPathRight =
    'path://M 5.45 7.41 A 8 8 0 1 1 6.34 17.66 M 10.84 18.62 L 6.34 17.66 L 7.30 22.16';
  const selfLoopPathLeft =
    'path://M 18.55 7.41 A 8 8 0 1 0 17.66 17.66 M 13.16 18.62 L 17.66 17.66 L 16.70 22.16';
  const loopBadgeNodes: Array<Record<string, unknown>> = [];
  const graphLinks: Array<Record<string, unknown>> = [];
  const selfLoopSlotsUsed = new Map<string, number>();
  const parallelSeen = new Map<string, number>();

  const pushSelfLoopBadge = (
    nodeId: string,
    badgeId: string,
    edgeId: string,
    tooltipHtml: string,
    strokeColor: string,
    strokeType: 'solid' | 'dashed' | 'dotted',
    strokeWidth: number,
    strokeOpacity: number,
  ): void => {
    const slot = selfLoopSlotsUsed.get(nodeId) ?? 0;
    selfLoopSlotsUsed.set(nodeId, slot + 1);
    const baseX = resolvedX.get(nodeId) ?? 0;
    const baseY = resolvedY.get(nodeId) ?? 0;
    const nodeRadius = (resolvedSize.get(nodeId) ?? 10.5) / 2;

    let side = 1;
    let loopSize = 20;
    let offsetDist = nodeRadius + 6.5;
    if (slot === 0) {
      if (baseX + 26 > maxBoundX) {
        side = -1;
      }
    } else if (baseX - 26 >= rowLabelX + 4 && baseX + 26 <= maxBoundX) {
      side = -1;
    } else {
      side = baseX + 34 <= maxBoundX ? 1 : -1;
      loopSize = 26;
      offsetDist = nodeRadius + 9.5;
    }

    loopBadgeNodes.push({
      id: badgeId,
      name: '',
      x: baseX,
      y: baseY,
      symbol: side > 0 ? selfLoopPathRight : selfLoopPathLeft,
      symbolKeepAspect: true,
      symbolSize: loopSize,
      symbolOffset: [side * offsetDist, 0],
      draggable: false,
      edgeId,
      itemStyle: {
        color: 'transparent',
        borderColor: strokeColor,
        borderWidth: strokeWidth,
        borderType: strokeType,
        borderCap: 'round',
        borderJoin: 'round',
        opacity: strokeOpacity,
      },
      tooltip: {formatter: tooltipHtml},
    });
    graphLinks.push({
      id: `${badgeId}:adj`,
      source: nodeId,
      target: badgeId,
      symbol: ['none', 'none'],
      lineStyle: {opacity: 0, width: 0},
      tooltip: {show: false},
      emphasis: {disabled: true},
    });
  };

  for (const e of data.edges) {
    const isSelf = e.source === e.target;
    const isRemoved = e.highlight === 'removed';
    const isAdded = e.highlight === 'added';
    const weight = Math.max(1, e.weight ?? 1);

    let color = e.color ?? defaultNodeColor;
    let lineType: 'solid' | 'dashed' | 'dotted' = e.lineType ?? 'solid';
    let opacity = e.opacity ?? (isSelf ? 0.95 : 0.72);
    let width =
      e.width ?? Math.min(4.5, (isSelf ? 1.8 : 1.2) + Math.log2(weight));

    if (isRemoved) {
      color = removedColor;
      lineType = 'dotted';
      opacity = 0.9;
      width = isSelf ? 2.6 : 2.8;
    } else if (isAdded) {
      color = addedColor;
      lineType = 'solid';
      opacity = 1;
      width = 3.2;
    }

    if (e.selected === true) {
      width += 1.6;
      opacity = 1;
    }

    const srcLabel = nodeLabelById.get(e.source) ?? e.source;
    const dstLabel = nodeLabelById.get(e.target) ?? e.target;
    const tooltipHtml = formatGraphTooltip(
      e.tooltip,
      `${srcLabel} → ${dstLabel}`,
    );

    if (isSelf && isPositioned) {
      pushSelfLoopBadge(
        e.source,
        isRemoved ? `loop-badge:removed:${e.id}` : `loop-badge:${e.id}`,
        e.id,
        tooltipHtml,
        color,
        lineType,
        Math.min(3.4, width),
        opacity,
      );
      continue;
    }

    const pairKey = `${e.source}\0${e.target}`;
    const pairCount = parallelCount.get(pairKey) ?? 1;
    const seenIdx = isRemoved ? 0 : (parallelSeen.get(pairKey) ?? 0);
    if (!isRemoved) {
      parallelSeen.set(pairKey, seenIdx + 1);
    }
    const defaultCurveness = isSelf
      ? 0.45
      : pairCount > 1
        ? Number((0.1 + seenIdx * 0.1).toFixed(2))
        : 0.1;

    graphLinks.push({
      id: isRemoved ? `removed:${e.id}` : e.id,
      source: e.source,
      target: e.target,
      edgeId: e.id,
      tooltip: {formatter: tooltipHtml},
      lineStyle: {
        color,
        type: lineType,
        opacity,
        width,
        curveness: e.curveness ?? defaultCurveness,
      },
    });
  }

  const hasLayerLabels = effectiveLayout === 'layered' && layers.length > 0;

  return {
    backgroundColor: 'transparent',
    tooltip: {
      confine: true,
    },
    series: [
      {
        id: 'perfetto-graph-series',
        type: 'graph',
        layout: effectiveLayout === 'force' ? 'force' : 'none',
        ...(effectiveLayout === 'force'
          ? {
              force: {
                repulsion: 180,
                edgeLength: [55, 130],
                layoutAnimation: false,
              },
            }
          : {
              preserveAspect: true,
              preserveAspectAlign: hasLayerLabels ? 'left' : 'center',
            }),
        left: hasLayerLabels ? 118 : 16,
        right: 14,
        top: 18,
        bottom: 16,
        roam: true,
        scaleLimit: {min: ROAM_MIN_ZOOM, max: ROAM_MAX_ZOOM},
        nodeScaleRatio,
        labelLayout: {hideOverlap: true},
        edgeSymbol: ['none', 'arrow'],
        edgeSymbolSize: [0, 6.5],
        emphasis: {
          focus: 'adjacency',
          label: {show: true},
          lineStyle: {width: 3, opacity: 1},
        },
        data: [...pinNodes, ...labelAnchors, ...loopBadgeNodes, ...graphNodes],
        links: graphLinks,
        animationDuration: 0,
        animationDurationUpdate: animateUpdates ? 220 : 0,
        animationEasingUpdate: 'cubicOut',
      },
    ],
  };
}

/**
 * Reusable interactive ECharts node-link graph component supporting layered
 * Sugiyama layouts (with barycenter crossing reduction), circular, force, and
 * custom-positioned graphs, circular self-loop badges, zoom-capped node/label
 * scaling, and WASD keyboard pan/zoom isolated from the main timeline.
 */
export class GraphChart implements m.ClassComponent<GraphChartAttrs> {
  private roamZoom = 1;
  private zoomOnlyUpdate = false;
  private lastResetToken?: number;
  private lastThemeBg?: string;
  private chartApi?: EChartApi;
  private canvasDom?: HTMLElement;
  private pointerInside = false;
  private focusedByPointer = false;
  private hoverOffset?: {readonly x: number; readonly y: number};
  private docKeyListener?: Disposable;
  private keyboardEnabled = true;

  oncreate() {
    this.docKeyListener = bindEventListener(
      document,
      'keydown',
      (ev: KeyboardEvent) => this.handleKeyDown(ev),
      {capture: true},
    );
  }

  onremove() {
    this.docKeyListener?.[Symbol.dispose]();
    this.docKeyListener = undefined;
    this.chartApi = undefined;
    this.canvasDom = undefined;
  }

  private handleKeyDown(ev: KeyboardEvent): void {
    if (
      !this.keyboardEnabled ||
      this.chartApi === undefined ||
      ev.ctrlKey ||
      ev.altKey ||
      ev.metaKey
    ) {
      return;
    }
    const keyboardFocused =
      this.canvasDom !== undefined &&
      document.activeElement === this.canvasDom &&
      !this.focusedByPointer;
    if (!this.pointerInside && !keyboardFocused) {
      return;
    }
    const target = ev.target;
    if (target instanceof Element && target.closest('select') !== null) {
      return;
    }
    if (
      elementIsEditable(target) &&
      !(target instanceof HTMLInputElement && target.type === 'range')
    ) {
      return;
    }
    const cmd = computeKeyboardRoam(ev.code, ev.key, ev.shiftKey);
    if (cmd === undefined) {
      return;
    }
    ev.preventDefault();
    ev.stopPropagation();
    if (cmd.kind === 'pan') {
      this.chartApi.dispatchAction({
        type: 'graphRoam',
        seriesId: 'perfetto-graph-series',
        dx: cmd.dx,
        dy: cmd.dy,
      });
    } else {
      const size = this.chartApi.getSize();
      this.chartApi.dispatchAction({
        type: 'graphRoam',
        seriesId: 'perfetto-graph-series',
        zoom: cmd.zoom,
        originX: this.hoverOffset?.x ?? size.width / 2,
        originY: this.hoverOffset?.y ?? size.height / 2,
      });
    }
  }

  view({attrs}: m.CVnode<GraphChartAttrs>) {
    const {
      data,
      layout,
      height,
      fillParent,
      className,
      ariaLabel = 'Interactive graph chart. Use W, A, S, D to pan and Shift+W or Shift+S to zoom.',
      resetToken = 0,
      animateUpdates = true,
      showSelfLoopCountOnLabel = true,
      keyboardNavigation = true,
    } = attrs;

    this.keyboardEnabled = keyboardNavigation;
    if (
      this.lastResetToken !== undefined &&
      this.lastResetToken !== resetToken
    ) {
      this.roamZoom = 1;
      this.zoomOnlyUpdate = true;
    }
    this.lastResetToken = resetToken;

    const isEmpty =
      data !== undefined &&
      data.nodes.length === 0 &&
      (data.layers === undefined || data.layers.length === 0);

    const animateThisFrame = animateUpdates && !this.zoomOnlyUpdate;
    this.zoomOnlyUpdate = false;

    const optionToken: EChartsCoreOption | undefined =
      data !== undefined && !isEmpty ? {} : undefined;

    return m(
      '.pf-graph-chart',
      {
        'className': classNames(
          fillParent && 'pf-graph-chart--fill-parent',
          className,
        ),
        'tabindex': keyboardNavigation ? 0 : undefined,
        'role': 'img',
        'aria-label': ariaLabel,
        'oncreate': ({dom}: m.VnodeDOM) => {
          this.canvasDom = toHTMLElement(dom);
        },
        'onremove': () => {
          this.canvasDom = undefined;
        },
        'onmouseenter': () => {
          this.pointerInside = true;
        },
        'onmouseleave': () => {
          this.pointerInside = false;
          this.hoverOffset = undefined;
          if (
            this.focusedByPointer &&
            this.canvasDom !== undefined &&
            document.activeElement === this.canvasDom
          ) {
            this.canvasDom.blur();
          }
          this.focusedByPointer = false;
        },
        'onpointerdown': () => {
          this.focusedByPointer = true;
          this.canvasDom?.focus({preventScroll: true});
        },
        'onfocus': () => {
          if (!this.pointerInside) {
            this.focusedByPointer = false;
          }
        },
        'onblur': () => {
          this.focusedByPointer = false;
        },
        'onmousemove': (ev: MouseEvent) => {
          this.pointerInside = true;
          const offset = currentTargetOffset(ev);
          this.hoverOffset = {x: offset.x, y: offset.y};
        },
      },
      m(EChartView, {
        key: resetToken,
        option: optionToken,
        height,
        fillParent,
        empty: isEmpty,
        notMerge: false,
        onReady: (api) => {
          this.chartApi = api;
        },
        resolveOption: (_opt, colors) => {
          if (data === undefined) {
            return {};
          }
          if (
            this.lastThemeBg !== undefined &&
            this.lastThemeBg !== colors.backgroundColor
          ) {
            this.roamZoom = 1;
          }
          this.lastThemeBg = colors.backgroundColor;
          return buildGraphChartOption(data, colors, {
            layout,
            nodeScaleRatio: effectiveNodeScaleRatio(this.roamZoom),
            animateUpdates: animateThisFrame,
            showSelfLoopCountOnLabel,
          });
        },
        onCanvasEmptyClick: attrs.onCanvasEmptyClick,
        eventHandlers: [
          {
            eventName: 'graphRoam',
            handler: (rawParams: unknown) => {
              const zoomDelta = parseRoamZoomDelta(rawParams);
              if (zoomDelta === undefined) {
                return;
              }
              const prevRatio = effectiveNodeScaleRatio(this.roamZoom);
              this.roamZoom = Math.max(
                ROAM_MIN_ZOOM,
                Math.min(ROAM_MAX_ZOOM, this.roamZoom * zoomDelta),
              );
              if (effectiveNodeScaleRatio(this.roamZoom) !== prevRatio) {
                this.zoomOnlyUpdate = true;
              }
            },
          },
          {
            eventName: 'click',
            handler: (rawParams: unknown) => {
              const params = rawParams as EChartClickParams;
              const parsed = parseGraphClickData(params.data);
              if (parsed === undefined || parsed.isAnchor === true) {
                return;
              }
              if (
                (params.dataType === 'edge' || parsed.nodeId === undefined) &&
                parsed.edgeId !== undefined
              ) {
                attrs.onEdgeClick?.(parsed.edgeId);
              } else if (
                params.dataType === 'node' &&
                parsed.nodeId !== undefined
              ) {
                attrs.onNodeClick?.(parsed.nodeId);
              }
            },
          },
        ],
      }),
    );
  }
}
