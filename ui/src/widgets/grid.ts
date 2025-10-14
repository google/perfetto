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
      children,
    );
  }
}

// Renders the `<thead>` element. It's designed to contain `GridRow` components.
export class GridHeader implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('.pf-grid__header', children);
  }
}

// Renders the `<tbody>` element. It will also contain `GridRow` components.
export class GridBody implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('.pf-grid__body', children);
  }
}

// Renders a `<tr>` element. It expects a list of cells.
export class GridRow implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m('.pf-grid__row', children);
  }
}

export type SortDirection = 'ASC' | 'DESC';

export type CellAlignment = 'left' | 'center' | 'right';

export type ReorderPosition = 'before' | 'after';

export interface GridHeaderCellAttrs extends m.Attributes {
  readonly width?: string | number;
  // The current sort direction, if any.
  readonly sort?: SortDirection;
  // Callback invoked when the user clicks the sort button.
  readonly onSort?: (direction: SortDirection) => void;
  // An array of Mithril children (e.g., MenuItem, MenuDivider) for the
  // context menu.
  readonly menuItems?: m.Children;
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
  // Callback invoked when the user resizes the column.
  readonly onResize?: (newWidth: number) => void;
  // Callback invoked when the user double-clicks the resize handle to
  // auto-size.
  readonly onAutoResize?: () => void;
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
  private resizeState: {
    isResizing: boolean;
    startX: number;
    startWidth: number;
  } = {
    isResizing: false,
    startX: 0,
    startWidth: 0,
  };

  view({attrs, children, key}: m.Vnode<GridHeaderCellAttrs>) {
    const {
      sort,
      onSort,
      menuItems,
      reorderable,
      onReorder,
      onResize,
      thickRightBorder,
      width = 100,
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
          'pf-grid__cell__sort-button',
          !sort && 'pf-grid__cell--hint',
          !sort && 'pf-visible-on-hover',
        ),
        rounded: true,
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
        PopupMenu,
        {
          trigger: m(Button, {
            className: 'pf-visible-on-hover pf-grid__cell__menu-button',
            icon: Icons.ContextMenuAlt,
            rounded: true,
          }),
        },
        menuItems,
      );
    };

    const renderResizeHandle = () => {
      if (!onResize) return null;

      return m('.pf-grid__resize-handle', {
        onmousedown: (e: MouseEvent) => {
          e.preventDefault();
          e.stopPropagation();

          this.resizeState.isResizing = true;
          this.resizeState.startX = e.clientX;
          this.resizeState.startWidth =
            typeof width === 'number' ? width : parseInt(String(width)) || 100;

          const handleMouseMove = (e: MouseEvent) => {
            if (this.resizeState.isResizing) {
              const delta = e.clientX - this.resizeState.startX;
              const newWidth = Math.max(
                50,
                this.resizeState.startWidth + delta,
              );
              onResize(newWidth);
              m.redraw();
            }
          };

          const handleMouseUp = () => {
            this.resizeState.isResizing = false;
            document.removeEventListener('mousemove', handleMouseMove);
            document.removeEventListener('mouseup', handleMouseUp);
          };

          document.addEventListener('mousemove', handleMouseMove);
          document.addEventListener('mouseup', handleMouseUp);
        },
        ondblclick: (e: MouseEvent) => {
          e.preventDefault();
          e.stopPropagation();
          attrs.onAutoResize?.();
        },
      });
    };

    const reorderHandle = reorderable?.handle;

    return m(
      '.pf-grid__cell',
      {
        ...rest,
        style: {
          width: typeof width === 'number' ? `${width}px` : width,
        },
        draggable: reorderable !== undefined,
        className: classNames(
          this.dragOverState.count > 0 && 'pf-grid__cell--drag-over',
          this.dragOverState.count > 0 &&
            `pf-grid__cell--drag-over-${this.dragOverState.position}`,
          thickRightBorder && 'pf-grid__cell--thick-right-border',
        ),
        ondragstart: (e: MithrilEvent<DragEvent>) => {
          e.redraw = false;
          e.dataTransfer!.setData(reorderable!.handle, JSON.stringify({key}));
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
      m(
        '.pf-grid__cell--stretch.pf-grid__cell--horiz',
        m('.pf-grid__cell--padded.pf-grid__cell--shrink', children),
        m('.pf-grid__cell__btn-container', renderSortButton()),
      ),
      // TODO: Could put a spacer in here to push the sort button up to the
      // content and the menu to the right.
      m('.pf-grid__cell__btn-container', renderMenu()),
      renderResizeHandle(),
    );
  }
}

export interface GridAggregationCellAttrs extends m.Attributes {
  readonly width?: string | number;
  readonly align?: CellAlignment;
  readonly thickRightBorder?: boolean;
  readonly symbol?: string;
}

export class GridAggregationCell
  implements m.ClassComponent<GridAggregationCellAttrs>
{
  view({attrs, children}: m.Vnode<GridAggregationCellAttrs>) {
    const {
      className,
      width = 100,
      align,
      thickRightBorder,
      symbol,
      ...rest
    } = attrs;
    return m(
      '.pf-grid__cell.pf-grid__cell--padded',
      {
        ...rest,
        className: classNames(
          className,
          thickRightBorder && 'pf-grid__cell--thick-right-border',
        ),
        style: {
          width: typeof width === 'number' ? `${width}px` : width,
        },
      },
      symbol,
      m(
        '.pf-grid__cell--stretch',
        {
          className: classNames(align && `pf-grid__cell--align-${align}`),
        },
        children,
      ),
    );
  }
}

export interface GridDataCellAttrs extends m.Attributes {
  // An array of Mithril children (e.g., MenuItem, MenuDivider) for the
  // context menu.
  readonly menuItems?: m.Children;
  // Horizontal alignment of the cell content.
  readonly align?: CellAlignment;
  // If true, the cell will be styled to indicate null or absent data.
  readonly nullish?: boolean;
  // If true, the cell will have a thick right border.
  readonly thickRightBorder?: boolean;
  readonly width?: string | number;
}

// Renders a `<td>` element, for use inside a `GridRow` within a `GridBody`.
export class GridDataCell implements m.ClassComponent<GridDataCellAttrs> {
  view({attrs, children}: m.Vnode<GridDataCellAttrs>) {
    const {
      menuItems,
      align,
      nullish,
      thickRightBorder,
      className,
      width = 100,
      ...rest
    } = attrs;

    const renderMenu = () => {
      return (
        Boolean(menuItems) &&
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              className: 'pf-grid__menu-button pf-visible-on-hover',
              icon: Icons.ContextMenuAlt,
              rounded: true,
            }),
          },
          menuItems,
        )
      );
    };

    return m(
      '.pf-grid__cell',
      {
        ...rest,
        className: classNames(
          className,
          thickRightBorder && 'pf-grid__cell--thick-right-border',
          align === 'right' && 'pf-grid__cell--rtl',
        ),
        style: {
          width: typeof width === 'number' ? `${width}px` : width,
        },
      },
      [
        m(
          '.pf-grid__cell--padded.pf-grid__cell--stretch',
          {
            className: classNames(
              align && `pf-grid__cell--align-${align}`,
              nullish && 'pf-grid__cell--nullish',
            ),
          },
          children,
        ),
        renderMenu(),
      ],
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
        label: 'Sort: highest first',
        icon: Icons.SortedDesc,
        onclick: () => sort('DESC'),
      }),
    sorted !== 'ASC' &&
      m(MenuItem, {
        label: 'Sort: lowest first',
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

export interface PageControlAttrs {
  readonly from: number;
  readonly to: number;
  readonly of: number;
  nextPageClick(): void;
  prevPageClick(): void;
  firstPageClick(): void;
  lastPageClick(): void;
}

import {Stack} from './stack';
import {Chip} from './chip';

export class PageControl implements m.ClassComponent<PageControlAttrs> {
  view({attrs}: m.Vnode<PageControlAttrs>) {
    const {
      from,
      to,
      of,
      firstPageClick,
      prevPageClick,
      nextPageClick,
      lastPageClick,
    } = attrs;

    const isFirstPage = from === 1;
    const isLastPage = to === of;

    return m(Stack, {className: 'pf-page-control', orientation: 'horizontal'}, [
      m('span', `${from} - ${to} of ${of}`),
      m(Button, {
        icon: Icons.FirstPage,
        disabled: isFirstPage,
        title: 'First Page',
        onclick: firstPageClick,
      }),
      m(Button, {
        icon: Icons.PrevPage,
        disabled: isFirstPage,
        title: 'Previous Page',
        onclick: prevPageClick,
      }),
      m(Button, {
        icon: Icons.NextPage,
        disabled: isLastPage,
        title: 'Next Page',
        onclick: nextPageClick,
      }),
      m(Button, {
        icon: Icons.LastPage,
        disabled: isLastPage,
        title: 'Last Page',
        onclick: lastPageClick,
      }),
    ]);
  }
}

export class GridFilterBar implements m.ClassComponent {
  view({children}: m.Vnode) {
    return m(Stack, {orientation: 'horizontal', wrap: true}, children);
  }
}

export interface GridFilterAttrs {
  readonly content: string;
  onRemove(): void;
}

export class GridFilterChip implements m.ClassComponent<GridFilterAttrs> {
  view({attrs}: m.Vnode<GridFilterAttrs>): m.Children {
    return m(Chip, {
      className: 'pf-grid-filter',
      label: attrs.content,
      removable: true,
      onRemove: attrs.onRemove,
      title: attrs.content,
    });
  }
}
