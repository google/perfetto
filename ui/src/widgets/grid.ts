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
import {isEmptyVnodes, MithrilEvent} from '../base/mithril_utils';
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
    const {
      sort,
      onSort,
      menuItems,
      subContent,
      hintSortDirection,
      ...htmlAttrs
    } = attrs;

    const renderSortButton = () => {
      if (!onSort) return undefined;

      const nextDirection: SortDirection = (() => {
        if (!sort) return hintSortDirection || 'ASC';
        if (sort === 'ASC') return 'DESC';
        if (sort === 'DESC') return 'ASC';
        return 'ASC';
      })();

      const sortIconDirection: SortDirection | undefined = (() => {
        if (!sort) return hintSortDirection;
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
      if (isEmptyVnodes(menuItems)) return undefined;
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            className: 'pf-visible-on-hover pf-grid-header-cell__menu-button',
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
        role: 'columnheader',
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
  readonly label?: string;
  readonly indent?: number;
  readonly chevron?: 'expanded' | 'collapsed' | 'leaf';
  readonly onChevronClick?: () => void;
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
      indent,
      chevron,
      onChevronClick,
      ...htmlAttrs
    } = attrs;

    const renderChevron = () => {
      if (chevron === undefined) return undefined;

      const icon = chevron === 'expanded' ? Icons.ExpandDown : Icons.GoForward;
      const ariaLabel = chevron === 'expanded' ? 'Collapse row' : 'Expand row';

      return m(Button, {
        className: classNames(
          'pf-grid-cell__chevron',
          chevron === 'leaf' && 'pf-grid-cell__chevron--leaf',
        ),
        icon,
        rounded: true,
        ariaLabel,
        onclick: (e: MouseEvent) => {
          if (onChevronClick) {
            onChevronClick();
            e.stopPropagation();
          }
        },
      });
    };

    const renderIndent = () => {
      if (indent === undefined || indent === 0) return undefined;

      return m('.pf-grid-cell__indent', {
        style: {
          width: `${indent * 16}px`,
        },
      });
    };

    return m(
      '.pf-grid-cell',
      {
        ...htmlAttrs,
        className: classNames(
          className,
          align === 'right' && !chevron && 'pf-grid-cell--align-right',
          padding && 'pf-grid-cell--padded',
          nullish && 'pf-grid-cell--nullish',
          wrap && 'pf-grid-cell--wrap',
        ),
        role: 'cell',
      },
      renderIndent(),
      renderChevron(),
      m('.pf-grid-cell__content', children),
      !isEmptyVnodes(menuItems) &&
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              className: 'pf-visible-on-hover pf-grid-cell__menu-button',
              icon: Icons.ContextMenuAlt,
              rounded: true,
              ariaLabel: 'Cell menu',
            }),
            position: PopupPosition.Bottom,
          },
          menuItems,
        ),
    );
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
  // A unique key identifying this column - used to store cached column widths
  // and for reordering callbacks.
  readonly key: string;

  // If defined, sets a fixed width for the column in pixels. The column will
  // not be resizable and will not auto-size.
  readonly widthPx?: number;

  // Controls the minimum width of the column in pixels.
  readonly minWidthPx?: number;

  // Controls the maximum initial width when auto-sizing columns. This is the
  // auto-size that is performed once the first time we see a column and we have
  // rows.
  readonly maxInitialWidthPx?: number;

  // If defined, the column can be reordered among other columns with the same
  // reorderGroup.
  readonly reorderable?: {readonly reorderGroup: string};

  // Renders this column with a thicker right border, useful for visually
  // grouping columns.
  readonly thickRightBorder?: boolean;

  // Content to put in the column header.
  readonly header?: m.Children;
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
/**
 * Configuration for the Grid component.
 * Grid is a low-level presentation component - consumers must wrap content
 * in GridHeaderCell and GridCell components.
 */
export interface GridAttrs {
  /**
   * Column definitions for the grid.
   * Each column specifies a key, optional header content, and display options.
   *
   * @example
   * columns: [
   *   {
   *     key: 'id',
   *     header: m(GridHeaderCell, {sort: 'ASC'}, 'ID'),
   *     minWidth: 100,
   *   },
   *   {
   *     key: 'name',
   *     header: m(GridHeaderCell, {menuItems: [...]}, 'Name'),
   *   },
   * ]
   */
  readonly columns: ReadonlyArray<GridColumn>;

  /**
   * Row data to display in the grid.
   * Can be either a full array of rows or a partial/paginated dataset.
   *
   * Full dataset (array):
   * - Use when all data fits in memory
   * - Virtualization is optional
   *
   * Partial dataset (PartialRowData):
   * - Use for large datasets with on-demand loading
   * - Virtualization is required
   *
   * @example Full dataset
   * rowData: [
   *   [m(GridCell, '1'), m(GridCell, 'Alice')],
   *   [m(GridCell, '2'), m(GridCell, 'Bob')],
   * ]
   *
   * @example Partial/paginated dataset
   * rowData: {
   *   data: currentRows,
   *   total: 1000000,
   *   offset: 0,
   *   onLoadData: (offset, limit) => {
   *     // Load data for requested range
   *   },
   * }
   */
  readonly rowData: GridRowData;

  /**
   * Virtual scrolling configuration.
   * When enabled, only visible rows are rendered for better performance.
   * Required when using PartialRowData, optional for full datasets.
   *
   * @example
   * virtualization: {
   *   rowHeightPx: 24,  // Fixed height for each row
   * }
   */
  readonly virtualization?: GridVirtualization;

  /**
   * Whether the grid should expand to fill its parent container's height.
   * When true, the grid will take up all available vertical space.
   * Default = false.
   *
   * @example
   * fillHeight: true
   */
  readonly fillHeight?: boolean;

  /**
   * Optional CSS class name to apply to the grid root element.
   * Used for custom styling.
   *
   * @example
   * className: 'my-custom-grid'
   */
  readonly className?: string;

  /**
   * Callback fired when the user hovers over a row.
   * Receives the absolute row index (not relative to current page).
   * Use with virtualized grids to implement row highlighting or preview features.
   *
   * @param rowIndex The absolute index of the hovered row
   *
   * @example
   * onRowHover: (rowIndex) => {
   *   console.log(`Hovering row ${rowIndex}`);
   * }
   */
  readonly onRowHover?: (rowIndex: number) => void;

  /**
   * Callback fired when the user's mouse leaves a row.
   * Pairs with onRowHover for implementing hover effects.
   *
   * @example
   * onRowOut: () => {
   *   console.log('Left row');
   * }
   */
  readonly onRowOut?: () => void;

  /**
   * Callback fired when columns are reordered via drag-and-drop.
   * Only called if column.reorderable is set on columns.
   *
   * @param from The key of the column being moved
   * @param to The key of the target column
   * @param position Whether to place before or after the target
   *
   * @example
   * onColumnReorder: (from, to, position) => {
   *   const newOrder = reorderArray(columnOrder, from, to, position);
   *   setColumnOrder(newOrder);
   * }
   */
  readonly onColumnReorder?: (
    from: string | number | undefined,
    to: string | number | undefined,
    position: ReorderPosition,
  ) => void;

  /**
   * Callback fired when the grid is fully initialized.
   * Receives an API object for programmatic control of the grid.
   * Use this to access methods like autoFitColumn() and autoFitAllColumns().
   *
   * @param api The grid's imperative API
   *
   * @example
   * onReady: (api) => {
   *   // Auto-fit all columns on mount
   *   api.autoFitAllColumns();
   * }
   */
  readonly onReady?: (api: GridApi) => void;

  /**
   * Content to display when the grid has no rows.
   * Typically used to show a helpful message or call-to-action.
   *
   * @example
   * emptyState: m(EmptyState, {
   *   icon: 'inbox',
   *   title: 'No data available',
   * })
   */
  readonly emptyState?: m.Children;
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

type ColumnAutosizeState = 'header-only' | 'sized';

export class Grid implements m.ClassComponent<GridAttrs> {
  private columnAutosizeState = new Map<string, ColumnAutosizeState>();
  private renderBounds?: {rowStart: number; rowEnd: number};
  private hasRenderedBodyRows = false;
  private fieldToId: Map<string, number> = new Map();
  private nextId = 0;
  private boundHandleCopy = this.handleCopy.bind(this);

  // Grid-level drag state for column reordering
  private dragState?: {
    fromKey: string;
    handle: string;
    targetKey?: string;
    position: ReorderPosition;
  };

  // Store column refs for hit testing during drag
  private columnRefs: Map<string, {left: number; width: number}> = new Map();

  // Find which column is at a given x position within the grid
  // Only returns columns that have a matching reorderable handle
  private findColumnAtX(
    x: number,
    columns: ReadonlyArray<GridColumn>,
  ): {key: string; position: ReorderPosition} | undefined {
    if (!this.dragState) return undefined;

    const handle = this.dragState.handle;

    for (const column of columns) {
      // Only consider columns with matching handle
      if (column.reorderable?.reorderGroup !== handle) continue;

      const bounds = this.columnRefs.get(column.key);
      if (bounds && x >= bounds.left && x < bounds.left + bounds.width) {
        const midpoint = bounds.left + bounds.width / 2;
        const position: ReorderPosition = x < midpoint ? 'before' : 'after';
        return {key: column.key, position};
      }
    }
    return undefined;
  }

  // Update column bounds from the header row
  private updateColumnBounds(gridDom: HTMLElement): void {
    const headerCells = gridDom.querySelectorAll(
      '.pf-grid__header .pf-grid__cell-container',
    );
    headerCells.forEach((cell) => {
      const htmlCell = cell as HTMLElement;
      const key = htmlCell.dataset['columnKey'];
      if (key) {
        const rect = htmlCell.getBoundingClientRect();
        this.columnRefs.set(key, {left: rect.left, width: rect.width});
      }
    });
  }

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

    // Remove all button elements to exclude them from the copy
    const buttons = tempDiv.querySelectorAll('button');
    buttons.forEach((button) => button.remove());

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

  private setColumnWidth(
    gridDom: HTMLElement,
    columnKey: string,
    widthPx: number,
  ): void {
    const columnId = this.getColumnId(columnKey);
    gridDom.style.setProperty(`--pf-grid-col-${columnId}`, `${widthPx}px`);
  }

  private clearColumnWidth(gridDom: HTMLElement, columnKey: string): void {
    const columnId = this.getColumnId(columnKey);
    gridDom.style.setProperty(`--pf-grid-col-${columnId}`, 'fit-content');
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

    // Check if any columns are reorderable
    const hasReorderableColumns = columns.some((c) => c.reorderable);

    // Render the grid structure inline
    return m(
      '.pf-grid',
      {
        className: classNames(
          fillHeight && 'pf-grid--fill-height',
          className,
          this.dragState && 'pf-grid--dragging',
        ),
        ref: 'scroll-container',
        role: 'table',
        // Grid-level drag handlers
        ondragover: hasReorderableColumns
          ? (e: MithrilEvent<DragEvent>) => {
              if (!this.dragState) return;
              e.preventDefault();
              e.dataTransfer!.dropEffect = 'move';

              // Update column bounds on drag (handles scrolling)
              const gridDom = e.currentTarget as HTMLElement;
              this.updateColumnBounds(gridDom);

              // Find which column we're over
              const hit = this.findColumnAtX(e.clientX, columns);
              if (hit) {
                const needsRedraw =
                  this.dragState.targetKey !== hit.key ||
                  this.dragState.position !== hit.position;
                this.dragState.targetKey = hit.key;
                this.dragState.position = hit.position;
                if (needsRedraw) {
                  m.redraw();
                }
              }
            }
          : undefined,
        ondrop: hasReorderableColumns
          ? (e: MithrilEvent<DragEvent>) => {
              if (!this.dragState || !attrs.onColumnReorder) return;
              e.preventDefault();

              const {fromKey, targetKey, position} = this.dragState;
              if (targetKey && fromKey !== targetKey) {
                attrs.onColumnReorder(fromKey, targetKey, position);
              }
              this.dragState = undefined;
            }
          : undefined,
        ondragend: hasReorderableColumns
          ? () => {
              this.dragState = undefined;
              m.redraw();
            }
          : undefined,
      },
      m(
        '.pf-grid__header',
        m(
          '.pf-grid__row',
          {
            role: 'row',
          },
          columns.map((column) => {
            return this.renderHeaderCell(column);
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
      totalRows === 0 &&
        attrs.emptyState !== undefined &&
        m('.pf-grid__empty-state', attrs.emptyState),
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

  oncreate({attrs, dom}: m.VnodeDOM<GridAttrs, this>) {
    const {virtualization, columns, rowData, onReady} = attrs;

    // Add copy event handler for spreadsheet-friendly formatting
    const gridDom = dom as HTMLElement;
    gridDom.addEventListener('copy', this.boundHandleCopy);

    this.maybeAutosizeColumns(gridDom, columns);

    // Only set up virtual scrolling if virtualization is enabled
    if (virtualization) {
      const rowHeight = virtualization.rowHeightPx;
      const onLoadData = isPartialRowData(rowData)
        ? rowData.onLoadData
        : undefined;

      const scrollContainer: HTMLElement = gridDom!;
      const slider: HTMLElement = gridDom.querySelector('[ref="slider"]')!;

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
    }

    // Call onReady callback with imperative API
    if (onReady) {
      onReady({
        autoFitColumn: (columnKey: string) => {
          const column = columns.find((c) => c.key === columnKey);
          if (!column) return;
          const {minWidthPx = COL_WIDTH_MIN_PX} = column;
          const columnWidths = this.measureColumns(gridDom, [column.key]);
          const width = columnWidths.get(column.key);
          if (width !== undefined) {
            this.setColumnWidth(
              gridDom,
              column.key,
              Math.max(width, minWidthPx),
            );
          }
        },
        autoFitAllColumns: () => {
          const columnWidths = this.measureColumns(
            gridDom,
            columns.map(({key}) => key),
          );
          for (const column of columns) {
            const {minWidthPx = COL_WIDTH_MIN_PX} = column;
            const width = columnWidths.get(column.key);
            if (width !== undefined) {
              this.setColumnWidth(
                gridDom,
                column.key,
                Math.max(width, minWidthPx),
              );
            }
          }
        },
      });
    }
  }

  onupdate(vnode: m.VnodeDOM<GridAttrs, this>) {
    const {columns} = vnode.attrs;
    this.maybeAutosizeColumns(vnode.dom as HTMLElement, columns);
  }

  onremove(vnode: m.VnodeDOM<GridAttrs, this>) {
    const gridDom = vnode.dom as HTMLElement;
    gridDom.removeEventListener('copy', this.boundHandleCopy);
  }

  // Check if any columns need resizing and update them
  private maybeAutosizeColumns(
    gridDom: HTMLElement,
    columns: readonly GridColumn[],
  ): void {
    // Handle fixed-width columns first (no measurement needed)
    for (const column of columns) {
      if (
        column.widthPx !== undefined &&
        this.columnAutosizeState.get(column.key) !== 'sized'
      ) {
        this.setColumnWidth(gridDom, column.key, column.widthPx);
        this.columnAutosizeState.set(column.key, 'sized');
      }
    }

    // Find auto-sized columns that need measurement:
    // - Skip fixed-width columns (handled above)
    // - Skip 'sized' columns (already done)
    // - Skip 'header-only' columns if we haven't rendered body rows yet
    const columnsToMeasure = columns.filter((col) => {
      if (col.widthPx !== undefined) return false;
      const state = this.columnAutosizeState.get(col.key);
      if (state === 'sized') return false;
      if (state === 'header-only' && !this.hasRenderedBodyRows) return false;
      return true;
    });

    if (columnsToMeasure.length === 0) return;

    const columnWidths = this.measureColumns(
      gridDom,
      columnsToMeasure.map(({key}) => key),
    );

    for (const column of columnsToMeasure) {
      const {
        key,
        minWidthPx = COL_WIDTH_MIN_PX,
        maxInitialWidthPx = COL_WIDTH_INITIAL_MAX_PX,
      } = column;
      const measuredWidth = columnWidths.get(key);
      if (measuredWidth === undefined) continue;

      const width = Math.min(
        Math.max(measuredWidth, minWidthPx),
        maxInitialWidthPx,
      );
      this.setColumnWidth(gridDom, key, width);
      this.columnAutosizeState.set(
        key,
        this.hasRenderedBodyRows ? 'sized' : 'header-only',
      );
    }
  }

  // Measures the natural width of each column by cloning the grid, setting
  // columns to fit-content, and measuring the max cell width per column.
  private measureColumns(
    gridDom: HTMLElement,
    columns: readonly string[],
  ): Map<string, number> {
    const gridClone = gridDom.cloneNode(true) as HTMLElement;
    gridDom.appendChild(gridClone);

    // Show any elements that are normally visible only on hover - this takes
    // into account the menu buttons, sort buttons, etc.
    const invisibleElements = gridClone.querySelectorAll(
      '.pf-visible-on-hover',
    );
    invisibleElements.forEach((el) => {
      (el as HTMLElement).style.display = 'block';
    });

    // Find all the cells in this column (header + data rows)
    const allCells = gridClone.querySelectorAll('.pf-grid__cell-container');

    // Clear any previously set widths to allow natural sizing
    columns.forEach((columnKey) => {
      this.clearColumnWidth(gridClone, columnKey);
    });

    // Measure all the cells we have available (this will cause a reflow)
    const columnKeyWidthTuples = columns.map((key) => {
      const id = this.getColumnId(key);

      // Find all the cells in this column and find the max width
      const maxWidth = Array.from(allCells)
        .filter((cell) => (cell as HTMLElement).dataset['columnId'] === `${id}`)
        .map((c) => c.scrollWidth)
        .reduce((acc, width) => Math.max(acc, width), 0);

      return [key, maxWidth + CELL_PADDING_PX] as const;
    });

    gridClone.remove();

    return new Map(columnKeyWidthTuples);
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

    // Generate a list of rows that should be rendered
    const renderableRows = indices.map((rowIndex) => {
      const relativeIndex = rowIndex - rowOffset;
      const row =
        relativeIndex >= 0 && relativeIndex < rows.length
          ? rows[relativeIndex]
          : undefined;
      return [rowIndex, row] as const;
    });

    const renderedRows = renderableRows
      .map(([rowIndex, row]) => {
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
                column.key,
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

    if (!this.hasRenderedBodyRows) {
      // Check if any rows have content in them so we know when to trigger our
      // initial column autosize
      const hasContent = renderableRows.some(
        ([_, row]) => row !== undefined && row.length > 0,
      );

      if (hasContent) {
        this.hasRenderedBodyRows = true;
      }
    }

    return renderedRows;
  }

  private renderAllRows(
    columns: ReadonlyArray<GridColumn>,
    rows: ReadonlyArray<GridRow>,
    onRowHover?: (rowIndex: number) => void,
    onRowOut?: () => void,
  ): m.Children {
    if (rows.length > 0) {
      this.hasRenderedBodyRows = true;
    }
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

          return this.renderCell(
            children,
            columnId,
            column.key,
            column.thickRightBorder,
          );
        }),
      );
    });
  }

  private renderCell(
    children: m.Children,
    columnId: number,
    columnKey: string,
    thickRightBorder?: boolean,
  ): m.Children {
    // Check if this column is the drag target (findColumnAtX already filters by handle)
    const isDragTarget =
      this.dragState &&
      this.dragState.targetKey === columnKey &&
      this.dragState.fromKey !== columnKey;

    return m(
      '.pf-grid__cell-container',
      {
        'style': {
          width: `var(--pf-grid-col-${columnId})`,
        },
        'data-column-id': columnId,
        'className': classNames(
          thickRightBorder && 'pf-grid__cell-container--border-right-thick',
          isDragTarget &&
            `pf-grid__cell-container--drag-over-${this.dragState!.position}`,
        ),
      },
      children,
    );
  }

  private renderHeaderCell(column: GridColumn): m.Children {
    const {
      key,
      reorderable,
      thickRightBorder,
      header,
      minWidthPx = COL_WIDTH_MIN_PX,
      widthPx,
    } = column;

    const columnId = this.getColumnId(column.key);
    const isFixedWidth = exists(widthPx);

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
            const minWidth = column.minWidthPx ?? COL_WIDTH_MIN_PX;
            const newWidth = Math.max(minWidth, startWidth + delta);
            this.setColumnWidth(gridDom, column.key, newWidth);
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

          const width = this.measureColumns(gridDom, [column.key]).get(
            column.key,
          )!;
          this.setColumnWidth(gridDom, column.key, Math.max(width, minWidthPx));
        },
      });
    };

    const reorderHandle = reorderable?.reorderGroup;

    // Check if this column is the drag target
    const isDragTarget =
      this.dragState &&
      this.dragState.targetKey === key &&
      this.dragState.fromKey !== key;

    return m(
      '.pf-grid__cell-container',
      {
        'data-column-id': columnId,
        'data-column-key': key,
        'key': key,
        'style': {
          width: `var(--pf-grid-col-${columnId})`,
        },
        'draggable': reorderable !== undefined,
        'className': classNames(
          thickRightBorder && 'pf-grid__cell-container--border-right-thick',
          isDragTarget &&
            `pf-grid__cell-container--drag-over-${this.dragState!.position}`,
        ),
        // Only ondragstart on header - other handlers are at grid level
        'ondragstart': (e: MithrilEvent<DragEvent>) => {
          if (!reorderHandle) return;
          e.dataTransfer!.setData(reorderHandle, JSON.stringify({key: key}));
          e.dataTransfer!.effectAllowed = 'move';
          // Initialize grid-level drag state
          this.dragState = {
            fromKey: key,
            handle: reorderHandle,
            targetKey: undefined,
            position: 'after',
          };
        },
      },
      header,
      !isFixedWidth && renderResizeHandle(),
    );
  }
}
