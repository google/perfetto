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
import type {Engine} from '../../../trace_processor/engine';
import {Spinner} from '../../../widgets/spinner';
import {EmptyState} from '../../../widgets/empty_state';
import type {StringListRow} from '../types';
import {fmtSize} from '../format';
import {type NavFn, Section, SortableTable} from '../components';
import {computeDuplicates, type DuplicateGroup} from './strings_helpers';
import * as queries from '../queries';

// --- StringsView -------------------------------------------------------------

interface StringsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  initialQuery?: string;
}

function StringsView(): m.Component<StringsViewAttrs> {
  let allRows: StringListRow[] | null = null;
  let query = '';
  let selectedHeap = 'all';
  let exactMatch = false;
  let alive = true;

  function handleChange(q: string) {
    query = q;
    exactMatch = false;
  }

  return {
    oninit(vnode) {
      query = vnode.attrs.initialQuery ?? '';
      queries
        .getStringList(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          allRows = r;
          m.redraw();
        })
        .catch(console.error);
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {navigate} = vnode.attrs;

      if (!allRows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      if (allRows.length === 0) {
        return m('div', [
          m('h2', {class: 'ah-view-heading'}, 'Strings'),
          m(EmptyState, {
            icon: 'text_fields',
            title: 'No string data available',
          }),
        ]);
      }

      // Compute heaps
      const heaps: string[] = [];
      {
        const s = new Set<string>();
        for (const r of allRows) s.add(r.heap);
        heaps.push(...[...s].sort());
      }

      // Compute filtered
      const qLower = query.toLowerCase();
      const filtered = allRows.filter((r) => {
        if (selectedHeap !== 'all' && r.heap !== selectedHeap) return false;
        if (!qLower) return true;
        if (exactMatch) return r.value === query;
        return r.value.toLowerCase().includes(qLower);
      });

      // Compute heapFiltered
      const heapFiltered =
        selectedHeap === 'all'
          ? allRows
          : allRows.filter((r) => r.heap === selectedHeap);

      const duplicates = computeDuplicates(heapFiltered);
      const totalRetained = heapFiltered.reduce(
        (s, r) => s + r.retainedSize,
        0,
      );
      const totalWasted = duplicates.reduce((s, d) => s + d.wastedBytes, 0);
      const uniqueCount = (() => {
        const seen = new Set<string>();
        for (const r of heapFiltered) seen.add(r.value);
        return seen.size;
      })();

      return m('div', [
        m(
          'div',
          {
            style: {
              display: 'flex',
              alignItems: 'center',
              gap: '0.75rem',
              marginBottom: '0.75rem',
            },
          },
          [
            m(
              'h2',
              {class: 'ah-view-heading', style: {marginBottom: 0}},
              'Strings',
            ),
            heaps.length > 1
              ? m(
                  'select',
                  {
                    value: selectedHeap,
                    onchange: (e: Event) => {
                      selectedHeap = (e.target as HTMLSelectElement).value;
                    },
                    class: 'ah-select',
                    style: {
                      backgroundImage: `url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 20 20' fill='%23888'%3E%3Cpath d='M5.23 7.21a.75.75 0 011.06.02L10 11.168l3.71-3.938a.75.75 0 111.08 1.04l-4.25 4.5a.75.75 0 01-1.08 0l-4.25-4.5a.75.75 0 01.02-1.06z'/%3E%3C/svg%3E")`,
                      backgroundRepeat: 'no-repeat',
                      backgroundPosition: 'right 0.4rem center',
                    },
                  },
                  [
                    m('option', {key: '__all__', value: 'all'}, 'All heaps'),
                    ...heaps.map((h) => m('option', {key: h, value: h}, h)),
                  ],
                )
              : null,
          ],
        ),

        // Summary
        m('div', {class: 'ah-card ah-mb-4'}, [
          m('div', {class: 'ah-info-grid'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Total strings:'),
            m('span', {class: 'ah-mono'}, heapFiltered.length.toLocaleString()),
            m('span', {class: 'ah-info-grid__label'}, 'Unique values:'),
            m('span', {class: 'ah-mono'}, uniqueCount.toLocaleString()),
            m('span', {class: 'ah-info-grid__label'}, 'Duplicate groups:'),
            m(
              'span',
              {class: 'ah-mono'},
              duplicates.length > 0
                ? m(
                    'span',
                    {style: {color: 'var(--ah-badge-warning)'}},
                    duplicates.length.toLocaleString(),
                  )
                : '0',
            ),
            ...(totalWasted > 0
              ? [
                  m(
                    'span',
                    {class: 'ah-info-grid__label'},
                    'Wasted by duplicates:',
                  ),
                  m(
                    'span',
                    {
                      class: 'ah-mono',
                      style: {color: 'var(--ah-badge-warning)'},
                    },
                    fmtSize(totalWasted),
                  ),
                ]
              : []),
            m('span', {class: 'ah-info-grid__label'}, 'Total retained:'),
            m('span', {class: 'ah-mono'}, fmtSize(totalRetained)),
          ]),
        ]),

        // Duplicates section
        duplicates.length > 0
          ? m('div', {class: 'ah-mb-4'}, [
              m(
                Section,
                {
                  title: `Duplicate strings (${duplicates.length} groups, ${fmtSize(totalWasted)} wasted)`,
                  defaultOpen: false,
                },
                m(SortableTable, {
                  columns: [
                    {
                      label: 'Wasted',
                      align: 'right',
                      sortKey: (r: DuplicateGroup) => r.wastedBytes,
                      render: (r: DuplicateGroup) =>
                        m('span', {class: 'ah-mono'}, fmtSize(r.wastedBytes)),
                    },
                    {
                      label: 'Count',
                      align: 'right',
                      sortKey: (r: DuplicateGroup) => r.count,
                      render: (r: DuplicateGroup) =>
                        m('span', {class: 'ah-mono'}, String(r.count)),
                    },
                    {
                      label: 'Value',
                      render: (r: DuplicateGroup) =>
                        m(
                          'span',
                          {
                            class: 'ah-mono ah-break-all',
                            style: {
                              color: 'var(--ah-badge-string)',
                            },
                          },
                          '"' +
                            (r.value.length > 200
                              ? r.value.slice(0, 200) + '\u2026'
                              : r.value) +
                            '"',
                        ),
                    },
                  ],
                  data: duplicates,
                  onRowClick: (r: DuplicateGroup) => {
                    query = r.value;
                    exactMatch = true;
                  },
                }),
              ),
            ])
          : null,

        // Search
        m('input', {
          type: 'text',
          value: query,
          oninput: (e: Event) =>
            handleChange((e.target as HTMLInputElement).value),
          placeholder: 'Filter strings\u2026',
          class: 'ah-input',
        }),

        filtered.length > 0
          ? [
              query || selectedHeap !== 'all'
                ? m(
                    'div',
                    {class: 'ah-table__more ah-mb-2'},
                    filtered.length.toLocaleString() +
                      ' match' +
                      (filtered.length !== 1 ? 'es' : ''),
                  )
                : null,
              m(SortableTable, {
                columns: [
                  {
                    label: 'Retained',
                    align: 'right',
                    sortKey: (r: StringListRow) => r.retainedSize,
                    render: (r: StringListRow) =>
                      m('span', {class: 'ah-mono'}, fmtSize(r.retainedSize)),
                  },
                  {
                    label: 'Length',
                    align: 'right',
                    sortKey: (r: StringListRow) => r.length,
                    render: (r: StringListRow) =>
                      m('span', {class: 'ah-mono'}, r.length.toLocaleString()),
                  },
                  {
                    label: 'Heap',
                    render: (r: StringListRow) =>
                      m('span', {class: 'ah-info-grid__label'}, r.heap),
                  },
                  {
                    label: 'Value',
                    render: (r: StringListRow) =>
                      m('span', [
                        m(
                          'button',
                          {
                            class: 'ah-link',
                            onclick: () =>
                              navigate('object', {
                                id: r.id,
                                label: `"${r.value.length > 40 ? r.value.slice(0, 40) + '\u2026' : r.value}"`,
                              }),
                          },
                          m(
                            'span',
                            {
                              class: 'ah-mono ah-break-all',
                              style: {
                                color: 'var(--ah-badge-string)',
                              },
                            },
                            '"' +
                              (r.value.length > 300
                                ? r.value.slice(0, 300) + '\u2026'
                                : r.value) +
                              '"',
                          ),
                        ),
                      ]),
                  },
                ],
                data: filtered,
                rowKey: (r: StringListRow) => r.id,
              }),
            ]
          : null,
        query || selectedHeap !== 'all'
          ? filtered.length === 0
            ? m('div', {class: 'ah-info-grid__label'}, 'No matching strings.')
            : null
          : null,
      ]);
    },
  };
}

export default StringsView;
