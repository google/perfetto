// Copyright (C) 2023 The Android Open Source Project
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
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {Popup, PopupPosition} from '../../widgets/popup';
import {AddDebugTrackMenu} from '../tracks/add_debug_track_menu';
import {getSelectableColumns, SqlTableState} from '../widgets/sql/table/state';
import {SqlTable} from '../widgets/sql/table/table';
import {SqlTableDefinition} from '../widgets/sql/table/table_description';
import {resolveTableDefinition} from '../widgets/sql/table/columns';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {addEphemeralTab} from './add_ephemeral_tab';
import {Tab} from '../../public/tab';
import {Filter, Filters, renderFilters} from '../widgets/sql/table/filters';
import {PivotTableState} from '../widgets/sql/pivot_table/pivot_table_state';
import {TableColumn} from '../widgets/sql/table/table_column';
import {PivotTable} from '../widgets/sql/pivot_table/pivot_table';
import {pivotId} from '../widgets/sql/pivot_table/ids';
import {BarChart} from '../widgets/charts/bar_chart';
import {SQLBarChartLoader} from '../widgets/charts/bar_chart_loader';
import {Histogram} from '../widgets/charts/histogram';
import {SQLHistogramLoader} from '../widgets/charts/histogram_loader';
import {sqlColumnId, SqlColumn} from '../widgets/sql/table/sql_column';
import {buildSqlQuery} from '../widgets/sql/table/query_builder';
import {uuidv4} from '../../base/uuid';
import {StandardFilters} from '../widgets/sql/table/filters';
import {TabOption, TabStrip} from '../../widgets/tab_strip';
import {Gate} from '../../base/mithril_utils';
import {isQuantitativeType} from '../../trace_processor/perfetto_sql_type';

export interface AddSqlTableTabParams {
  table: SqlTableDefinition;
  filters?: Filter[];
  imports?: string[];
}

// State for bar chart tabs using ECharts
// Loader is recreated when filters change (query changes)
interface BarChartTabState {
  readonly uuid: string;
  readonly column: SqlColumn;
  readonly columnName: string;
  loader?: SQLBarChartLoader;
  lastQuery?: string;
}

// State for histogram tabs using ECharts
// Loader is recreated when filters change (query changes)
interface HistogramTabState {
  readonly uuid: string;
  readonly column: SqlColumn;
  readonly columnName: string;
  loader?: SQLHistogramLoader;
  lastQuery?: string;
}

export function addLegacyTableTab(
  trace: Trace,
  config: AddSqlTableTabParams,
): void {
  const resolvedTable = resolveTableDefinition(trace, config.table);
  addSqlTableTabWithState(
    trace,
    new SqlTableState(trace, resolvedTable, {
      filters: new Filters(config.filters),
      imports: config.imports,
    }),
  );
}

function addSqlTableTabWithState(trace: Trace, state: SqlTableState) {
  addEphemeralTab(trace, 'sqlTable', new SqlTableTab(state));
}

class SqlTableTab implements Tab {
  constructor(private readonly tableState: SqlTableState) {
    this.selectedTab = tableState.uuid;
  }

  private selectedTab: string;

  private pivots: PivotTableState[] = [];
  private barCharts: BarChartTabState[] = [];
  private histograms: HistogramTabState[] = [];

  private getTableButtons() {
    const range = this.tableState.getDisplayedRange();
    const rowCount = this.tableState.getTotalRowCount();
    const navigation = [
      exists(range) &&
        exists(rowCount) &&
        `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !this.tableState.canGoBack(),
        onclick: () => this.tableState.goBack(),
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !this.tableState.canGoForward(),
        onclick: () => this.tableState.goForward(),
      }),
    ];
    const {selectStatement, columns} = this.tableState.getCurrentRequest();
    const debugTrackColumns = Object.values(columns).filter(
      (c) => !c.startsWith('__'),
    );
    const addDebugTrack = m(
      Popup,
      {
        trigger: m(Button, {label: 'Add debug track'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        trace: this.tableState.trace,
        query: `SELECT ${debugTrackColumns.join(', ')} FROM (${selectStatement})`,
        availableColumns: debugTrackColumns,
      }),
    );
    return [
      ...navigation,
      addDebugTrack,
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.Menu,
          }),
        },
        m(MenuItem, {
          label: 'Duplicate',
          icon: 'tab_duplicate',
          onclick: () =>
            addSqlTableTabWithState(
              this.tableState.trace,
              this.tableState.clone(),
            ),
        }),
        m(MenuItem, {
          label: 'Copy SQL query',
          icon: Icons.Copy,
          onclick: () =>
            copyToClipboard(this.tableState.getNonPaginatedSQLQuery()),
        }),
      ),
    ];
  }

  private tableMenuItems(column: TableColumn) {
    return m(
      MenuItem,
      {
        label: 'Analyze',
        icon: Icons.Analyze,
      },
      m(MenuItem, {
        label: 'Pivot',
        icon: Icons.Pivot,
        onclick: () => {
          const state = new PivotTableState({
            pivots: [column],
            table: this.tableState.config,
            trace: this.tableState.trace,
            filters: this.tableState.filters,
          });
          this.selectedTab = state.uuid;
          this.pivots.push(state);
        },
      }),
      m(MenuItem, {
        label: 'Add bar chart',
        icon: Icons.Chart,
        onclick: () => {
          const uuid = uuidv4();
          const columnName = sqlColumnId(column.column);
          const state: BarChartTabState = {
            uuid,
            column: column.column,
            columnName,
          };
          this.selectedTab = uuid;
          this.barCharts.push(state);
        },
      }),
      (column.type === undefined ? true : isQuantitativeType(column.type)) &&
        m(MenuItem, {
          label: 'Add histogram',
          icon: Icons.Chart,
          onclick: () => {
            const uuid = uuidv4();
            const columnName = sqlColumnId(column.column);
            const state: HistogramTabState = {
              uuid,
              column: column.column,
              columnName,
            };
            this.selectedTab = uuid;
            this.histograms.push(state);
          },
        }),
    );
  }

  render() {
    const hasFilters = this.tableState.filters.get().length > 0;

    const tabs: (TabOption & {content: m.Children})[] = [
      {
        key: this.tableState.uuid,
        title: 'Table',
        content: m(SqlTable, {
          state: this.tableState,
          addColumnMenuItems: this.tableMenuItems.bind(this),
        }),
      },
    ];

    for (const pivot of this.pivots) {
      tabs.push({
        key: pivot.uuid,
        title: `Pivot: ${pivot.getPivots().map(pivotId).join(', ')}`,
        rightIcon: m(Button, {
          icon: Icons.Close,
          onclick: () => {
            this.pivots = this.pivots.filter((p) => p.uuid !== pivot.uuid);
          },
        }),
        content: m(PivotTable, {
          state: pivot,
          getSelectableColumns: () => getSelectableColumns(this.tableState),
          extraRowButton: (node) =>
            // Do not show any buttons for root as it doesn't have any filters anyway.
            !node.isRoot() &&
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  icon: Icons.GoTo,
                }),
              },
              m(MenuItem, {
                label: 'Add filters',
                onclick: () => {
                  this.tableState.filters.addFilters(node.getFilters());
                },
              }),
              m(MenuItem, {
                label: 'Open tab with filters',
                onclick: () => {
                  const newState = this.tableState.clone();
                  newState.filters.addFilters(node.getFilters());
                  addSqlTableTabWithState(this.tableState.trace, newState);
                },
              }),
            ),
        }),
      });
    }

    for (const chart of this.barCharts) {
      // Build query with current filters
      const query = buildSqlQuery({
        table: this.tableState.config.name,
        filters: this.tableState.filters.get(),
        columns: {value: chart.column},
      });

      // Recreate loader if query changed
      if (chart.lastQuery !== query) {
        chart.loader?.dispose();
        chart.loader = new SQLBarChartLoader({
          engine: this.tableState.trace.engine,
          query,
          dimensionColumn: 'value',
          measureColumn: 'value',
        });
        chart.lastQuery = query;
      }

      const result = chart.loader!.use({aggregation: 'COUNT_DISTINCT'});
      tabs.push({
        key: chart.uuid,
        title: `Bar chart: ${chart.columnName}`,
        rightIcon: m(Button, {
          icon: Icons.Close,
          onclick: () => {
            chart.loader?.dispose();
            this.barCharts = this.barCharts.filter(
              (c) => c.uuid !== chart.uuid,
            );
          },
        }),
        content: m(BarChart, {
          data: result.data,
          orientation: 'horizontal',
          dimensionLabel: chart.columnName,
          measureLabel: 'Count',
          fillParent: true,
          onBrush: (labels) => {
            this.tableState.filters.addFilter(
              StandardFilters.valueIsOneOf(
                chart.column,
                labels.map((l) => (l === '(null)' ? null : l)),
              ),
            );
          },
        }),
      });
    }

    for (const histogram of this.histograms) {
      // Build query with current filters
      const query = buildSqlQuery({
        table: this.tableState.config.name,
        filters: this.tableState.filters.get(),
        columns: {value: histogram.column},
      });

      // Recreate loader if query changed
      if (histogram.lastQuery !== query) {
        histogram.loader?.dispose();
        histogram.loader = new SQLHistogramLoader({
          engine: this.tableState.trace.engine,
          query,
          valueColumn: 'value',
        });
        histogram.lastQuery = query;
      }

      const result = histogram.loader!.use({});
      tabs.push({
        key: histogram.uuid,
        title: `Histogram: ${histogram.columnName}`,
        rightIcon: m(Button, {
          icon: Icons.Close,
          onclick: () => {
            histogram.loader?.dispose();
            this.histograms = this.histograms.filter(
              (h) => h.uuid !== histogram.uuid,
            );
          },
        }),
        content: m(Histogram, {
          fillParent: true,
          data: result.data,
          xAxisLabel: histogram.columnName,
          yAxisLabel: 'Count',
        }),
      });
    }

    // Fall back to the table view if the selected tab was closed.
    if (!tabs.some((tab) => tab.key === this.selectedTab)) {
      this.selectedTab = this.tableState.uuid;
    }

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
            hasFilters && renderFilters(this.tableState.filters),
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

  getTitle(): string {
    const rowCount = this.tableState.getTotalRowCount();
    const rows = rowCount === undefined ? '' : ` (${rowCount})`;
    return `Table ${this.getDisplayName()}${rows}`;
  }

  private getDisplayName(): string {
    return this.tableState.config.displayName ?? this.tableState.config.name;
  }

  isLoading(): boolean {
    return this.tableState.isLoading();
  }
}
