// Copyright (C) 2023 The Android Open Source Project
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
import {classNames} from '../../base/classnames';
import {raf} from '../../core/raf_scheduler';

interface ColumnDescriptor<T> {
  name: string;
  getData: (row: T) => string;
}

export interface TreeTableAttrs<T> {
  columns: ColumnDescriptor<T>[];
  getChildren: (row: T) => T[] | undefined;
  rows: T[];
}

export class TreeTable<T> implements m.ClassComponent<TreeTableAttrs<T>> {
  private collapsedPaths = new Set<string>();

  view({attrs}: m.Vnode<TreeTableAttrs<T>, this>): void | m.Children {
    const {columns, rows} = attrs;
    const headers = columns.map(({name}) => m('th', name));
    const renderedRows = this.renderRows(rows, 0, attrs, []);
    return m(
      'table.pf-treetable',
      m('thead', m('tr', headers)),
      m('tbody', renderedRows),
    );
  }

  private renderRows(
    rows: T[],
    indentLevel: number,
    attrs: TreeTableAttrs<T>,
    path: string[],
  ): m.Children {
    const {columns, getChildren} = attrs;
    const renderedRows: m.Children = [];
    for (const row of rows) {
      const childRows = getChildren(row);
      const key = this.keyForRow(row, attrs);
      const thisPath = path.concat([key]);
      const hasChildren = childRows && childRows.length > 0;
      const cols = columns.map(({getData}, index) => {
        const classes = classNames(
          hasChildren && 'pf-treetable-node',
          this.isCollapsed(thisPath) && 'pf-collapsed',
        );
        if (index === 0) {
          const style = {
            '--indentation-level': indentLevel,
          };
          return m(
            'td',
            {style, class: classNames(classes, 'pf-treetable-maincol')},
            m('.pf-treetable-gutter', {
              onclick: () => {
                if (this.isCollapsed(thisPath)) {
                  this.expandPath(thisPath);
                } else {
                  this.collapsePath(thisPath);
                }
                raf.scheduleFullRedraw();
              },
            }),
            getData(row),
          );
        } else {
          const style = {
            '--indentation-level': 0,
          };
          return m('td', {style}, getData(row));
        }
      });
      renderedRows.push(m('tr', cols));
      if (childRows && !this.isCollapsed(thisPath)) {
        renderedRows.push(
          this.renderRows(childRows, indentLevel + 1, attrs, thisPath),
        );
      }
    }
    return renderedRows;
  }

  collapsePath(path: string[]) {
    const pathStr = path.join('/');
    this.collapsedPaths.add(pathStr);
  }

  expandPath(path: string[]) {
    const pathStr = path.join('/');
    this.collapsedPaths.delete(pathStr);
  }

  isCollapsed(path: string[]) {
    const pathStr = path.join('/');
    return this.collapsedPaths.has(pathStr);
  }

  keyForRow(row: T, attrs: TreeTableAttrs<T>): string {
    const {columns} = attrs;
    return columns[0].getData(row);
  }
}
