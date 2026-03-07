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
import type {BitmapListRow} from '../types';
import {fmtSize} from '../format';
import {type NavFn, InstanceLink, SortableTable} from '../components';
import * as queries from '../queries';

interface BitmapGalleryViewAttrs {
  engine: Engine;
  navigate: NavFn;
}

function BitmapGalleryView(): m.Component<BitmapGalleryViewAttrs> {
  let rows: BitmapListRow[] | null = null;
  let alive = true;

  return {
    oninit(vnode) {
      queries
        .getBitmapList(vnode.attrs.engine)
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
      const {navigate} = vnode.attrs;

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      const totalRetained = rows.reduce(
        (sum, r) => sum + r.row.retainedTotal,
        0,
      );

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Bitmaps'),

        rows.length === 0
          ? m(EmptyState, {icon: 'image', title: 'No bitmap data available'})
          : [
              m('div', {class: 'ah-card ah-mb-4'}, [
                m('div', {class: 'ah-info-grid'}, [
                  m('span', {class: 'ah-info-grid__label'}, 'Total bitmaps:'),
                  m('span', {class: 'ah-mono'}, String(rows.length)),
                  m('span', {class: 'ah-info-grid__label'}, 'Total retained:'),
                  m('span', {class: 'ah-mono'}, fmtSize(totalRetained)),
                ]),
              ]),

              m(SortableTable, {
                columns: [
                  {
                    label: 'Size',
                    render: (r: BitmapListRow) =>
                      m(
                        'span',
                        {class: 'ah-mono'},
                        r.width + '\u00d7' + r.height,
                      ),
                  },
                  {
                    label: 'Density',
                    align: 'right',
                    sortKey: (r: BitmapListRow) => r.density,
                    render: (r: BitmapListRow) =>
                      m(
                        'span',
                        {class: 'ah-mono'},
                        r.density > 0 ? String(r.density) : '\u2014',
                      ),
                  },
                  {
                    label: 'Retained',
                    align: 'right',
                    sortKey: (r: BitmapListRow) => r.row.retainedTotal,
                    render: (r: BitmapListRow) =>
                      m(
                        'span',
                        {class: 'ah-mono'},
                        fmtSize(r.row.retainedTotal),
                      ),
                  },
                  {
                    label: 'Object',
                    render: (r: BitmapListRow) =>
                      m(InstanceLink, {
                        row: r.row,
                        navigate,
                      }),
                  },
                ],
                data: rows,
                rowKey: (r: BitmapListRow) => r.row.id,
              }),
            ],
      ]);
    },
  };
}

export default BitmapGalleryView;
