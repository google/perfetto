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
import {PopupPosition} from '../../../widgets/popup';
import {CursorTooltip} from '../../../widgets/cursor_tooltip';
import './flamegraph_chart.scss';

const TOOLTIP_OFFSET_PX = 12;

export interface FlamegraphChartSegment {
  readonly name: string;
  readonly value: number;
  readonly cssColor: string;
  readonly children: readonly FlamegraphChartSegment[];
  // Optional rich tooltip body. When omitted a default tooltip is shown with
  // the name, the formatted value and the share of the root.
  readonly tooltip?: m.Children;
}

export interface FlamegraphChartAttrs {
  readonly data: FlamegraphChartSegment;
  // Formats a segment's value for labels and the default tooltip. Defaults to
  // String(value) — pass e.g. formatBytes for a memory breakdown.
  readonly formatValue?: (value: number) => string;
  // Height of a single row, in px. Defaults to ROW_HEIGHT.
  readonly rowHeight?: number;
  // Invoked when a segment is clicked.
  readonly onSegmentClick?: (seg: FlamegraphChartSegment) => void;
  // When true, the root segment is not drawn: its children become the top row
  // (spanning the full width, since they already sum to the root). Useful when
  // the root is just a redundant total. Tooltip shares stay relative to the
  // (hidden) root.
  readonly hideRoot?: boolean;
}

const ROW_HEIGHT = 26;
const ROW_GAP = 3;
// Horizontal gap (px) shaved off each segment's right edge, so adjacent
// segments are visually separated. Kept small; thin segments fall back to the
// CSS min-width.
const SEG_GAP = 3;
// Below this width fraction (of the whole chart) a segment renders no label —
// a sliver of "…" is just noise. The tooltip still works.
const MIN_LABEL_FRACTION = 0.04;

// A segment placed in the layout: fractions are 0..1 of the chart width.
interface PlacedSegment {
  readonly seg: FlamegraphChartSegment;
  readonly depth: number;
  readonly xFrac: number; // left edge, fraction of chart width
  readonly widthFrac: number; // width, fraction of chart width (== share of root)
}

// Recursively lays the tree out left-to-right. Each node spans its share of its
// parent's width; the root spans the full width, so a node's widthFrac is also
// its share of the root total.
// Children are distributed proportionally to their sum — if they don't sum to
// the parent's value, the remaining width is left empty (useful for showing
// "unaccounted" portions in profiles).
function placeSegments(
  seg: FlamegraphChartSegment,
  depth: number,
  xFrac: number,
  widthFrac: number,
  out: PlacedSegment[],
): void {
  out.push({seg, depth, xFrac, widthFrac});

  const childTotal = seg.children.reduce((s, c) => s + c.value, 0);
  if (childTotal <= 0) return;
  let childX = xFrac;
  for (const child of seg.children) {
    const childWidth = (child.value / childTotal) * widthFrac;
    placeSegments(child, depth + 1, childX, childWidth, out);
    childX += childWidth;
  }
}

// Parses a #rgb / #rrggbb colour. Returns undefined for anything else (e.g. a
// gradient or a var()), in which case we fall back to a neutral label colour.
function parseHexColor(c: string): [number, number, number] | undefined {
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i.exec(c);
  if (short) {
    return [
      parseInt(short[1] + short[1], 16),
      parseInt(short[2] + short[2], 16),
      parseInt(short[3] + short[3], 16),
    ];
  }
  const long = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c);
  if (long) {
    return [
      parseInt(long[1], 16),
      parseInt(long[2], 16),
      parseInt(long[3], 16),
    ];
  }
  return undefined;
}

// Picks a dark or light label colour for legibility against `bg`.
function labelColor(bg: string): string {
  const rgb = parseHexColor(bg);
  if (rgb === undefined) return 'rgba(0, 0, 0, 0.82)';
  const [r, g, b] = rgb;
  const luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
  return luminance > 0.6 ? 'rgba(0, 0, 0, 0.82)' : 'rgba(255, 255, 255, 0.95)';
}

function defaultTooltip(
  seg: FlamegraphChartSegment,
  shareFrac: number,
  formatValue: (v: number) => string,
): m.Children {
  return [
    m('.pf-flamechart-tooltip__name', seg.name),
    m(
      '.pf-flamechart-tooltip__value',
      `${formatValue(seg.value)} · ${(shareFrac * 100).toFixed(1)}%`,
    ),
  ];
}

function renderSegment(
  placed: PlacedSegment,
  index: number,
  attrs: FlamegraphChartAttrs,
  onHover: (index: number) => void,
  onUnhover: (index: number) => void,
): m.Children {
  const {seg, depth, xFrac, widthFrac} = placed;
  const {formatValue = String, rowHeight = ROW_HEIGHT, onSegmentClick} = attrs;

  const showLabel = widthFrac >= MIN_LABEL_FRACTION;

  return m(
    '.pf-flamechart-segment',
    {
      style: {
        top: `${depth * (rowHeight + ROW_GAP)}px`,
        left: `${xFrac * 100}%`,
        width: `calc(${widthFrac * 100}% - ${SEG_GAP}px)`,
        height: `${rowHeight}px`,
        background: seg.cssColor,
        color: labelColor(seg.cssColor),
        cursor: onSegmentClick !== undefined ? 'pointer' : 'default',
      },
      onclick:
        onSegmentClick !== undefined ? () => onSegmentClick(seg) : undefined,
      onmouseenter: () => onHover(index),
      onmouseleave: () => onUnhover(index),
    },
    showLabel &&
      m('.pf-flamechart-segment__label', [
        m('span.pf-flamechart-segment__name', seg.name),
        m('span.pf-flamechart-segment__value', formatValue(seg.value)),
      ]),
  );
}

export function FlamegraphChart(): m.Component<FlamegraphChartAttrs> {
  // Index (into `placed`) of the hovered segment, or undefined. Tracked by
  // index rather than object identity because `placed` is rebuilt every redraw.
  let hovered: number | undefined;

  return {
    view({attrs}: m.Vnode<FlamegraphChartAttrs>) {
      const {rowHeight = ROW_HEIGHT, formatValue = String} = attrs;

      const all: PlacedSegment[] = [];
      placeSegments(attrs.data, 0, 0, 1, all);
      // Optionally drop the root row and pull every level up one. The root's
      // children already span [0, 1], so the breakdown fills the full width.
      const placed = attrs.hideRoot
        ? all
            .filter((p) => p.depth > 0)
            .map((p) => ({...p, depth: p.depth - 1}))
        : all;

      const maxDepth = placed.reduce((d, p) => Math.max(d, p.depth), 0) + 1;
      const height = maxDepth * (rowHeight + ROW_GAP) - ROW_GAP;

      const hoveredSeg =
        hovered !== undefined && hovered < placed.length
          ? placed[hovered]
          : undefined;

      return m(
        '.pf-flamechart',
        {style: {height: `${height}px`}},
        placed.map((p, i) =>
          renderSegment(
            p,
            i,
            attrs,
            (idx) => (hovered = idx),
            (idx) => {
              if (hovered === idx) hovered = undefined;
            },
          ),
        ),
        hoveredSeg !== undefined &&
          m(
            CursorTooltip,
            {
              position: PopupPosition.BottomStart,
              offset: TOOLTIP_OFFSET_PX,
              skidOffset: TOOLTIP_OFFSET_PX,
            },
            m(
              '.pf-flamechart-tooltip',
              hoveredSeg.seg.tooltip ??
                defaultTooltip(
                  hoveredSeg.seg,
                  hoveredSeg.widthFrac,
                  formatValue,
                ),
            ),
          ),
      );
    },
  };
}
