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
import type {InstanceRow} from '../types';
import {fmtSize} from '../format';
import {type NavFn, SortableTable, InstanceLink} from '../components';
import * as queries from '../queries';

export interface ObjectsParams {
  className: string;
  heap: string | null;
}

interface ObjectsViewAttrs {
  engine: Engine;
  navigate: NavFn;
  params: ObjectsParams;
}

function ObjectsView(): m.Component<ObjectsViewAttrs> {
  let rows: InstanceRow[] | null = null;
  let prevClassName: string | undefined;
  let prevHeap: string | null | undefined;
  let alive = true;

  function fetchData(attrs: ObjectsViewAttrs) {
    const cls = attrs.params.className ?? '';
    const heap = attrs.params.heap ?? null;
    prevClassName = cls;
    prevHeap = heap;
    rows = null;
    queries
      .getObjects(attrs.engine, cls, heap)
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
      const cls = vnode.attrs.params.className ?? '';
      const heap = vnode.attrs.params.heap ?? null;
      if (cls !== prevClassName || heap !== prevHeap) {
        fetchData(vnode.attrs);
      }
    },
    view(vnode) {
      const className: string = vnode.attrs.params.className ?? '';
      const heap: string | null = vnode.attrs.params.heap ?? null;
      const navigate = vnode.attrs.navigate;

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Instances'),
        m('div', {class: 'ah-card--compact ah-mb-3'}, [
          m('div', {class: 'ah-info-grid--compact'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Class:'),
            m('span', {class: 'ah-mono'}, className),
            ...(heap
              ? [
                  m('span', {class: 'ah-info-grid__label'}, 'Heap:'),
                  m('span', heap),
                ]
              : []),
            m('span', {class: 'ah-info-grid__label'}, 'Count:'),
            m('span', {class: 'ah-mono'}, rows.length.toLocaleString()),
          ]),
        ]),
        m(SortableTable, {
          columns: [
            {
              label: 'Size',
              align: 'right',
              sortKey: (r: InstanceRow) => r.shallowJava + r.shallowNative,
              render: (r: InstanceRow) =>
                m(
                  'span',
                  {class: 'ah-mono'},
                  fmtSize(r.shallowJava + r.shallowNative),
                ),
            },
            {
              label: 'Heap',
              render: (r: InstanceRow) => m('span', r.heap),
            },
            {
              label: 'Object',
              render: (r: InstanceRow) => m(InstanceLink, {row: r, navigate}),
            },
          ],
          data: rows,
          rowKey: (r: InstanceRow) => r.id,
        }),
      ]);
    },
  };
}

export default ObjectsView;
