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

/**
 * MIGRATED TO DATAGRID
 *
 * This file has been migrated from SqlTable to DataGrid.
 * Key changes:
 * 1. SqlTableDefinition → SchemaRegistry + SQLDataSource
 * 2. SqlTableState → DataGrid controlled state
 * 3. Filters class → DataGridFilter array (with adapter for compatibility)
 * 4. TableColumn → Schema-based column definitions
 */

import m from 'mithril';
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {addEphemeralTab} from './add_ephemeral_tab';
import {Tab} from '../../public/tab';
import {TabOption, TabStrip} from '../../widgets/tabs';
import {Popup, PopupPosition} from '../../widgets/popup';
import {AddDebugTrackMenu} from '../tracks/add_debug_track_menu';
import {Gate} from '../../base/mithril_utils';
import {DataGrid, DataGridApi} from '../widgets/datagrid/datagrid';
import {DataGridFilter, Sorting} from '../widgets/datagrid/common';
import {SchemaRegistry} from '../widgets/datagrid/column_schema';
import {SQLDataSource} from '../widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../widgets/datagrid/sql_schema';
import {
  GridFilterChip,
  GridFilterBar,
} from '../widgets/datagrid/datagrid_toolbar';
import {uuidv4} from '../../base/uuid';
import {SqlTableDefinition} from '../widgets/sql/table/table_description';
// Keep old Filter type for backward compatibility
import {Filter} from '../widgets/sql/table/filters';

/**
 * Helper: Convert old SqlTableDefinition to new SchemaRegistry format
 */
function convertTableDefinitionToSchema(def: SqlTableDefinition): {
  schema: SchemaRegistry;
  rootSchema: string;
  sqlSchema: SQLSchemaRegistry;
} {
  const schemaName = def.name;
  const columnSchema: SchemaRegistry[string] = {};

  // Build SQL schema for datasource
  const sqlSchemaColumns: SQLSchemaRegistry[string]['columns'] = {};

  for (const col of def.columns) {
    const columnName =
      typeof col.column === 'string' ? col.column : col.column.column;

    // Basic column def for display schema
    columnSchema[columnName] = {
      title: columnName,
      // Map types if available
      filterType:
        col.type?.kind === 'timestamp' || col.type?.kind === 'duration'
          ? 'numeric'
          : undefined,
    };

    // Add to SQL schema for query building
    sqlSchemaColumns[columnName] = {};

    // TODO: Handle special column types:
    // - timestamp → custom cellRenderer
    // - duration → custom cellRenderer
    // - id/joinid → schema refs for JOIN support
    // - arg_set_id → parameterized columns
  }

  const sqlSchema: SQLSchemaRegistry = {
    [schemaName]: {
      table: def.name,
      columns: sqlSchemaColumns,
    },
  };

  return {
    schema: {[schemaName]: columnSchema},
    rootSchema: schemaName,
    sqlSchema,
  };
}

/**
 * Convert old Filter format to DataGridFilter format
 */
function convertFilter(oldFilter: Filter): DataGridFilter | undefined {
  // This is a simplified conversion - the old Filter format is more complex
  // and uses SQL expressions. For now, we'll return undefined and log a warning.
  console.warn(
    'Filter conversion not fully implemented, filter will be ignored:',
    oldFilter,
  );
  return undefined;
}

export interface AddSqlTableTabParams {
  table: SqlTableDefinition;
  filters?: Filter[];
  imports?: string[];
}

/**
 * Main entry point - maintains backward compatibility with original API
 */
export function addLegacyTableTab(
  trace: Trace,
  config: AddSqlTableTabParams,
): void {
  // Convert old filters to new format
  const dataGridFilters: DataGridFilter[] = (config.filters ?? [])
    .map(convertFilter)
    .filter((f): f is DataGridFilter => f !== undefined);

  addEphemeralTab(
    trace,
    'sqlTable',
    new DataGridTableTab(trace, {
      table: config.table,
      filters: dataGridFilters,
      imports: config.imports,
    }),
  );
}

interface DataGridTableTabConfig {
  table: SqlTableDefinition;
  filters?: DataGridFilter[];
  imports?: string[];
}

/**
 * DataGrid-based table tab implementation
 */
class DataGridTableTab implements Tab {
  private readonly uuid: string;
  private readonly trace: Trace;
  private readonly table: SqlTableDefinition;
  private readonly schema: SchemaRegistry;
  private readonly rootSchema: string;
  private readonly dataSource: SQLDataSource;

  // State management (controlled mode)
  private columns?: ReadonlyArray<string>;
  private sorting: Sorting = {direction: 'UNSORTED'};
  private filters: ReadonlyArray<DataGridFilter> = [];
  private dataGridApi?: DataGridApi;

  // UI state
  private selectedTab: string;

  constructor(trace: Trace, config: DataGridTableTabConfig) {
    this.uuid = uuidv4();
    this.selectedTab = this.uuid;
    this.trace = trace;
    this.table = config.table;
    this.filters = config.filters ?? [];

    // Convert old table definition to new schema
    const {schema, rootSchema, sqlSchema} = convertTableDefinitionToSchema(
      config.table,
    );
    this.schema = schema;
    this.rootSchema = rootSchema;

    const tableImports = config.table.imports ?? [];
    const additionalImports = config.imports ?? [];

    // Create SQL data source with schema for proper JOIN support
    this.dataSource = new SQLDataSource({
      engine: trace.engine,
      sqlSchema,
      rootSchemaName: rootSchema,
      prelude: tableImports
        .concat(additionalImports)
        .map((i) => `INCLUDE PERFETTO MODULE ${i};`)
        .join('\n'),
    });
  }

  private getTableButtons() {
    const rowCount = this.dataGridApi?.getRowCount();

    const navigation = [
      exists(rowCount) && `Total rows: ${rowCount}`,
      // Note: DataGrid handles pagination internally via virtual scrolling
      // No manual back/forward buttons needed
    ];

    // Get current query for debug track and copy
    const currentQuery = this.dataSource.getCurrentQuery();

    const addDebugTrack = m(
      Popup,
      {
        trigger: m(Button, {label: 'Show debug track'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        trace: this.trace,
        query: currentQuery,
        availableColumns: this.columns ?? [],
      }),
    );

    const copyQueryButton = m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.Menu,
        }),
      },
      m(MenuItem, {
        label: 'Duplicate',
        icon: 'tab_duplicate',
        onclick: () => {
          // Clone the tab with current state
          addEphemeralTab(
            this.trace,
            'sqlTable',
            new DataGridTableTab(this.trace, {
              table: this.table,
              filters: [...this.filters],
            }),
          );
        },
      }),
      m(MenuItem, {
        label: 'Copy SQL query',
        icon: Icons.Copy,
        onclick: () => {
          // Copy the actual working query from datasource
          copyToClipboard(currentQuery || `SELECT * FROM ${this.table.name}`);
        },
      }),
    );

    return [...navigation, addDebugTrack, copyQueryButton];
  }

  render() {
    const hasFilters = this.filters.length > 0;

    const tabs: (TabOption & {content: m.Children})[] = [
      {
        key: this.uuid,
        title: 'Table',
        content: m(DataGrid, {
          schema: this.schema,
          rootSchema: this.rootSchema,
          data: this.dataSource,

          // Controlled state
          columns: this.columns,
          sorting: this.sorting,
          filters: this.filters,

          // State change callbacks
          onColumnsChanged: (cols) => {
            this.columns = cols;
          },
          onSort: (sorting) => {
            this.sorting = sorting;
          },
          onFilterAdd: (filter) => {
            this.filters = [...this.filters, filter];
          },
          onFilterRemove: (index) => {
            this.filters = this.filters.filter((_, i) => i !== index);
          },
          clearFilters: () => {
            this.filters = [];
          },

          // UI options
          fillHeight: true,
          showExportButton: true,
          showRowCount: true,
          enableSortingControls: true,
          enableFilterControls: true,
          enablePivotControls: true,

          // Get API reference
          onReady: (api) => {
            this.dataGridApi = api;
          },
        }),
      },
    ];

    // Note: Pivot tables, bar charts, and histograms removed
    // They need separate migration to work with DataGrid

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.getDisplayName(),
        buttons: this.getTableButtons(),
        fillHeight: true,
      },
      m(
        '.pf-sql-table',
        (hasFilters || tabs.length > 1) &&
          m('.pf-sql-table__toolbar', [
            hasFilters &&
              m(GridFilterBar, [
                this.filters.map((filter, idx) => {
                  const filterText = this.formatFilter(filter);
                  return m(GridFilterChip, {
                    content: filterText,
                    onRemove: () => {
                      this.filters = this.filters.filter((_, i) => i !== idx);
                    },
                  });
                }),
              ]),
            tabs.length > 1 &&
              m(TabStrip, {
                tabs,
                currentTabKey: this.selectedTab,
                onTabChange: (key) => (this.selectedTab = key),
              }),
          ]),
        m(
          '.pf-sql-table__table',
          tabs.map((tab) =>
            m(
              Gate,
              {
                open: tab.key == this.selectedTab,
              },
              tab.content,
            ),
          ),
        ),
      ),
    );
  }

  private formatFilter(filter: DataGridFilter): string {
    if ('value' in filter) {
      if (Array.isArray(filter.value)) {
        return `${filter.column} ${filter.op} (${filter.value.length} values)`;
      }
      return `${filter.column} ${filter.op} ${filter.value}`;
    } else {
      return `${filter.column} ${filter.op}`;
    }
  }

  getTitle(): string {
    const rowCount = this.dataGridApi?.getRowCount();
    const rows = rowCount === undefined ? '' : ` (${rowCount})`;
    return `Table ${this.getDisplayName()}${rows}`;
  }

  private getDisplayName(): string {
    return this.table.displayName ?? this.table.name;
  }

  isLoading(): boolean {
    return this.dataSource.isLoading ?? false;
  }
}
