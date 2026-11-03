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
import {SimpleResizeObserver} from '../../../base/resize_observer';
import type {ChartThemeColors} from './chart_theme';
import './common';
import {
  type EChartApi,
  type EChartClickParams,
  EChartView,
  type EChartZrElement,
} from './echart_view';

export const DEFAULT_GRAPH_CANVAS_W = 480;
export const DEFAULT_GRAPH_LEFT_MARGIN_X = 230;
export const DEFAULT_GRAPH_ROW_LABEL_X = 215;
export const ROAM_MIN_ZOOM = 0.5;
export const ROAM_MAX_ZOOM = 15;

const NODE_SCALE_SLOPE = 0.25;
const MAX_NODE_GROWTH = 0.5; // Caps screen-space node & label growth at 1.5x.
const MIN_AUTO_NODE_SIZE = 9.5;
const MAX_AUTO_NODE_SIZE = 22;
const MIN_NODE_GAP_PX = 16;
const MIN_NODE_CENTER_DIST_PX = 28;
const SELF_LOOP_CLEARANCE_PX = 18;
const KEYBOARD_PAN_STEP_PX = 48;
const KEYBOARD_ZOOM_FACTOR = 1.18;

export type GraphChartLayout = 'layered' | 'force' | 'circular' | 'none';
export type GraphLayerOrientation = 'vertical' | 'horizontal';
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
  readonly x?: number;
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
  readonly normPosition?: number;
  readonly size?: number;
  readonly symbol?: string;
  readonly symbolKeepAspect?: boolean;
  readonly symbolSizeScale?: number;
  readonly color?: string;
  readonly borderColor?: string;
  readonly borderWidth?: number;
  readonly labelBold?: boolean;
  readonly accentRingColor?: string;
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
  readonly layerOrientation?: GraphLayerOrientation;
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

export interface GraphDragOffset {
  readonly dx: number;
  readonly dy: number;
}

export interface GraphLayoutRuntimeState {
  readonly positions: ReadonlyMap<
    string,
    {readonly x: number; readonly y: number}
  >;
  readonly sizes: ReadonlyMap<string, number>;
  readonly scaleX: number;
  readonly scaleY: number;
}

export interface BuildGraphOptionConfig {
  readonly layout?: GraphChartLayout;
  readonly layerOrientation?: GraphLayerOrientation;
  readonly nodeScaleRatio?: number;
  readonly animateUpdates?: boolean;
  readonly showSelfLoopCountOnLabel?: boolean;
  readonly viewportSize?: {readonly width: number; readonly height: number};
  readonly dragOffsets?: ReadonlyMap<string, GraphDragOffset>;
  readonly outBasePositions?: Map<
    string,
    {readonly x: number; readonly y: number}
  >;
  readonly outLayoutState?: {current?: GraphLayoutRuntimeState};
}

function clampNodeToMinDistance(
  nodeId: string,
  startX: number,
  startY: number,
  nodeIds: Iterable<string>,
  posX: ReadonlyMap<string, number>,
  posY: ReadonlyMap<string, number>,
  sizes: ReadonlyMap<string, number>,
  scaleX: number,
  scaleY: number,
): {readonly x: number; readonly y: number} {
  let x = startX;
  let y = startY;
  const rSelf = (sizes.get(nodeId) ?? MIN_AUTO_NODE_SIZE) / 2;
  const sx = Math.max(0.05, scaleX);
  const sy = Math.max(0.05, scaleY);
  for (let pass = 0; pass < 3; pass++) {
    let moved = false;
    for (const otherId of nodeIds) {
      if (otherId === nodeId) continue;
      const ox = posX.get(otherId);
      const oy = posY.get(otherId);
      if (ox === undefined || oy === undefined) continue;
      const rOther = (sizes.get(otherId) ?? MIN_AUTO_NODE_SIZE) / 2;
      const minDistPx = Math.max(
        MIN_NODE_CENTER_DIST_PX,
        rSelf + rOther + MIN_NODE_GAP_PX,
      );
      const dxPx = (x - ox) * sx;
      const dyPx = (y - oy) * sy;
      const distPx = Math.hypot(dxPx, dyPx);
      if (distPx < minDistPx) {
        const ux = distPx > 1e-3 ? dxPx / distPx : 1;
        const uy = distPx > 1e-3 ? dyPx / distPx : 0;
        x = ox + (ux * minDistPx) / sx;
        y = oy + (uy * minDistPx) / sy;
        moved = true;
      }
    }
    if (!moved) break;
  }
  return {x, y};
}

interface LinearSlot {
  readonly id: string;
  readonly desired: number;
  readonly beforeExtent: number;
  readonly afterExtent: number;
}

/**
 * Separates overlapping nodes along a 1D row or column via Pool Adjacent
 * Violators (L2 minimum displacement) while keeping non-colliding nodes at
 * their exact desired coordinates.
 */
function solveLinearPositions(
  slots: ReadonlyArray<LinearSlot>,
  minBound: number,
  maxBound: number,
  minGapData: number,
  minCenterDistData: number,
): ReadonlyMap<string, number> {
  const out = new Map<string, number>();
  const n = slots.length;
  if (n === 0) return out;
  if (n === 1) {
    const only = slots[0];
    if (only !== undefined) {
      out.set(only.id, Math.min(maxBound, Math.max(minBound, only.desired)));
    }
    return out;
  }

  const seps: number[] = [];
  let totalSep = 0;
  for (let i = 0; i < n - 1; i++) {
    const cur = slots[i];
    const next = slots[i + 1];
    const s = Math.max(
      minCenterDistData,
      (cur?.afterExtent ?? 0) + (next?.beforeExtent ?? 0) + minGapData,
    );
    seps.push(s);
    totalSep += s;
  }

  const availSpan = Math.max(1, maxBound - minBound);
  const compress = totalSep > availSpan ? availSpan / totalSep : 1;
  const prefix: number[] = [0];
  for (let i = 0; i < seps.length; i++) {
    const prev = prefix[i] ?? 0;
    const step = (seps[i] ?? 0) * compress;
    seps[i] = step;
    prefix.push(prev + step);
  }

  const blocks: Array<{
    start: number;
    end: number;
    sum: number;
    count: number;
  }> = [];
  for (let i = 0; i < n; i++) {
    const target = (slots[i]?.desired ?? 0) - (prefix[i] ?? 0);
    blocks.push({start: i, end: i, sum: target, count: 1});
    while (blocks.length >= 2) {
      const b2 = blocks[blocks.length - 1];
      const b1 = blocks[blocks.length - 2];
      if (
        b1 === undefined ||
        b2 === undefined ||
        b1.sum / b1.count <= b2.sum / b2.count
      ) {
        break;
      }
      blocks.pop();
      b1.end = b2.end;
      b1.sum += b2.sum;
      b1.count += b2.count;
    }
  }

  const coords = new Array<number>(n).fill(0);
  for (const b of blocks) {
    const mean = b.sum / b.count;
    for (let i = b.start; i <= b.end; i++) {
      coords[i] = mean + (prefix[i] ?? 0);
    }
  }

  if ((coords[0] ?? 0) < minBound) {
    coords[0] = minBound;
    for (let i = 1; i < n; i++) {
      const minAllowed = (coords[i - 1] ?? minBound) + (seps[i - 1] ?? 0);
      if ((coords[i] ?? 0) < minAllowed) {
        coords[i] = minAllowed;
      }
    }
  }
  if ((coords[n - 1] ?? 0) > maxBound) {
    coords[n - 1] = maxBound;
    for (let i = n - 2; i >= 0; i--) {
      const maxAllowed = (coords[i + 1] ?? maxBound) - (seps[i] ?? 0);
      if ((coords[i] ?? 0) > maxAllowed) {
        coords[i] = maxAllowed;
      }
    }
  }

  for (let i = 0; i < n; i++) {
    const slot = slots[i];
    const c = coords[i];
    if (slot !== undefined && c !== undefined) {
      out.set(slot.id, c);
    }
  }
  return out;
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
  const layerOrientation: GraphLayerOrientation =
    config.layerOrientation ?? 'vertical';
  const isHorizontalLayered =
    effectiveLayout === 'layered' && layerOrientation === 'horizontal';
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
  const selfLoopBadgeCountById = new Map<string, number>();
  const parallelCount = new Map<string, number>();
  for (const e of data.edges) {
    if (e.source === e.target) {
      selfLoopBadgeCountById.set(
        e.source,
        (selfLoopBadgeCountById.get(e.source) ?? 0) + 1,
      );
    }
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

  // Resolve visual node sizes up front so collision avoidance accounts for radii.
  const isPositioned = effectiveLayout !== 'force';
  const resolvedX = new Map<string, number>();
  const resolvedY = new Map<string, number>();
  const resolvedSize = new Map<string, number>();
  for (const n of data.nodes) {
    const d = deg.get(n.id) ?? 0;
    const isGhost = n.highlight === 'ghost' || n.highlight === 'removed';
    const baseSize =
      n.size ??
      (isGhost
        ? 8
        : Math.min(
            MAX_AUTO_NODE_SIZE,
            Math.max(MIN_AUTO_NODE_SIZE, 8 + 2.2 * Math.sqrt(d)),
          ));
    resolvedSize.set(n.id, baseSize * (n.symbolSizeScale ?? 1));
  }

  const layers = data.layers ?? [];
  const hasLayerLabels = effectiveLayout === 'layered' && layers.length > 0;
  const preserveAspect =
    effectiveLayout === 'circular' || effectiveLayout === 'none';
  const padLeft = isHorizontalLayered ? 18 : hasLayerLabels ? 106 : 16;
  const padRight = isHorizontalLayered ? 18 : 12;
  const padTop = isHorizontalLayered ? 26 : 18;
  const padBottom = isHorizontalLayered ? 12 : 14;
  const vpW =
    config.viewportSize !== undefined && config.viewportSize.width > 0
      ? config.viewportSize.width
      : isHorizontalLayered
        ? 760
        : 440;
  const vpH =
    config.viewportSize !== undefined && config.viewportSize.height > 0
      ? config.viewportSize.height
      : isHorizontalLayered
        ? 280
        : 400;
  const viewW = Math.max(
    isHorizontalLayered ? 200 : 120,
    vpW - padLeft - padRight,
  );
  const viewH = Math.max(
    isHorizontalLayered ? 140 : 120,
    vpH - padTop - padBottom,
  );

  const layerIndexById = new Map<string | number, number>();
  const layerYById = new Map<string | number, number>();
  const layerXById = new Map<string | number, number>();
  const defaultCanvasH = Math.max(360, (layers.length - 1) * 40);
  const layerDenom = Math.max(1, layers.length - 1);

  const colCount = layers.length;
  const outerMarginX = Math.min(96, Math.max(54, viewW * 0.085));
  const usableColSpan = Math.max(80, viewW - 2 * outerMarginX);
  const colGapWeights: number[] = [];
  let totalColGapWeight = 0;
  for (let i = 0; i + 1 < colCount; i++) {
    const wLeft = layers[i]?.dimmed === true ? 0.4 : 1;
    const wRight = layers[i + 1]?.dimmed === true ? 0.4 : 1;
    const gw = (wLeft + wRight) / 2;
    colGapWeights.push(gw);
    totalColGapWeight += gw;
  }
  const unitColStep =
    totalColGapWeight <= 0
      ? 0
      : Math.min(230, usableColSpan / totalColGapWeight);
  const startColX = (viewW - unitColStep * totalColGapWeight) / 2;
  let minColGapPx = Number.POSITIVE_INFINITY;
  for (const gw of colGapWeights) {
    minColGapPx = Math.min(minColGapPx, gw * unitColStep);
  }
  const colHeaderY = 2;
  const laneTopY = 14;
  const laneBotY = Math.max(laneTopY + 40, viewH - 4);
  const colNodeMinY = laneTopY + 18;
  const colNodeMaxY = Math.max(colNodeMinY + 1, laneBotY - 10);

  let curColX = colCount <= 1 ? viewW / 2 : startColX;
  for (let i = 0; i < layers.length; i++) {
    const layer = layers[i];
    if (layer !== undefined) {
      layerIndexById.set(layer.id, i);
      if (isHorizontalLayered) {
        layerXById.set(layer.id, layer.x ?? curColX);
        curColX += (colGapWeights[i] ?? 0) * unitColStep;
      } else {
        layerYById.set(
          layer.id,
          layer.y ?? 30 + (i / layerDenom) * defaultCanvasH,
        );
      }
    }
  }

  if (effectiveLayout === 'layered') {
    const needsAutoOrder = isHorizontalLayered
      ? data.nodes.some(
          (n) => n.y === undefined && n.normPosition === undefined,
        )
      : data.nodes.some(
          (n) => n.x === undefined && n.normPosition === undefined,
        );
    const autoOrder = needsAutoOrder
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
      const autoNorm = Math.min(
        1,
        Math.max(0, n.normPosition ?? autoOrder?.normX.get(n.id) ?? 0.5),
      );
      if (isHorizontalLayered) {
        const fallbackX =
          n.layer !== undefined
            ? (layerXById.get(n.layer) ?? viewW / 2)
            : viewW / 2;
        resolvedX.set(n.id, n.x ?? fallbackX);
        resolvedY.set(
          n.id,
          n.y ?? colNodeMinY + autoNorm * (colNodeMaxY - colNodeMinY),
        );
      } else {
        resolvedX.set(
          n.id,
          n.x ??
            DEFAULT_GRAPH_LEFT_MARGIN_X + autoNorm * DEFAULT_GRAPH_CANVAS_W,
        );
        const fallbackY =
          n.layer !== undefined
            ? (layerYById.get(n.layer) ?? defaultCanvasH + 55)
            : 30;
        resolvedY.set(n.id, n.y ?? fallbackY);
      }
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
    (isHorizontalLayered
      ? {minX: 0, maxX: viewW, minY: 0, maxY: viewH}
      : effectiveLayout === 'layered' && layers.length > 0
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

  const dataW = Math.max(
    1,
    maxBoundX - (effectiveBounds?.minX ?? (Number.isFinite(minX) ? minX : 0)),
  );
  const dataH = Math.max(
    1,
    (effectiveBounds?.maxY ?? (Number.isFinite(maxY) ? maxY + 40 : 400)) -
      (effectiveBounds?.minY ?? (Number.isFinite(minY) ? minY : 0)),
  );
  const scaleX = preserveAspect
    ? Math.min(viewW / dataW, viewH / dataH)
    : viewW / dataW;
  const scaleY = preserveAspect ? scaleX : viewH / dataH;
  const pxToData = 1 / Math.max(0.05, scaleX);
  const pxToDataY = 1 / Math.max(0.05, scaleY);

  const preferredLoopSide = new Map<string, 1 | -1>();
  if (effectiveLayout === 'layered' && data.nodes.length > 0) {
    if (isHorizontalLayered) {
      const byCol = new Map<number, string[]>();
      for (const n of data.nodes) {
        const dx = resolvedX.get(n.id) ?? 0;
        if ((selfLoopBadgeCountById.get(n.id) ?? 0) > 0) {
          preferredLoopSide.set(n.id, dx + 28 * pxToData <= maxBoundX ? 1 : -1);
        }
        const xKey = Math.round(dx * 100);
        let col = byCol.get(xKey);
        if (col === undefined) {
          col = [];
          byCol.set(xKey, col);
        }
        col.push(n.id);
      }
      for (const colIds of byCol.values()) {
        colIds.sort(
          (a, b) =>
            (resolvedY.get(a) ?? 0) - (resolvedY.get(b) ?? 0) ||
            a.localeCompare(b),
        );
        if (colIds.length >= 2) {
          const slots: LinearSlot[] = colIds.map((id) => {
            const rPx = (resolvedSize.get(id) ?? MIN_AUTO_NODE_SIZE) / 2;
            return {
              id,
              desired: resolvedY.get(id) ?? colNodeMinY,
              beforeExtent: (rPx + 11) * pxToDataY,
              afterExtent: (rPx + 4) * pxToDataY,
            };
          });
          const adjusted = solveLinearPositions(
            slots,
            colNodeMinY,
            colNodeMaxY,
            MIN_NODE_GAP_PX * pxToDataY,
            MIN_NODE_CENTER_DIST_PX * pxToDataY,
          );
          for (const [id, y] of adjusted) {
            resolvedY.set(id, y);
          }
        }
      }
    } else {
      // Separate adjacent nodes sharing a layered row and orient self-loops toward
      // whichever side of the node has more horizontal clearance.
      const rowMinX = hasLayerLabels
        ? rowLabelX + Math.max(15, 12 * pxToData)
        : (effectiveBounds?.minX ?? (Number.isFinite(minX) ? minX : 0)) +
          10 * pxToData;
      const rowMaxX = Math.max(
        rowMinX + 1,
        maxBoundX - Math.max(12, 10 * pxToData),
      );
      const byRow = new Map<number, string[]>();
      for (const n of data.nodes) {
        const yKey = Math.round((resolvedY.get(n.id) ?? 0) * 100);
        let row = byRow.get(yKey);
        if (row === undefined) {
          row = [];
          byRow.set(yKey, row);
        }
        row.push(n.id);
      }

      for (const rowIds of byRow.values()) {
        rowIds.sort(
          (a, b) =>
            (resolvedX.get(a) ?? 0) - (resolvedX.get(b) ?? 0) ||
            a.localeCompare(b),
        );
        const slots: LinearSlot[] = rowIds.map((id, idx) => {
          const dx = resolvedX.get(id) ?? 0;
          const rPx = (resolvedSize.get(id) ?? MIN_AUTO_NODE_SIZE) / 2;
          const loopCount = selfLoopBadgeCountById.get(id) ?? 0;
          let side: 1 | -1 = dx + 26 * pxToData <= maxBoundX ? 1 : -1;
          if (loopCount > 0) {
            const prevId = idx > 0 ? rowIds[idx - 1] : undefined;
            const nextId =
              idx + 1 < rowIds.length ? rowIds[idx + 1] : undefined;
            const leftNeighborX =
              prevId !== undefined
                ? (resolvedX.get(prevId) ?? rowMinX)
                : rowMinX;
            const rightNeighborX =
              nextId !== undefined
                ? (resolvedX.get(nextId) ?? rowMaxX)
                : rowMaxX;
            const leftRoom = dx - leftNeighborX;
            const rightRoom = rightNeighborX - dx;
            const needRoom = (rPx + SELF_LOOP_CLEARANCE_PX + 6) * pxToData;
            if (rightRoom >= needRoom && dx + 26 * pxToData <= maxBoundX) {
              side = 1;
            } else if (
              leftRoom > rightRoom &&
              dx - 26 * pxToData >= rowLabelX + 4
            ) {
              side = -1;
            }
            preferredLoopSide.set(id, side);
          }
          const extraLeftPx =
            loopCount >= 2 || (loopCount === 1 && side === -1)
              ? SELF_LOOP_CLEARANCE_PX
              : 0;
          const extraRightPx =
            loopCount >= 2 || (loopCount === 1 && side === 1)
              ? SELF_LOOP_CLEARANCE_PX
              : 0;
          return {
            id,
            desired: dx,
            beforeExtent: (rPx + extraLeftPx) * pxToData,
            afterExtent: (rPx + extraRightPx) * pxToData,
          };
        });

        if (slots.length >= 2) {
          const adjusted = solveLinearPositions(
            slots,
            rowMinX,
            rowMaxX,
            MIN_NODE_GAP_PX * pxToData,
            MIN_NODE_CENTER_DIST_PX * pxToData,
          );
          for (const [id, x] of adjusted) {
            resolvedX.set(id, x);
          }
        }
      }
    }
  }

  const preDragX = new Map<string, number>(resolvedX);
  if (config.outBasePositions !== undefined) {
    config.outBasePositions.clear();
    if (isPositioned) {
      for (const n of data.nodes) {
        config.outBasePositions.set(n.id, {
          x: resolvedX.get(n.id) ?? 0,
          y: resolvedY.get(n.id) ?? 0,
        });
      }
    }
  }
  if (isPositioned && config.dragOffsets !== undefined) {
    const allIds = data.nodes.map((n) => n.id);
    for (const [id, offset] of config.dragOffsets) {
      const curX = resolvedX.get(id);
      const curY = resolvedY.get(id);
      if (curX !== undefined && curY !== undefined) {
        const clamped = clampNodeToMinDistance(
          id,
          curX + offset.dx,
          curY + offset.dy,
          allIds,
          resolvedX,
          resolvedY,
          resolvedSize,
          scaleX,
          scaleY,
        );
        resolvedX.set(id, clamped.x);
        resolvedY.set(id, clamped.y);
      }
    }
  }
  if (config.outLayoutState !== undefined) {
    const positions = new Map<
      string,
      {readonly x: number; readonly y: number}
    >();
    for (const n of data.nodes) {
      positions.set(n.id, {
        x: resolvedX.get(n.id) ?? 0,
        y: resolvedY.get(n.id) ?? 0,
      });
    }
    config.outLayoutState.current = {
      positions,
      sizes: new Map(resolvedSize),
      scaleX,
      scaleY,
    };
  }

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

  const guideNodes: Array<Record<string, unknown>> = [];
  const guideLinks: Array<Record<string, unknown>> = [];
  const labelAnchors =
    effectiveLayout === 'layered'
      ? layers.map((layer) => {
          const textColor = isDark
            ? (layer.color ?? colors.textColor)
            : (layer.lightTextColor ?? layer.color ?? colors.textColor);
          const opacity = layer.dimmed === true ? 0.38 : 1;
          if (isHorizontalLayered) {
            const cx = layerXById.get(layer.id) ?? viewW / 2;
            const topId = `col-top:${layer.id}`;
            const botId = `col-bot:${layer.id}`;
            guideNodes.push(
              {
                id: topId,
                name: '',
                x: cx,
                y: laneTopY,
                symbolSize: 0.1,
                draggable: false,
                silent: true,
                itemStyle: {color: 'transparent'},
                tooltip: {show: false},
                emphasis: {disabled: true},
                isAnchor: true,
              },
              {
                id: botId,
                name: '',
                x: cx,
                y: laneBotY,
                symbolSize: 0.1,
                draggable: false,
                silent: true,
                itemStyle: {color: 'transparent'},
                tooltip: {show: false},
                emphasis: {disabled: true},
                isAnchor: true,
              },
            );
            guideLinks.push({
              id: `col-guide:${layer.id}`,
              source: topId,
              target: botId,
              symbol: ['none', 'none'],
              silent: true,
              tooltip: {show: false},
              emphasis: {disabled: true},
              lineStyle: {
                color: colors.borderColor,
                type: 'dashed',
                width: 1,
                opacity: layer.dimmed === true ? 0.18 : 0.35,
                curveness: 0,
              },
            });
            return {
              id: `col-label:${layer.id}`,
              name: layer.label,
              x: cx,
              y: colHeaderY,
              symbol: 'circle',
              symbolSize: 0.1,
              draggable: false,
              silent: true,
              itemStyle: {color: 'transparent', borderColor: 'transparent'},
              tooltip: {show: false},
              emphasis: {disabled: true},
              label: {
                show: true,
                position: 'top',
                rotate:
                  colCount > 1 && minColGapPx < 55
                    ? 22
                    : colCount > 1 && minColGapPx < 105
                      ? 16
                      : 0,
                formatter: '{b}',
                color: textColor,
                opacity,
                fontWeight: 'bold',
                fontSize: colCount > 12 ? 9 : 9.5,
              },
              isAnchor: true,
            };
          }
          return {
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
              color: textColor,
              opacity,
              fontWeight: 'bold',
              fontSize: 9.5,
            },
            isAnchor: true,
          };
        })
      : [];

  const accentRingPath =
    'path://M 12 2 A 10 10 0 1 0 12 22 A 10 10 0 1 0 12 2 Z';
  const accentRingNodes: Array<Record<string, unknown>> = [];
  const selectedNodeIds = new Set<string>();
  const nodeLabelById = new Map<string, string>();
  const graphNodes = data.nodes.map((n) => {
    nodeLabelById.set(n.id, n.label);
    if (n.selected === true) {
      selectedNodeIds.add(n.id);
    }
    const d = deg.get(n.id) ?? 0;
    const isGhost = n.highlight === 'ghost' || n.highlight === 'removed';
    const size = resolvedSize.get(n.id) ?? MIN_AUTO_NODE_SIZE;
    const nx = resolvedX.get(n.id) ?? 0;
    const ny = resolvedY.get(n.id) ?? 0;

    if (isPositioned && n.accentRingColor !== undefined && !isGhost) {
      const ringId = `accent-ring:${n.id}`;
      accentRingNodes.push({
        id: ringId,
        name: '',
        x: nx,
        y: ny,
        symbol: accentRingPath,
        symbolKeepAspect: true,
        symbolSize: size + 8,
        draggable: false,
        silent: true,
        itemStyle: {
          color: 'transparent',
          borderColor: n.accentRingColor,
          borderWidth: 1.8,
          opacity: 0.9,
        },
        tooltip: {show: false},
        emphasis: {disabled: true},
        isAnchor: true,
      });
      guideLinks.push({
        id: `${ringId}:adj`,
        source: n.id,
        target: ringId,
        symbol: ['none', 'none'],
        silent: true,
        lineStyle: {opacity: 0, width: 0},
        tooltip: {show: false},
        emphasis: {disabled: true},
      });
    }

    if (
      isPositioned &&
      n.selected === true &&
      n.symbol !== undefined &&
      !isGhost
    ) {
      const selRingId = `selected-ring:${n.id}`;
      const extraPad = n.accentRingColor !== undefined ? 12 : 7;
      accentRingNodes.push({
        id: selRingId,
        name: '',
        x: nx,
        y: ny,
        symbol: accentRingPath,
        symbolKeepAspect: true,
        symbolSize: size + extraPad,
        draggable: false,
        silent: true,
        itemStyle: {
          color: 'transparent',
          borderColor: selectedBorder,
          borderWidth: 2,
          opacity: 0.95,
        },
        tooltip: {show: false},
        emphasis: {disabled: true},
        isAnchor: true,
      });
      guideLinks.push({
        id: `${selRingId}:adj`,
        source: n.id,
        target: selRingId,
        symbol: ['none', 'none'],
        silent: true,
        lineStyle: {opacity: 0, width: 0},
        tooltip: {show: false},
        emphasis: {disabled: true},
      });
    }

    const selfCount = selfLoopWeightById.get(n.id) ?? 0;
    const displayLabel =
      showSelfLoopCount && selfCount > 0 && !isGhost
        ? `${n.label} ↻${selfCount}`
        : n.label;

    let borderColor = n.borderColor ?? defaultBorder;
    let borderWidth = n.borderWidth ?? 1.5;
    let borderType: 'solid' | 'dashed' = 'solid';
    if (isGhost) {
      borderColor = removedColor;
      borderWidth = n.symbol !== undefined ? (n.borderWidth ?? 1.6) : 2;
      borderType = 'dashed';
    } else if (n.highlight === 'changed') {
      borderColor = changedColor;
      borderWidth = n.symbol !== undefined ? (n.borderWidth ?? 1.6) : 3;
    } else if (n.highlight === 'added') {
      borderColor = addedColor;
      borderWidth = n.symbol !== undefined ? (n.borderWidth ?? 1.6) : 3;
    }
    if (n.selected === true && n.symbol === undefined) {
      borderColor = selectedBorder;
      borderWidth = 3;
    }

    const labelColor = isGhost
      ? removedColor
      : n.highlight === 'changed'
        ? changedColor
        : n.highlight === 'added'
          ? addedColor
          : colors.textColor;

    const showLabel =
      n.showLabel ??
      (d > 0 ||
        n.highlight !== undefined ||
        n.selected === true ||
        n.labelBold === true ||
        !isPositioned);

    return {
      id: n.id,
      nodeId: n.id,
      name: displayLabel,
      ...(isPositioned ? {x: nx, y: ny} : {}),
      ...(n.symbol !== undefined ? {symbol: n.symbol} : {}),
      ...(n.symbolKeepAspect !== undefined
        ? {symbolKeepAspect: n.symbolKeepAspect}
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
        ...(n.labelBold === true ? {fontWeight: 'bold' as const} : {}),
        padding: [1, 3],
        color: labelColor,
        textBorderColor: colors.backgroundColor,
        textBorderWidth: 2.5,
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

  // Order larger nodes first so smaller and selected nodes always paint above
  // high-degree hubs and remain hoverable/clickable.
  graphNodes.sort((a, b) => {
    const aSel = selectedNodeIds.has(a.id);
    const bSel = selectedNodeIds.has(b.id);
    if (aSel !== bSel) {
      return aSel ? 1 : -1;
    }
    return b.symbolSize - a.symbolSize;
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
    const layoutX = preDragX.get(nodeId) ?? baseX;
    const nodeRadius = (resolvedSize.get(nodeId) ?? MIN_AUTO_NODE_SIZE) / 2;

    const primarySide: 1 | -1 =
      preferredLoopSide.get(nodeId) ?? (layoutX + 26 <= maxBoundX ? 1 : -1);
    let side: 1 | -1 = primarySide;
    let loopSize = 20;
    let offsetDist = nodeRadius + 6.5;
    if (slot === 1) {
      const oppositeSide: 1 | -1 = primarySide === 1 ? -1 : 1;
      const canUseOpposite =
        oppositeSide < 0
          ? layoutX - 26 >= rowLabelX + 4
          : layoutX + 26 <= maxBoundX;
      if (canUseOpposite) {
        side = oppositeSide;
      } else {
        loopSize = 26;
        offsetDist = nodeRadius + 9.5;
      }
    } else if (slot >= 2) {
      loopSize = 26;
      offsetDist = nodeRadius + 9.5 + (slot - 1) * 4;
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
                repulsion: 220,
                edgeLength: [65, 140],
                layoutAnimation: false,
              },
            }
          : preserveAspect
            ? {
                preserveAspect: true,
                preserveAspectAlign: 'center',
              }
            : {}),
        left: padLeft,
        right: padRight,
        top: padTop,
        bottom: padBottom,
        roam: true,
        scaleLimit: {min: ROAM_MIN_ZOOM, max: ROAM_MAX_ZOOM},
        nodeScaleRatio,
        labelLayout: isHorizontalLayered
          ? (params: {
              labelRect?: {x: number; y: number; width: number; height: number};
            }) => {
              const rect = params.labelRect;
              if (rect === undefined) return {hideOverlap: true};
              let nx = rect.x;
              if (nx < 6) {
                nx = 6;
              } else if (nx + rect.width > vpW - 6) {
                nx = Math.max(6, vpW - 6 - rect.width);
              }
              return nx !== rect.x
                ? {x: nx, align: 'left' as const, hideOverlap: true}
                : {hideOverlap: true};
            }
          : {hideOverlap: true},
        edgeSymbol: ['none', 'arrow'],
        edgeSymbolSize: [0, 6.5],
        emphasis: {
          focus: 'adjacency',
          label: {show: true},
          lineStyle: {width: 3, opacity: 1},
        },
        data: [
          ...pinNodes,
          ...guideNodes,
          ...labelAnchors,
          ...accentRingNodes,
          ...loopBadgeNodes,
          ...graphNodes,
        ],
        links: [...guideLinks, ...graphLinks],
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
  private lastLayout?: GraphChartLayout;
  private lastLayerOrientation?: GraphLayerOrientation;
  private lastThemeBg?: string;
  private chartApi?: EChartApi;
  private canvasDom?: HTMLElement;
  private resizeObserver?: Disposable;
  private lastObservedSize?: {readonly width: number; readonly height: number};
  private pointerInside = false;
  private focusedByPointer = false;
  private hoverOffset?: {readonly x: number; readonly y: number};
  private docKeyListener?: Disposable;
  private keyboardEnabled = true;
  private dragOffsets = new Map<string, GraphDragOffset>();
  private basePositions = new Map<
    string,
    {readonly x: number; readonly y: number}
  >();
  private layoutState: {current?: GraphLayoutRuntimeState} = {};
  private companionNodeIds = new Set<string>();
  private pendingDragNodeId?: string;
  private dragStartState?: {
    readonly el: EChartZrElement;
    readonly startX: number;
    readonly startY: number;
  };
  private activeDrag?: {
    readonly nodeId: string;
    readonly el: EChartZrElement;
    readonly badgeEls: ReadonlyArray<EChartZrElement>;
  };

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
    this.resizeObserver?.[Symbol.dispose]();
    this.resizeObserver = undefined;
    this.lastObservedSize = undefined;
    this.chartApi = undefined;
    this.canvasDom = undefined;
    this.activeDrag = undefined;
    this.dragStartState = undefined;
    this.pendingDragNodeId = undefined;
  }

  private bindDragSync(api: EChartApi): void {
    // ZRender's Draggable._dragStart runs before ECharts' mousedown listener,
    // so capture the starting Symbol coords on dragstart, the nodeId on
    // mousedown, and pair them lazily on the first drag frame.
    api.on('mousedown', (rawParams: unknown) => {
      const params = rawParams as EChartClickParams;
      const parsed = parseGraphClickData(params.data);
      this.pendingDragNodeId =
        params.dataType === 'node' &&
        parsed !== undefined &&
        parsed.isAnchor !== true
          ? parsed.nodeId
          : undefined;
    });
    api.onZr('dragstart', (ev) => {
      this.activeDrag = undefined;
      this.pendingDragNodeId = undefined;
      const el = ev.target?.parent;
      this.dragStartState =
        el !== undefined && typeof el.x === 'number' && typeof el.y === 'number'
          ? {el, startX: el.x, startY: el.y}
          : undefined;
    });
    api.onZr('drag', () => {
      if (this.activeDrag === undefined) {
        const start = this.dragStartState;
        const nodeId = this.pendingDragNodeId;
        if (start === undefined || nodeId === undefined) {
          return;
        }
        const badgeEls: EChartZrElement[] = [];
        if (this.companionNodeIds.has(nodeId)) {
          const siblings = start.el.parent?.children?.() ?? [];
          for (const sib of siblings) {
            if (
              sib !== start.el &&
              sib.getSymbolType?.()?.startsWith('path://') === true &&
              Math.abs(sib.x - start.startX) < 0.05 &&
              Math.abs(sib.y - start.startY) < 0.05
            ) {
              badgeEls.push(sib);
            }
          }
        }
        this.activeDrag = {nodeId, el: start.el, badgeEls};
      }
      const {el, badgeEls} = this.activeDrag;
      for (const badgeEl of badgeEls) {
        badgeEl.stopAnimation?.();
        badgeEl.setPosition?.([el.x, el.y]);
        badgeEl.markRedraw?.();
      }
    });
    api.onZr('dragend', () => {
      const drag = this.activeDrag;
      this.activeDrag = undefined;
      this.dragStartState = undefined;
      this.pendingDragNodeId = undefined;
      if (drag === undefined) {
        return;
      }
      const basePos = this.basePositions.get(drag.nodeId);
      if (basePos === undefined) {
        return;
      }
      let finalX = drag.el.x;
      let finalY = drag.el.y;
      const rt = this.layoutState.current;
      if (rt !== undefined) {
        const posX = new Map<string, number>();
        const posY = new Map<string, number>();
        for (const [id, p] of rt.positions) {
          posX.set(id, p.x);
          posY.set(id, p.y);
        }
        const clamped = clampNodeToMinDistance(
          drag.nodeId,
          finalX,
          finalY,
          rt.positions.keys(),
          posX,
          posY,
          rt.sizes,
          rt.scaleX,
          rt.scaleY,
        );
        finalX = clamped.x;
        finalY = clamped.y;
        drag.el.setPosition?.([finalX, finalY]);
        drag.el.markRedraw?.();
        for (const badgeEl of drag.badgeEls) {
          badgeEl.setPosition?.([finalX, finalY]);
          badgeEl.markRedraw?.();
        }
      }
      this.dragOffsets.set(drag.nodeId, {
        dx: finalX - basePos.x,
        dy: finalY - basePos.y,
      });
      this.zoomOnlyUpdate = true;
      m.redraw();
    });
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
      layerOrientation = 'vertical',
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
      this.dragOffsets.clear();
      this.zoomOnlyUpdate = true;
    }
    this.lastResetToken = resetToken;
    if (this.lastLayout !== undefined && this.lastLayout !== layout) {
      this.dragOffsets.clear();
    }
    this.lastLayout = layout;
    if (
      this.lastLayerOrientation !== undefined &&
      this.lastLayerOrientation !== layerOrientation
    ) {
      this.dragOffsets.clear();
    }
    this.lastLayerOrientation = layerOrientation;

    this.companionNodeIds.clear();
    if (data !== undefined) {
      for (const e of data.edges) {
        if (e.source === e.target) {
          this.companionNodeIds.add(e.source);
        }
      }
      for (const n of data.nodes) {
        if (
          n.accentRingColor !== undefined ||
          (n.selected === true && n.symbol !== undefined)
        ) {
          this.companionNodeIds.add(n.id);
        }
      }
    }

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
          const el = toHTMLElement(dom);
          this.canvasDom = el;
          this.resizeObserver?.[Symbol.dispose]();
          this.resizeObserver = new SimpleResizeObserver(el, () => {
            const w = el.clientWidth;
            const h = el.clientHeight;
            if (w <= 0 || h <= 0) return;
            const prev = this.lastObservedSize;
            if (
              prev === undefined ||
              Math.abs(w - prev.width) >= 2 ||
              Math.abs(h - prev.height) >= 2
            ) {
              this.lastObservedSize = {width: w, height: h};
              requestAnimationFrame(() =>
                requestAnimationFrame(() => {
                  this.zoomOnlyUpdate = true;
                  m.redraw();
                }),
              );
            }
          });
        },
        'onremove': () => {
          this.resizeObserver?.[Symbol.dispose]();
          this.resizeObserver = undefined;
          this.lastObservedSize = undefined;
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
          if (api !== undefined) {
            this.bindDragSync(api);
          }
        },
        resolveOption: (_opt, colors, size) => {
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
          const liveSize =
            this.canvasDom !== undefined &&
            this.canvasDom.clientWidth > 0 &&
            this.canvasDom.clientHeight > 0
              ? {
                  width: this.canvasDom.clientWidth,
                  height: this.canvasDom.clientHeight,
                }
              : size;
          return buildGraphChartOption(data, colors, {
            layout,
            layerOrientation,
            nodeScaleRatio: effectiveNodeScaleRatio(this.roamZoom),
            animateUpdates: animateThisFrame,
            showSelfLoopCountOnLabel,
            viewportSize: liveSize,
            dragOffsets: this.dragOffsets,
            outBasePositions: this.basePositions,
            outLayoutState: this.layoutState,
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
