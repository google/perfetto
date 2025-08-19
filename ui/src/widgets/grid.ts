// Copyright (C) 2025 The Android Open Source Project
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
import {classNames} from '../base/classnames';
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import {MenuItem, PopupMenu} from './menu';
import {MithrilEvent} from '../base/mithril_utils';

export interface GridAttrs {
  // If true, the grid will fill the height of its parent container.
  readonly fillHeight?: boolean;
  // An optional class name to add to the root element of the grid.
  readonly className?: string;
}

// The top-level container. It creates the main `<table>` element and expects
// `GridHeader` and `GridBody` as children.
export class Grid implements m.ClassComponent<GridAttrs> {
  view({attrs, children}: m.Vnode<GridAttrs>) {
    const {fillHeight = false, className} = attrs;
    return m(
      '.pf-grid',
      {
        className: classNames(fillHeight && 'pf-grid--fill-height', className),
      },
      m('.pf-grid__table', m('table', children)),
    );
  }
}

// Renders the `<thead>` element. It's designed to contain `GridRow` components.
export class GridHeader implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('thead', children);
  }
}

// Renders the `<tbody>` element. It will also contain `GridRow` components.
export class GridBody implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('tbody', children);
  }
}

// Renders a `<tr>` element. It expects a list of cells.
export class GridRow implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('tr', children);
  }
}

export type SortDirection = 'ASC' | 'DESC';

export type CellAlignment = 'left' | 'center' | 'right';

export type ReorderPosition = 'before' | 'after';

export interface GridHeaderCellAttrs extends m.Attributes {
  // The current sort direction, if any.
  readonly sort?: SortDirection;
  // Callback invoked when the user clicks the sort button.
  readonly onSort?: (direction: SortDirection) => void;
  // An array of Mithril children (e.g., MenuItem, MenuDivider) for the
  // context menu.
  readonly menuItems?: m.Children;
  // Horizontal alignment of the cell content.
  readonly aggregation?: {
    readonly left: m.Children;
    readonly right: m.Children;
  };
  // A handle to identify a group of reorderable columns. Columns can only be
  // reordered within the same group.
  readonly reorderable?: {
    readonly handle: string;
  };
  // Called when a column is dragged and dropped onto another column.
  // The first argument is the `key` of the column being dragged, the second
  // is the `key` of the column being dropped on.
  readonly onReorder?: (
    from: string | number | undefined,
    to: string | number | undefined,
    position: ReorderPosition,
  ) => void;
  // If true, the cell will have a thick right border, useful for separating
  // groups of columns.
  readonly thickRightBorder?: boolean;
}

// Renders a `<th>` element, for use inside a `GridRow` within a `GridHeader`.
export class GridHeaderCell implements m.ClassComponent<GridHeaderCellAttrs> {
  private dragOverState: {count: number; position: ReorderPosition} = {
    count: 0,
    position: 'after',
  };

  view({attrs, children}: m.Vnode<GridHeaderCellAttrs>) {
    const {
      sort,
      onSort,
      menuItems,
      aggregation,
      reorderable,
      onReorder,
      thickRightBorder,
      ...rest
    } = attrs;

    const renderSortButton = () => {
      if (!onSort) return null;

      const nextDirection: SortDirection = (() => {
        if (!sort) return 'ASC';
        if (sort === 'ASC') return 'DESC';
        if (sort === 'DESC') return 'ASC';
        // Default to ascending if no sort is defined.
        return 'ASC';
      })();

      return m(Button, {
        className: classNames(
          !sort && 'pf-grid-cell__hint',
          !sort && 'pf-visible-on-hover',
        ),
        compact: true,
        icon: sort === 'DESC' ? Icons.SortDesc : Icons.SortAsc,
        onclick: (e: MouseEvent) => {
          onSort(nextDirection);
          e.stopPropagation();
        },
      });
    };

    const renderMenu = () => {
      if (menuItems === undefined) return null;
      return m(
        '.pf-grid-cell__actions',
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              className: 'pf-grid-cell__menu-button pf-visible-on-hover',
              icon: Icons.ContextMenuAlt,
              rounded: true,
            }),
          },
          menuItems,
        ),
      );
    };

    const hasAggregation = aggregation !== undefined;
    const reorderHandle = reorderable?.handle;

    return m(
      'th',
      {
        ...rest,
        draggable: reorderable !== undefined,
        className: classNames(
          this.dragOverState.count > 0 && 'pf-drag-over',
          this.dragOverState.count > 0 &&
            `pf-drag-over--${this.dragOverState.position}`,
          thickRightBorder && 'pf-grid-cell--thick-right-border',
        ),
        ondragstart: (e: MithrilEvent<DragEvent>) => {
          e.redraw = false;
          e.dataTransfer!.setData(
            reorderable!.handle,
            JSON.stringify({
              key: attrs.key,
            }),
          );
        },
        ondragenter: (e: MithrilEvent<DragEvent>) => {
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            ++this.dragOverState.count;
          }
        },
        ondragleave: (e: MithrilEvent<DragEvent>) => {
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            --this.dragOverState.count;
          }
        },
        ondragover: (e: MithrilEvent<DragEvent>) => {
          e.preventDefault();
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            e.dataTransfer!.dropEffect = 'move';
            const target = e.currentTarget as HTMLElement;
            const rect = target.getBoundingClientRect();
            this.dragOverState.position =
              e.clientX < rect.left + rect.width / 2 ? 'before' : 'after';
          } else {
            e.dataTransfer!.dropEffect = 'none';
          }
        },
        ondrop: (e: MithrilEvent<DragEvent>) => {
          this.dragOverState.count = 0;
          if (reorderHandle) {
            const data = e.dataTransfer!.getData(reorderHandle);
            if (data) {
              e.preventDefault();
              const {key: from} = JSON.parse(data);
              const to = attrs.key as string | number | undefined;
              const target = e.currentTarget as HTMLElement;
              const rect = target.getBoundingClientRect();
              const position =
                e.clientX < rect.left + rect.width / 2 ? 'before' : 'after';
              onReorder?.(from, to, position);
            }
          }
        },
      },
      m('.pf-grid-cell-header', [
        m(
          '.pf-grid-cell',
          m('.pf-grid-cell__content', [children, renderSortButton()]),
          renderMenu(),
        ),
        hasAggregation &&
          m(
            '.pf-grid-cell__aggregation',
            m('.pf-grid-cell__aggregation-left', aggregation.left),
            m('.pf-grid-cell__aggregation-right', aggregation.right),
          ),
      ]),
    );
  }
}

export interface GridDataCellAttrs extends m.Attributes {
  // An array of Mithril children (e.g., MenuItem, MenuDivider) for the
  // context menu.
  readonly menuItems?: m.Children;
  // Horizontal alignment of the cell content.
  readonly align?: CellAlignment;
  // If true, the cell will be styled to indicate missing data.
  readonly isMissing?: boolean;
  // If true, the cell will have a thick right border.
  readonly thickRightBorder?: boolean;
}

// Renders a `<td>` element, for use inside a `GridRow` within a `GridBody`.
export class GridDataCell implements m.ClassComponent<GridDataCellAttrs> {
  view({attrs, children}: m.Vnode<GridDataCellAttrs>) {
    const {menuItems, align, isMissing, thickRightBorder, className, ...rest} =
      attrs;

    const renderMenu = () => {
      if (menuItems === undefined) return null;
      return m(
        '.pf-grid-cell__actions',
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              className: 'pf-grid-cell__menu-button pf-visible-on-hover',
              icon: Icons.ContextMenuAlt,
              rounded: true,
            }),
          },
          menuItems,
        ),
      );
    };

    return m(
      'td',
      {
        ...rest,
        className: classNames(
          className,
          thickRightBorder && 'pf-grid-cell--thick-right-border',
        ),
      },
      m('.pf-grid-cell', [
        m(
          '.pf-grid-cell__content',
          {
            className: classNames(
              align && `pf-grid-cell--align-${align}`,
              isMissing && 'pf-grid-cell--missing',
            ),
          },
          children,
        ),
        renderMenu(),
      ]),
    );
  }
}

export function renderSortMenuItems(
  sorted: SortDirection | undefined,
  sort: (direction: SortDirection | undefined) => void,
) {
  return [
    sorted !== 'DESC' &&
      m(MenuItem, {
        label: 'Sort DESC',
        icon: Icons.SortedDesc,
        onclick: () => sort('DESC'),
      }),
    sorted !== 'ASC' &&
      m(MenuItem, {
        label: 'Sort ASC',
        icon: Icons.SortedAsc,
        onclick: () => sort('ASC'),
      }),
    sorted !== undefined &&
      m(MenuItem, {
        label: 'Unsort',
        icon: Icons.Close,
        onclick: () => sort(undefined),
      }),
  ];
}
