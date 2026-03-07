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
import type {InstanceRow, HeapInfo} from '../types';
import {fmtSize} from '../format';
import {type NavFn, SortableTable, InstanceLink} from '../components';
import * as queries from '../queries';

interface RootedViewAttrs {
  engine: Engine;
  heaps: HeapInfo[];
  navigate: NavFn;
}

function RootedView(): m.Component<RootedViewAttrs> {
  let rows: InstanceRow[] | null = null;
  let alive = true;

  return {
    oninit(vnode) {
      queries
        .getRooted(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          rows = r;
          m.redraw();
        })
        .catch(console.error);
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {heaps, navigate} = vnode.attrs;

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      const heapCols = heaps.filter((h) => h.java + h.native_ > 0);

      type Col = {
        label: string;
        align?: string;
        minWidth?: string;
        sortKey?: (r: InstanceRow) => number;
        render: (r: InstanceRow, idx: number) => m.Children;
      };
      const cols: Col[] = [
        {
          label: 'Retained',
          align: 'right',
          minWidth: '5rem',
          sortKey: (r) => r.retainedTotal,
          render: (r) =>
            m(
              'span',
              {
                class: `ah-mono${r.isPlaceHolder ? ' ah-opacity-60' : ''}`,
              },
              fmtSize(r.retainedTotal),
            ),
        },
      ];
      for (const h of heapCols) {
        cols.push({
          label: h.name,
          align: 'right',
          minWidth: '5rem',
          sortKey: (r: InstanceRow) => {
            const s = r.retainedByHeap.find((x) => x.heap === h.name);
            return (s?.java ?? 0) + (s?.native_ ?? 0);
          },
          render: (r: InstanceRow) => {
            const s = r.retainedByHeap.find((x) => x.heap === h.name);
            return m(
              'span',
              {
                class: `ah-mono${r.isPlaceHolder ? ' ah-opacity-60' : ''}`,
              },
              fmtSize((s?.java ?? 0) + (s?.native_ ?? 0)),
            );
          },
        });
      }
      cols.push({
        label: 'Object',
        render: (r) =>
          m(
            'span',
            {class: r.isPlaceHolder ? 'ah-opacity-60' : ''},
            m(InstanceLink, {row: r, navigate}),
          ),
      });

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Rooted'),
        m(SortableTable, {
          columns: cols,
          data: rows,
          rowKey: (r: InstanceRow) => r.id,
        }),
      ]);
    },
  };
}

export default RootedView;
