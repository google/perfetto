// Copyright (C) 2026 The Android Open Source Project
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
import {Trace} from '../../public/trace';
import {SqlDataSource, HttpDataSource} from '../../components/d3';
import {Chart, FilterStore, ChartSpec} from '../../widgets/charts/d3';
import {
  ChartType,
  AggregationFunction,
  SortBy,
  SortDirection,
} from '../../widgets/charts/d3/data/types';
import {DataSource} from '../../widgets/charts/d3/data/source';
import {ChartWidget} from '../../widgets/d3_chart_widget';
import {Button} from '../../widgets/button';
import {Editor} from '../../widgets/editor';
import {Icon} from '../../widgets/icon';
import {Switch} from '../../widgets/switch';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {ChartCreatorSidebar} from './chart_creator_sidebar';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {Row as DataGridRow} from '../../trace_processor/query_result';
import {Filter as DataGridFilter} from '../../components/widgets/datagrid/model';
import {Filter as D3Filter} from '../../widgets/charts/d3/data/types';
import {shortUuid} from '../../base/uuid';

interface D3ChartsPageAttrs {
  trace?: Trace;
  useBrushBackend?: boolean;
  initialQuery?: string;
  hideSqlEditor?: boolean;
  sidebarVisible?: boolean;
  onToggleSidebar?: () => void;
}

const DEFAULT_SQL = `SELECT 
  name,
  category,
  dur,
  ts,
  id
FROM slice
LIMIT 1000`;

// Table view state with filter subscription
interface TableView {
  id: number;
  dataSource: InMemoryDataSource;
  schema: SchemaRegistry;
  columns: string[];
  allFilters: DataGridFilter[]; // All filters (from charts + tables) shown as chips
  filterGroupMap: Map<string, string>; // Maps filter key to filter group ID
  unsubscribe: () => void;
}

export class D3ChartsPage implements m.ClassComponent<D3ChartsPageAttrs> {
  private charts: Array<{id: number; chart: Chart}> = [];
  private tables: TableView[] = [];
  private filterStore = new FilterStore();
  private sqlQuery = DEFAULT_SQL;
  private trace?: Trace;
  private useBrushBackend = false;
  private hideSqlEditor = false;
  private errorMessage = '';
  private sqlSource?: SqlDataSource;
  private dataSource?: DataSource;
  private availableColumns: string[] = [];
  private sidebarOpen = false;
  private nextTableId = 0;
  private nextChartId = 0;
  private isLoading = false;
  private limit = 1000;
  private defaultChartCount = 0; // Track number of default charts
  private defaultTableCount = 0; // Track number of default tables

  oninit({attrs}: m.Vnode<D3ChartsPageAttrs>) {
    this.trace = attrs.trace;
    this.useBrushBackend = attrs.useBrushBackend || false;
    this.hideSqlEditor = attrs.hideSqlEditor || false;
    if (attrs.initialQuery) {
      this.sqlQuery = attrs.initialQuery;
    }
    this.runQuery();
  }

  view({attrs}: m.Vnode<D3ChartsPageAttrs>) {
    return m('.d3-charts-page', [
      // Global loading indicator - thin blue line at the very top
      this.isLoading && m('.loading-indicator'),
      // Main content area
      m('.main-content', [
        !this.hideSqlEditor && this.renderSqlEditor(attrs),
        this.renderChartsColumn(),
      ]),
      this.sidebarOpen && this.renderSidebar(),
    ]);
  }

  private renderSqlEditor(attrs: D3ChartsPageAttrs) {
    return m('.sql-editor-section', [
      m('.editor-header.pf-stack.pf-stack--horiz.pf-spacing-medium', [
        // Hamburger button (only show if sidebar toggle callback provided and sidebar is hidden)
        attrs.onToggleSidebar &&
          attrs.sidebarVisible === false &&
          m(Button, {
            icon: 'menu',
            onclick: attrs.onToggleSidebar,
            style: {
              fontSize: '24px',
            },
          }),
        this.useBrushBackend &&
          m('input.pf-text-input[type=number]', {
            value: this.limit,
            placeholder: 'Limit',
            style: {
              width: '100px',
            },
            onchange: (e: Event) => {
              const newLimit = parseInt(
                (e.target as HTMLInputElement).value,
                10,
              );
              if (!isNaN(newLimit) && newLimit > 0) {
                this.limit = newLimit;
              }
            },
          }),
        m(Button, {
          label: 'Run Query',
          icon: 'play_arrow',
          onclick: () => this.runQuery(),
        }),
        m('.pf-stack.pf-stack--horiz.pf-spacing-medium', [
          'or press',
          m('span.pf-hotkey', [
            m(
              'span.pf-keycap.pf-spacing-medium',
              m(Icon, {icon: 'keyboard_command_key'}),
            ),
            m(
              'span.pf-keycap.pf-spacing-medium',
              m(Icon, {icon: 'keyboard_return'}),
            ),
          ]),
        ]),
        m('.pf-stack-auto'),
        m(Switch, {
          label: 'Update source chart',
          checked: this.filterStore.getUpdateSourceChart(),
          onchange: (e: Event) => {
            const checked = (e.target as HTMLInputElement).checked;
            this.filterStore.setUpdateSourceChart(checked);
          },
        }),
        m(Button, {
          icon: 'add',
          onclick: () => {
            this.sidebarOpen = true;
            m.redraw();
          },
        }),
      ]),
      m(
        '.editor-container',
        m(Editor, {
          text: this.sqlQuery,
          language: 'perfetto-sql',
          fillHeight: true,
          onUpdate: (text: string) => {
            this.sqlQuery = text;
          },
          onExecute: () => this.runQuery(),
        }),
      ),
      this.errorMessage && m('.error-message', this.errorMessage),
    ]);
  }

  private renderChartsColumn() {
    return m(
      '.charts-column',
      this.charts.length === 0 && this.tables.length === 0
        ? this.renderEmptyState()
        : [
            ...this.tables.map((table, index) =>
              this.renderTable(table, index),
            ),
            ...this.charts.map((chartWrapper, index) =>
              this.renderChart(chartWrapper, index),
            ),
          ],
    );
  }

  private renderEmptyState() {
    return m(
      '.empty-state',
      {
        onclick: () => {
          this.sidebarOpen = true;
          m.redraw();
        },
      },
      [
        m(Icon, {icon: 'add_circle', style: {fontSize: '64px'}}),
        m(
          'p',
          {style: {fontSize: '16px', fontWeight: 500}},
          'Click to create your first chart',
        ),
      ],
    );
  }

  private renderTable(table: TableView, index: number) {
    return m(
      '.table-wrapper',
      {
        key: `table-${table.id}`,
      },
      [
        m(
          'button.pf-button.pf-button--minimal',
          {
            onclick: () => this.removeTable(index),
          },
          m(Icon, {icon: 'close'}),
        ),
        m(DataGrid, {
          schema: table.schema,
          rootSchema: 'data',
          data: table.dataSource,
          initialColumns: table.columns.map((col) => ({
            id: shortUuid(),
            field: col,
          })),
          filters: table.allFilters,
          onFiltersChanged: (newFilters) =>
            this.handleTableFiltersChanged(table, newFilters),
          fillHeight: false,
          showExportButton: true,
        }),
      ],
    );
  }

  private renderChart(chartWrapper: {id: number; chart: Chart}, index: number) {
    return m(
      '.chart-wrapper',
      {
        key: `chart-${chartWrapper.id}`,
      },
      [
        m(
          'button.pf-button.pf-button--minimal',
          {
            onclick: () => this.removeChart(index),
          },
          m(Icon, {icon: 'close'}),
        ),
        m(ChartWidget, {chart: chartWrapper.chart}),
      ],
    );
  }

  private renderSidebar() {
    return m(ChartCreatorSidebar, {
      availableColumns: this.availableColumns,
      onClose: () => {
        this.sidebarOpen = false;
        m.redraw();
      },
      onCreate: (type, spec) => {
        if (type === 'table') {
          this.createTable();
        } else if (spec && this.dataSource) {
          this.charts.push({
            id: this.nextChartId++,
            chart: new Chart(spec, this.dataSource, this.filterStore),
          });
          m.redraw();
        }
        this.sidebarOpen = false;
      },
    });
  }

  private handleTableFiltersChanged(
    table: TableView,
    newFilters: readonly DataGridFilter[],
  ) {
    // Determine which filters were removed
    const removedFilters = table.allFilters.filter((oldFilter) => {
      return !newFilters.some((newFilter) => {
        const oldKey = `${oldFilter.field}:${oldFilter.op}:${JSON.stringify(
          'value' in oldFilter ? oldFilter.value : null,
        )}`;
        const newKey = `${newFilter.field}:${newFilter.op}:${JSON.stringify(
          'value' in newFilter ? newFilter.value : null,
        )}`;
        return oldKey === newKey;
      });
    });

    // For each removed filter, find its group and clear it
    for (const removedFilter of removedFilters) {
      const key = `${removedFilter.field}:${removedFilter.op}:${JSON.stringify(
        'value' in removedFilter ? removedFilter.value : null,
      )}`;
      const groupId = table.filterGroupMap.get(key);
      if (groupId) {
        this.filterStore.clearFilterGroup(groupId, `table-${table.id}`);
      }
    }

    // Determine which filters were added (exist in newFilters but not in allFilters)
    const addedFilters = newFilters.filter((newFilter) => {
      return !table.allFilters.some((oldFilter) => {
        const oldKey = `${oldFilter.field}:${oldFilter.op}:${JSON.stringify(
          'value' in oldFilter ? oldFilter.value : null,
        )}`;
        const newKey = `${newFilter.field}:${newFilter.op}:${JSON.stringify(
          'value' in newFilter ? newFilter.value : null,
        )}`;
        return oldKey === newKey;
      });
    });

    // If there are new filters added by the table, create a filter group
    if (addedFilters.length > 0) {
      const d3Filters = this.convertDataGridFiltersToD3(addedFilters);
      this.filterStore.setFilterGroup(
        {
          id: `table-${table.id}-${Date.now()}`,
          filters: d3Filters,
          label: 'Table filter',
        },
        `table-${table.id}`,
      );
    }
  }

  private async runQuery() {
    this.errorMessage = '';
    this.isLoading = true;
    m.redraw();

    // Clean up only default charts (preserve user-added charts)
    // Default charts are always at the beginning of the array
    for (let i = 0; i < this.defaultChartCount; i++) {
      const chartWrapper = this.charts[i];
      if (chartWrapper !== undefined) {
        chartWrapper.chart.destroy();
      }
    }
    this.charts.splice(0, this.defaultChartCount);
    this.defaultChartCount = 0;

    // Clean up only default tables (preserve user-added tables)
    // Default tables are always at the beginning of the array
    for (let i = 0; i < this.defaultTableCount; i++) {
      const table = this.tables[i];
      if (table !== undefined) {
        table.unsubscribe();
      }
    }
    this.tables.splice(0, this.defaultTableCount);
    this.defaultTableCount = 0;

    try {
      if (this.useBrushBackend) {
        // Use Brush backend
        this.dataSource = new HttpDataSource(
          this.sqlQuery,
          'android_telemetry.field_trace_summaries_prod.last30days',
          this.limit,
        );

        // Fetch a sample row to get available columns
        // Use a scatter chart spec since it returns raw data without aggregation
        const sampleData = await this.dataSource.query([], {
          type: ChartType.Scatter,
          x: '',
          y: '',
        });
        if (sampleData.length > 0) {
          this.availableColumns = Object.keys(sampleData[0]);
        } else {
          this.availableColumns = [];
          this.errorMessage =
            'Query executed successfully but returned no data. Try adjusting your query or increasing the LIMIT.';
        }
      } else {
        // Use local trace engine
        if (!this.trace) return;

        const engine = this.trace.engine;

        // Handle INCLUDE PERFETTO MODULE statements
        const lines = this.sqlQuery.trim().split('\n');
        const includeStatements: string[] = [];
        const queryLines: string[] = [];

        for (const line of lines) {
          const trimmedLine = line.trim();
          if (trimmedLine.toUpperCase().startsWith('INCLUDE PERFETTO MODULE')) {
            includeStatements.push(trimmedLine);
          } else if (trimmedLine) {
            queryLines.push(line);
          }
        }

        // Execute INCLUDE statements first
        for (const includeStmt of includeStatements) {
          await engine.query(includeStmt);
        }

        // Use the remaining query (without INCLUDE statements)
        const actualQuery = queryLines.join('\n').trim();

        if (!actualQuery) {
          this.errorMessage =
            'No SELECT query found. Please add a SELECT statement after the INCLUDE statements.';
          m.redraw();
          return;
        }

        // Create a SQL-based data source with the actual query
        this.sqlSource = new SqlDataSource(engine, actualQuery);
        this.dataSource = this.sqlSource;

        // Fetch a sample row to get available columns
        // Use a scatter chart spec since it returns raw data without aggregation
        const sampleData = await this.dataSource.query([], {
          type: ChartType.Scatter,
          x: '',
          y: '',
        });
        if (sampleData.length > 0) {
          this.availableColumns = Object.keys(sampleData[0]);
        } else {
          this.availableColumns = [];
          this.errorMessage =
            'Query executed successfully but returned no data. Try adjusting your query or increasing the LIMIT.';
        }

        // Add default table at the beginning
        await this.createDefaultTable();

        // Add default charts at the beginning
        const defaultChartSpecs: ChartSpec[] = [
          {
            type: ChartType.Histogram,
            x: 'dur',
            bins: 20,
          },
          {
            type: ChartType.Bar,
            x: 'name',
            y: 'id',
            aggregation: AggregationFunction.Count,
            sort: {
              by: SortBy.Y,
              direction: SortDirection.Desc,
            },
          },
        ];

        const dataSource = this.dataSource; // Capture for type safety
        const wrappedCharts = defaultChartSpecs.map((spec) => ({
          id: this.nextChartId++,
          chart: new Chart(spec, dataSource, this.filterStore),
        }));
        this.charts.unshift(...wrappedCharts);
        this.defaultChartCount = wrappedCharts.length;
      }

      m.redraw();
    } catch (error) {
      this.errorMessage = `Error: ${error}`;
      console.error('Query error:', error);
      m.redraw();
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }

  private async createDefaultTable() {
    try {
      const tableView = await this.buildTableView();
      if (tableView) {
        this.tables.unshift(tableView);
        this.defaultTableCount = 1;
        m.redraw();
      }
    } catch (error) {
      console.error('Default table creation error:', error);
    }
  }

  private async createTable() {
    try {
      const tableView = await this.buildTableView();
      if (tableView) {
        this.tables.push(tableView);
        m.redraw();
      } else {
        this.errorMessage = 'No data available to display in table';
        m.redraw();
      }
    } catch (error) {
      this.errorMessage = `Error creating table: ${error}`;
      console.error('Table creation error:', error);
      m.redraw();
    }
  }

  private async buildTableView(): Promise<TableView | null> {
    if (!this.dataSource) return null;

    // Use a scatter chart spec to get raw data without aggregation
    const data = await this.dataSource.query([], {
      type: ChartType.Scatter,
      x: '',
      y: '',
    });
    if (data.length === 0) return null;

    const columnSchema: Record<string, {}> = {};
    for (const col of this.availableColumns) {
      columnSchema[col] = {};
    }

    const schema: SchemaRegistry = {data: columnSchema};

    const dataGridData: DataGridRow[] = data.map((row) => {
      const cleanRow: DataGridRow = {};
      for (const key in row) {
        if (!row.hasOwnProperty(key)) continue;
        const value = row[key];
        if (value !== undefined) {
          cleanRow[key] = typeof value === 'boolean' ? (value ? 1 : 0) : value;
        }
      }
      return cleanRow;
    });

    const dataSource = new InMemoryDataSource(dataGridData);

    const tableView: TableView = {
      id: this.nextTableId++,
      dataSource,
      schema,
      columns: this.availableColumns,
      allFilters: [],
      filterGroupMap: new Map(),
      unsubscribe: () => {},
    };

    const unsubscribe = this.filterStore.subscribe((notification) => {
      const dataGridFilters: DataGridFilter[] = notification.filters.map(
        (f) => {
          if (f.val === null) {
            return {
              field: f.col,
              op: f.op as unknown as 'is null' | 'is not null',
            } as DataGridFilter;
          } else {
            return {
              field: f.col,
              op: f.op as Exclude<
                DataGridFilter['op'],
                'is null' | 'is not null'
              >,
              value: f.val,
            } as DataGridFilter;
          }
        },
      );

      const filterGroupMap = new Map<string, string>();
      for (const group of this.filterStore.getFilterGroups()) {
        for (const filter of group.filters) {
          const key = `${filter.col}:${filter.op}:${JSON.stringify(filter.val)}`;
          filterGroupMap.set(key, group.id);
        }
      }

      tableView.allFilters = dataGridFilters;
      tableView.filterGroupMap = filterGroupMap;
      m.redraw();
    });

    tableView.unsubscribe = unsubscribe;
    return tableView;
  }

  /**
   * Convert DataGrid filters to D3 chart filters.
   */
  private convertDataGridFiltersToD3(
    dataGridFilters: readonly DataGridFilter[],
  ): D3Filter[] {
    return dataGridFilters.map((filter) => {
      const col = filter.field;
      const op = filter.op;

      // Convert value back (numbers to booleans if needed, though we don't have that info)
      let val: D3Filter['val'];
      if ('value' in filter) {
        val = filter.value as D3Filter['val'];
      } else {
        val = null;
      }

      return {col, op: op as D3Filter['op'], val};
    });
  }

  private removeChart(index: number) {
    const chartWrapper = this.charts[index];
    if (chartWrapper !== undefined) {
      chartWrapper.chart.destroy();
      this.charts.splice(index, 1);
      m.redraw();
    }
  }

  private removeTable(index: number) {
    const table = this.tables[index];
    if (table !== undefined) {
      // Unsubscribe from filter updates
      table.unsubscribe();
      this.tables.splice(index, 1);
      m.redraw();
    }
  }

  onremove() {
    // Clean up charts and tables when page is removed
    this.charts.forEach((chartWrapper) => chartWrapper.chart.destroy());
    this.charts = [];
    this.tables.forEach((table) => table.unsubscribe());
    this.tables = [];
  }
}
