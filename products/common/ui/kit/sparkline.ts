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
import {classNames} from '../../../../ui/src/base/classnames';
import {Icon} from '../../../../ui/src/widgets/icon';

// Shared Android OOM score adj buckets matching Memscope's OOM_SCORE_BUCKETS
// so chips, bars, swimlanes, and stacked charts share one consistent palette.
export interface OomBucketDef {
  readonly idx: number;
  readonly name: string;
  readonly short: string;
  readonly color: string;
  readonly minScore: number;
  readonly maxScore: number;
  readonly privileged: boolean;
}

export const OOM_BUCKETS: ReadonlyArray<OomBucketDef> = [
  {
    idx: 0,
    name: 'Native (< 0)',
    short: 'Native',
    color: '#2563eb',
    minScore: -1000,
    maxScore: -1,
    privileged: true,
  },
  {
    idx: 1,
    name: 'Foreground (0)',
    short: 'FG',
    color: '#16a34a',
    minScore: 0,
    maxScore: 0,
    privileged: true,
  },
  {
    idx: 2,
    name: 'Visible (1-99)',
    short: 'Visible',
    color: '#0d9488',
    minScore: 1,
    maxScore: 99,
    privileged: true,
  },
  {
    idx: 3,
    name: 'Perceptible (100-299)',
    short: 'Perceptible',
    color: '#d97706',
    minScore: 100,
    maxScore: 299,
    privileged: true,
  },
  {
    idx: 4,
    name: 'Service (300-599)',
    short: 'Service',
    color: '#ea580c',
    minScore: 300,
    maxScore: 599,
    privileged: false,
  },
  {
    idx: 5,
    name: 'Cached (600-899)',
    short: 'Home/Prev',
    color: '#e11d48',
    minScore: 600,
    maxScore: 899,
    privileged: false,
  },
  {
    idx: 6,
    name: 'Cached (900+)',
    short: 'Cached',
    color: '#9333ea',
    minScore: 900,
    maxScore: 1001,
    privileged: false,
  },
];

export function oomBucketFor(score: number | null): OomBucketDef | undefined {
  if (score === null) return undefined;
  return OOM_BUCKETS.find((b) => score >= b.minScore && score <= b.maxScore);
}

export function oomBucketByIdx(idx: number): OomBucketDef | undefined {
  return OOM_BUCKETS.find((b) => b.idx === idx);
}

// Soft pastel-tinted pill matching Memscope's ColorChip.
export function colorChip(
  label: m.Children,
  color?: string,
  title?: string,
): m.Child {
  return m(
    'span.pf-prod-chip',
    {
      title,
      style: color ? {'--pf-prod-chip-color': color} : undefined,
    },
    label,
  );
}

export interface SparklineOpts {
  readonly width?: number;
  readonly height?: number;
  // Fixed color overrides directional rising=danger / falling=success coloring.
  readonly color?: string;
  // Render a faint area fill underneath (nice for Scorecard KPIs).
  readonly area?: boolean;
  // Minimum y-axis span to avoid amplifying tiny noise on flat metrics.
  readonly minSpan?: number;
}

// Compact SVG sparkline matching Memscope's processes_table.ts trend column.
export function sparklineSvg(
  values: ReadonlyArray<number>,
  opts: SparklineOpts = {},
): m.Child {
  if (values.length < 2) {
    return m('span.pf-prod-sparkline-empty', '—');
  }
  const width = opts.width ?? 68;
  const height = opts.height ?? 18;
  let min = values[0]!;
  let max = values[0]!;
  for (const v of values) {
    if (v < min) min = v;
    if (v > max) max = v;
  }
  const span = Math.max(max - min, opts.minSpan ?? 0);
  const padY = 2;
  const drawH = height - padY * 2;
  const stepX = width / (values.length - 1);
  const coords = values.map((v, i) => {
    const x = i * stepX;
    const y =
      span <= 1e-6
        ? padY + drawH / 2
        : padY + drawH - ((v - min) / span) * drawH;
    return [x, y] as const;
  });
  const points = coords.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ');
  const first = values[0]!;
  const last = values[values.length - 1]!;
  const stroke =
    opts.color ??
    (last > first + 1e-3
      ? 'var(--pf-color-danger)'
      : last < first - 1e-3
        ? 'var(--pf-color-success)'
        : 'var(--pf-color-text-muted)');

  const areaPath =
    opts.area && coords.length >= 2
      ? `M ${coords[0]![0].toFixed(1)},${height} ` +
        coords.map(([x, y]) => `L ${x.toFixed(1)},${y.toFixed(1)}`).join(' ') +
        ` L ${coords[coords.length - 1]![0].toFixed(1)},${height} Z`
      : undefined;

  return m(
    'svg.pf-prod-sparkline',
    {
      width,
      height,
      viewBox: `0 0 ${width} ${height}`,
      preserveAspectRatio: 'none',
    },
    [
      areaPath &&
        m('path', {
          d: areaPath,
          fill: stroke,
          'fill-opacity': 0.14,
        }),
      m('polyline', {
        points,
        fill: 'none',
        stroke,
        'stroke-width': 1.4,
        'stroke-linejoin': 'round',
        'stroke-linecap': 'round',
      }),
    ],
  );
}

export interface ScorecardTileAttrs {
  readonly label: string;
  readonly value: string;
  readonly sub?: string;
  readonly icon?: string;
  readonly status?: 'ok' | 'warn' | 'bad';
  readonly sparkline?: ReadonlyArray<number>;
  readonly sparkColor?: string;
  readonly onclick?: () => void;
  readonly title?: string;
}

// Clickable KPI scorecard tile combining Perfetto Scorecard + Memscope Billboard
// + subtle live sparkline heartbeat.
export function scorecardTile(attrs: ScorecardTileAttrs): m.Child {
  return m(
    attrs.onclick ? 'button.pf-prod-scorecard' : '.pf-prod-scorecard',
    {
      type: attrs.onclick ? 'button' : undefined,
      className: classNames(
        attrs.onclick && 'pf-prod-scorecard--clickable',
        attrs.status && `pf-prod-scorecard--${attrs.status}`,
      ),
      onclick: attrs.onclick,
      title: attrs.title,
    },
    [
      m('.pf-prod-scorecard__top', [
        m('.pf-prod-scorecard__label', [
          attrs.status &&
            m('span.pf-prod-scorecard__dot', {
              className: `pf-prod-scorecard__dot--${attrs.status}`,
            }),
          attrs.icon && m(Icon, {icon: attrs.icon}),
          m('span', attrs.label),
        ]),
        attrs.sparkline &&
          attrs.sparkline.length >= 2 &&
          sparklineSvg(attrs.sparkline, {
            width: 56,
            height: 20,
            color: attrs.sparkColor,
            area: true,
          }),
      ]),
      m('.pf-prod-scorecard__val', {title: attrs.value}, attrs.value),
      attrs.sub && m('.pf-prod-scorecard__sub', {title: attrs.sub}, attrs.sub),
    ],
  );
}

export function formatAgeShort(secs: number | null): string {
  if (secs === null || secs < 0) return '—';
  const d = Math.floor(secs / 86400);
  const h = Math.floor((secs % 86400) / 3600);
  const mn = Math.floor((secs % 3600) / 60);
  const s = Math.floor(secs % 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${mn}m`;
  if (mn > 0) return `${mn}m ${s}s`;
  return `${s}s`;
}

export interface SwimlaneInterval {
  readonly upid: number;
  readonly pid: number;
  readonly processName: string;
  readonly bucketIdx: number;
  readonly bucketName: string;
  readonly oomScoreAdj: number;
  readonly startRelS: number;
  readonly endRelS: number;
  readonly changed: boolean;
  readonly isTop: boolean;
  readonly killed: boolean;
  readonly frozen: boolean;
}

// Declarative crisp SVG/CSS App-States swimlane timeline: one row per notable
// app, colored horizontal intervals by OOM state band, frozen stripes, and
// click-through to open that process's closable tab.
export function appStateSwimlane(attrs: {
  readonly intervals: ReadonlyArray<SwimlaneInterval>;
  readonly durationS: number;
  readonly onSelectProcess: (upid: number, processName: string) => void;
}): m.Child {
  const {intervals, durationS, onSelectProcess} = attrs;
  if (intervals.length === 0) return undefined;

  const spanS = Math.max(1, durationS);
  const laneOrder: number[] = [];
  const byUpid = new Map<number, SwimlaneInterval[]>();
  for (const iv of intervals) {
    let list = byUpid.get(iv.upid);
    if (!list) {
      list = [];
      byUpid.set(iv.upid, list);
      laneOrder.push(iv.upid);
    }
    list.push(iv);
  }

  const presentBuckets = new Set(intervals.map((i) => i.bucketIdx));
  const ticks = [0, 0.25, 0.5, 0.75, 1].map((f) => Math.round(f * spanS));

  return m('.pf-prod-swimlane', [
    m('.pf-prod-swimlane__legend', [
      ...OOM_BUCKETS.filter((b) => presentBuckets.has(b.idx)).map((b) =>
        m('span.pf-prod-swimlane__legend-item', [
          m('span.pf-prod-swimlane__swatch', {style: {background: b.color}}),
          b.name,
        ]),
      ),
      m('span.pf-prod-swimlane__legend-hint', 'Click any lane to open process'),
    ]),
    m(
      '.pf-prod-swimlane__rows',
      laneOrder.map((upid) => {
        const segs = byUpid.get(upid)!;
        const first = segs[0]!;
        const last = segs[segs.length - 1]!;
        const lastBucket = oomBucketByIdx(last.bucketIdx);
        return m(
          '.pf-prod-swimlane__row',
          {
            onclick: () => onSelectProcess(upid, first.processName),
            title: `Open ${first.processName} (PID ${first.pid})`,
          },
          [
            m('.pf-prod-swimlane__label', [
              first.isTop && m('span.pf-prod-swimlane__badge', 'TOP'),
              first.killed &&
                m('span.pf-prod-swimlane__badge.pf-prod-swimlane__badge--kill', 'LMK'),
              first.frozen && m('span.pf-prod-swimlane__frozen', '❄'),
              m('span.pf-prod-swimlane__name', first.processName),
            ]),
            m('.pf-prod-swimlane__track', [
              segs.map((s) => {
                const leftPct = Math.max(0, Math.min(100, (s.startRelS / spanS) * 100));
                const widthPct = Math.max(
                  1.2,
                  Math.min(100 - leftPct, ((s.endRelS - s.startRelS) / spanS) * 100),
                );
                const b = oomBucketByIdx(s.bucketIdx);
                return m('.pf-prod-swimlane__seg', {
                  className: classNames(s.frozen && 'pf-prod-swimlane__seg--frozen'),
                  style: {
                    left: `${leftPct.toFixed(2)}%`,
                    width: `${widthPct.toFixed(2)}%`,
                    background: b?.color ?? '#64748b',
                  },
                  title: `${s.processName} · ${s.bucketName} (adj ${s.oomScoreAdj}) · ${s.startRelS.toFixed(1)}s → ${s.endRelS.toFixed(1)}s`,
                });
              }),
              first.killed &&
                m('.pf-prod-swimlane__kill-mark', {
                  style: {
                    left: `${Math.min(99, (last.endRelS / spanS) * 100).toFixed(2)}%`,
                  },
                  title: 'Killed by LMKD',
                }),
            ]),
            m(
              '.pf-prod-swimlane__end',
              colorChip(
                `${lastBucket?.short ?? '—'} (${last.oomScoreAdj})`,
                lastBucket?.color,
              ),
            ),
          ],
        );
      }),
    ),
    m('.pf-prod-swimlane__axis', [
      m('.pf-prod-swimlane__label'),
      m(
        '.pf-prod-swimlane__ticks',
        ticks.map((t) => m('span', `${t}s`)),
      ),
      m('.pf-prod-swimlane__end'),
    ]),
  ]);
}
