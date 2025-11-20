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
import {MithrilEvent} from '../base/mithril_utils';
import {Icons} from '../base/semantic_icons';
import {exists} from '../base/utils';
import {Button} from './button';
import {MenuItem, PopupMenu} from './menu';
import {PopupPosition} from './popup';
import {VirtualScrollHelper} from './virtual_scroll_helper';
import {HTMLAttrs} from './common';

const DEFAULT_ROW_HEIGHT = 24;
const COL_WIDTH_INITIAL_MAX_PX = 600;
const COL_WIDTH_MIN_PX = 50;
const CELL_PADDING_PX = 5;

export type SortDirection = 'ASC' | 'DESC';
export type CellAlignment = 'left' | 'center' | 'right';
export type ReorderPosition = 'before' | 'after';

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

export interface GridHeaderCellAttrs extends m.Attributes {
  readonly sort?: SortDirection;
  readonly onSort?: (direction: SortDirection) => void;
  readonly menuItems?: m.Children;
  readonly subContent?: m.Children;
  readonly hintSortDirection?: SortDirection;
}

export class GridHeaderCell implements m.ClassComponent<GridHeaderCellAttrs> {
  view({attrs, children}: m.Vnode<GridHeaderCellAttrs>) {
    const {sort, onSort, menuItems, subContent, ...htmlAttrs} = attrs;

    const renderSortButton = () => {
      if (!onSort) return undefined;

      const nextDirection: SortDirection = (() => {
        if (!sort) return attrs.hintSortDirection || 'ASC';
        if (sort === 'ASC') return 'DESC';
        if (sort === 'DESC') return 'ASC';
        return 'ASC';
      })();

      const sortIconDirection: SortDirection | undefined = (() => {
        if (!sort) return attrs.hintSortDirection;
        return sort;
      })();

      return m(Button, {
        className: classNames(
          'pf-grid-header-cell__sort-button',
          !sort && 'pf-grid-cell--hint',
          !sort && 'pf-visible-on-hover',
        ),
        ariaLabel: 'Sort column',
        rounded: true,
        icon: sortIconDirection === 'DESC' ? Icons.SortDesc : Icons.SortAsc,
        onclick: (e: MouseEvent) => {
          onSort(nextDirection);
          e.stopPropagation();
        },
      });
    };

    const renderMenu = () => {
      if (menuItems === undefined) return undefined;
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            className:
              'pf-visible-on-hover pf-grid-header-cell__menu-button pf-grid--no-measure',
            icon: Icons.ContextMenuAlt,
            rounded: true,
            ariaLabel: 'Column menu',
          }),
        },
        menuItems,
      );
    };

    return m(
      '.pf-grid-header-cell',
      {
        ...htmlAttrs,
      },
      [
        m(
          '.pf-grid-header-cell__main-content',
          m(
            '.pf-grid-header-cell__title',
            m('.pf-grid-header-cell__title-wrapper', children),
            renderSortButton(),
          ),
          renderMenu(),
        ),
        subContent !== undefined &&
          m('.pf-grid-header-cell__sub-content', subContent),
      ],
    );
  }
}

export interface GridCellAttrs extends HTMLAttrs {
  readonly menuItems?: m.Children;
  readonly align?: CellAlignment;
  readonly nullish?: boolean;
  readonly padding?: boolean;
  readonly wrap?: boolean;
}

export class GridCell implements m.ClassComponent<GridCellAttrs> {
  view({attrs, children}: m.Vnode<GridCellAttrs>) {
    const {
      menuItems,
      align = 'left',
      nullish,
      className,
      padding = true,
      wrap,
      ...rest
    } = attrs;

    const cell = m(
      '.pf-grid-cell',
      {
        ...rest,
        className: classNames(
          className,
          align && `pf-grid-cell--align-${align}`,
          padding && 'pf-grid-cell--padded',
          nullish && 'pf-grid-cell--nullish',
          wrap && 'pf-grid-cell--wrap',
        ),
      },
      children,
    );

    if (Boolean(menuItems)) {
      return m(
        PopupMenu,
        {
          trigger: cell,
          isContextMenu: true,
          position: PopupPosition.Bottom,
        },
        menuItems,
      );
    } else {
      return cell;
    }
  }
}

/**
 * Row data with cells and optional styling.
 */
export type GridRow = ReadonlyArray<m.Children>;

/**
 * Column definition for Grid.
 */
export interface GridColumn {
  readonly key: string;
  readonly maxInitialWidthPx?: number;
  readonly header?: m.Children;
  readonly minWidth?: number;
  readonly thickRightBorder?: boolean;
  readonly reorderable?: {readonly handle: string};
}

/**
 * Partial row data for virtual scrolling with paginated data.
 * When using this, virtualization must be enabled.
 */
export interface PartialRowData {
  readonly offset: number;
  readonly total: number;
  readonly data: ReadonlyArray<GridRow>;
  readonly onLoadData: (offset: number, limit: number) => void;
}

/**
 * Row data can be either:
 * - Full dataset (array)
 * - Partial/paginated dataset (object with offset, total, data, and load callback)
 */
export type GridRowData = ReadonlyArray<GridRow> | PartialRowData;

/**
 * Virtual scrolling configuration.
 * Required when using PartialRowData, optional otherwise.
 */
export interface GridVirtualization {
  readonly rowHeightPx: number;
}

/**
 * Imperative API for Grid component.
 * Provides methods to control the grid programmatically.
 */
export interface GridApi {
  /**
   * Auto-fit a column to its content width.
   * @param columnKey The key of the column to auto-fit
   */
  autoFitColumn(columnKey: string): void;

  /**
   * Auto-fit all columns to their content widths.
   */
  autoFitAllColumns(): void;
}

/**
 * Attributes for the Grid component.
 */
export interface GridAttrs {
  readonly columns: ReadonlyArray<GridColumn>;
  readonly rowData: GridRowData;
  readonly virtualization?: GridVirtualization;
  readonly fillHeight?: boolean;
  readonly className?: string;
  readonly onRowHover?: (rowIndex: number) => void;
  readonly onRowOut?: () => void;
  readonly onColumnReorder?: (
    from: string | number | undefined,
    to: string | number | undefined,
    position: ReorderPosition,
  ) => void;
  readonly onReady?: (api: GridApi) => void;
}

/**
 * Grid is a purely presentational component that renders tabular data with
 * virtual scrolling and column resizing. It provides the layout structure but
 * does NO automatic wrapping or transformation of content.
 *
 * Key features:
 * - Virtual scrolling for efficient rendering of large datasets
 * - Automatic column sizing based on content
 * - Manual column resizing via drag handles
 * - Double-click to auto-resize columns
 *
 * IMPORTANT: Grid is completely data-agnostic:
 * - Headers must be provided as GridHeaderCell components (for sorting, menus, reordering)
 * - Cells must be provided as GridCell components (for alignment, menus)
 * - Grid does NO automatic wrapping or injection of components
 * - Parent component is responsible for ALL content rendering
 *
 * For automatic features like sorting and filtering, use DataGrid instead.
 *
 * # Row Data API
 *
 * Grid supports two modes for providing row data:
 *
 * ## 1. Full Dataset (Array)
 * When you have all rows in memory, pass them as a simple array:
 * ```typescript
 * rowData: [
 *   [m(GridCell, '1'), m(GridCell, 'Alice')],
 *   [m(GridCell, '2'), m(GridCell, 'Bob')],
 * ]
 * ```
 *
 * ## 2. Partial/Paginated Dataset (PartialRowData)
 * For large datasets where you load data on-demand:
 * ```typescript
 * rowData: {
 *   data: [...],           // Current page of rows
 *   total: 1000000,        // Total number of rows
 *   offset: 0,             // Current offset
 *   onLoadData: (offset, limit) => {
 *     // Load and set data for the requested range
 *   }
 * }
 * ```
 * When using PartialRowData, virtualization MUST be enabled.
 *
 * # Virtualization
 *
 * Virtualization is optional for full datasets but required for partial data:
 * ```typescript
 * virtualization: {
 *   rowHeightPx: 24  // Height of each row in pixels
 * }
 * ```
 *
 * # Complete Examples
 *
 * Simple grid with full dataset (no virtualization):
 * ```typescript
 * m(Grid, {
 *   columns: [
 *     {key: 'id', header: m(GridHeaderCell, 'ID')},
 *     {key: 'name', header: m(GridHeaderCell, 'Name')},
 *   ],
 *   rowData: [
 *     [m(GridCell, {align: 'right'}, '1'), m(GridCell, 'Alice')],
 *     [m(GridCell, {align: 'right'}, '2'), m(GridCell, 'Bob')],
 *   ],
 *   fillHeight: true,
 * })
 * ```
 *
 * Grid with full dataset and DOM virtualization:
 * ```typescript
 * m(Grid, {
 *   columns: [...],
 *   rowData: [...1000 rows...],
 *   virtualization: {
 *     rowHeightPx: 24,  // Enables virtual scrolling
 *   },
 *   fillHeight: true,
 * })
 * ```
 *
 * Grid with partial/paginated data (virtualization required):
 * ```typescript
 * m(Grid, {
 *   columns: [...],
 *   rowData: {
 *     data: currentPageRows,
 *     total: 1000000,
 *     offset: currentOffset,
 *     onLoadData: (offset, limit) => {
 *       // Fetch and update currentPageRows, currentOffset
 *     },
 *   },
 *   virtualization: {
 *     rowHeightPx: 24,  // Required for PartialRowData
 *   },
 *   fillHeight: true,
 * })
 * ```
 */
function isPartialRowData(rowData: GridRowData): rowData is PartialRowData {
  return !Array.isArray(rowData);
}

export class Grid implements m.ClassComponent<GridAttrs> {
  private sizedColumns: Set<string> = new Set();
  private renderBounds?: {rowStart: number; rowEnd: number};
  private columnDragState: Map<
    string,
    {count: number; position: ReorderPosition}
  > = new Map();
  private fieldToId: Map<string, number> = new Map();
  private nextId = 0;
  private boundHandleCopy = this.handleCopy.bind(this);
  private handleCopy(e: ClipboardEvent): void {
    const selection = window.getSelection();
    if (!selection || selection.rangeCount === 0) return;

    const range = selection.getRangeAt(0);
    const container = range.commonAncestorContainer;

    // Find the grid element
    const gridElement =
      container.nodeType === Node.ELEMENT_NODE
        ? (container as Element).closest('.pf-grid')
        : (container.parentElement?.closest('.pf-grid') as Element | null);

    if (!gridElement) return;

    // Clone the selection's content
    const fragment = range.cloneContents();
    const tempDiv = document.createElement('div');
    tempDiv.appendChild(fragment);

    // Find all rows in the cloned content
    const rows = Array.from(
      tempDiv.querySelectorAll('.pf-grid__row'),
    ) as HTMLElement[];

    if (rows.length === 0) return;

    // Extract text from cells in TSV format
    const tsvRows = rows
      .map((row) => {
        const cells = Array.from(
          row.querySelectorAll('.pf-grid__cell-container'),
        ) as HTMLElement[];
        const cellTexts = cells
          .map((cell) => cell.textContent?.trim() || '')
          .filter((text) => text.length > 0);
        return cellTexts.join('\t');
      })
      .filter((row) => row.length > 0);

    if (tsvRows.length > 0) {
      const tsvData = tsvRows.join('\n');
      e.clipboardData?.setData('text/plain', tsvData);
      e.preventDefault();
    }
  }
  private getColumnId(field: string): number {
    if (!this.fieldToId.has(field)) {
      this.fieldToId.set(field, this.nextId++);
    }
    return this.fieldToId.get(field)!;
  }

  view({attrs}: m.Vnode<GridAttrs>) {
    const {
      columns,
      rowData,
      virtualization,
      fillHeight = false,
      className,
    } = attrs;

    // Validate: PartialRowData requires virtualization
    if (isPartialRowData(rowData) && virtualization === undefined) {
      throw new Error(
        'Grid: virtualization is required when using PartialRowData',
      );
    }

    // Extract row information
    const rows = isPartialRowData(rowData) ? rowData.data : rowData;
    const totalRows = isPartialRowData(rowData)
      ? rowData.total
      : rowData.length;
    const rowOffset = isPartialRowData(rowData) ? rowData.offset : 0;

    // Virtualization settings
    const isVirtualized = virtualization !== undefined;
    const rowHeight = virtualization?.rowHeightPx ?? DEFAULT_ROW_HEIGHT;

    // Render the grid structure inline
    return m(
      '.pf-grid',
      {
        className: classNames(fillHeight && 'pf-grid--fill-height', className),
        ref: 'scroll-container',
        role: 'table',
      },
      m(
        '.pf-grid__header',
        m(
          '.pf-grid__row',
          {
            role: 'row',
          },
          columns.map((column) => {
            return this.renderHeaderCell(column, attrs.onColumnReorder);
          }),
        ),
      ),
      isVirtualized
        ? this.renderVirtualizedGridBody(
            totalRows,
            rowHeight,
            columns,
            rows,
            rowOffset,
            attrs,
          )
        : this.renderGridBody(columns, rows, attrs),
    );
  }

  private renderVirtualizedGridBody(
    totalRows: number,
    rowHeight: number,
    columns: ReadonlyArray<GridColumn>,
    rows: ReadonlyArray<GridRow>,
    rowOffset: number,
    attrs: GridAttrs,
  ) {
    return m(
      '.pf-grid__body',
      {
        ref: 'slider',
        style: {
          height: `${totalRows * rowHeight}px`,
          // Ensure the puck cannot escape the slider and affect the height of
          // the scrollable region.
          overflowY: 'hidden',
        },
      },
      m(
        '.pf-grid__puck',
        {
          style: {
            transform: `translateY(${
              this.renderBounds?.rowStart !== undefined
                ? this.renderBounds.rowStart * rowHeight
                : 0
            }px)`,
          },
        },
        this.renderRows(
          columns,
          rows,
          rowOffset,
          rowHeight,
          attrs.onRowHover,
          attrs.onRowOut,
        ),
      ),
    );
  }

  private renderGridBody(
    columns: ReadonlyArray<GridColumn>,
    rows: ReadonlyArray<GridRow>,
    attrs: GridAttrs,
  ) {
    return m(
      '.pf-grid__body',
      this.renderAllRows(columns, rows, attrs.onRowHover, attrs.onRowOut),
    );
  }

  oncreate(vnode: m.VnodeDOM<GridAttrs, this>) {
    const {virtualization, columns, rowData} = vnode.attrs;

    // Extract rows from rowData
    const rows = isPartialRowData(rowData) ? rowData.data : rowData;

    // Add copy event handler for spreadsheet-friendly formatting
    const gridDom = vnode.dom as HTMLElement;
    gridDom.addEventListener('copy', this.boundHandleCopy);

    if (rows.length > 0) {
      // Check if there are new columns that need sizing
      const newColumns = columns.filter(
        (column) => !this.sizedColumns.has(column.key),
      );

      if (newColumns.length > 0) {
        this.measureAndApplyWidths(
          vnode.dom as HTMLElement,
          newColumns.map((col) => {
            const {
              key,
              minWidth = COL_WIDTH_MIN_PX,
              maxInitialWidthPx = COL_WIDTH_INITIAL_MAX_PX,
            } = col;

            return {
              key,
              minWidth,
              maxWidth: maxInitialWidthPx,
            };
          }),
        );
      }
    }

    // Only set up virtual scrolling if virtualization is enabled
    if (virtualization === undefined) {
      return;
    }

    const rowHeight = virtualization.rowHeightPx;
    const onLoadData = isPartialRowData(rowData)
      ? rowData.onLoadData
      : undefined;

    const scrollContainer: HTMLElement = (vnode.dom as HTMLElement)!;
    const slider: HTMLElement = (vnode.dom as HTMLElement).querySelector(
      '[ref="slider"]',
    )!;

    new VirtualScrollHelper(slider, scrollContainer, [
      {
        overdrawPx: 500,
        tolerancePx: 250,
        callback: (rect) => {
          const rowStart = Math.floor(rect.top / rowHeight);
          const rowCount = Math.ceil(rect.height / rowHeight);
          this.renderBounds = {rowStart, rowEnd: rowStart + rowCount};
          m.redraw();
        },
      },
      {
        overdrawPx: 2000,
        tolerancePx: 1000,
        callback: (rect) => {
          const rowStart = Math.floor(rect.top / rowHeight);
          const rowEnd = Math.ceil(rect.bottom / rowHeight);
          if (onLoadData !== undefined) {
            onLoadData(rowStart, rowEnd - rowStart);
          }
          m.redraw();
        },
      },
    ]);

    // Call onReady callback with imperative API
    if (vnode.attrs.onReady) {
      vnode.attrs.onReady({
        autoFitColumn: (columnKey: string) => {
          const gridDom = vnode.dom as HTMLElement;
          const column = columns.find((c) => c.key === columnKey);
          if (!column) return;

          this.measureAndApplyWidths(gridDom, [
            {
              key: column.key,
              minWidth: column.minWidth ?? COL_WIDTH_MIN_PX,
              maxWidth: Infinity,
            },
          ]);
          m.redraw();
        },
        autoFitAllColumns: () => {
          const gridDom = vnode.dom as HTMLElement;
          this.measureAndApplyWidths(
            gridDom,
            columns.map((column) => ({
              key: column.key,
              minWidth: column.minWidth ?? COL_WIDTH_MIN_PX,
              maxWidth: Infinity,
            })),
          );
          m.redraw();
        },
      });
    }
  }

  onupdate(vnode: m.VnodeDOM<GridAttrs, this>) {
    const {columns, rowData} = vnode.attrs;

    // Extract rows from rowData
    const rows = isPartialRowData(rowData) ? rowData.data : rowData;

    if (rows.length > 0) {
      // Check if there are new columns that need sizing
      const newColumns = columns.filter(
        (column) => !this.sizedColumns.has(column.key),
      );

      if (newColumns.length > 0) {
        this.measureAndApplyWidths(
          vnode.dom as HTMLElement,
          newColumns.map((col) => {
            const {
              key,
              minWidth = COL_WIDTH_MIN_PX,
              maxInitialWidthPx = COL_WIDTH_INITIAL_MAX_PX,
            } = col;

            return {
              key,
              minWidth,
              maxWidth: maxInitialWidthPx,
            };
          }),
        );
      }
    }
  }

  onremove(vnode: m.VnodeDOM<GridAttrs, this>) {
    const gridDom = vnode.dom as HTMLElement;
    gridDom.removeEventListener('copy', this.boundHandleCopy);
  }

  private measureAndApplyWidths(
    gridDom: HTMLElement,
    columns: ReadonlyArray<{
      readonly key: string;
      readonly minWidth: number;
      readonly maxWidth: number;
    }>,
  ): void {
    const gridClone = gridDom.cloneNode(true) as HTMLElement;
    gridDom.appendChild(gridClone);

    // Hide any elements that are not part of the measurement - these are
    // elements with class .pf-grid--no-measure
    const noMeasureElements = gridClone.querySelectorAll(
      '.pf-grid--no-measure',
    );
    noMeasureElements.forEach((el) => {
      (el as HTMLElement).style.display = 'none';
    });

    // Now read the actual widths (this will cause a reflow)
    // Find all the cells in this column (header + data rows)
    const allCells = gridClone.querySelectorAll(`.pf-grid__cell-container`);

    // Only continue if we have more cells than just the header
    if (allCells.length <= columns.length) {
      gridClone.remove();
      return;
    }

    columns.forEach((column) => {
      const columnId = this.getColumnId(column.key);

      // Clear the existing width to allow natural sizing
      gridClone.style.setProperty(`--pf-grid-col-${columnId}`, 'fit-content');

      // Find all the cells in this column
      const cellsInThisColumn = Array.from(allCells).filter(
        (cell) => (cell as HTMLElement).dataset['columnId'] === `${columnId}`,
      );

      const widths = cellsInThisColumn.map((c) => {
        return c.scrollWidth;
      });
      const maxCellWidth = Math.max(...widths);
      const unboundedWidth = maxCellWidth + CELL_PADDING_PX;
      const width = Math.min(
        column.maxWidth,
        Math.max(column.minWidth, unboundedWidth),
      );

      gridDom.style.setProperty(`--pf-grid-col-${columnId}`, `${width}px`);

      // Store the width
      this.sizedColumns.add(column.key);
    });

    gridClone.remove();
  }

  private renderRows(
    columns: ReadonlyArray<GridColumn>,
    rows: ReadonlyArray<GridRow>,
    rowOffset: number,
    rowHeight: number,
    onRowHover?: (rowIndex: number) => void,
    onRowOut?: () => void,
  ): m.Children {
    if (this.renderBounds === undefined) {
      return undefined;
    }

    const {rowStart, rowEnd} = this.renderBounds;
    const displayRowCount = rowEnd - rowStart;

    const indices = Array.from(
      {length: displayRowCount},
      (_, i) => rowStart + i,
    );

    return indices
      .map((rowIndex) => {
        const relativeIndex = rowIndex - rowOffset;
        const row =
          relativeIndex >= 0 && relativeIndex < rows.length
            ? rows[relativeIndex]
            : undefined;

        if (row !== undefined) {
          return m(
            '.pf-grid__row',
            {
              key: rowIndex,
              role: 'row',
              style: {
                height: `${rowHeight}px`,
              },
              onmouseenter: onRowHover ? () => onRowHover(rowIndex) : undefined,
              onmouseleave: onRowOut,
            },
            columns.map((column, index) => {
              const children = row[index];
              const columnId = this.getColumnId(column.key);

              return this.renderCell(
                children,
                columnId,
                column.thickRightBorder,
              );
            }),
          );
        } else {
          // Return empty spacer instead if row is not present
          return m('.pf-grid__row', {
            key: rowIndex,
            role: 'row',
            style: {
              height: `${rowHeight}px`,
            },
          });
        }
      })
      .filter(exists);
  }

  private renderAllRows(
    columns: ReadonlyArray<GridColumn>,
    rows: ReadonlyArray<GridRow>,
    onRowHover?: (rowIndex: number) => void,
    onRowOut?: () => void,
  ): m.Children {
    return rows.map((row, rowIndex) => {
      return m(
        '.pf-grid__row',
        {
          key: rowIndex,
          role: 'row',
          onmouseenter: onRowHover ? () => onRowHover(rowIndex) : undefined,
          onmouseleave: onRowOut,
        },
        columns.map((column, index) => {
          const children = row[index];
          const columnId = this.getColumnId(column.key);

          return this.renderCell(children, columnId, column.thickRightBorder);
        }),
      );
    });
  }

  private renderCell(
    children: m.Children,
    columnId: number,
    thickRightBorder?: boolean,
  ): m.Children {
    return m(
      '.pf-grid__cell-container',
      {
        'style': {
          width: `var(--pf-grid-col-${columnId})`,
        },
        'role': 'cell',
        'data-column-id': columnId,
        'className': classNames(
          thickRightBorder && 'pf-grid__cell-container--border-right-thick',
        ),
      },
      children,
    );
  }

  private renderHeaderCell(
    column: GridColumn,
    onColumnReorder?: (
      from: string | number | undefined,
      to: string | number | undefined,
      position: ReorderPosition,
    ) => void,
  ): m.Children {
    const columnId = this.getColumnId(column.key);

    const renderResizeHandle = () => {
      return m('.pf-grid__resize-handle', {
        onpointerdown: (e: MouseEvent) => {
          e.preventDefault();
          e.stopPropagation();

          // Find the nearest header cell to get the starting width
          const headerCell = (e.currentTarget as HTMLElement).closest(
            '.pf-grid__cell-container',
          );

          if (!headerCell) return;

          const startX = e.clientX;
          const startWidth = headerCell.scrollWidth;

          const gridDom = (e.currentTarget as HTMLElement).closest(
            '.pf-grid',
          ) as HTMLElement | null;
          if (gridDom === null) return;

          const handlePointerMove = (e: MouseEvent) => {
            const delta = e.clientX - startX;
            const minWidth = column.minWidth ?? COL_WIDTH_MIN_PX;
            const newWidth = Math.max(minWidth, startWidth + delta);

            // Set the css variable for the column being resized
            gridDom.style.setProperty(
              `--pf-grid-col-${columnId}`,
              `${newWidth}px`,
            );
          };

          const handlePointerUp = () => {
            document.removeEventListener('pointermove', handlePointerMove);
            document.removeEventListener('pointerup', handlePointerUp);
          };

          document.addEventListener('pointermove', handlePointerMove);
          document.addEventListener('pointerup', handlePointerUp);
        },
        oncontextmenu: (e: MouseEvent) => {
          // Prevent right click, as this can interfere with mouse/pointer
          // events
          e.preventDefault();
        },
        ondblclick: (e: MouseEvent) => {
          e.preventDefault();
          e.stopPropagation();

          // Auto-resize this column by measuring actual DOM
          const target = e.currentTarget as HTMLElement;
          const headerCell = target.parentElement as HTMLElement;
          const gridDom = headerCell.closest('.pf-grid') as HTMLElement | null;

          if (gridDom === null) return;

          this.measureAndApplyWidths(gridDom, [
            {
              key: column.key,
              minWidth: column.minWidth ?? COL_WIDTH_MIN_PX,
              // No max - columns can grow as wide as needed on double-click
              maxWidth: Infinity,
            },
          ]);
        },
      });
    };

    const reorderHandle = column.reorderable?.handle;
    const dragOverState = this.columnDragState.get(column.key) ?? {
      count: 0,
      position: 'after' as ReorderPosition,
    };

    return m(
      '.pf-grid__cell-container',
      {
        'role': 'columnheader',
        'ariaLabel': column.key,
        'data-column-id': columnId,
        'key': column.key,
        'style': {
          width: `var(--pf-grid-col-${columnId})`,
        },
        'draggable': column.reorderable !== undefined,
        'className': classNames(
          column.thickRightBorder &&
            'pf-grid__cell-container--border-right-thick',
          dragOverState.count > 0 && 'pf-grid__cell-container--drag-over',
          dragOverState.count > 0 &&
            `pf-grid__cell-container--drag-over-${dragOverState.position}`,
        ),
        'ondragstart': (e: MithrilEvent<DragEvent>) => {
          if (!reorderHandle) return;
          e.redraw = false;
          e.dataTransfer!.setData(
            reorderHandle,
            JSON.stringify({key: column.key}),
          );
        },
        'ondragenter': (e: MithrilEvent<DragEvent>) => {
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            const state = this.columnDragState.get(column.key) ?? {
              count: 0,
              position: 'after' as ReorderPosition,
            };
            this.columnDragState.set(column.key, {
              ...state,
              count: state.count + 1,
            });
          }
        },
        'ondragleave': (e: MithrilEvent<DragEvent>) => {
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            const state = this.columnDragState.get(column.key);
            if (state) {
              this.columnDragState.set(column.key, {
                ...state,
                count: state.count - 1,
              });
            }
          }
        },
        'ondragover': (e: MithrilEvent<DragEvent>) => {
          e.preventDefault();
          if (reorderHandle && e.dataTransfer!.types.includes(reorderHandle)) {
            e.dataTransfer!.dropEffect = 'move';
            const target = e.currentTarget as HTMLElement;
            const rect = target.getBoundingClientRect();
            const position: ReorderPosition =
              e.clientX < rect.left + rect.width / 2 ? 'before' : 'after';
            const state = this.columnDragState.get(column.key) ?? {
              count: 0,
              position: 'after' as ReorderPosition,
            };
            if (state.position !== position) {
              this.columnDragState.set(column.key, {...state, position});
            }
          } else {
            e.dataTransfer!.dropEffect = 'none';
          }
        },
        'ondrop': (e: MithrilEvent<DragEvent>) => {
          this.columnDragState.set(column.key, {count: 0, position: 'after'});
          if (reorderHandle && onColumnReorder) {
            const data = e.dataTransfer!.getData(reorderHandle);
            if (data) {
              e.preventDefault();
              const {key: from} = JSON.parse(data);
              const to = column.key;
              const target = e.currentTarget as HTMLElement;
              const rect = target.getBoundingClientRect();
              const position =
                e.clientX < rect.left + rect.width / 2 ? 'before' : 'after';
              onColumnReorder(from, to, position);
            }
          }
        },
      },
      column.header ?? column.key,
      renderResizeHandle(),
    );
  }
}
