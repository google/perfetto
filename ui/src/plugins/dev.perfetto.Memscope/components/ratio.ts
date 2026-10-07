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

import './ratio.scss';
import m from 'mithril';
import {ProportionBar} from '../../../components/widgets/charts/proportion_bar';
import {Inset} from './inset';
import {
  deltaColor,
  formatBytes,
  formatDelta,
} from '../views/landing_page/mem_format';

// A single ratio shown as a labelled block (recessed in an Inset): an uppercase
// label, a big percentage headline, and a two-tone ProportionBar that splits a
// whole into its "A" part and "B" remainder — e.g. reachable vs unreachable
// Java heap, or native RSS seen-vs-unseen by the profiler. In compare mode each
// leg carries its Δ vs the baseline dump, and the headline shows the pts delta.
export interface RatioAttrs {
  // Uppercase caption, e.g. "Heap reachability".
  readonly label: string;
  readonly tooltip: string;
  // The A-part share, 0..100 (clamped).
  readonly pct: number;
  // Text beside the big percentage, e.g. "of the Java heap is reachable".
  readonly headline: string;
  // Colour of the A segment; the B (remainder) segment is a neutral grey.
  readonly color: string;
  readonly aLabel: string;
  readonly aBytes: number;
  readonly bLabel: string;
  readonly bBytes: number;
  // When comparing, the change in each leg vs the baseline dump — rendered as a
  // muted "(±X)" beside the absolute value, and a "Δ … pts" on the headline.
  readonly aDelta?: number;
  readonly bDelta?: number;
  readonly pctDelta?: number;
}

export class Ratio implements m.ClassComponent<RatioAttrs> {
  view({attrs}: m.Vnode<RatioAttrs>): m.Children {
    const pct = Math.max(0, Math.min(100, attrs.pct));
    const legDelta = (d?: number) =>
      d !== undefined &&
      m(
        'span.pf-memscope-ratio__legend-change',
        {style: {color: deltaColor(d)}},
        ` (${formatDelta(d)})`,
      );
    return m(Inset, {className: 'pf-memscope-ratio'}, [
      m('.pf-memscope-ratio__label', {title: attrs.tooltip}, attrs.label),
      m('.pf-memscope-ratio__headline', [
        m('span.pf-memscope-ratio__pct', `${Math.round(pct)}%`),
        m('span.pf-memscope-ratio__text', attrs.headline),
        attrs.pctDelta !== undefined &&
          m(
            'span.pf-memscope-ratio__pct-delta',
            {style: {color: deltaColor(attrs.pctDelta)}},
            `Δ ${attrs.pctDelta >= 0 ? '+' : ''}${Math.round(attrs.pctDelta)} pts vs baseline`,
          ),
      ]),
      m(ProportionBar, {
        segments: [
          {
            label: attrs.aLabel,
            weight: pct,
            color: attrs.color,
            value: [formatBytes(attrs.aBytes), legDelta(attrs.aDelta)],
          },
          {
            label: attrs.bLabel,
            weight: 100 - pct,
            color: '#c3c7cc',
            value: [formatBytes(attrs.bBytes), legDelta(attrs.bDelta)],
          },
        ],
      }),
    ]);
  }
}
