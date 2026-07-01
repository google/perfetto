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

// Presentational widgets shared by the Memscope per-section panels (Java,
// bitmaps, native): the compact top-N table, class-name cells, the share bar,
// the delta cell and the single-ratio bar.

import m from 'mithril';
import {ProportionBar} from '../../../../components/widgets/charts/proportion_bar';
import {Panel} from '../../components/panel';
import {Table} from '../../components/table';
import {Inset} from '../../components/inset';
import {deltaColor, formatBytes, formatDelta} from './mem_format';
import {EmptyState} from '../../../../widgets/empty_state';
import {Stack} from '../../../../widgets/stack';

// A section panel shown when the trace has no data for it: keeps the panel's
// title/subtitle (so the page reads consistently) and states what's missing
// in the body, rather than rendering nothing.
export function emptyPanel(opts: {
  title: string;
  subtitle?: string;
  message: string;
  detail?: m.Children;
}): m.Child {
  return m(
    Panel,
    m(Panel.Header, {title: opts.title, subtitle: opts.subtitle}),
    m(
      Panel.Body,
      m(
        Stack,
        {spacing: 'large'},
        m(
          '.pf-memscope-landing__empty',
          m(EmptyState, {title: opts.message}, opts.detail),
        ),
      ),
    ),
  );
}

// A section panel shown while its data is loading: the panel (title/subtitle)
// is laid out immediately with a loading placeholder in the body, so the page
// doesn't jump as sections resolve.
export function loadingPanel(opts: {
  title: string;
  subtitle?: string;
}): m.Child {
  return m(
    Panel,
    m(Panel.Header, {title: opts.title, subtitle: opts.subtitle}),
    m(
      Panel.Body,
      m(
        Stack,
        {spacing: 'large'},
        m(
          '.pf-memscope-placeholder',
          m(EmptyState, {icon: 'hourglass', title: 'Loading...'}),
        ),
      ),
    ),
  );
}

// Trims a fully-qualified class name to its last segment(s) for legends, e.g.
// 'java.util.HashMap$Node' -> 'HashMap$Node'. Arrays and 'Other' pass through.
export function shortClassName(name: string): string {
  if (name === 'Other') return name;
  const arrayDepth = (name.match(/\[\]/g) ?? []).length;
  const base = name.replace(/\[\]/g, '');
  const last = base.slice(base.lastIndexOf('.') + 1);
  return last + '[]'.repeat(arrayDepth);
}

// Compact titled table used by the Java / bitmaps / native sections.
export function topTable(opts: {
  title: string;
  subtitle?: string;
  cols: {label: string; num?: boolean}[];
  rows: m.Children[][];
}): m.Child {
  return m('.pf-memscope-toptable', [
    m('.pf-memscope-toptable__title', [
      opts.title,
      opts.subtitle !== undefined &&
        m('span.pf-memscope-toptable__subtitle', ` · ${opts.subtitle}`),
    ]),
    m(
      Table,
      {className: 'pf-memscope-table--flush'},
      m(
        'thead',
        m(
          'tr',
          opts.cols.map((c) =>
            m(c.num ? 'th.pf-memscope-table__num' : 'th', c.label),
          ),
        ),
      ),
      m(
        'tbody',
        opts.rows.map((cells) =>
          m(
            'tr',
            cells.map((cell, i) =>
              m(opts.cols[i].num ? 'td.pf-memscope-table__num' : 'td', cell),
            ),
          ),
        ),
      ),
    ),
  ]);
}

// Link to the Heap Dump Explorer's object list, filtered to one class. The HDE
// page (com.android.HeapDumpExplorer) parses `cls` out of the query string, so
// the value is URI-encoded the same way the explorer's own links are.
export function heapDumpClassHref(cls: string): string {
  return `#!/heapdump/objects?cls=${encodeURIComponent(cls)}`;
}

// A class-name cell. The class name links to that class's instances in the Heap
// Dump Explorer. When `retainers` are given, each one adds a `↳ via <owner>`
// line naming an app-side class that dominates this (library) class's instances
// up the dominator chain, with the bytes attributed to that owner; the owner
// name links to the explorer too. A class held through several owners shows
// several lines, so a single misleading "via" can't imply all the retained
// bytes flow through one owner.
export function classNameCell(
  full: string,
  retainers?: ReadonlyArray<{name: string; bytes: number}>,
): m.Child {
  const vias = (retainers ?? []).filter((r) => r.name !== full);
  // With one owner the byte count just echoes the row's retained size, so omit
  // it; with several, the split is the whole point, so show each owner's bytes.
  const showBytes = vias.length > 1;
  return m('.pf-memscope-classcell', [
    m(
      'a.pf-memscope-classname',
      {href: heapDumpClassHref(full), title: full},
      shortClassName(full),
    ),
    ...vias.map((r) =>
      m(
        'span.pf-memscope-classcell__via',
        {title: `Retained via ${r.name}: ${formatBytes(r.bytes)}`},
        [
          '↳ via ',
          m(
            'a.pf-memscope-classcell__vialink',
            {href: heapDumpClassHref(r.name)},
            shortClassName(r.name),
          ),
          showBytes ? ` · ${formatBytes(r.bytes)}` : '',
        ],
      ),
    ),
  ]);
}

// Two-line table cell: value on top, signed Δ vs baseline below (colored
// red up / green down, "new" when the row has no baseline). When not
// comparing, just the value.
export function deltaCell(
  text: m.Children,
  delta: number | undefined,
  comparing: boolean,
  fmt: (n: number) => string = formatDelta,
): m.Children {
  if (!comparing) return text;
  return [
    m('div', text),
    m(
      'div.pf-memscope-table__delta',
      {style: {color: delta === undefined ? undefined : deltaColor(delta)}},
      delta === undefined ? 'new' : fmt(delta),
    ),
  ];
}

// Tiny horizontal progress bar used for the "share %" table columns.
export function shareBar(frac: number): m.Child {
  const pct = Math.max(0, Math.min(100, frac * 100));
  return m('.pf-memscope-sharebar', [
    m(
      '.pf-memscope-sharebar__track',
      m('.pf-memscope-sharebar__fill', {style: {width: `${pct}%`}}),
    ),
    m('span.pf-memscope-sharebar__pct', `${Math.round(pct)}%`),
  ]);
}

// Single-ratio bar (heap reachability, profiler coverage): an uppercase
// label, a big percentage headline, a two-tone bar and a two-entry legend.
export function ratioBar(opts: {
  label: string;
  tooltip: string;
  pct: number;
  headline: string;
  color: string;
  aLabel: string;
  aBytes: number;
  bLabel: string;
  bBytes: number;
  // When comparing, the change in each leg vs the baseline dump. Rendered as
  // a muted "(±X)" beside the absolute value, and a "Δ … pts" on the headline.
  aDelta?: number;
  bDelta?: number;
  pctDelta?: number;
}): m.Child {
  const pct = Math.max(0, Math.min(100, opts.pct));
  const legDelta = (d?: number) =>
    d !== undefined &&
    m(
      'span.pf-memscope-ratio__legend-change',
      {style: {color: deltaColor(d)}},
      ` (${formatDelta(d)})`,
    );
  return m(Inset, {className: 'pf-memscope-ratio'}, [
    m('.pf-memscope-ratio__label', {title: opts.tooltip}, opts.label),
    m('.pf-memscope-ratio__headline', [
      m('span.pf-memscope-ratio__pct', `${Math.round(pct)}%`),
      m('span.pf-memscope-ratio__text', opts.headline),
      opts.pctDelta !== undefined &&
        m(
          'span.pf-memscope-ratio__pct-delta',
          {style: {color: deltaColor(opts.pctDelta)}},
          `Δ ${opts.pctDelta >= 0 ? '+' : ''}${Math.round(opts.pctDelta)} pts vs baseline`,
        ),
    ]),
    m(ProportionBar, {
      segments: [
        {
          label: opts.aLabel,
          weight: pct,
          color: opts.color,
          value: [formatBytes(opts.aBytes), legDelta(opts.aDelta)],
        },
        {
          label: opts.bLabel,
          weight: 100 - pct,
          color: '#c3c7cc',
          value: [formatBytes(opts.bBytes), legDelta(opts.bDelta)],
        },
      ],
    }),
  ]);
}
