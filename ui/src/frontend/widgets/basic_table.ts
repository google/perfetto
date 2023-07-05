// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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

export interface ColumnDescriptor<T> {
  title: m.Children;
  render: (row: T) => m.Children;
}

export interface TableAttrs<T> {
  data: T[];
  columns: ColumnDescriptor<T>[];
}

export class BasicTable implements m.ClassComponent<TableAttrs<any>> {
  renderColumnHeader(
      _vnode: m.Vnode<TableAttrs<any>>,
      column: ColumnDescriptor<any>): m.Children {
    return m('td', column.title);
  }

  view(vnode: m.Vnode<TableAttrs<any>>): m.Child {
    const attrs = vnode.attrs;

    return m(
        'table.generic-table',
        {
          // TODO(altimin, stevegolton): this should be the default for
          // generic-table, but currently it is overriden by
          // .pf-details-shell .pf-content table, so specify this here for now.
          style: {
            'table-layout': 'auto',
          },
        },
        m('thead',
          m('tr.header',
            attrs.columns.map(
                (column) => this.renderColumnHeader(vnode, column)))),
        attrs.data.map(
            (row) =>
                m('tr',
                  attrs.columns.map((column) => m('td', column.render(row))))));
  }
}
