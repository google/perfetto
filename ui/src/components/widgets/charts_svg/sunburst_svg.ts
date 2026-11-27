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

// Pure SVG interactive Sunburst Chart widget.
// Renders hierarchical proportional data as concentric radial rings around a
// live readout donut center, with ancestor-lineage hover highlighting, rich
// cursor tooltips, multi-level click-to-zoom subtree exploration, and a
// synchronized breakdown legend.

import m from 'mithril';
import {classNames} from '../../../base/classnames';
import {CursorTooltip} from '../../../widgets/cursor_tooltip';
import {Icon} from '../../../widgets/icon';
import {PopupPosition} from '../../../widgets/popup';
import './sunburst_svg.scss';

const TOOLTIP_OFFSET_PX = 14;
const TWO_PI = Math.PI * 2;
const START_ANGLE = -Math.PI / 2; // 12 o'clock
const FULL_CIRCLE_EPS = 1e-4;
const RING_GAP_PX = 2.5;
const MIN_LABEL_ANGLE = 0.34;
const MIN_LABEL_ARC_PX = 48;

export interface SunburstSegment {
  // Optional stable key to disambiguate siblings that share the same display
  // name. Falls back to `name`.
  readonly key?: string;
  readonly name: string;
  readonly value: number;
  readonly cssColor: string;
  readonly children: readonly SunburstSegment[];
  // Optional signed delta badge displayed inline in the synchronized legend
  // (e.g. when comparing two snapshots).
  readonly delta?: m.Children;
  // Optional rich tooltip body. When omitted a default tooltip is shown with
  // the name, formatted value, and share of the current root.
  readonly tooltip?: m.Children;
}

export interface SunburstSvgAttrs {
  readonly data: SunburstSegment;
  // Formats a segment's value for labels, center readout, and default tooltip.
  readonly formatValue?: (value: number) => string;
  // Diameter of the SVG sunburst in px (default: 350).
  readonly size?: number;
  // Maximum number of concentric rings rendered at once (default: 4). Deeper
  // children remain reachable by clicking a parent slice to zoom in.
  readonly maxVisibleDepth?: number;
  // Invoked when a segment is clicked.
  readonly onSegmentClick?: (seg: SunburstSegment) => void;
  // When true (default), clicking a segment with children zooms into it and
  // clicking the center donut steps back up one level.
  readonly zoomable?: boolean;
  // Show a synchronized interactive breakdown list beside the sunburst
  // (default: true).
  readonly showLegend?: boolean;
  readonly className?: string;
}

interface PlacedArc {
  readonly id: string;
  readonly parentId?: string;
  readonly seg: SunburstSegment;
  readonly depth: number; // 1-based ring index relative to current focus root
  readonly startAngle: number;
  readonly endAngle: number;
  readonly shareOfRoot: number;
  readonly pathKeys: readonly string[];
  readonly ancestorIds: ReadonlySet<string>;
  readonly hasHiddenChildren: boolean;
}

function segmentKey(seg: SunburstSegment, index: number, dup: boolean): string {
  const base = seg.key ?? seg.name;
  return dup ? `${base}#${index}` : base;
}

function collectArcs(
  node: SunburstSegment,
  depth: number,
  maxDepth: number,
  startAngle: number,
  endAngle: number,
  rootTotal: number,
  parentId: string | undefined,
  parentPathKeys: readonly string[],
  ancestorIds: ReadonlySet<string>,
  out: PlacedArc[],
): void {
  if (depth > maxDepth) return;
  const childTotal = node.children.reduce(
    (s, c) => s + Math.max(0, c.value),
    0,
  );
  if (childTotal <= 0) return;

  // Detect duplicate raw keys among siblings so every placed arc gets a unique
  // key/path even if callers pass two slices with the same label.
  const keyCounts = new Map<string, number>();
  for (const c of node.children) {
    const k = c.key ?? c.name;
    keyCounts.set(k, (keyCounts.get(k) ?? 0) + 1);
  }

  const denom = node.value > 0 ? Math.max(node.value, childTotal) : childTotal;
  const span = endAngle - startAngle;
  let angle = startAngle;
  for (let i = 0; i < node.children.length; i++) {
    const child = node.children[i];
    if (child === undefined || child.value <= 0) continue;
    const sweep = (child.value / denom) * span;
    const nextAngle = angle + sweep;
    const rawKey = child.key ?? child.name;
    const stepKey = segmentKey(child, i, (keyCounts.get(rawKey) ?? 0) > 1);
    const pathKeys = [...parentPathKeys, stepKey];
    const id = pathKeys.join('\u001f');
    const nextAncestors = new Set(ancestorIds);
    nextAncestors.add(id);
    const hasPositiveChildren = child.children.some((c) => c.value > 0);
    const hasHiddenChildren = depth === maxDepth && hasPositiveChildren;

    out.push({
      id,
      parentId,
      seg: child,
      depth,
      startAngle: angle,
      endAngle: nextAngle,
      shareOfRoot: rootTotal > 0 ? child.value / rootTotal : 0,
      pathKeys,
      ancestorIds: nextAncestors,
      hasHiddenChildren,
    });

    if (depth < maxDepth) {
      collectArcs(
        child,
        depth + 1,
        maxDepth,
        angle,
        nextAngle,
        rootTotal,
        id,
        pathKeys,
        nextAncestors,
        out,
      );
    }
    angle = nextAngle;
  }
}

function polarToCartesian(
  cx: number,
  cy: number,
  r: number,
  angle: number,
): {x: number; y: number} {
  return {
    x: cx + r * Math.cos(angle),
    y: cy + r * Math.sin(angle),
  };
}

// Builds an SVG path for an annular sector (wedge) between [r0, r1] and
// [startAngle, endAngle]. Splits near-360° rings into two 180° arcs so SVG
// arc rendering never collapses when start and end points coincide.
function annularArcPath(
  cx: number,
  cy: number,
  r0: number,
  r1: number,
  startAngle: number,
  endAngle: number,
): string {
  const sweep = endAngle - startAngle;
  if (sweep >= TWO_PI - FULL_CIRCLE_EPS) {
    const mid = startAngle + Math.PI;
    const o1 = polarToCartesian(cx, cy, r1, startAngle);
    const o2 = polarToCartesian(cx, cy, r1, mid);
    const i1 = polarToCartesian(cx, cy, r0, startAngle);
    const i2 = polarToCartesian(cx, cy, r0, mid);
    return [
      `M ${o1.x.toFixed(2)} ${o1.y.toFixed(2)}`,
      `A ${r1.toFixed(2)} ${r1.toFixed(2)} 0 1 1 ${o2.x.toFixed(2)} ${o2.y.toFixed(2)}`,
      `A ${r1.toFixed(2)} ${r1.toFixed(2)} 0 1 1 ${o1.x.toFixed(2)} ${o1.y.toFixed(2)}`,
      `M ${i1.x.toFixed(2)} ${i1.y.toFixed(2)}`,
      `A ${r0.toFixed(2)} ${r0.toFixed(2)} 0 1 0 ${i2.x.toFixed(2)} ${i2.y.toFixed(2)}`,
      `A ${r0.toFixed(2)} ${r0.toFixed(2)} 0 1 0 ${i1.x.toFixed(2)} ${i1.y.toFixed(2)}`,
      'Z',
    ].join(' ');
  }

  const largeArc = sweep > Math.PI ? 1 : 0;
  const pOuterStart = polarToCartesian(cx, cy, r1, startAngle);
  const pOuterEnd = polarToCartesian(cx, cy, r1, endAngle);
  const pInnerEnd = polarToCartesian(cx, cy, r0, endAngle);
  const pInnerStart = polarToCartesian(cx, cy, r0, startAngle);

  return [
    `M ${pOuterStart.x.toFixed(2)} ${pOuterStart.y.toFixed(2)}`,
    `A ${r1.toFixed(2)} ${r1.toFixed(2)} 0 ${largeArc} 1 ${pOuterEnd.x.toFixed(2)} ${pOuterEnd.y.toFixed(2)}`,
    `L ${pInnerEnd.x.toFixed(2)} ${pInnerEnd.y.toFixed(2)}`,
    `A ${r0.toFixed(2)} ${r0.toFixed(2)} 0 ${largeArc} 0 ${pInnerStart.x.toFixed(2)} ${pInnerStart.y.toFixed(2)}`,
    'Z',
  ].join(' ');
}

function parseHexColor(c: string): [number, number, number] | undefined {
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i.exec(c);
  if (short !== null) {
    const r = short[1] ?? '0';
    const g = short[2] ?? '0';
    const b = short[3] ?? '0';
    return [parseInt(r + r, 16), parseInt(g + g, 16), parseInt(b + b, 16)];
  }
  const long = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c);
  if (long !== null) {
    return [
      parseInt(long[1] ?? '00', 16),
      parseInt(long[2] ?? '00', 16),
      parseInt(long[3] ?? '00', 16),
    ];
  }
  return undefined;
}

function arcLabelFill(bg: string): string {
  const rgb = parseHexColor(bg);
  if (rgb === undefined) return 'rgba(0, 0, 0, 0.85)';
  const [r, g, b] = rgb;
  const luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
  return luminance > 0.62 ? 'rgba(0, 0, 0, 0.84)' : 'rgba(255, 255, 255, 0.96)';
}

function truncateLabel(text: string, maxChars: number): string {
  if (text.length <= maxChars) return text;
  if (maxChars <= 3) return '';
  return `${text.slice(0, maxChars - 1)}…`;
}

// Walks a saved zoom path (list of child keys) against the latest data tree.
// Falls back to the deepest still-matching ancestor if data changed.
function resolveZoomTrail(
  root: SunburstSegment,
  zoomPath: readonly string[],
): {readonly trail: SunburstSegment[]; readonly focus: SunburstSegment} {
  const trail: SunburstSegment[] = [root];
  let cur = root;
  for (const targetKey of zoomPath) {
    const keyCounts = new Map<string, number>();
    for (const c of cur.children) {
      const k = c.key ?? c.name;
      keyCounts.set(k, (keyCounts.get(k) ?? 0) + 1);
    }
    let next: SunburstSegment | undefined;
    for (let i = 0; i < cur.children.length; i++) {
      const child = cur.children[i];
      if (child === undefined) continue;
      const rawKey = child.key ?? child.name;
      const stepKey = segmentKey(child, i, (keyCounts.get(rawKey) ?? 0) > 1);
      if (stepKey === targetKey || rawKey === targetKey) {
        next = child;
        break;
      }
    }
    if (next === undefined || next.children.length === 0) break;
    trail.push(next);
    cur = next;
  }
  return {trail, focus: cur};
}

function defaultTooltip(
  seg: SunburstSegment,
  shareFrac: number,
  formatValue: (v: number) => string,
): m.Children {
  return [
    m('.pf-sunburst-tooltip__name', seg.name),
    m(
      '.pf-sunburst-tooltip__value',
      `${formatValue(seg.value)} · ${(shareFrac * 100).toFixed(1)}%`,
    ),
  ];
}

export class SunburstSvg implements m.ClassComponent<SunburstSvgAttrs> {
  private hoveredId?: string;
  private zoomPath: string[] = [];

  view({attrs}: m.Vnode<SunburstSvgAttrs>): m.Children {
    const {
      data,
      formatValue = String,
      size = 350,
      maxVisibleDepth = 4,
      onSegmentClick,
      zoomable = true,
      showLegend = true,
      className,
    } = attrs;

    const {trail, focus} = zoomable
      ? resolveZoomTrail(data, this.zoomPath)
      : {trail: [data], focus: data};

    const focusTotal = focus.value;
    const arcs: PlacedArc[] = [];
    collectArcs(
      focus,
      1,
      Math.max(1, maxVisibleDepth),
      START_ANGLE,
      START_ANGLE + TWO_PI,
      focusTotal,
      undefined,
      [],
      new Set(),
      arcs,
    );

    const maxDepth = Math.max(
      1,
      arcs.reduce((d, a) => Math.max(d, a.depth), 0),
    );

    const cx = size / 2;
    const cy = size / 2;
    const outerR = size / 2 - 6;
    const innerHoleR = Math.round(outerR * 0.3);
    const ringSpan = (outerR - innerHoleR) / maxDepth;

    const hoveredArc =
      this.hoveredId !== undefined
        ? arcs.find((a) => a.id === this.hoveredId)
        : undefined;

    const isZoomed = trail.length > 1;
    const centerSeg = hoveredArc?.seg ?? focus;
    const centerShare =
      hoveredArc !== undefined
        ? hoveredArc.shareOfRoot
        : data.value > 0
          ? focus.value / data.value
          : 1;

    const zoomInto = (pathKeys: readonly string[], seg: SunburstSegment) => {
      if (zoomable && seg.children.some((c) => c.value > 0)) {
        this.zoomPath = [
          ...this.zoomPath.slice(0, trail.length - 1),
          ...pathKeys,
        ];
        this.hoveredId = undefined;
      }
      onSegmentClick?.(seg);
    };

    const stepOut = () => {
      if (!isZoomed) return;
      this.zoomPath = this.zoomPath.slice(0, trail.length - 2);
      this.hoveredId = undefined;
    };

    // Legend displays the focus node's immediate children (and their top
    // sub-children) sorted largest-first, linked bidirectionally with the SVG.
    const legendRoots = arcs
      .filter((a) => a.depth === 1)
      .slice()
      .sort((a, b) => b.seg.value - a.seg.value);

    const childrenByParentId = new Map<string, PlacedArc[]>();
    for (const a of arcs) {
      if (a.depth !== 2 || a.parentId === undefined) continue;
      const list = childrenByParentId.get(a.parentId) ?? [];
      list.push(a);
      childrenByParentId.set(a.parentId, list);
    }

    return m('.pf-sunburst', {className}, [
      // Zoom breadcrumb trail (shown when zoomed into a subtree)
      isZoomed &&
        m(
          '.pf-sunburst__crumbs',
          trail.map((node, idx) => {
            const isLast = idx === trail.length - 1;
            return m(
              'span.pf-sunburst__crumb-item',
              {key: `${idx}-${node.key ?? node.name}`},
              [
                idx > 0 &&
                  m(Icon, {
                    icon: 'chevron_right',
                    className: 'pf-sunburst__crumb-sep',
                  }),
                m(
                  'button.pf-sunburst__crumb',
                  {
                    type: 'button',
                    disabled: isLast,
                    className: classNames(
                      isLast && 'pf-sunburst__crumb--current',
                    ),
                    onclick: () => {
                      this.zoomPath = this.zoomPath.slice(0, idx);
                      this.hoveredId = undefined;
                    },
                  },
                  node.name,
                ),
              ],
            );
          }),
        ),

      m('.pf-sunburst__layout', [
        // Left: Pure SVG Sunburst
        m('.pf-sunburst__chart', [
          m(
            'svg.pf-sunburst__svg',
            {
              'width': size,
              'height': size,
              'viewBox': `0 0 ${size} ${size}`,
              'role': 'img',
              'aria-label': `${focus.name} sunburst chart`,
            },
            [
              // Concentric arcs
              arcs.map((arc) => {
                const r0 = innerHoleR + (arc.depth - 1) * ringSpan;
                const r1 = Math.max(
                  r0 + 2,
                  innerHoleR + arc.depth * ringSpan - RING_GAP_PX,
                );
                const d = annularArcPath(
                  cx,
                  cy,
                  r0,
                  r1,
                  arc.startAngle,
                  arc.endAngle,
                );

                const isHovered = hoveredArc?.id === arc.id;
                const isRelated =
                  hoveredArc === undefined ||
                  hoveredArc.ancestorIds.has(arc.id) ||
                  arc.ancestorIds.has(hoveredArc.id);
                const canZoom =
                  zoomable && arc.seg.children.some((c) => c.value > 0);

                // Tangential label inside wide-enough wedges
                const sweep = arc.endAngle - arc.startAngle;
                const midR = (r0 + r1) / 2;
                const arcLen = midR * sweep;
                const showArcLabel =
                  sweep >= MIN_LABEL_ANGLE &&
                  arcLen >= MIN_LABEL_ARC_PX &&
                  r1 - r0 >= 18;
                const midAngle = (arc.startAngle + arc.endAngle) / 2;
                const labelPos = polarToCartesian(cx, cy, midR, midAngle);
                // Orient tangentially and flip bottom-half labels upright
                let deg = (midAngle * 180) / Math.PI + 90;
                const normDeg = ((deg % 360) + 360) % 360;
                if (normDeg > 90 && normDeg < 270) {
                  deg += 180;
                }
                const maxChars = Math.floor((arcLen - 10) / 6.5);
                const shortText = showArcLabel
                  ? truncateLabel(arc.seg.name, maxChars)
                  : '';

                return m(
                  'g.pf-sunburst__slice',
                  {
                    key: arc.id,
                    class: classNames(
                      isHovered && 'pf-sunburst__slice--hovered',
                      !isRelated && 'pf-sunburst__slice--dimmed',
                      (canZoom || onSegmentClick !== undefined) &&
                        'pf-sunburst__slice--clickable',
                      arc.hasHiddenChildren && 'pf-sunburst__slice--has-more',
                    ),
                    onmouseenter: () => {
                      this.hoveredId = arc.id;
                    },
                    onmouseleave: () => {
                      if (this.hoveredId === arc.id) {
                        this.hoveredId = undefined;
                      }
                    },
                    onclick: () => zoomInto(arc.pathKeys, arc.seg),
                  },
                  [
                    m('path.pf-sunburst__arc', {
                      d,
                      fill: arc.seg.cssColor,
                    }),
                    shortText !== '' &&
                      m(
                        'text.pf-sunburst__arc-label',
                        {
                          x: labelPos.x,
                          y: labelPos.y,
                          fill: arcLabelFill(arc.seg.cssColor),
                          transform: `rotate(${deg.toFixed(1)} ${labelPos.x.toFixed(1)} ${labelPos.y.toFixed(1)})`,
                        },
                        shortText,
                      ),
                  ],
                );
              }),

              // Interactive Center Donut Hole
              m(
                'g.pf-sunburst__center',
                {
                  class: classNames(isZoomed && 'pf-sunburst__center--back'),
                  onclick: isZoomed ? stepOut : undefined,
                },
                [
                  m('circle.pf-sunburst__center-bg', {
                    cx,
                    cy,
                    r: Math.max(10, innerHoleR - 3),
                  }),
                  m(
                    'text.pf-sunburst__center-name',
                    {x: cx, y: cy - 16},
                    truncateLabel(centerSeg.name, 16),
                  ),
                  m(
                    'text.pf-sunburst__center-value',
                    {x: cx, y: cy + 5},
                    formatValue(centerSeg.value),
                  ),
                  m(
                    'text.pf-sunburst__center-sub',
                    {x: cx, y: cy + 22},
                    hoveredArc !== undefined
                      ? `${(centerShare * 100).toFixed(1)}%` +
                          (hoveredArc.hasHiddenChildren ? ' · click ▸' : '')
                      : isZoomed
                        ? '← Zoom out'
                        : '100%',
                  ),
                ],
              ),
            ],
          ),
        ]),

        // Right: Synchronized Breakdown Legend
        showLegend &&
          legendRoots.length > 0 &&
          m('.pf-sunburst__legend', [
            legendRoots.map((rootArc) => {
              const subs = (childrenByParentId.get(rootArc.id) ?? [])
                .slice()
                .sort((a, b) => b.seg.value - a.seg.value);
              const rootHovered =
                hoveredArc !== undefined &&
                (hoveredArc.id === rootArc.id ||
                  hoveredArc.ancestorIds.has(rootArc.id));
              const canZoomRoot =
                zoomable && rootArc.seg.children.some((c) => c.value > 0);

              return m('.pf-sunburst__legend-group', {key: rootArc.id}, [
                m(
                  'button.pf-sunburst__legend-row.pf-sunburst__legend-row--parent',
                  {
                    type: 'button',
                    className: classNames(
                      rootHovered && 'pf-sunburst__legend-row--active',
                      canZoomRoot && 'pf-sunburst__legend-row--zoomable',
                    ),
                    onmouseenter: () => {
                      this.hoveredId = rootArc.id;
                    },
                    onmouseleave: () => {
                      if (this.hoveredId === rootArc.id) {
                        this.hoveredId = undefined;
                      }
                    },
                    onclick: () => zoomInto(rootArc.pathKeys, rootArc.seg),
                  },
                  [
                    m('span.pf-sunburst__swatch', {
                      style: {background: rootArc.seg.cssColor},
                    }),
                    m('span.pf-sunburst__legend-name', rootArc.seg.name),
                    m(
                      'span.pf-sunburst__legend-val',
                      formatValue(rootArc.seg.value),
                    ),
                    rootArc.seg.delta !== undefined &&
                      m('span.pf-sunburst__legend-delta', rootArc.seg.delta),
                    m(
                      'span.pf-sunburst__legend-pct',
                      `${(rootArc.shareOfRoot * 100).toFixed(1)}%`,
                    ),
                    canZoomRoot &&
                      m(Icon, {
                        icon: 'chevron_right',
                        className: 'pf-sunburst__legend-chevron',
                      }),
                  ],
                ),
                subs.length > 0 &&
                  m(
                    '.pf-sunburst__legend-children',
                    subs.map((subArc) => {
                      const subHovered =
                        hoveredArc !== undefined &&
                        (hoveredArc.id === subArc.id ||
                          hoveredArc.ancestorIds.has(subArc.id));
                      const canZoomSub =
                        zoomable &&
                        subArc.seg.children.some((c) => c.value > 0);
                      return m(
                        'button.pf-sunburst__legend-row.pf-sunburst__legend-row--child',
                        {
                          key: subArc.id,
                          type: 'button',
                          className: classNames(
                            subHovered && 'pf-sunburst__legend-row--active',
                            canZoomSub && 'pf-sunburst__legend-row--zoomable',
                          ),
                          onmouseenter: () => {
                            this.hoveredId = subArc.id;
                          },
                          onmouseleave: () => {
                            if (this.hoveredId === subArc.id) {
                              this.hoveredId = undefined;
                            }
                          },
                          onclick: () => zoomInto(subArc.pathKeys, subArc.seg),
                        },
                        [
                          m(
                            'span.pf-sunburst__swatch.pf-sunburst__swatch--sm',
                            {
                              style: {background: subArc.seg.cssColor},
                            },
                          ),
                          m('span.pf-sunburst__legend-name', subArc.seg.name),
                          m(
                            'span.pf-sunburst__legend-val',
                            formatValue(subArc.seg.value),
                          ),
                          subArc.seg.delta !== undefined &&
                            m(
                              'span.pf-sunburst__legend-delta',
                              subArc.seg.delta,
                            ),
                          m(
                            'span.pf-sunburst__legend-pct',
                            `${(subArc.shareOfRoot * 100).toFixed(1)}%`,
                          ),
                          canZoomSub &&
                            m(Icon, {
                              icon: 'chevron_right',
                              className: 'pf-sunburst__legend-chevron',
                            }),
                        ],
                      );
                    }),
                  ),
              ]);
            }),
          ]),
      ]),

      // Cursor-following rich tooltip
      hoveredArc !== undefined &&
        m(
          CursorTooltip,
          {
            position: PopupPosition.BottomStart,
            offset: TOOLTIP_OFFSET_PX,
            skidOffset: TOOLTIP_OFFSET_PX,
          },
          m('.pf-sunburst-tooltip', [
            hoveredArc.seg.tooltip ??
              defaultTooltip(
                hoveredArc.seg,
                hoveredArc.shareOfRoot,
                formatValue,
              ),
            hoveredArc.hasHiddenChildren &&
              m('.pf-sunburst-tooltip__hint', 'Click to drill down further'),
          ]),
        ),
    ]);
  }
}
