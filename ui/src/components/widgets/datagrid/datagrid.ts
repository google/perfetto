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
import {Icons} from '../../../base/semantic_icons';
import {exists, isNumeric} from '../../../base/utils';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
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
import {renderCellFilterMenuItem} from './cell_filter_menu';
import {renderFilterMenuItems} from './column_filter_menu';
import {
  SchemaRegistry,
  getColumnCellContextMenuRenderer,
  getColumnCellFormatter,
  getColumnCellRenderer,
  getColumnContextMenuRenderer,
  getColumnDisplayTitleString,
  getColumnDistinctValues,
  getColumnFilterType,
  getColumnTitle,
  getColumnTitleParts,
  getColumnTitleString,
  getDefaultVisibleColumns,
  resolveColumnPath,
} from './column_schema';
import {
  AggregationFunction,
  DataGridColumn,
  Filter,
  PivotModel,
  PivotValue,
  SortBy,
} from './model';
import {DataGridToolbar} from './datagrid_toolbar';
import {
  defaultValueFormatter,
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
} from './export_utils';
import {InMemoryDataSource} from './in_memory_data_source';
import {
  OnPivotChanged,
  renderPivotMenuForNormalColumn,
  renderPivotMenuForGroupByColumn,
  renderPivotMenuForAggregateColumn,
} from './pivot_menu';
import {
  buildAddColumnMenuFromSchema,
  ParameterizedColumnSubmenu,
} from './add_column_menu';
import {DataSource} from './data_source';

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

type OnFilterAdd = (filter: Filter) => void;
export type OnFilterRemove = (index: number) => void;
type OnSortingChanged = (sorting: SortBy) => void;

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
   * In controlled mode: Provide this prop along with onColumnsChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage columns internally,
   * defaulting to all leaf columns in the root schema.
   *
   * When pivot mode is active (pivot prop is set without drillDown), the pivot
   * groupBy and aggregate columns take precedence for display. However, columns
   * is still used when drilling down into pivot groups.
   *
   * Each column can be either a string (column path) or a DataGridColumn object
   * with an optional aggregation function.
   */
  readonly columns?: ReadonlyArray<string | DataGridColumn>;

  /**
   * Initial columns to show on first load.
   * This is ignored in controlled mode (i.e. when `columns` is provided).
   */
  readonly initialColumns?: ReadonlyArray<string | DataGridColumn>;

  /**
   * Callback triggered when visible columns change (add, remove, reorder).
   * Required for controlled mode - when provided with columns,
   * the parent component becomes responsible for updating the columns prop.
   */
  readonly onColumnsChanged?: (columns: ReadonlyArray<string>) => void;

  /**
   * The data source that provides rows to the grid. Responsible for fetching,
   * filtering, and sorting data based on the current state.
   *
   * The data source is responsible for applying the filters, sorting, and
   * paging and providing the rows that are displayed in the grid.
   */
  readonly data: DataSource | ReadonlyArray<Row>;

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
  readonly sorting?: SortBy;

  /**
   * Initial sorting to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `sorting` is provided).
   */
  readonly initialSorting?: SortBy;

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
  readonly filters?: ReadonlyArray<Filter>;

  /**
   * Initial filters to apply to the grid on first load.
   * This is ignored in controlled mode (i.e. when `filters` is provided).
   */
  readonly initialFilters?: ReadonlyArray<Filter>;

  /**
   * These callbacks are triggered when filters are added or removed by the
   * user. They are only called in controlled mode, e.g. only if filters is
   * provided.
   */
  readonly onFilterAdd?: OnFilterAdd;
  readonly onFilterRemove?: OnFilterRemove;
  readonly clearFilters?: () => void;

  /**
   * Defines how data should be pivoted - can operate in controlled or
   * uncontrolled mode.
   *
   * In controlled mode: Provide this prop along with onPivotChanged callback.
   * In uncontrolled mode: Omit this prop to let the grid manage pivot state
   * internally.
   *
   * Specifies groupBy columns and aggregation values for pivoting.
   * If not provided, defaults to undefined (no pivoting).
   */
  readonly pivot?: PivotModel;

  /**
   * Initial pivot configuration to apply on first load.
   * This is ignored in controlled mode (i.e. when `pivot` is provided).
   */
  readonly initialPivot?: PivotModel;

  /**
   * Callback triggered when the pivot configuration changes.
   * Allows parent components to react to pivot changes.
   * Required for controlled mode - when provided with pivot,
   * the parent component becomes responsible for updating the pivot prop.
   * @param pivot The new pivot configuration (or undefined to disable pivoting)
   */
  readonly onPivotChanged?: OnPivotChanged;

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
   * Whether to show sorting controls in column header menus.
   * Default = true.
   */
  readonly enableSortingControls?: boolean;

  /**
   * Whether to show filter controls in column header and cell menus.
   * Default = true.
   */
  readonly enableFilterControls?: boolean;

  /**
   * Whether to show pivot controls (group by, aggregate) in column menus.
   * Default = false.
   */
  readonly enablePivotControls?: boolean;
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

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  // Internal state
  private sorting: SortBy = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<Filter> = [];
  private pivot: PivotModel | undefined = undefined;
  private internalColumns: ReadonlyArray<DataGridColumn> = [];
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
      return this.currentDataSource?.result?.totalRows ?? 0;
    },
  };
  private currentDataSource?: DataSource;
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

    if (attrs.initialPivot) {
      this.pivot = attrs.initialPivot;
    }

    // Initialize columns from initial prop, columns, or default from schema
    if (attrs.initialColumns) {
      this.internalColumns = attrs.initialColumns.map(normalizeColumn);
    } else if (attrs.columns) {
      this.internalColumns = attrs.columns.map(normalizeColumn);
    } else {
      // Default to all leaf columns in the root schema
      this.internalColumns = getDefaultVisibleColumns(
        attrs.schema,
        attrs.rootSchema,
      ).map(normalizeColumn);
    }
  }

  view({attrs}: m.Vnode<DataGridAttrs>) {
    const {
      schema,
      rootSchema,
      columns: propsColumns,
      onColumnsChanged: propsOnColumnsChanged,
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
      pivot = this.pivot,
      onPivotChanged = pivot === this.pivot
        ? (x: PivotModel | undefined) => (this.pivot = x)
        : noOp,
      fillHeight = false,
      toolbarItemsLeft,
      toolbarItemsRight,
      className,
      showExportButton = false,
      showRowCount = false,
      onReady,
      structuredQueryCompatMode = false,
      enableSortingControls = true,
      enableFilterControls = true,
      enablePivotControls = false,
    } = attrs;

    // Use props columns or internal state, defaulting to schema leaf columns
    // Normalize to DataGridColumn[] for internal use
    const columns: ReadonlyArray<DataGridColumn> =
      propsColumns?.map(normalizeColumn) ??
      (this.internalColumns.length > 0
        ? this.internalColumns
        : getDefaultVisibleColumns(schema, rootSchema).map(normalizeColumn));
    // Extract just the column names for APIs that need string[]
    const columnNames: ReadonlyArray<string> = columns.map((c) => c.column);
    const onColumnsChanged =
      propsOnColumnsChanged ??
      ((cols: ReadonlyArray<string>) => {
        this.internalColumns = cols.map(normalizeColumn);
      });

    // Initialize the datasource if required
    let dataSource: DataSource;
    if (Array.isArray(data)) {
      // If raw data supplied - just create a new in memory data source every
      // render cycle.
      dataSource = new InMemoryDataSource(data);
    } else {
      dataSource = data as DataSource;
    }

    // Update datasource with current state (sorting, filtering, pagination, pivot)
    // This is called every view cycle to catch changes
    dataSource.notify({
      columns: [...columns],
      sorting,
      filters,
      pagination: {
        offset: this.paginationOffset,
        limit: this.paginationLimit,
      },
      pivot,
      distinctValuesColumns: this.distinctValuesColumns,
      parameterKeyColumns: this.parameterKeyColumns,
    });

    // Store current state for API access
    this.currentDataSource = dataSource;
    this.currentSchema = schema;
    this.currentRootSchema = rootSchema;

    // Create and expose DataGrid API if needed
    onReady?.(this.dataGridApi);

    const sortControls = enableSortingControls;
    const filterControls = enableFilterControls;
    const isDrillDown = pivot?.drillDown !== undefined;
    const showDrillDownColumn = pivot && !isDrillDown;

    // Build display columns based on mode:
    // - Pivot mode (not drill-down): iterate groupBy columns, then aggregate columns
    // - Normal mode or drill-down: iterate visible columns
    type DisplayColumn = {
      columnPath: string;
      isGroupBy: boolean;
      isAggregate: boolean;
      isLastGroupBy: boolean;
      pivotValue?: PivotValue;
      // Column-level aggregation (for non-pivot mode)
      columnAggregation?: AggregationFunction;
    };

    const displayColumns: ReadonlyArray<DisplayColumn> = (() => {
      if (pivot && !isDrillDown) {
        // Pivot mode: groupBy columns first, then aggregate columns
        const lastGroupByIndex = pivot.groupBy.length - 1;
        const groupByCols: DisplayColumn[] = pivot.groupBy.map((col, i) => ({
          columnPath: col,
          isGroupBy: true,
          isAggregate: false,
          isLastGroupBy: i === lastGroupByIndex,
        }));
        const aggregateCols: DisplayColumn[] = Object.entries(pivot.values).map(
          ([alias, value]) => ({
            columnPath: alias,
            isGroupBy: false,
            isAggregate: true,
            isLastGroupBy: false,
            pivotValue: value,
          }),
        );
        return [...groupByCols, ...aggregateCols];
      }
      // Normal mode or drill-down: just the visible columns
      return columns.map((col) => ({
        columnPath: col.column,
        isGroupBy: false,
        isAggregate: false,
        isLastGroupBy: false,
        columnAggregation: col.aggregation,
      }));
    })();

    // Store visible columns for export - use displayColumns in pivot mode
    this.currentVisibleColumns = displayColumns.map((dc) => dc.columnPath);

    // Build VirtualGrid columns with all DataGrid features
    const virtualGridColumns = displayColumns.map((displayCol) => {
      const {
        columnPath,
        isGroupBy: isGroupByColumn,
        isAggregate: isAggregateColumn,
        isLastGroupBy,
        pivotValue,
        columnAggregation: displayColAggregation,
      } = displayCol;

      // For aggregate columns, get title from the source column if available
      const sourceColumnPath =
        isAggregateColumn && pivotValue && 'col' in pivotValue
          ? pivotValue.col
          : columnPath;

      // Look up column properties from schema (use source column for aggregates)
      const columnTitleParts = getColumnTitleParts(
        schema,
        rootSchema,
        sourceColumnPath,
      );
      const columnFilterType = getColumnFilterType(
        schema,
        rootSchema,
        sourceColumnPath,
      );
      const columnDistinctValues = getColumnDistinctValues(
        schema,
        rootSchema,
        sourceColumnPath,
      );
      const columnCellRenderer = getColumnCellRenderer(
        schema,
        rootSchema,
        sourceColumnPath,
      );
      // For aggregate columns, use the pivot aggregation function for display
      // For non-pivot columns with aggregation, use the column-level aggregation
      const columnAggregation = isAggregateColumn
        ? pivotValue?.func
        : displayColAggregation;
      const columnContextMenuRenderer = getColumnContextMenuRenderer(
        schema,
        rootSchema,
        sourceColumnPath,
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
        pivot?: m.Children;
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
        const distinctState =
          dataSource.result?.distinctValues?.get(columnPath);

        const filterSubmenuItems = renderFilterMenuItems({
          columnPath,
          columnDistinctValues,
          columnFilterType,
          structuredQueryCompatMode,
          distinctState,
          formatValue: this.formatDistinctValue.bind(this),
          onFilterAdd,
          onDistinctValuesOpen: () =>
            this.distinctValuesColumns.add(columnPath),
          onDistinctValuesClose: () =>
            this.distinctValuesColumns.delete(columnPath),
        });

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

      // Pivot menu group - use appropriate function based on column type
      const columnInfo = {
        name: columnPath,
        title: isAggregateColumn
          ? pivotValue?.func === 'COUNT'
            ? 'Count'
            : getColumnTitle(schema, rootSchema, sourceColumnPath)
          : getColumnTitle(schema, rootSchema, columnPath),
        filterType: columnFilterType,
      };

      if (enablePivotControls) {
        if (pivot && isGroupByColumn) {
          // GroupBy column in pivot mode
          defaultGroups.pivot = renderPivotMenuForGroupByColumn(
            schema,
            rootSchema,
            pivot,
            columnInfo,
            onPivotChanged,
          );
        } else if (pivot && isAggregateColumn) {
          // Aggregate column in pivot mode
          defaultGroups.pivot = renderPivotMenuForAggregateColumn(
            schema,
            rootSchema,
            pivot,
            columnInfo,
            onPivotChanged,
          );
        } else {
          // Normal column (not in pivot mode) - show "Pivot on this" option
          defaultGroups.pivot = renderPivotMenuForNormalColumn(
            columnInfo,
            columnNames,
            onPivotChanged,
          );
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
      // Column management is only available when not in pivot mode
      // In pivot mode, column visibility is controlled by the pivot state
      // (use "Add pivot..." and "Add aggregate" from the pivot menu instead)
      const columnManagementItems: m.Children[] = [];

      if (!pivot) {
        // Check if this column is a parameterized column (e.g., skills.typescript)
        const resolvedColumn = resolveColumnPath(
          schema,
          rootSchema,
          columnPath,
        );
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
          const availableKeys = dataSource.result?.parameterKeys?.get(basePath);

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
                columns: columnNames,
                availableKeys,
                onSelect: (newColumnPath) => {
                  // Replace the current column with the new one
                  const newColumns = columnNames.map((col) =>
                    col === columnPath ? newColumnPath : col,
                  );
                  onColumnsChanged(newColumns);
                },
              }),
            ),
          );
        }
        // Remove current column (only if more than 1 visible)
        if (columnNames.length > 1) {
          columnManagementItems.push(
            m(MenuItem, {
              label: 'Remove column',
              icon: Icons.Remove,
              onclick: () => {
                const newColumns = columnNames.filter(
                  (name) => name !== columnPath,
                );
                onColumnsChanged(newColumns);
              },
            }),
          );
        }

        // Build "Add column" menu from schema
        const currentColumnIndex = columnNames.indexOf(columnPath);
        const addColumnMenuItems = buildAddColumnMenuFromSchema(
          schema,
          rootSchema,
          '',
          0,
          columnNames,
          (columnName) => {
            // Don't add if column already exists
            if (columnNames.includes(columnName)) return;
            // Add the new column after the current one
            const newColumns = [...columnNames];
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
            m(MenuDivider),
            defaultGroups.pivot,
          ];

      // Render column title with chevron icons between parts
      // For COUNT columns, show "Count" as the title
      const displayTitleParts =
        isAggregateColumn && pivotValue?.func === 'COUNT'
          ? ['Count']
          : columnTitleParts;
      const columnTitleContent: m.Children = displayTitleParts.flatMap(
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

      // Get the aggregate total value for this column (grand total across all pivot groups)
      const aggregateTotalValue: SqlValue =
        dataSource.result?.aggregateTotals?.get(columnPath) ?? null;

      // Build aggregation sub-content for pivot aggregate columns
      // Don't show grand total for ANY aggregation (it's just an arbitrary value)
      const subContent = isGroupByColumn
        ? undefined
        : columnAggregation
          ? m(
              AggregationCell,
              {symbol: columnAggregation},
              columnAggregation !== 'ANY'
                ? columnCellRenderer
                  ? columnCellRenderer(aggregateTotalValue, {})
                  : renderCell(aggregateTotalValue, columnPath)
                : undefined,
            )
          : undefined;

      const gridColumn: GridColumn = {
        key: columnPath,
        header: m(
          GridHeaderCell,
          {
            className: isGroupByColumn
              ? 'pf-data-grid__groupby-column'
              : undefined,
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
        thickRightBorder: isLastGroupBy && !isDrillDown,
        reorderable: (() => {
          // In pivot mode (not drill-down), use separate handles for groupBy vs aggregate columns
          if (pivot && !isDrillDown) {
            if (isGroupByColumn) {
              return {reorderGroup: 'pivot-groupby'};
            } else if (isAggregateColumn) {
              return {reorderGroup: 'pivot-aggregate'};
            }
          }
          return {reorderGroup: 'datagrid-columns'};
        })(),
      };

      return gridColumn;
    });

    // Add drill-down column when in pivot mode (not during drill-down)
    if (showDrillDownColumn) {
      virtualGridColumns.push({
        key: '__drilldown__',
        header: m(GridHeaderCell, ''),
      });
    }

    const rows = dataSource.result;
    const virtualGridRows = (() => {
      if (!rows) return [];

      // Find the intersection of rows between what we have and what is required
      // and only render those.

      const start = Math.max(rows.rowOffset, this.paginationOffset);

      const rowIndices = Array.from(
        {length: this.paginationLimit},
        (_, i) => i + start,
      );

      // Convert Row data to vnode rows for VirtualGrid
      return rowIndices
        .map((index) => {
          const row = rows.rows[index - rows.rowOffset];
          if (row === undefined) return undefined;
          const cellRow: m.Children[] = [];

          displayColumns.forEach((displayCol) => {
            const {
              columnPath: colPath,
              isAggregate: isColAggregate,
              isGroupBy: isGroupByColumn,
              pivotValue: colPivotValue,
            } = displayCol;
            const value = row[colPath];

            // For aggregate columns, use the source column for schema lookups
            const colSourcePath =
              isColAggregate && colPivotValue && 'col' in colPivotValue
                ? colPivotValue.col
                : colPath;

            const colFilterType = getColumnFilterType(
              schema,
              rootSchema,
              colSourcePath,
            );
            const colCellRenderer = getColumnCellRenderer(
              schema,
              rootSchema,
              colSourcePath,
            );
            const colCellContextMenuRenderer = getColumnCellContextMenuRenderer(
              schema,
              rootSchema,
              colSourcePath,
            );
            const menuItems: m.Children = [];

            // Build filter menu items if filtering is enabled
            if (filterControls) {
              const addFilterItem = renderCellFilterMenuItem({
                columnPath: colSourcePath,
                value,
                colFilterType,
                onFilterAdd,
              });

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
                  className: isGroupByColumn
                    ? 'pf-data-grid__groupby-column'
                    : undefined,
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

          // Add drill-down button cell when in pivot mode
          if (showDrillDownColumn) {
            // Build the drillDown values from the groupBy columns
            const drillDownValues: Row = {};
            for (const colName of pivot.groupBy) {
              drillDownValues[colName] = row[colName];
            }
            cellRow.push(
              m(Button, {
                icon: Icons.GoTo,
                title: 'Drill down into this group',
                onclick: () => {
                  onPivotChanged({
                    ...pivot,
                    drillDown: drillDownValues,
                  });
                },
              }),
            );
          }

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
        showRowCount,
        showExportButton,
        toolbarItemsLeft,
        toolbarItemsRight,
        dataGridApi: this.dataGridApi,
        onFilterRemove,
        formatFilter: (filter) => this.formatFilter(filter, schema, rootSchema),
        drillDown: pivot?.drillDown
          ? {
              onBack: () => {
                // Clear drillDown to go back to pivoted view
                onPivotChanged({
                  groupBy: pivot.groupBy,
                  values: pivot.values,
                });
              },
              groupBy: pivot.groupBy,
              values: pivot.drillDown,
              formatColumnName: (colName: string) =>
                getColumnTitleString(schema, rootSchema, colName),
            }
          : undefined,
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
        onColumnReorder: (from, to, position) => {
          if (typeof from !== 'string' || typeof to !== 'string') return;

          // Handle pivot groupBy column reordering
          if (pivot && !isDrillDown) {
            const fromIsGroupBy = pivot.groupBy.includes(from);
            const toIsGroupBy = pivot.groupBy.includes(to);
            const fromIsAggregate = from in pivot.values;
            const toIsAggregate = to in pivot.values;

            if (fromIsGroupBy && toIsGroupBy) {
              // Reorder within groupBy columns
              const newGroupBy = this.reorderColumns(
                [...pivot.groupBy],
                from,
                to,
                position,
              );
              onPivotChanged({
                ...pivot,
                groupBy: newGroupBy,
              });
              return;
            }

            if (fromIsAggregate && toIsAggregate) {
              // Reorder within aggregate columns
              const valueKeys = Object.keys(pivot.values);
              const newValueKeys = this.reorderColumns(
                valueKeys,
                from,
                to,
                position,
              );
              // Rebuild values object in new order
              const newValues: {[key: string]: PivotValue} = {};
              for (const key of newValueKeys) {
                newValues[key] = pivot.values[key];
              }
              onPivotChanged({
                ...pivot,
                values: newValues,
              });
              return;
            }
          }

          // Normal column reordering (not pivot mode)
          const newOrder = this.reorderColumns(columnNames, from, to, position);
          onColumnsChanged(newOrder);
        },
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
    dataSource: DataSource,
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    columns: ReadonlyArray<string>,
    format: 'tsv' | 'json' | 'markdown' = 'tsv',
  ): Promise<string> {
    // Get all rows from the data source
    const rows = await dataSource.exportData();

    // Format the data based on the requested format
    switch (format) {
      case 'tsv':
        return this.formatAsTSV(rows, schema, rootSchema, columns);
      case 'json':
        return this.formatAsJSON(rows, schema, rootSchema, columns);
      case 'markdown':
        return this.formatAsMarkdown(rows, schema, rootSchema, columns);
    }
  }

  private formatAsTSV(
    rows: readonly Row[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    columns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(rows, schema, rootSchema, columns);
    const columnNames = this.buildColumnNames(schema, rootSchema, columns);
    return formatAsTSV([...columns], columnNames, formattedRows);
  }

  private formatAsJSON(
    rows: readonly Row[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    columns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(rows, schema, rootSchema, columns);
    return formatAsJSON(formattedRows);
  }

  private formatAsMarkdown(
    rows: readonly Row[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    columns: ReadonlyArray<string>,
  ): string {
    const formattedRows = this.formatRows(rows, schema, rootSchema, columns);
    const columnNames = this.buildColumnNames(schema, rootSchema, columns);
    return formatAsMarkdown([...columns], columnNames, formattedRows);
  }

  private formatRows(
    rows: readonly Row[],
    schema: SchemaRegistry | undefined,
    rootSchema: string | undefined,
    columns: ReadonlyArray<string>,
  ): Array<Record<string, string>> {
    return rows.map((row) => {
      const formattedRow: Record<string, string> = {};
      for (const colPath of columns) {
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
    columns: ReadonlyArray<string>,
  ): Record<string, string> {
    // Use titleString for exports, falling back to column path
    const columnNames: Record<string, string> = {};
    for (const colPath of columns) {
      columnNames[colPath] =
        schema && rootSchema
          ? getColumnTitleString(schema, rootSchema, colPath)
          : colPath;
    }
    return columnNames;
  }

  private formatFilter(
    filter: Filter,
    schema: SchemaRegistry,
    rootSchema: string,
  ) {
    // Use the display title (e.g., "Manager > Name") for filter chips
    const columnDisplay = getColumnDisplayTitleString(
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
    currentOrder: ReadonlyArray<string>,
    fromKey: string | number | undefined,
    toKey: string | number | undefined,
    position: 'before' | 'after',
  ): ReadonlyArray<string> {
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

// Helper to normalize column input (string or DataGridColumn) to DataGridColumn
export function normalizeColumn(col: string | DataGridColumn): DataGridColumn {
  return typeof col === 'string' ? {column: col} : col;
}
