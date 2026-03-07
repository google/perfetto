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
import type {ClassRow, HeapInfo} from '../types';
import {fmtSize} from '../format';
import {type NavFn, SortableTable} from '../components';
import * as queries from '../queries';

interface AllocationsParams {
  heap: string | null;
}

interface AllocationsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  heaps: HeapInfo[];
  params: AllocationsParams;
}

function AllocationsView(): m.Component<AllocationsViewAttrs> {
  let rows: ClassRow[] | null = null;
  let prevHeap: string | null | undefined;
  let alive = true;

  function fetchData(attrs: AllocationsViewAttrs) {
    const heap = attrs.params.heap ?? null;
    prevHeap = heap;
    rows = null;
    queries
      .getAllocations(attrs.engine, heap)
      .then((r) => {
        if (!alive) return;
        rows = r;
        m.redraw();
      })
      .catch(console.error);
  }

  return {
    oninit(vnode) {
      fetchData(vnode.attrs);
    },
    onremove() {
      alive = false;
    },
    onupdate(vnode) {
      const heap = vnode.attrs.params.heap ?? null;
      if (heap !== prevHeap) {
        fetchData(vnode.attrs);
      }
    },
    view(vnode) {
      const {navigate, heaps, params} = vnode.attrs;
      const heap = params.heap ?? null;
      const activeHeaps = heaps.filter((h) => h.java + h.native_ > 0);

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Allocations'),

        activeHeaps.length > 1
          ? m('div', {class: 'ah-card--compact ah-mb-3'}, [
              m('span', {class: 'ah-info-grid__label'}, 'Heap: '),
              m(
                'select',
                {
                  class: 'ah-select',
                  value: heap ?? '',
                  onchange: (e: Event) => {
                    const val = (e.target as HTMLSelectElement).value;
                    navigate('allocations', {heap: val || null});
                  },
                },
                [
                  m('option', {value: '', key: '__all__'}, 'All heaps'),
                  ...activeHeaps.map((h) =>
                    m('option', {value: h.name, key: h.name}, h.name),
                  ),
                ],
              ),
            ])
          : null,

        m('div', {class: 'ah-card--compact ah-mb-3'}, [
          m('div', {class: 'ah-info-grid--compact'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Classes:'),
            m(
              'span',
              {class: 'ah-mono'},
              new Set(rows.map((r) => r.className)).size.toLocaleString(),
            ),
            m('span', {class: 'ah-info-grid__label'}, 'Instances:'),
            m(
              'span',
              {class: 'ah-mono'},
              rows.reduce((a, r) => a + r.count, 0).toLocaleString(),
            ),
          ]),
        ]),

        m(SortableTable, {
          columns: [
            {
              label: 'Retained',
              align: 'right',
              sortKey: (r: ClassRow) => r.retainedSize,
              render: (r: ClassRow) =>
                m('span', {class: 'ah-mono'}, fmtSize(r.retainedSize)),
            },
            {
              label: 'Shallow',
              align: 'right',
              sortKey: (r: ClassRow) => r.shallowSize,
              render: (r: ClassRow) =>
                m('span', {class: 'ah-mono'}, fmtSize(r.shallowSize)),
            },
            {
              label: '#',
              align: 'right',
              sortKey: (r: ClassRow) => r.count,
              render: (r: ClassRow) =>
                m('span', {class: 'ah-mono'}, r.count.toLocaleString()),
            },
            {
              label: 'Heap',
              render: (r: ClassRow) => m('span', r.heap),
            },
            {
              label: 'Class',
              render: (r: ClassRow) =>
                m(
                  'button',
                  {
                    class: 'ah-link',
                    onclick: () =>
                      navigate('objects', {
                        className: r.className,
                        heap: r.heap === 'default' ? null : r.heap,
                      }),
                  },
                  r.className,
                ),
            },
          ],
          data: rows,
          rowKey: (r: ClassRow) => r.className + '/' + r.heap,
        }),
      ]);
    },
  };
}

export default AllocationsView;
