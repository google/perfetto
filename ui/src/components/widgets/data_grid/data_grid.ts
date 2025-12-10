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
import {classNames} from '../../../base/classnames';
import {download} from '../../../base/download_utils';
import {FuzzyFinder} from '../../../base/fuzzy';
import {Icons} from '../../../base/semantic_icons';
import {exists, maybeUndefined} from '../../../base/utils';
import {SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {Form} from '../../../widgets/form';
import {
  Grid,
  GridApi,
  GridCell,
  GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
} from '../../../widgets/grid';
import {Icon} from '../../../widgets/icon';
import {LinearProgress} from '../../../widgets/linear_progress';
import {MenuDivider, MenuItem} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import {
  ColumnSchema,
  SchemaRegistry,
  getColumnAggregation,
  getColumnCellContextMenuRenderer,
  getColumnCellFormatter,
  getColumnCellRenderer,
  getColumnContextMenuRenderer,
  getColumnDistinctValues,
  getColumnFilterType,
  getColumnTitleParts,
  getColumnTitleString,
  getDefaultVisibleColumns,
  isColumnDef,
  isParameterizedColumnDef,
  isSchemaRef,
  resolveColumnPath,
} from './column_schema';
import {
  AggregationFunction,
  ColumnDefinition,
  DataGridDataSource,
  DataGridFilter,
  RowDef,
  Sorting,
} from './common';
import {DataGridToolbar} from './data_grid_toolbar';
import {
  defaultValueFormatter,
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
} from './export_utils';
import {InMemoryDataSource} from './in_memory_data_source';

export interface AggregationCellAttrs extends m.Attributes {
  readonly symbol?: string;
}

export class AggregationCell implements m.ClassComponent<AggregationCellAttrs> {
  view({attrs, children}: m.Vnode<AggregationCellAttrs>) {
    const {className, symbol, ...rest} = attrs;
    return m(
      '.pf-aggr-cell',
      {
        ...rest,
        className: classNames(className),
      },
      m('.pf-aggr-cell__symbol', symbol),
      m('.pf-aggr-cell__content', children),
    );
  }
}

/**
 * DataGrid is designed to be a flexible and efficient data viewing and analysis
 * tool. DataGrid at its core merely defines how the UI looks and UX feels. It
 * has no opinions about how data should be loaded or cached - that's what
 * DataSources are for.
 *
 * DataGrid comes with a few datasources out of the box - such as a simple
 * in-memory one where the source data is a simply javascript array and all the
 * operations are performed in javascript, and a simple SQL one where the source
 * of truth is defined using a SQL query and the data is loaded from TP.
 *
 * Most of DataGrid's state can operate in controlled or uncontrolled modes,
 * which allow the developer using this component to store the state anywhere
 * they please and also control it and mutate it from outside (controlled mode),
 * or it can operate internally which can make it easier to get up and running
 * (uncontrolled mode).
 */

type OnFilterAdd = (filter: DataGridFilter) => void;
export type OnFilterRemove = (index: number) => void;
type OnSortingChanged = (sorting: Sorting) => void;
type ColumnOrder = ReadonlyArray<string>;
type OnColumnOrderChanged = (columnOrder: ColumnOrder) => void;

function noOp() {}

export interface DataGridAttrs {
  /**
   * Schema registry defining the shape of available data.
   * Contains named schemas that can reference each other for nested relationships.
   */
  readonly schema: SchemaRegistry;

  /**
   * The name of the root schema in the registry to use for this grid.
   */
  readonly rootSchema: string;

  /**
   * Array of column paths that are currently visible, in display order.
   * Each path is a dot-separated string like 'id', 'parent.name', 'thread.process.pid'.
   *
   * In controlled mode: Provide this prop along with onVisibleColumnsChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage columns internally,
   * defaulting to all leaf columns in the root schema.
   */
  readonly visibleColumns?: ReadonlyArray<string>;

  /**
   * The data source that provides rows to the grid. Responsible for fetching,
   * filtering, and sorting data based on the current state.
   *
   * The data source is responsible for applying the filters, sorting, and
   * paging and providing the rows that are displayed in the grid.
   */
  readonly data: DataGridDataSource | ReadonlyArray<RowDef>;

  /**
   * Current sort configuration - can operate in controlled or uncontrolled
   * mode.
   *
   * In controlled mode: Provide this prop along with onSortingChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage sorting state
   * internally.
   *
   * Specifies which column to sort by and the direction (asc/DESC/unsorted). If
   * not provided, defaults to internal state with direction 'unsorted'.
   */
  readonly sorting?: Sorting;

  /**
   * Initial sorting to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `sorting` is provided).
   */
  readonly initialSorting?: Sorting;

  /**
   * Callback triggered when the sort configuration changes.
   * Allows parent components to react to sorting changes.
   * Required for controlled mode sorting - when provided with sorting,
   * the parent component becomes responsible for updating the sorting prop.
   * @param sorting The new sort configuration
   */
  readonly onSort?: OnSortingChanged;

  /**
   * Array of filters to apply to the data - can operate in controlled or
   * uncontrolled mode.
   *
   * In controlled mode: Provide this prop along with onFiltersChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage filter state
   * internally.
   *
   * Each filter contains a column name, operator, and comparison value. If not
   * provided, defaults to an empty array (no filters initially applied).
   */
  readonly filters?: ReadonlyArray<DataGridFilter>;

  /**
   * Initial filters to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `filters` is provided).
   */
  readonly initialFilters?: ReadonlyArray<DataGridFilter>;

  /**
   * These callbacks are triggered when filters are added or removed by the
   * user. They are only called in controlled mode, e.g. only if filters is
   * provided.
   */
  readonly onFilterAdd?: OnFilterAdd;
  readonly onFilterRemove?: OnFilterRemove;
  readonly clearFilters?: () => void;

  /**
   * Order of columns to display - can operate in controlled or uncontrolled
   * mode.
   *
   * In controlled mode: Provide this prop along with onColumnOrderChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage order internally.
   *
   * Array of column names in the order they should be displayed.
   * If not provided, columns are displayed in the order given in the columns prop.
   */
  readonly columnOrder?: ColumnOrder;

  /**
   * Initial column order to apply on first load.
   * This is ignored in controlled mode (i.e. when `columnOrder` is provided).
   */
  readonly initialColumnOrder?: ColumnOrder;

  /**
   * Callback triggered when columns are reordered via drag-and-drop.
   * Allows parent components to react to reordering changes.
   * Required for controlled mode - when provided with columnOrder,
   * the parent component becomes responsible for updating the columnOrder prop.
   * @param columnOrder The new array of column names in display order
   */
  readonly onColumnOrderChanged?: OnColumnOrderChanged;

  /**
   * Whether to enable column reordering via drag-and-drop.
   * Default = true if onColumnOrderChanged is provided, false otherwise.
   */
  readonly columnReordering?: boolean;

  /**
   * Display applied filters in the toolbar. Set to false to hide them, for
   * example, if filters are displayed elsewhere in the UI. This does not
   * disable filtering functionality.
   *
   * Defaults to true.
   */
  readonly showFiltersInToolbar?: boolean;

  /**
   * Fill parent container vertically.
   */
  readonly fillHeight?: boolean;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsLeft?: m.Children;

  /**
   * Extra items to place on the toolbar.
   */
  readonly toolbarItemsRight?: m.Children;

  /**
   * Optional class name added to the root element of the data grid.
   */
  readonly className?: string;

  /**
   * Enable export buttons in toolbar. When enabled, adds Copy and Download
   * buttons that export the current filtered/sorted data.
   * Default = false.
   */
  readonly showExportButton?: boolean;

  /**
   * Show row count in toolbar. Displays the number of filtered rows and total rows.
   * Default = false.
   */
  readonly showRowCount?: boolean;

  /**
   * Callback that receives the DataGrid API when the grid is ready.
   * Allows parent components to programmatically export data.
   */
  readonly onReady?: (api: DataGridApi) => void;

  /**
   * When true, disables 'not glob' and 'not contains' filter options. Use this
   * when the backend (e.g., structured query) doesn't support negated glob
   * operations.
   *
   * TODO(stevegolton/mazyner): Remove this flag when structured query supports
   * not glob.
   *
   * Default = false.
   */
  readonly structuredQueryCompatMode?: boolean;

  /**
   * Callback triggered when visible columns change (add, remove, reorder).
   * Required for controlled mode - when provided with visibleColumns,
   * the parent component becomes responsible for updating the visibleColumns prop.
   */
  readonly onVisibleColumnsChanged?: (columns: ReadonlyArray<string>) => void;
}

export interface DataGridApi {
  /**
   * Export all filtered and sorted data from the grid.
   * @param format The format to export in
   * @returns Promise<string> The formatted data as a string
   */
  exportData(format: 'tsv' | 'json' | 'markdown'): Promise<string>;

  /**
   * Get the total number of rows in the current filtered dataset.
   * @returns The total row count
   */
  getRowCount(): number;
}

/**
 * Helper function to convert old-style ColumnDefinition[] to the new schema format.
 * This provides backwards compatibility during migration.
 *
 * The grid will operate in uncontrolled mode for columns, with initialColumnOrder
 * set to show all columns in the order they are defined. Users can add/remove/reorder
 * columns via the column header menu.
 *
 * @param columns Array of column definitions in the old format
 * @returns An object with schema, rootSchema, and initialColumnOrder for use with DataGrid
 */
export function columnsToSchema(columns: ReadonlyArray<ColumnDefinition>): {
  schema: SchemaRegistry;
  rootSchema: string;
  initialColumnOrder: ReadonlyArray<string>;
} {
  const schema: ColumnSchema = {};

  // Build schema in the order columns are provided
  for (const col of columns) {
    schema[col.name] = {
      title: typeof col.title === 'string' ? col.title : undefined,
      filterType: col.filterType,
      cellRenderer: col.cellRenderer,
      cellFormatter: col.cellFormatter,
      aggregation: col.aggregation,
      distinctValues: col.distinctValues,
      contextMenuRenderer: col.contextMenuRenderer,
      cellContextMenuRenderer: col.cellContextMenuRenderer,
    };
  }

  return {
    schema: {data: schema},
    rootSchema: 'data',
    initialColumnOrder: columns.map((col) => col.name),
  };
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private sorting: Sorting = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<DataGridFilter> = [];
  private internalVisibleColumns: ReadonlyArray<string> = [];
  // Track pagination state from virtual scrolling
  private paginationOffset: number = 0;
  private paginationLimit: number = 100;
  private gridApi?: GridApi;
  // Track columns needing distinct values
  private distinctValuesColumns = new Set<string>();
  // Track parameterized columns needing key discovery
  private parameterKeyColumns = new Set<string>();
  private dataGridApi: DataGridApi = {
    exportData: async (format) => {
      if (!this.currentDataSource || !this.currentVisibleColumns) {
        throw new Error('DataGrid not ready for export');
      }
      return await this.formatData(
        this.currentDataSource,
        this.currentSchema,
        this.currentRootSchema,
        this.currentVisibleColumns,
        format,
      );
    },
    getRowCount: () => {
      return this.currentDataSource?.rows?.totalRows ?? 0;
    },
  };
  private currentDataSource?: DataGridDataSource;
  private currentSchema?: SchemaRegistry;
  private currentRootSchema?: string;
  private currentVisibleColumns?: ReadonlyArray<string>;

  oninit({attrs}: m.Vnode<DataGridAttrs>) {
    if (attrs.initialSorting) {
      this.sorting = attrs.initialSorting;
    }

    if (attrs.initialFilters) {
      this.filters = attrs.initialFilters;
    }

    // Initialize visible columns from initial prop, visibleColumns, or default from schema
    if (attrs.initialColumnOrder) {
      this.internalVisibleColumns = attrs.initialColumnOrder;
    } else if (attrs.visibleColumns) {
      this.internalVisibleColumns = attrs.visibleColumns;
    } else {
      // Default to all leaf columns in the root schema
      this.internalVisibleColumns = getDefaultVisibleColumns(
        attrs.schema,
        attrs.rootSchema,
      );
    }
  }

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      schema,
      rootSchema,
      visibleColumns: propsVisibleColumns,
      data,
      sorting = this.sorting,
      onSort = sorting === this.sorting ? (x) => (this.sorting = x) : noOp,
      filters = this.filters,
      onFilterAdd = filters === this.filters
        ? (filter) => {
            this.filters = [...this.filters, filter];
          }
        : noOp,
      onFilterRemove = filters === this.filters
        ? (index) => {
            const newFilters = this.filters.filter((_, i) => i !== index);
            this.filters = newFilters;
          }
        : noOp,
      clearFilters = filters === this.filters
        ? () => {
            this.filters = [];
          }
        : noOp,
      onVisibleColumnsChanged,
      // Enable reordering in uncontrolled mode or when callback is provided
      columnReordering = propsVisibleColumns === undefined ||
        onVisibleColumnsChanged !== undefined,
      showFiltersInToolbar = true,
      fillHeight = false,
      toolbarItemsLeft,
      toolbarItemsRight,
      className,
      showExportButton = false,
      showRowCount = false,
      onReady,
      structuredQueryCompatMode = false,
    } = attrs;

    // Use props visible columns or internal state, defaulting to schema leaf columns
    const visibleColumns =
      propsVisibleColumns ??
      (this.internalVisibleColumns.length > 0
        ? this.internalVisibleColumns
        : getDefaultVisibleColumns(schema, rootSchema));
    const onColumnsChanged =
      onVisibleColumnsChanged ??
      ((cols) => {
        this.internalVisibleColumns = cols;
      });

    // Initialize the datasource if required
    let dataSource: DataGridDataSource;
    if (Array.isArray(data)) {
      // If raw data supplied - just create a new in memory data source every
      // render cycle.
      dataSource = new InMemoryDataSource(data);
    } else {
      dataSource = data as DataGridDataSource;
    }

    // Build aggregates from schema - find columns with aggregation set
    const aggregates = visibleColumns
      .map((colPath) => {
        const agg = getColumnAggregation(schema, rootSchema, colPath);
        return agg ? {col: colPath, func: agg} : null;
      })
      .filter((x): x is {col: string; func: AggregationFunction} => x !== null);

    // Update datasource with current state (sorting, filtering, pagination)
    // This is called every view cycle to catch changes
    dataSource.notifyUpdate({
      columns: [...visibleColumns],
      sorting,
      filters,
      pagination: {
        offset: this.paginationOffset,
        limit: this.paginationLimit,
      },
      aggregates,
      distinctValuesColumns: this.distinctValuesColumns,
      parameterKeyColumns: this.parameterKeyColumns,
    });

    // Store current state for API access
    this.currentDataSource = dataSource;
    this.currentSchema = schema;
    this.currentRootSchema = rootSchema;
    this.currentVisibleColumns = visibleColumns;

    // Create and expose DataGrid API if needed
    onReady?.(this.dataGridApi);

    const sortControls = onSort !== noOp;
    const filtersUncontrolled = filters === this.filters;
    const filterControls = Boolean(
      filtersUncontrolled || onFilterAdd !== noOp || onFilterRemove !== noOp,
    );

    // Build VirtualGrid columns with all DataGrid features
    const virtualGridColumns = visibleColumns.map((columnPath) => {
      // Look up column properties from schema
      const columnTitleParts = getColumnTitleParts(
        schema,
        rootSchema,
        columnPath,
      );
      const columnFilterType = getColumnFilterType(
        schema,
        rootSchema,
        columnPath,
      );
      const columnDistinctValues = getColumnDistinctValues(
        schema,
        rootSchema,
        columnPath,
      );
      const columnAggregation = getColumnAggregation(
        schema,
        rootSchema,
        columnPath,
      );
      const columnCellRenderer = getColumnCellRenderer(
        schema,
        rootSchema,
        columnPath,
      );
      const columnContextMenuRenderer = getColumnContextMenuRenderer(
        schema,
        rootSchema,
        columnPath,
      );

      const sort = (() => {
        if (sorting.direction === 'UNSORTED') {
          return undefined;
        } else if (sorting.column === columnPath) {
          return sorting.direction;
        } else {
          return undefined;
        }
      })();

      // Build default menu groups
      const defaultGroups: {
        sorting?: m.Children;
        filters?: m.Children;
        fitToContent?: m.Children;
        columnManagement?: m.Children;
      } = {};

      // Sorting group
      if (sortControls) {
        defaultGroups.sorting = [
          ...renderSortMenuItems(sort, (direction) => {
            if (direction) {
              onSort({
                column: columnPath,
                direction: direction,
              });
            } else {
              onSort({
                direction: 'UNSORTED',
              });
            }
          }),
        ];
      }

      // Filters group
      if (filterControls) {
        const distinctState = dataSource.rows?.distinctValues?.get(columnPath);

        // Build filter submenu - just add dividers freely, CSS will clean them up
        const filterSubmenuItems: m.Children = [
          // Null filters
          m(MenuItem, {
            label: 'Filter out nulls',
            onclick: () => {
              onFilterAdd({column: columnPath, op: 'is not null'});
            },
          }),
          m(MenuItem, {
            label: 'Only show nulls',
            onclick: () => {
              onFilterAdd({column: columnPath, op: 'is null'});
            },
          }),
          m(MenuDivider),
          // Value-based filters for columns with distinct values enabled
          (columnDistinctValues ?? true) &&
            m(
              MenuItem,
              {
                label: 'Equals to...',
                onChange: (isOpen) => {
                  if (isOpen === true) {
                    this.distinctValuesColumns.add(columnPath);
                  } else {
                    this.distinctValuesColumns.delete(columnPath);
                  }
                },
              },
              m(DistinctValuesSubmenu, {
                columnName: columnPath,
                distinctState,
                formatValue: this.formatDistinctValue.bind(this),
                onApply: (selectedValues) => {
                  onFilterAdd({
                    column: columnPath,
                    op: 'in',
                    value: Array.from(selectedValues),
                  });
                },
              }),
            ),
          (columnDistinctValues ?? true) &&
            m(
              MenuItem,
              {
                label: 'Not equals to...',
                onChange: (isOpen) => {
                  if (isOpen === true) {
                    this.distinctValuesColumns.add(columnPath);
                  } else {
                    this.distinctValuesColumns.delete(columnPath);
                  }
                },
              },
              m(DistinctValuesSubmenu, {
                columnName: columnPath,
                distinctState,
                formatValue: this.formatDistinctValue.bind(this),
                onApply: (selectedValues) => {
                  onFilterAdd({
                    column: columnPath,
                    op: 'not in',
                    value: Array.from(selectedValues),
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Free-text equals/not equals filters for columns without distinct values
          !(columnDistinctValues ?? true) &&
            m(
              MenuItem,
              {
                label: 'Equals to...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '=',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '=',
                    value,
                  });
                },
              }),
            ),
          !(columnDistinctValues ?? true) &&
            m(
              MenuItem,
              {
                label: 'Not equals to...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '!=',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '!=',
                    value,
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Numeric comparison filters (only for numeric columns)
          columnFilterType === 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Greater than...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '>',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '>',
                    value,
                  });
                },
              }),
            ),
          columnFilterType === 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Greater than or equal...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '>=',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '>=',
                    value,
                  });
                },
              }),
            ),
          columnFilterType === 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Less than...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '<',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '<',
                    value,
                  });
                },
              }),
            ),
          columnFilterType === 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Less than or equal...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: '<=',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: '<=',
                    value,
                  });
                },
              }),
            ),
          m(MenuDivider),
          // Text-based filters (only if filterType is not 'numeric')
          columnFilterType !== 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Contains...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: 'contains',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: 'glob',
                    value: toCaseInsensitiveGlob(String(value)),
                  });
                },
              }),
            ),
          // Not contains - hidden in structuredQueryCompatMode
          columnFilterType !== 'numeric' &&
            !structuredQueryCompatMode &&
            m(
              MenuItem,
              {
                label: 'Not contains...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: 'not contains',
                onApply: (value) => {
                  onFilterAdd({
                    column: columnPath,
                    op: 'not glob',
                    value: toCaseInsensitiveGlob(String(value)),
                  });
                },
              }),
            ),
          columnFilterType !== 'numeric' &&
            m(
              MenuItem,
              {
                label: 'Glob...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: 'glob',
                onApply: (value) => {
                  onFilterAdd({column: columnPath, op: 'glob', value});
                },
              }),
            ),
          // Not glob - hidden in structuredQueryCompatMode
          columnFilterType !== 'numeric' &&
            !structuredQueryCompatMode &&
            m(
              MenuItem,
              {
                label: 'Not glob...',
              },
              m(TextFilterSubmenu, {
                columnName: columnPath,
                operator: 'not glob',
                onApply: (value) => {
                  onFilterAdd({column: columnPath, op: 'not glob', value});
                },
              }),
            ),
        ];

        // Only set filters group if there are any filter options
        // (filterSubmenuItems will be empty array if all conditions are false)
        if (filterSubmenuItems.some((item) => item !== false)) {
          defaultGroups.filters = [
            m(
              MenuItem,
              {label: 'Add filter...', icon: Icons.Filter},
              filterSubmenuItems,
            ),
          ];
        }
      }

      // Fit to content button (separate from column management)
      if (this.gridApi) {
        const gridApi = this.gridApi;
        defaultGroups.fitToContent = m(MenuItem, {
          label: 'Fit to content',
          icon: 'fit_width',
          onclick: () => gridApi.autoFitColumn(columnPath),
        });
      }

      // Column management options
      const columnManagementItems: m.Children[] = [];

      // Check if this column is a parameterized column (e.g., skills.typescript)
      const resolvedColumn = resolveColumnPath(schema, rootSchema, columnPath);
      const isParameterized =
        resolvedColumn && resolvedColumn.paramKey !== undefined;

      // For parameterized columns, add "Change parameter..." option
      if (isParameterized && resolvedColumn.paramKey) {
        // Extract the base path (e.g., "skills" from "skills.typescript")
        const basePath = columnPath.slice(
          0,
          columnPath.length - resolvedColumn.paramKey.length - 1,
        );

        // Get available keys from the datasource
        const availableKeys = dataSource.rows?.parameterKeys?.get(basePath);

        columnManagementItems.push(
          m(
            MenuItem,
            {
              label: 'Change parameter...',
              icon: 'edit',
              onChange: (isOpen) => {
                if (isOpen === true) {
                  this.parameterKeyColumns.add(basePath);
                } else {
                  this.parameterKeyColumns.delete(basePath);
                }
              },
            },
            m(ParameterizedColumnSubmenu, {
              pathPrefix: basePath,
              visibleColumns,
              availableKeys,
              onSelect: (newColumnPath) => {
                // Replace the current column with the new one
                const newColumns = visibleColumns.map((col) =>
                  col === columnPath ? newColumnPath : col,
                );
                onColumnsChanged(newColumns);
              },
            }),
          ),
        );
      }

      // Remove current column (only if more than 1 visible)
      if (visibleColumns.length > 1) {
        columnManagementItems.push(
          m(MenuItem, {
            label: 'Remove column',
            icon: Icons.Remove,
            onclick: () => {
              const newColumns = visibleColumns.filter(
                (name) => name !== columnPath,
              );
              onColumnsChanged(newColumns);
            },
          }),
        );
      }

      // Build "Add column" menu from schema
      const currentColumnIndex = visibleColumns.indexOf(columnPath);
      const addColumnMenuItems = buildAddColumnMenuFromSchema(
        schema,
        rootSchema,
        '',
        0,
        visibleColumns,
        (columnName) => {
          // Don't add if column already exists
          if (visibleColumns.includes(columnName)) return;
          // Add the new column after the current one
          const newColumns = [...visibleColumns];
          newColumns.splice(currentColumnIndex + 1, 0, columnName);
          onColumnsChanged(newColumns);
        },
        {
          dataSource,
          parameterKeyColumns: this.parameterKeyColumns,
        },
      );

      if (addColumnMenuItems.length > 0) {
        columnManagementItems.push(
          m(
            MenuItem,
            {
              label: 'Add column...',
              icon: 'add_column_right',
            },
            addColumnMenuItems,
          ),
        );
      }

      if (columnManagementItems.length > 0) {
        defaultGroups.columnManagement = columnManagementItems;
      }

      // Build final menu items using contextMenuRenderer if provided
      const menuItems: m.Children = columnContextMenuRenderer
        ? columnContextMenuRenderer(defaultGroups)
        : [
            defaultGroups.sorting,
            m(MenuDivider),
            defaultGroups.filters,
            m(MenuDivider),
            defaultGroups.fitToContent,
            m(MenuDivider),
            defaultGroups.columnManagement,
          ];

      // Build aggregation sub-content if needed
      const subContent =
        columnAggregation && dataSource.rows?.aggregates
          ? m(
              AggregationCell,
              {
                symbol: aggregationFunIcon(columnAggregation),
              },
              columnCellRenderer
                ? columnCellRenderer(
                    dataSource.rows.aggregates[columnPath],
                    dataSource.rows.aggregates,
                  )
                : renderCell(
                    dataSource.rows.aggregates[columnPath],
                    columnPath,
                  ),
            )
          : undefined;

      // Render column title with chevron icons between parts
      const columnTitleContent: m.Children = columnTitleParts.flatMap(
        (part, i) => {
          if (i === 0) return part;
          return [
            m(Icon, {
              icon: 'chevron_right',
              className: 'pf-data-grid__title-separator',
            }),
            part,
          ];
        },
      );

      const gridColumn: GridColumn = {
        key: columnPath,
        header: m(
          GridHeaderCell,
          {
            sort,
            hintSortDirection:
              sorting.direction === 'UNSORTED' ? undefined : sorting.direction,
            onSort: sortControls
              ? (direction) => {
                  onSort({
                    column: columnPath,
                    direction,
                  });
                }
              : undefined,
            menuItems:
              menuItems !== undefined &&
              Array.isArray(menuItems) &&
              menuItems.length > 0
                ? menuItems
                : menuItems !== undefined
                  ? menuItems
                  : undefined,
            subContent,
          },
          columnTitleContent,
        ),
        reorderable: columnReordering
          ? {handle: 'datagrid-columns'}
          : undefined,
      };

      return gridColumn;
    });

    const rows = dataSource.rows;
    const virtualGridRows = (() => {
      if (!rows) return [];

      // Find the intersection of rows between what we have and what is required
      // and only render those.

      const start = Math.max(rows.rowOffset, this.paginationOffset);

      const rowIndices = Array.from(
        {length: this.paginationLimit},
        (_, i) => i + start,
      );

      // Convert RowDef data to vnode rows for VirtualGrid
      return rowIndices
        .map((index) => {
          const row = rows.rows[index - rows.rowOffset];
          if (row === undefined) return undefined;
          const cellRow: m.Children[] = [];

          visibleColumns.forEach((colPath) => {
            const value = row[colPath];
            const colFilterType = getColumnFilterType(
              schema,
              rootSchema,
              colPath,
            );
            const colCellRenderer = getColumnCellRenderer(
              schema,
              rootSchema,
              colPath,
            );
            const colCellContextMenuRenderer = getColumnCellContextMenuRenderer(
              schema,
              rootSchema,
              colPath,
            );
            const menuItems: m.Children = [];

            // Build filter menu items if filtering is enabled
            if (filterControls) {
              const cellFilterItems: m.Children[] = [];

              if (value !== null) {
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '=',
                        value: value,
                      });
                    },
                  }),
                );
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Not equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '!=',
                        value: value,
                      });
                    },
                  }),
                );
              }

              // Add glob filter option for string columns with text selection
              // Only show if filterType is not 'numeric'
              if (typeof value === 'string' && colFilterType !== 'numeric') {
                const selectedText = window.getSelection()?.toString().trim();
                if (selectedText && selectedText.length > 0) {
                  cellFilterItems.push(
                    m(
                      MenuItem,
                      {
                        label: 'Filter glob',
                      },
                      m(MenuItem, {
                        label: `"${selectedText}*"`,
                        onclick: () => {
                          onFilterAdd({
                            column: colPath,
                            op: 'glob',
                            value: `${selectedText}*`,
                          });
                        },
                      }),
                      m(MenuItem, {
                        label: `"*${selectedText}"`,
                        onclick: () => {
                          onFilterAdd({
                            column: colPath,
                            op: 'glob',
                            value: `*${selectedText}`,
                          });
                        },
                      }),
                      m(MenuItem, {
                        label: `"*${selectedText}*"`,
                        onclick: () => {
                          onFilterAdd({
                            column: colPath,
                            op: 'glob',
                            value: `*${selectedText}*`,
                          });
                        },
                      }),
                    ),
                  );
                }
              }

              // Numeric comparison filters - only show if filterType is not 'string'
              if (isNumeric(value) && colFilterType !== 'string') {
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Greater than this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '>',
                        value: value,
                      });
                    },
                  }),
                );
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Greater than or equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '>=',
                        value: value,
                      });
                    },
                  }),
                );
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Less than this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '<',
                        value: value,
                      });
                    },
                  }),
                );
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Less than or equal to this',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: '<=',
                        value: value,
                      });
                    },
                  }),
                );
              }

              if (value === null) {
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Filter out nulls',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: 'is not null',
                      });
                    },
                  }),
                );
                cellFilterItems.push(
                  m(MenuItem, {
                    label: 'Only show nulls',
                    onclick: () => {
                      onFilterAdd({
                        column: colPath,
                        op: 'is null',
                      });
                    },
                  }),
                );
              }

              // Build "Add filter..." menu item to pass to renderer
              const addFilterItem =
                cellFilterItems.length > 0
                  ? m(
                      MenuItem,
                      {label: 'Add filter...', icon: Icons.Filter},
                      cellFilterItems,
                    )
                  : undefined;

              // Use custom cell context menu renderer if provided
              if (colCellContextMenuRenderer) {
                const customMenuItems = colCellContextMenuRenderer(value, row, {
                  addFilter: addFilterItem,
                });
                if (customMenuItems !== undefined && customMenuItems !== null) {
                  menuItems.push(customMenuItems);
                }
              } else if (addFilterItem !== undefined) {
                // Use default: just add the filter menu
                menuItems.push(addFilterItem);
              }
            }

            // Build cell - use GridDataCell when we have menus or special rendering
            cellRow.push(
              m(
                GridCell,
                {
                  align: isNumeric(value)
                    ? 'right'
                    : value === null
                      ? 'center'
                      : 'left',
                  nullish: value === null,
                  menuItems: menuItems.length > 0 ? menuItems : undefined,
                },
                colCellRenderer
                  ? colCellRenderer(value, row)
                  : renderCell(value, colPath),
              ),
            );
          });

          return cellRow;
        })
        .filter(exists);
    })();

    return m(
      '.pf-data-grid',
      {
        className: classNames(
          fillHeight && 'pf-data-grid--fill-height',
          className,
        ),
      },
      m(DataGridToolbar, {
        filters,
        schema,
        rootSchema,
        totalRows: rows?.totalRows ?? 0,
        showFilters: showFiltersInToolbar,
        showRowCount,
        showExportButton,
        toolbarItemsLeft,
        toolbarItemsRight,
        dataGridApi: this.dataGridApi,
        onFilterRemove,
        formatFilter: (filter) => this.formatFilter(filter, schema, rootSchema),
      }),
      m(LinearProgress, {
        className: 'pf-data-grid__loading',
        state: dataSource.isLoading ? 'indeterminate' : 'none',
      }),
      m(Grid, {
        className: 'pf-data-grid__table',
        columns: virtualGridColumns,
        rowData: {
          data: virtualGridRows,
          total: rows?.totalRows ?? 0,
          offset: Math.max(rows?.rowOffset ?? 0, this.paginationOffset),
          onLoadData: (offset, limit) => {
            // Store pagination state and trigger redraw
            this.paginationOffset = offset;
            this.paginationLimit = limit;
            m.redraw();
          },
        },
        virtualization: {
          rowHeightPx: 25,
        },
        fillHeight: true,
        onColumnReorder: columnReordering
          ? (from, to, position) => {
              const newOrder = this.reorderColumns(
                visibleColumns,
                from,
                to,
                position,
              );
              onColumnsChanged(newOrder);
            }
          : undefined,
        onReady: (api) => {
          this.gridApi = api;
        },
        emptyState:
          rows?.totalRows === 0 && !dataSource.isLoading
            ? m(
                EmptyState,
                {
                  title:
                    filters.length > 0
                      ? 'No results match your filters'
                      : 'No data available',
                  fillHeight: true,
                },
                filters.length > 0 &&
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    icon: Icons.FilterOff,
                    label: 'Clear filters',
                    onclick: clearFilters,
                  }),
              )
            : undefined,
      }),
    );
  }

  private formatDistinctValue(value: SqlValue): string {
    if (value === null) {
      return 'NULL';
    }
    if (value instanceof Uint8Array) {
      return `Blob (${value.length} bytes)`;
    }
    return String(value);
  }

  private async formatData(
    dataSource: DataGridDataSource,
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData();

    // Format the data based on the requested format
    switch (format) {
      case 'tsv':
        return this.formatAsTSV(rows, schema, rootSchema, visibleColumns);
      case 'json':
        return this.formatAsJSON(rows, schema, rootSchema, visibleColumns);
      case 'markdown':
        return this.formatAsMarkdown(rows, schema, rootSchema, visibleColumns);
    }
  }

  private formatAsTSV(
    rows: readonly RowDef[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(
      rows,
      schema,
      rootSchema,
      visibleColumns,
    );
    const columnNames = this.buildColumnNames(
      schema,
      rootSchema,
      visibleColumns,
    );
    return formatAsTSV([...visibleColumns], columnNames, formattedRows);
  }

  private formatAsJSON(
    rows: readonly RowDef[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(
      rows,
      schema,
      rootSchema,
      visibleColumns,
    );
    return formatAsJSON(formattedRows);
  }

  private formatAsMarkdown(
    rows: readonly RowDef[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(
      rows,
      schema,
      rootSchema,
      visibleColumns,
    );
    const columnNames = this.buildColumnNames(
      schema,
      rootSchema,
      visibleColumns,
    );
    return formatAsMarkdown([...visibleColumns], columnNames, formattedRows);
  }

  private formatRows(
    rows: readonly RowDef[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
  ): Array<Record<string, string>> {
    return rows.map((row) => {
      const formattedRow: Record<string, string> = {};
      for (const colPath of visibleColumns) {
        const value = row[colPath];
        const formatter =
          schema && rootSchema
            ? getColumnCellFormatter(schema, rootSchema, colPath) ??
              defaultValueFormatter
            : defaultValueFormatter;
        formattedRow[colPath] = formatter(value, row);
      }
      return formattedRow;
    });
  }

  private buildColumnNames(
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    visibleColumns: ReadonlyArray<string>,
  ): Record<string, string> {
    // Use titleString for exports, falling back to column path
    const columnNames: Record<string, string> = {};
    for (const colPath of visibleColumns) {
      columnNames[colPath] =
        schema && rootSchema
          ? getColumnTitleString(schema, rootSchema, colPath)
          : colPath;
    }
    return columnNames;
  }

  private formatFilter(
    filter: DataGridFilter,
    schema: SchemaRegistry,
    rootSchema: string,
  ) {
    // Use titleString for filter display, falling back to column path
    const columnDisplay = getColumnTitleString(
      schema,
      rootSchema,
      filter.column,
    );

    if ('value' in filter) {
      // Handle array values
      if (Array.isArray(filter.value)) {
        if (filter.value.length > 3) {
          return `${columnDisplay} ${filter.op} (${filter.value.length} values)`;
        } else {
          return `${columnDisplay} ${filter.op} (${filter.value.join(', ')})`;
        }
      }
      return `${columnDisplay} ${filter.op} ${filter.value}`;
    } else {
      return `${columnDisplay} ${filter.op}`;
    }
  }

  private reorderColumns(
    currentOrder: ColumnOrder,
    fromKey: string | number | undefined,
    toKey: string | number | undefined,
    position: 'before' | 'after',
  ): ColumnOrder {
    if (typeof fromKey !== 'string' || typeof toKey !== 'string') {
      return currentOrder;
    }

    const newOrder = [...currentOrder];
    const fromIndex = newOrder.indexOf(fromKey);
    const toIndex = newOrder.indexOf(toKey);

    if (fromIndex === -1 || toIndex === -1) return currentOrder;

    // Can't drag a column relative to itself
    if (fromKey === toKey) return currentOrder;

    // Remove from old position
    newOrder.splice(fromIndex, 1);

    // Calculate new position
    let insertIndex = toIndex;
    if (fromIndex < toIndex) insertIndex--;
    if (position === 'after') insertIndex++;

    // Insert at new position
    newOrder.splice(insertIndex, 0, fromKey);

    return newOrder;
  }
}

/**
 * Converts a string to a case-insensitive glob pattern.
 * For example: "abc" becomes "*[aA][bB][cC]*"
 */
function toCaseInsensitiveGlob(text: string): string {
  const pattern = text
    .split('')
    .map((char) => {
      const lower = char.toLowerCase();
      const upper = char.toUpperCase();
      // Only create character class for letters
      if (lower !== upper) {
        return `[${lower}${upper}]`;
      }
      // Non-letters remain as-is
      return char;
    })
    .join('');
  return `*${pattern}*`;
}

export function renderCell(value: SqlValue, columnName: string) {
  if (value === undefined) {
    return '';
  } else if (value instanceof Uint8Array) {
    return m(
      Anchor,
      {
        icon: Icons.Download,
        onclick: () =>
          download({
            fileName: `${columnName}.blob`,
            content: value,
          }),
      },
      `Blob (${value.length} bytes)`,
    );
  } else {
    return String(value);
  }
}

// Check if the value is numeric (number or bigint)
export function isNumeric(value: SqlValue): value is number | bigint {
  return typeof value === 'number' || typeof value === 'bigint';
}

// Creates a unicode icon for the aggregation function.
function aggregationFunIcon(func: AggregationFunction): string {
  switch (func) {
    case 'SUM':
      return 'Σ';
    case 'COUNT':
      return '#';
    case 'AVG':
      return '⌀';
    case 'MIN':
      return '↓';
    case 'MAX':
      return '↑';
    default:
      throw new Error(`Unknown aggregation function: ${func}`);
  }
}

// Helper component to manage distinct values selection
interface DistinctValuesSubmenuAttrs {
  readonly columnName: string;
  readonly distinctState: ReadonlyArray<SqlValue> | undefined;
  readonly formatValue: (value: SqlValue) => string;
  readonly onApply: (selectedValues: Set<SqlValue>) => void;
}

class DistinctValuesSubmenu
  implements m.ClassComponent<DistinctValuesSubmenuAttrs>
{
  private selectedValues = new Set<SqlValue>();
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<DistinctValuesSubmenuAttrs>) {
    const {distinctState, formatValue, onApply} = attrs;

    if (distinctState === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all values without highlighting
        return distinctState.map((value) => ({
          value,
          segments: [{matching: false, value: formatValue(value)}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(distinctState, (v) => formatValue(v));
        return finder.find(this.searchQuery).map((result) => ({
          value: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(
      0,
      DistinctValuesSubmenu.MAX_VISIBLE_ITEMS,
    );
    const remainingCount =
      fuzzyResults.length - DistinctValuesSubmenu.MAX_VISIBLE_ITEMS;

    return m('.pf-distinct-values-menu', [
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (e: MouseEvent) => {
            // Prevent menu from closing when clicking search box
            e.stopPropagation();
          },
        },
        m(TextInput, {
          placeholder: 'Search...',
          value: this.searchQuery,
          oninput: (e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (this.searchQuery !== '' && e.key === 'Escape') {
              this.searchQuery = '';
              e.stopPropagation(); // Prevent menu from closing
            }
          },
        }),
      ),
      m(
        '.pf-distinct-values-menu__list',
        fuzzyResults.length > 0
          ? [
              visibleResults.map((result) => {
                const isSelected = this.selectedValues.has(result.value);
                // Render highlighted label
                const labelContent = result.segments.map((segment) => {
                  if (segment.matching) {
                    return m('strong.pf-fuzzy-match', segment.value);
                  } else {
                    return segment.value;
                  }
                });

                // Render custom menu item with highlighted content
                return m(
                  'button.pf-menu-item',
                  {
                    onclick: () => {
                      if (isSelected) {
                        this.selectedValues.delete(result.value);
                      } else {
                        this.selectedValues.add(result.value);
                      }
                    },
                  },
                  m(Icon, {
                    className: 'pf-menu-item__left-icon',
                    icon: isSelected ? Icons.Checkbox : Icons.BlankCheckbox,
                  }),
                  m('.pf-menu-item__label', labelContent),
                );
              }),
              remainingCount > 0 &&
                m(MenuItem, {
                  label: `...and ${remainingCount} more`,
                  disabled: true,
                }),
            ]
          : m(EmptyState, {
              title: 'No matches',
            }),
      ),
      m('.pf-distinct-values-menu__footer', [
        m(MenuItem, {
          label: 'Apply',
          icon: 'check',
          disabled: this.selectedValues.size === 0,
          onclick: () => {
            if (this.selectedValues.size > 0) {
              onApply(this.selectedValues);
              this.selectedValues.clear();
              this.searchQuery = '';
            }
          },
        }),
        m(MenuItem, {
          label: 'Clear selection',
          icon: 'close',
          disabled: this.selectedValues.size === 0,
          closePopupOnClick: false,
          onclick: () => {
            this.selectedValues.clear();
            m.redraw();
          },
        }),
      ]),
    ]);
  }
}

// Helper component for text-based filter input
interface TextFilterSubmenuAttrs {
  readonly columnName: string;
  readonly operator:
    | 'glob'
    | 'not glob'
    | 'contains'
    | 'not contains'
    | '='
    | '!='
    | '>'
    | '>='
    | '<'
    | '<=';
  readonly onApply: (value: string | number) => void;
}

class TextFilterSubmenu implements m.ClassComponent<TextFilterSubmenuAttrs> {
  private inputValue = '';

  view({attrs}: m.Vnode<TextFilterSubmenuAttrs>) {
    const {operator, onApply} = attrs;

    const placeholder = (() => {
      switch (operator) {
        case 'glob':
          return 'Enter glob pattern (e.g., *text*)...';
        case 'not glob':
          return 'Enter glob pattern to exclude...';
        case 'contains':
          return 'Enter text to include...';
        case 'not contains':
          return 'Enter text to exclude...';
        case '=':
          return 'Enter value to match...';
        case '!=':
          return 'Enter value to exclude...';
        case '>':
          return 'Enter number...';
        case '>=':
          return 'Enter number...';
        case '<':
          return 'Enter number...';
        case '<=':
          return 'Enter number...';
      }
    })();

    // Check if this is a numeric comparison operator
    const isNumericOperator = ['>', '>=', '<', '<='].includes(operator);

    const applyFilter = () => {
      if (this.inputValue.trim().length > 0) {
        let value: string | number = this.inputValue.trim();

        // For numeric operators, try to parse as number
        if (isNumericOperator) {
          const numValue = Number(value);
          if (!isNaN(numValue)) {
            value = numValue;
          }
        }

        onApply(value);
        this.inputValue = '';
      }
    };

    return m(
      Form,
      {
        className: 'pf-data-grid__text-filter-form',
        submitLabel: 'Add Filter',
        submitIcon: 'check',
        onSubmit: (e: Event) => {
          e.preventDefault();
          applyFilter();
        },
        validation: () => this.inputValue.trim().length > 0,
      },
      m(TextInput, {
        placeholder,
        value: this.inputValue,
        autofocus: true,
        oninput: (e: InputEvent) => {
          this.inputValue = (e.target as HTMLInputElement).value;
        },
      }),
    );
  }
}

interface AddColumnMenuContext {
  readonly dataSource: DataGridDataSource;
  readonly parameterKeyColumns: Set<string>;
}

/**
 * Builds menu items for adding columns from a schema.
 * Recursively builds submenus for schema references.
 *
 * @param registry The schema registry
 * @param schemaName The name of the current schema to build from
 * @param pathPrefix The current path prefix (e.g., 'parent' or 'thread.process')
 * @param depth Current recursion depth (to prevent infinite menus)
 * @param visibleColumns Currently visible columns (to disable duplicates)
 * @param onSelect Callback when a column is selected
 * @param context Context containing dataSource and parameterKeyColumns for key discovery
 * @param maxDepth Maximum recursion depth (default 5)
 */
function buildAddColumnMenuFromSchema(
  registry: SchemaRegistry,
  schemaName: string,
  pathPrefix: string,
  depth: number,
  visibleColumns: ReadonlyArray<string>,
  onSelect: (columnPath: string) => void,
  context: AddColumnMenuContext,
  maxDepth: number = 5,
): m.Children[] {
  const schema = maybeUndefined(registry[schemaName]);
  if (!schema) return [];

  // Stop if we've gone too deep (prevents infinite menus for self-referential schemas)
  if (depth > maxDepth) {
    return [m(MenuItem, {label: '(max depth reached)', disabled: true})];
  }

  const menuItems: m.Children[] = [];

  for (const [columnName, entry] of Object.entries(schema)) {
    const fullPath = pathPrefix ? `${pathPrefix}.${columnName}` : columnName;

    if (isColumnDef(entry)) {
      // Leaf column - clicking adds it (disabled if already visible)
      const title = entry.title ?? columnName;
      const isAlreadyVisible = visibleColumns.includes(fullPath);
      menuItems.push(
        m(MenuItem, {
          label: title,
          disabled: isAlreadyVisible,
          onclick: () => onSelect(fullPath),
        }),
      );
    } else if (isSchemaRef(entry)) {
      // Reference to another schema - create a submenu
      const refTitle = entry.title ?? columnName;
      const childMenuItems = buildAddColumnMenuFromSchema(
        registry,
        entry.ref,
        fullPath,
        depth + 1,
        visibleColumns,
        onSelect,
        context,
        maxDepth,
      );

      if (childMenuItems.length > 0) {
        menuItems.push(m(MenuItem, {label: refTitle}, childMenuItems));
      }
    } else if (isParameterizedColumnDef(entry)) {
      // Parameterized column - show available keys from datasource
      const title = typeof entry.title === 'string' ? entry.title : columnName;
      const availableKeys =
        context.dataSource.rows?.parameterKeys?.get(fullPath);
      menuItems.push(
        m(
          MenuItem,
          {
            label: `${title}...`,
            onChange: (isOpen) => {
              if (isOpen === true) {
                context.parameterKeyColumns.add(fullPath);
              } else {
                context.parameterKeyColumns.delete(fullPath);
              }
            },
          },
          m(ParameterizedColumnSubmenu, {
            pathPrefix: fullPath,
            visibleColumns,
            availableKeys,
            onSelect,
          }),
        ),
      );
    }
  }

  return menuItems;
}

// Helper component for parameterized column input
interface ParameterizedColumnSubmenuAttrs {
  readonly pathPrefix: string;
  readonly visibleColumns: ReadonlyArray<string>;
  readonly availableKeys: ReadonlyArray<string> | undefined;
  readonly onSelect: (columnPath: string) => void;
}

class ParameterizedColumnSubmenu
  implements m.ClassComponent<ParameterizedColumnSubmenuAttrs>
{
  private searchQuery = '';
  private static readonly MAX_VISIBLE_ITEMS = 100;

  view({attrs}: m.Vnode<ParameterizedColumnSubmenuAttrs>) {
    const {pathPrefix, visibleColumns, availableKeys, onSelect} = attrs;

    // Show loading state if availableKeys is undefined
    if (availableKeys === undefined) {
      return m('.pf-distinct-values-menu', [
        m(MenuItem, {label: 'Loading...', disabled: true}),
      ]);
    }

    // Use fuzzy search to filter and get highlighted segments
    const fuzzyResults = (() => {
      if (this.searchQuery === '') {
        // No search - show all keys without highlighting
        return availableKeys.map((key) => ({
          key,
          segments: [{matching: false, value: key}],
        }));
      } else {
        // Fuzzy search with highlighting
        const finder = new FuzzyFinder(availableKeys, (k) => k);
        return finder.find(this.searchQuery).map((result) => ({
          key: result.item,
          segments: result.segments,
        }));
      }
    })();

    // Limit the number of items rendered
    const visibleResults = fuzzyResults.slice(
      0,
      ParameterizedColumnSubmenu.MAX_VISIBLE_ITEMS,
    );
    const remainingCount =
      fuzzyResults.length - ParameterizedColumnSubmenu.MAX_VISIBLE_ITEMS;

    // Check if search query could be used as a custom key
    const customKeyPath =
      this.searchQuery.trim().length > 0
        ? `${pathPrefix}.${this.searchQuery.trim()}`
        : '';
    const isCustomKeyAlreadyVisible =
      customKeyPath !== '' && visibleColumns.includes(customKeyPath);
    const isCustomKeyInResults =
      this.searchQuery.trim().length > 0 &&
      availableKeys.includes(this.searchQuery.trim());

    return m('.pf-distinct-values-menu', [
      // Search input
      m(
        '.pf-distinct-values-menu__search',
        {
          onclick: (e: MouseEvent) => {
            // Prevent menu from closing when clicking search box
            e.stopPropagation();
          },
        },
        m(TextInput, {
          placeholder: 'Search or enter key name...',
          value: this.searchQuery,
          oninput: (e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (this.searchQuery !== '' && e.key === 'Escape') {
              this.searchQuery = '';
              e.stopPropagation(); // Prevent menu from closing
            }
          },
        }),
      ),
      // List of available keys
      m(
        '.pf-distinct-values-menu__list',
        fuzzyResults.length > 0
          ? [
              visibleResults.map((result) => {
                const keyPath = `${pathPrefix}.${result.key}`;
                const isKeyAlreadyVisible = visibleColumns.includes(keyPath);

                // Render highlighted label
                const labelContent = result.segments.map((segment) => {
                  if (segment.matching) {
                    return m('strong.pf-fuzzy-match', segment.value);
                  } else {
                    return segment.value;
                  }
                });

                return m(
                  'button.pf-menu-item' +
                    (isKeyAlreadyVisible ? '[disabled]' : ''),
                  {
                    onclick: () => {
                      if (!isKeyAlreadyVisible) {
                        onSelect(keyPath);
                        this.searchQuery = '';
                      }
                    },
                  },
                  m('.pf-menu-item__label', labelContent),
                  isKeyAlreadyVisible &&
                    m(Icon, {
                      className: 'pf-menu-item__right-icon',
                      icon: 'check',
                    }),
                );
              }),
              remainingCount > 0 &&
                m(MenuItem, {
                  label: `...and ${remainingCount} more`,
                  disabled: true,
                }),
            ]
          : m(EmptyState, {
              title: 'No matches',
            }),
      ),
      // Footer with "Add custom" option when search query doesn't match existing keys
      this.searchQuery.trim().length > 0 &&
        !isCustomKeyInResults &&
        m('.pf-distinct-values-menu__footer', [
          m(MenuItem, {
            label: `Add "${this.searchQuery.trim()}"`,
            icon: 'add',
            disabled: isCustomKeyAlreadyVisible,
            onclick: () => {
              if (!isCustomKeyAlreadyVisible) {
                onSelect(customKeyPath);
                this.searchQuery = '';
              }
            },
          }),
        ]),
    ]);
  }
}
