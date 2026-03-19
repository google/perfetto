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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Card} from '../../../widgets/card';
import {MiniTimeline} from './mini_timeline';
import {SummaryTables} from './summary_tables';
import {setVerdict, ensureCache, updateSlider} from '../state';
import type {TraceState, Cluster} from '../state';
import {fmtDur} from '../utils/format';
import {buildTraceLink} from '../utils/export';

const expanded = new Set<string>();

function toggleExpand(uuid: string): void {
  if (expanded.has(uuid)) expanded.delete(uuid);
  else expanded.add(uuid);
}

function renderSlider(ts: TraceState): m.Children {
  ensureCache(ts);
  return m('.qs-trace-slider', [
    m('span.qs-slider-label', 'Slices'),
    m('span.qs-slider-num', String(ts.currentSeq.length)),
    m('input[type=range].qs-slider-input', {
      min: 2,
      max: ts.origN,
      value: ts.sliderValue,
      step: 1,
      onclick: (e: Event) => e.stopPropagation(),
      oninput: (e: Event) => {
        e.stopPropagation();
        updateSlider(ts, +(e.target as HTMLInputElement).value);
      },
    }),
    m('span.qs-slider-of', `/ ${ts.origN}`),
  ]);
}

export interface TraceCardAttrs {
  cl: Cluster;
  ts: TraceState;
  idx: number;
}

export class TraceCard implements m.ClassComponent<TraceCardAttrs> {
  view({attrs}: m.CVnode<TraceCardAttrs>): m.Children {
    const {cl, ts, idx} = attrs;
    const key = ts._key;
    const isExpanded = expanded.has(key);
    const verdict = cl.verdicts.get(key);

    const verdictClass =
      verdict === 'like'
        ? 'qs-verdict-positive'
        : verdict === 'dislike'
          ? 'qs-verdict-negative'
          : verdict === 'discard'
            ? 'qs-verdict-discard'
            : '';

    const href = buildTraceLink(ts.trace.trace_uuid, ts.trace.package_name);

    return m(
      Card,
      {className: `qs-trace-card ${verdictClass}`},
      m(
        '.qs-card-header',
        {
          onclick: () => toggleExpand(key),
        },
        [
          m(
            'span.qs-collapse-arrow',
            {className: isExpanded ? 'qs-open' : ''},
            '\u25b6',
          ),
          m('span.qs-trace-idx', `#${idx + 1}`),
          m('span.qs-trace-pkg', ts.trace.package_name),
          ts.trace.startup_dur
            ? m('span.qs-trace-startup-dur', fmtDur(ts.trace.startup_dur))
            : null,
          href
            ? m(
                'a.qs-trace-link',
                {
                  href,
                  target: '_blank',
                  rel: 'noopener',
                  onclick: (e: Event) => e.stopPropagation(),
                  title: 'Open in trace viewer',
                },
                '\u2197',
              )
            : null,
          m('span.qs-trace-actions', [
            m(Button, {
              label: '+',
              variant: ButtonVariant.Minimal,
              intent: verdict === 'like' ? Intent.Success : Intent.None,
              active: verdict === 'like',
              compact: true,
              className: 'qs-verdict-btn',
              tooltip: 'Positive',
              onclick: (e: PointerEvent) => {
                e.stopPropagation();
                setVerdict(cl, key, 'like');
              },
            }),
            m(Button, {
              label: '\u2212',
              variant: ButtonVariant.Minimal,
              intent: verdict === 'dislike' ? Intent.Danger : Intent.None,
              active: verdict === 'dislike',
              compact: true,
              className: 'qs-verdict-btn',
              tooltip: 'Negative',
              onclick: (e: PointerEvent) => {
                e.stopPropagation();
                setVerdict(cl, key, 'dislike');
              },
            }),
            m(Button, {
              label: '\u00d7',
              variant: ButtonVariant.Minimal,
              active: verdict === 'discard',
              compact: true,
              className: 'qs-verdict-btn',
              tooltip: 'Discard',
              onclick: (e: PointerEvent) => {
                e.stopPropagation();
                setVerdict(cl, key, 'discard');
              },
            }),
          ]),
        ],
      ),
      m('.qs-card-body', [m(MiniTimeline, {ts}), renderSlider(ts)]),
      isExpanded
        ? m('.qs-card-detail', [
            m('.qs-detail-section', [
              m('.qs-detail-label', 'Breakdown'),
              m(SummaryTables, {ts, sortState: cl.tableSortState}),
            ]),
            m('.qs-detail-meta', [
              m('.qs-tt-grid', [
                m('span.qs-tt-k', 'UUID'),
                m('span.qs-tt-v', ts.trace.trace_uuid),
                m('span.qs-tt-k', 'Package'),
                m('span.qs-tt-v', ts.trace.package_name),
                m('span.qs-tt-k', 'Startup'),
                m(
                  'span.qs-tt-v',
                  ts.trace.startup_dur
                    ? fmtDur(ts.trace.startup_dur)
                    : '\u2014',
                ),
                m('span.qs-tt-k', 'Slices'),
                m('span.qs-tt-v', String(ts.origN)),
                m('span.qs-tt-k', 'Total dur'),
                m('span.qs-tt-v', fmtDur(ts.totalDur)),
                ...(ts.trace.extra
                  ? Object.entries(ts.trace.extra).flatMap(([k, v]) => [
                      m('span.qs-tt-k', k),
                      m('span.qs-tt-v', String(v)),
                    ])
                  : []),
              ]),
            ]),
          ])
        : null,
    );
  }
}
