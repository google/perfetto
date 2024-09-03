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
import {allUnique, range} from '../../base/array_utils';
import {
  compareUniversal,
  comparingBy,
  ComparisonFn,
  SortableValue,
  SortDirection,
  withDirection,
} from '../../base/comparison_utils';
import {raf} from '../../core/raf_scheduler';
import {
  menuItem,
  PopupMenuButton,
  popupMenuIcon,
  PopupMenuItem,
} from '../popup_menu';

export interface ColumnDescriptorAttrs<T> {
  // Context menu items displayed on the column header.
  contextMenu?: PopupMenuItem[];

  // Unique column ID, used to identify which column is currently sorted.
  columnId?: string;

  // Sorting predicate: if provided, column would be sortable.
  ordering?: ComparisonFn<T>;

  // Simpler way to provide a sorting: instead of full predicate, the function
  // can map the row for "sorting key" associated with the column.
  sortKey?: (value: T) => SortableValue;
}

export class ColumnDescriptor<T> {
  name: string;
  render: (row: T) => m.Child;
  id: string;
  contextMenu?: PopupMenuItem[];
  ordering?: ComparisonFn<T>;

  constructor(
    name: string,
    render: (row: T) => m.Child,
    attrs?: ColumnDescriptorAttrs<T>,
  ) {
    this.name = name;
    this.render = render;
    this.id = attrs?.columnId === undefined ? name : attrs.columnId;

    if (attrs === undefined) {
      return;
    }

    if (attrs.sortKey !== undefined && attrs.ordering !== undefined) {
      throw new Error('only one way to order a column should be specified');
    }

    if (attrs.sortKey !== undefined) {
      this.ordering = comparingBy(attrs.sortKey, compareUniversal);
    }
    if (attrs.ordering !== undefined) {
      this.ordering = attrs.ordering;
    }
  }
}

export function numberColumn<T>(
  name: string,
  getter: (t: T) => number,
  contextMenu?: PopupMenuItem[],
): ColumnDescriptor<T> {
  return new ColumnDescriptor<T>(name, getter, {contextMenu, sortKey: getter});
}

export function stringColumn<T>(
  name: string,
  getter: (t: T) => string,
  contextMenu?: PopupMenuItem[],
): ColumnDescriptor<T> {
  return new ColumnDescriptor<T>(name, getter, {contextMenu, sortKey: getter});
}

export function widgetColumn<T>(
  name: string,
  getter: (t: T) => m.Child,
): ColumnDescriptor<T> {
  return new ColumnDescriptor<T>(name, getter);
}

interface SortingInfo<T> {
  columnId: string;
  direction: SortDirection;
  // TODO(ddrone): figure out if storing this can be avoided.
  ordering: ComparisonFn<T>;
}

// Encapsulated table data, that contains the input to be displayed, as well as
// some helper information to allow sorting.
export class TableData<T> {
  data: T[];
  private _sortingInfo?: SortingInfo<T>;
  private permutation: number[];

  constructor(data: T[]) {
    this.data = data;
    this.permutation = range(data.length);
  }

  *iterateItems(): Generator<T> {
    for (const index of this.permutation) {
      yield this.data[index];
    }
  }

  items(): T[] {
    return Array.from(this.iterateItems());
  }

  setItems(newItems: T[]) {
    this.data = newItems;
    this.permutation = range(newItems.length);
    if (this._sortingInfo !== undefined) {
      this.reorder(this._sortingInfo);
    }
    raf.scheduleFullRedraw();
  }

  resetOrder() {
    this.permutation = range(this.data.length);
    this._sortingInfo = undefined;
    raf.scheduleFullRedraw();
  }

  get sortingInfo(): SortingInfo<T> | undefined {
    return this._sortingInfo;
  }

  reorder(info: SortingInfo<T>) {
    this._sortingInfo = info;
    this.permutation.sort(
      withDirection(
        comparingBy((index: number) => this.data[index], info.ordering),
        info.direction,
      ),
    );
    raf.scheduleFullRedraw();
  }
}

export interface TableAttrs<T> {
  data: TableData<T>;
  columns: ColumnDescriptor<T>[];
}

function directionOnIndex(
  columnId: string,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  info?: SortingInfo<any>,
): SortDirection | undefined {
  if (info === undefined) {
    return undefined;
  }
  return info.columnId === columnId ? info.direction : undefined;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export class Table implements m.ClassComponent<TableAttrs<any>> {
  renderColumnHeader(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    vnode: m.Vnode<TableAttrs<any>>,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    column: ColumnDescriptor<any>,
  ): m.Child {
    let currDirection: SortDirection | undefined = undefined;

    let items = column.contextMenu;
    if (column.ordering !== undefined) {
      const ordering = column.ordering;
      currDirection = directionOnIndex(column.id, vnode.attrs.data.sortingInfo);
      const newItems: PopupMenuItem[] = [];
      if (currDirection !== 'ASC') {
        newItems.push(
          menuItem('Sort ascending', () => {
            vnode.attrs.data.reorder({
              columnId: column.id,
              direction: 'ASC',
              ordering,
            });
          }),
        );
      }
      if (currDirection !== 'DESC') {
        newItems.push(
          menuItem('Sort descending', () => {
            vnode.attrs.data.reorder({
              columnId: column.id,
              direction: 'DESC',
              ordering,
            });
          }),
        );
      }
      if (currDirection !== undefined) {
        newItems.push(
          menuItem('Restore original order', () => {
            vnode.attrs.data.resetOrder();
          }),
        );
      }
      items = [...newItems, ...(items ?? [])];
    }

    return m(
      'td',
      column.name,
      items === undefined
        ? null
        : m(PopupMenuButton, {
            icon: popupMenuIcon(currDirection),
            items,
          }),
    );
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  checkValid(attrs: TableAttrs<any>) {
    if (!allUnique(attrs.columns.map((c) => c.id))) {
      throw new Error('column IDs should be unique');
    }
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  oncreate(vnode: m.VnodeDOM<TableAttrs<any>, this>) {
    this.checkValid(vnode.attrs);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  onupdate(vnode: m.VnodeDOM<TableAttrs<any>, this>) {
    this.checkValid(vnode.attrs);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  view(vnode: m.Vnode<TableAttrs<any>>): m.Child {
    const attrs = vnode.attrs;

    return m(
      'table.generic-table',
      m(
        'thead',
        m(
          'tr.header',
          attrs.columns.map((column) => this.renderColumnHeader(vnode, column)),
        ),
      ),
      attrs.data.items().map((row) =>
        m(
          'tr',
          attrs.columns.map((column) => m('td', column.render(row))),
        ),
      ),
    );
  }
}
