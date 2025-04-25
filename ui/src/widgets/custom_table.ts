// Copyright (C) 2025 The Android Open Source Project
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
  readonly title: m.Children;
  readonly render: (row: T) => {
    colspan?: number;
    className?: string;
    cell: m.Children;
  };
}

// This is a class to be able to perform runtime checks on `columns` below.
export interface ReorderableColumns<T> {
  readonly columns: ColumnDescriptor<T>[];
  // Enables drag'n'drop reordering of columns.
  readonly reorder?: (from: number, to: number) => void;
  // Whether the first column should have a left border. True by default.
  readonly hasLeftBorder?: boolean;
}

export interface CustomTableAttrs<T> {
  readonly data: ReadonlyArray<T>;
  readonly columns: ReadonlyArray<ReorderableColumns<T> | undefined>;
  readonly className?: string;
}

export class CustomTable<T> implements m.ClassComponent<CustomTableAttrs<T>> {
  view({attrs}: m.Vnode<CustomTableAttrs<T>>): m.Children {
    const columns: {column: ColumnDescriptor<T>; extraClasses: string}[] = [];
    const headers: m.Children[] = [];
    for (const [index, columnGroup] of attrs.columns
      .filter((c) => c !== undefined)
      .entries()) {
      const hasLeftBorder = (columnGroup.hasLeftBorder ?? true) && index !== 0;
      const currentColumns = columnGroup.columns.map((column, columnIndex) => ({
        column,
        extraClasses:
          hasLeftBorder && columnIndex === 0 ? '.has-left-border' : '',
      }));
      if (columnGroup.reorder === undefined) {
        for (const {column, extraClasses} of currentColumns) {
          headers.push(m(`td${extraClasses}`, column.title));
        }
      } else {
        headers.push(
          m(ReorderableCellGroup, {
            cells: currentColumns.map(({column, extraClasses}) => ({
              content: column.title,
              extraClasses,
            })),
            onReorder: columnGroup.reorder,
          }),
        );
      }
      columns.push(...currentColumns);
    }

    return m(
      `table.generic-table`,
      {
        className: attrs.className,
        // TODO(altimin, stevegolton): this should be the default for
        // generic-table, but currently it is overriden by
        // .pf-details-shell .pf-content table, so specify this here for now.
        style: {
          'table-layout': 'auto',
        },
      },
      m('thead', m('tr.header', headers)),
      m(
        'tbody',
        attrs.data.map((row) => {
          const cells = [];
          for (let i = 0; i < columns.length; ) {
            const {column, extraClasses} = columns[i];
            const {colspan, className, cell} = column.render(row);
            cells.push(m(`td${extraClasses}`, {colspan, className}, cell));
            i += colspan ?? 1;
          }
          return m('tr', cells);
        }),
      ),
    );
  }
}

export interface ReorderableCellGroupAttrs {
  cells: {
    content: m.Children;
    extraClasses: string;
  }[];
  onReorder: (from: number, to: number) => void;
}

const placeholderElement = document.createElement('span');

// A component that renders a group of cells on the same row that can be
// reordered between each other by using drag'n'drop.
//
// On completed reorder, a callback is fired.
class ReorderableCellGroup
  implements m.ClassComponent<ReorderableCellGroupAttrs>
{
  private drag?: {
    from: number;
    to?: number;
  };

  private getClassForIndex(index: number): string {
    if (this.drag?.from === index) {
      return 'dragged';
    }
    if (this.drag?.to === index) {
      return 'highlight-left';
    }
    if (this.drag?.to === index + 1) {
      return 'highlight-right';
    }
    return '';
  }

  view(vnode: m.Vnode<ReorderableCellGroupAttrs>): m.Children {
    return vnode.attrs.cells.map((cell, index) =>
      m(
        `td.reorderable-cell${cell.extraClasses}`,
        {
          draggable: 'draggable',
          class: this.getClassForIndex(index),
          ondragstart: (e: DragEvent) => {
            this.drag = {
              from: index,
            };
            if (e.dataTransfer !== null) {
              e.dataTransfer.setDragImage(placeholderElement, 0, 0);
            }
          },
          ondragover: (e: DragEvent) => {
            let target = e.target as HTMLElement;
            if (this.drag === undefined || this.drag?.from === index) {
              // Don't do anything when hovering on the same cell that's
              // been dragged, or when dragging something other than the
              // cell from the same group.
              return;
            }

            while (
              target.tagName.toLowerCase() !== 'td' &&
              target.parentElement !== null
            ) {
              target = target.parentElement;
            }

            // When hovering over cell on the right half, the cell will be
            // moved to the right of it, vice versa for the left side. This
            // is done such that it's possible to put dragged cell to every
            // possible position.
            const offset = e.clientX - target.getBoundingClientRect().x;
            const direction =
              offset > target.clientWidth / 2 ? 'right' : 'left';
            const dest = direction === 'left' ? index : index + 1;
            const adjustedDest =
              dest === this.drag.from || dest === this.drag.from + 1
                ? undefined
                : dest;
            if (adjustedDest !== this.drag.to) {
              this.drag.to = adjustedDest;
            }
          },
          ondragleave: (e: DragEvent) => {
            if (this.drag?.to !== index) return;
            this.drag.to = undefined;
            if (e.dataTransfer !== null) {
              e.dataTransfer.dropEffect = 'none';
            }
          },
          ondragend: () => {
            if (
              this.drag !== undefined &&
              this.drag.to !== undefined &&
              this.drag.from !== this.drag.to
            ) {
              vnode.attrs.onReorder(this.drag.from, this.drag.to);
            }

            this.drag = undefined;
          },
        },
        cell.content,
      ),
    );
  }
}
