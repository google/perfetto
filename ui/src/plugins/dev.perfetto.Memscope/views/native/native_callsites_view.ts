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

import './native.scss';
import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {classNames} from '../../../../base/classnames';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {RadioGroup} from '../../../../widgets/radio_group';
import {Spinner} from '../../../../widgets/spinner';
import {TextInput} from '../../../../widgets/text_input';
import {Page, SubPage} from '../../components/page';
import {Panel} from '../../components/panel';
import {loadingPanel} from '../landing_page/section_widgets';
import {
  fmtNativeBytes,
  loadCallsiteStack,
  loadNativeCallsites,
  loadNativeLibraries,
  type NativeCallsiteRow,
  type NativeLibraryRow,
  type NativeProfileMeta,
  type NativeStackFrame,
  type NativeWindow,
} from './native_queries';
import {renderNativeToolbar} from './native_toolbar';

type GroupMode = 'libraries' | 'callsites';
type BehaviorFilter = 'all' | 'leak' | 'churn';
type SortKey = 'unreleased' | 'alloc' | 'freed' | 'count' | 'avg';

interface ListData {
  readonly libraries: ReadonlyArray<NativeLibraryRow>;
  readonly callsites: ReadonlyArray<NativeCallsiteRow>;
}

function classifyFreed(freedPct: number): {
  readonly label: string;
  readonly cls: string;
} {
  if (freedPct <= 25) return {label: 'Retained', cls: 'leak'};
  if (freedPct >= 80) return {label: 'Churn', cls: 'churn'};
  return {label: 'Mixed', cls: 'mixed'};
}

function miniSparkline(values: ReadonlyArray<number>): m.Children {
  if (values.length < 2) return m('span.pf-memscope-nlist__sub', '—');
  const w = 60;
  const h = 16;
  let min = values[0]!;
  let max = values[0]!;
  for (const v of values) {
    if (v < min) min = v;
    if (v > max) max = v;
  }
  const range = max - min || 1;
  const stepX = w / (values.length - 1);
  const pts = values
    .map(
      (v, i) =>
        `${(i * stepX).toFixed(1)},${(2 + (h - 4) - ((v - min) / range) * (h - 4)).toFixed(1)}`,
    )
    .join(' ');
  const growing = values[values.length - 1]! > values[0]!;
  const stroke = growing ? '#dc2626' : '#4285f4';
  return m(
    'svg',
    {width: w, height: h, viewBox: `0 0 ${w} ${h}`},
    m('polyline', {
      'points': pts,
      'fill': 'none',
      stroke,
      'stroke-width': 1.5,
      'stroke-linejoin': 'round',
      'stroke-linecap': 'round',
    }),
  );
}

// Lazily loads and renders a callsite's full frame chain when expanded.
class CallsiteDetailPanel implements m.ClassComponent<{
  readonly trace: Trace;
  readonly callsiteId: number;
  readonly leafFrame: string;
  readonly onOpenFlamegraph: (filter: string) => void;
}> {
  private readonly memo = new AsyncMemo<NativeStackFrame[]>();

  onremove() {
    this.memo.dispose();
  }

  view({
    attrs,
  }: m.CVnode<{
    readonly trace: Trace;
    readonly callsiteId: number;
    readonly leafFrame: string;
    readonly onOpenFlamegraph: (filter: string) => void;
  }>): m.Children {
    const frames = this.memo.use({
      key: attrs.callsiteId,
      compute: () => loadCallsiteStack(attrs.trace, attrs.callsiteId),
    }).data;

    return m('.pf-memscope-nlist__detail', [
      frames === undefined
        ? m(Spinner, {easing: true})
        : m(
            '.pf-memscope-nlist__frames',
            frames.slice(0, 18).map((f, idx) =>
              m(
                '.pf-memscope-nlist__frame',
                {
                  className: f.isApp
                    ? 'pf-memscope-nlist__frame--app'
                    : 'pf-memscope-nlist__frame--sys',
                },
                [
                  m('span', `#${idx} ${f.name}`),
                  m('span.pf-memscope-nlist__frame-map', f.shortMapping),
                ],
              ),
            ),
          ),
      m('.pf-memscope-nlist__detail-actions', [
        m(Button, {
          label: 'Focus in Flamegraph',
          icon: 'local_fire_department',
          rightIcon: 'arrow_forward',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () => attrs.onOpenFlamegraph(attrs.leafFrame),
        }),
      ]),
    ]);
  }
}

// Lazily loads top callsites inside an expanded library (.so).
class LibraryDetailPanel implements m.ClassComponent<{
  readonly trace: Trace;
  readonly upid: number;
  readonly win: NativeWindow;
  readonly mapping: string;
  readonly shortName: string;
  readonly onOpenFlamegraph: (filter: string) => void;
}> {
  private readonly memo = new AsyncMemo<NativeCallsiteRow[]>();

  onremove() {
    this.memo.dispose();
  }

  view({
    attrs,
  }: m.CVnode<{
    readonly trace: Trace;
    readonly upid: number;
    readonly win: NativeWindow;
    readonly mapping: string;
    readonly shortName: string;
    readonly onOpenFlamegraph: (filter: string) => void;
  }>): m.Children {
    const sites = this.memo.use({
      key: {
        upid: attrs.upid,
        mapping: attrs.mapping,
        heap: attrs.win.heapName,
        from: attrs.win.fromTs?.toString() ?? 'start',
        to: attrs.win.toTs?.toString() ?? 'end',
      },
      compute: () =>
        loadNativeCallsites(attrs.trace, attrs.upid, attrs.win, {
          mappingFilter: attrs.mapping,
          limit: 6,
        }),
    }).data;

    return m('.pf-memscope-nlist__detail', [
      m('.pf-memscope-nlist__sub', attrs.mapping),
      sites === undefined
        ? m(Spinner, {easing: true})
        : m(
            '.pf-memscope-nlist__frames',
            sites.map((s) =>
              m('.pf-memscope-nlist__frame.pf-memscope-nlist__frame--app', [
                m('span', s.leafFrame),
                m(
                  'span.pf-memscope-nlist__frame-map',
                  `${fmtNativeBytes(s.unreleasedBytes)} unrel · ${fmtNativeBytes(s.totalAllocBytes)} alloc · ${s.freedPct.toFixed(0)}% freed`,
                ),
              ]),
            ),
          ),
      m('.pf-memscope-nlist__detail-actions', [
        m(Button, {
          label: `Focus ${attrs.shortName} in Flamegraph`,
          icon: 'local_fire_department',
          rightIcon: 'arrow_forward',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () => attrs.onOpenFlamegraph(attrs.shortName),
        }),
      ]),
    ]);
  }
}

export interface NativeCallsitesViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly meta: NativeProfileMeta;
  readonly window: NativeWindow;
  readonly onWindowChange: (next: NativeWindow) => void;
  readonly onOpenFlamegraph: (filter: string) => void;
}

export class NativeCallsitesView implements m.ClassComponent<NativeCallsitesViewAttrs> {
  private readonly memo = new AsyncMemo<ListData>();
  private mode: GroupMode = 'libraries';
  private behavior: BehaviorFilter = 'all';
  private appOnly = false;
  private search = '';
  private sortKey: SortKey = 'unreleased';
  private expanded = new Set<string>();

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<NativeCallsitesViewAttrs>): m.Children {
    const {
      trace,
      upid,
      meta,
      window: win,
      onWindowChange,
      onOpenFlamegraph,
    } = attrs;

    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        heap: win.heapName,
        from: win.fromTs?.toString() ?? 'start',
        to: win.toTs?.toString() ?? 'end',
      },
      compute: async () => {
        const [libraries, callsites] = await Promise.all([
          loadNativeLibraries(trace, upid, win, 120),
          loadNativeCallsites(trace, upid, win, {
            sortBy: 'unreleased',
            limit: 150,
          }),
        ]);
        return {libraries, callsites};
      },
    }).data;

    if (data === undefined) {
      return m(
        Page,
        m(
          SubPage,
          renderNativeToolbar({
            trace,
            meta,
            window: win,
            onChange: onWindowChange,
          }),
          loadingPanel({title: 'Libraries & Callsites'}),
        ),
      );
    }

    const needle = this.search.trim().toLowerCase();
    const matchBehavior = (freedPct: number) =>
      this.behavior === 'all' ||
      (this.behavior === 'leak' && freedPct <= 25) ||
      (this.behavior === 'churn' && freedPct >= 80);

    const sortFn = <
      T extends {
        unreleasedBytes: number;
        totalAllocBytes: number;
        freedPct: number;
        totalAllocCount: number;
        avgAllocBytes: number;
      },
    >(
      a: T,
      b: T,
    ) => {
      switch (this.sortKey) {
        case 'alloc':
          return b.totalAllocBytes - a.totalAllocBytes;
        case 'freed':
          return a.freedPct - b.freedPct;
        case 'count':
          return b.totalAllocCount - a.totalAllocCount;
        case 'avg':
          return b.avgAllocBytes - a.avgAllocBytes;
        case 'unreleased':
        default:
          return (
            b.unreleasedBytes - a.unreleasedBytes ||
            b.totalAllocBytes - a.totalAllocBytes
          );
      }
    };

    const libs = data.libraries
      .filter(
        (l) =>
          (!this.appOnly || l.isApp) &&
          matchBehavior(l.freedPct) &&
          (needle === '' || l.mapping.toLowerCase().includes(needle)),
      )
      .sort(sortFn);

    const sites = data.callsites
      .filter(
        (s) =>
          (!this.appOnly || s.isApp) &&
          matchBehavior(s.freedPct) &&
          (needle === '' ||
            s.leafFrame.toLowerCase().includes(needle) ||
            s.mapping.toLowerCase().includes(needle)),
      )
      .sort(sortFn);

    const toggle = (k: string) => {
      if (this.expanded.has(k)) this.expanded.delete(k);
      else this.expanded.add(k);
    };

    const colHeader = (label: string, key: SortKey) =>
      m(
        'span.pf-memscope-nlist__num',
        {
          style: {cursor: 'pointer'},
          onclick: () => {
            this.sortKey = key;
          },
        },
        this.sortKey === key ? `${label} ↓` : label,
      );

    return m(
      Page,
      m(
        SubPage,
        renderNativeToolbar({
          trace,
          meta,
          window: win,
          onChange: onWindowChange,
        }),
        m(
          Panel,
          m(Panel.Header, {
            title: 'Libraries & Callsites',
            subtitle:
              'Click any row to expand its callstacks or top sites and jump directly into Flamegraph.',
            controls: [
              m(
                RadioGroup,
                {
                  selectedValue: this.mode,
                  onValueChange: (id: string) => {
                    this.mode = id as GroupMode;
                  },
                },
                [
                  m(
                    RadioGroup.Button,
                    {value: 'libraries'},
                    `Libraries (${data.libraries.length})`,
                  ),
                  m(
                    RadioGroup.Button,
                    {value: 'callsites'},
                    `Callsites (${data.callsites.length})`,
                  ),
                ],
              ),
              m(
                RadioGroup,
                {
                  selectedValue: this.behavior,
                  onValueChange: (id: string) => {
                    this.behavior = id as BehaviorFilter;
                  },
                },
                [
                  m(RadioGroup.Button, {value: 'all'}, 'All'),
                  m(RadioGroup.Button, {value: 'leak'}, 'Retained'),
                  m(RadioGroup.Button, {value: 'churn'}, 'Churn'),
                ],
              ),
              m(Button, {
                label: this.appOnly ? 'App only' : 'All mappings',
                icon: this.appOnly ? 'filter_alt' : 'layers',
                variant: ButtonVariant.Outlined,
                compact: true,
                onclick: () => {
                  this.appOnly = !this.appOnly;
                },
              }),
              m(TextInput, {
                placeholder: 'Filter library or symbol…',
                leftIcon: 'search',
                value: this.search,
                onInput: (v: string) => {
                  this.search = v;
                },
              }),
            ],
          }),
          m(
            Panel.Body,
            m('.pf-memscope-nlist', [
              m('.pf-memscope-nlist__head', [
                m('span'),
                m(
                  'span',
                  this.mode === 'libraries' ? 'Library (.so)' : 'Callsite',
                ),
                m('span', 'Signal'),
                colHeader('Unreleased', 'unreleased'),
                colHeader('Total Alloc', 'alloc'),
                colHeader('Freed %', 'freed'),
                colHeader('Allocs', 'count'),
                colHeader('Avg Size', 'avg'),
                m('span.pf-memscope-nlist__num', 'Trend'),
              ]),
              this.mode === 'libraries'
                ? libs.length === 0
                  ? m(
                      '.pf-memscope-placeholder',
                      {style: {padding: '20px'}},
                      'No libraries match the current filter.',
                    )
                  : libs.map((l) => {
                      const rowKey = `lib:${l.mapping}`;
                      const isOpen = this.expanded.has(rowKey);
                      const tag = classifyFreed(l.freedPct);
                      return m(
                        '.pf-memscope-nlist__item',
                        {
                          key: rowKey,
                          className: classNames(
                            isOpen && 'pf-memscope-nlist__item--open',
                          ),
                        },
                        [
                          m(
                            '.pf-memscope-nlist__row',
                            {onclick: () => toggle(rowKey)},
                            [
                              m(Icon, {
                                icon: isOpen ? 'expand_more' : 'chevron_right',
                              }),
                              m('.pf-memscope-nlist__name', [
                                m('code', {title: l.mapping}, l.shortName),
                                m(
                                  '.pf-memscope-nlist__sub',
                                  `${l.callsiteCount} callsites · ${l.mapping}`,
                                ),
                              ]),
                              m(
                                `span.pf-memscope-nlist__tag.pf-memscope-nlist__tag--${tag.cls}`,
                                tag.label,
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(l.unreleasedBytes),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(l.totalAllocBytes),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                `${l.freedPct.toFixed(0)}%`,
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                l.totalAllocCount.toLocaleString(),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(l.avgAllocBytes),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                miniSparkline(l.trend),
                              ),
                            ],
                          ),
                          isOpen &&
                            m(LibraryDetailPanel, {
                              trace,
                              upid,
                              win,
                              mapping: l.mapping,
                              shortName: l.shortName,
                              onOpenFlamegraph,
                            }),
                        ],
                      );
                    })
                : sites.length === 0
                  ? m(
                      '.pf-memscope-placeholder',
                      {style: {padding: '20px'}},
                      'No callsites match the current filter.',
                    )
                  : sites.map((s) => {
                      const rowKey = `cs:${s.callsiteId}`;
                      const isOpen = this.expanded.has(rowKey);
                      const tag = classifyFreed(s.freedPct);
                      return m(
                        '.pf-memscope-nlist__item',
                        {
                          key: rowKey,
                          className: classNames(
                            isOpen && 'pf-memscope-nlist__item--open',
                          ),
                        },
                        [
                          m(
                            '.pf-memscope-nlist__row',
                            {onclick: () => toggle(rowKey)},
                            [
                              m(Icon, {
                                icon: isOpen ? 'expand_more' : 'chevron_right',
                              }),
                              m('.pf-memscope-nlist__name', [
                                m('code', {title: s.leafFrame}, s.leafFrame),
                                m('.pf-memscope-nlist__sub', s.shortMapping),
                              ]),
                              m(
                                `span.pf-memscope-nlist__tag.pf-memscope-nlist__tag--${tag.cls}`,
                                tag.label,
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(s.unreleasedBytes),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(s.totalAllocBytes),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                `${s.freedPct.toFixed(0)}%`,
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                s.totalAllocCount.toLocaleString(),
                              ),
                              m(
                                'span.pf-memscope-nlist__num',
                                fmtNativeBytes(s.avgAllocBytes),
                              ),
                              m('span.pf-memscope-nlist__num', '—'),
                            ],
                          ),
                          isOpen &&
                            m(CallsiteDetailPanel, {
                              trace,
                              callsiteId: s.callsiteId,
                              leafFrame: s.leafFrame,
                              onOpenFlamegraph,
                            }),
                        ],
                      );
                    }),
            ]),
          ),
        ),
      ),
    );
  }
}
