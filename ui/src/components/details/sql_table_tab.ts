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
import {SqlTableState} from '../widgets/sql/table/state';
import {SqlTable} from '../widgets/sql/table/table';
import {SqlTableDescription} from '../widgets/sql/table/table_description';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {addEphemeralTab} from './add_ephemeral_tab';
import {Tab} from '../../public/tab';
import {Filter, Filters, renderFilters} from '../widgets/sql/table/filters';
import {PivotTableState} from '../widgets/sql/pivot_table/pivot_table_state';
import {TableColumn} from '../widgets/sql/table/table_column';
import {PivotTable} from '../widgets/sql/pivot_table/pivot_table';
import {pivotId} from '../widgets/sql/pivot_table/ids';
import {SqlBarChart, SqlBarChartState} from '../widgets/charts/sql_bar_chart';
import {SqlHistogram, SqlHistogramState} from '../widgets/charts/sql_histogram';
import {sqlColumnId} from '../widgets/sql/table/sql_column';
import {Stack} from '../../widgets/stack';

export interface AddSqlTableTabParams {
  table: SqlTableDescription;
  filters?: Filter[];
  imports?: string[];
}

export function addLegacyTableTab(
  trace: Trace,
  config: AddSqlTableTabParams,
): void {
  addSqlTableTabWithState(
    trace,
    new SqlTableState(trace, config.table, {
      filters: new Filters(config.filters),
      imports: config.imports,
    }),
  );
}

function addSqlTableTabWithState(trace: Trace, state: SqlTableState) {
  addEphemeralTab(trace, 'sqlTable', new LegacySqlTableTab(state));
}

class LegacySqlTableTab implements Tab {
  constructor(private readonly state: SqlTableState) {
    this.selected = {
      kind: 'table',
      state,
    };
  }

  private selected:
    | {
        kind: 'table';
        state: SqlTableState;
      }
    | {
        kind: 'pivot';
        state: PivotTableState;
      }
    | {
        kind: 'bar_chart';
        state: SqlBarChartState;
      }
    | {
        kind: 'histogram';
        state: SqlHistogramState;
      };

  private pivots: PivotTableState[] = [];
  private bar_charts: SqlBarChartState[] = [];
  private histograms: SqlHistogramState[] = [];

  private getTableButtons() {
    const range = this.state.getDisplayedRange();
    const rowCount = this.state.getTotalRowCount();
    const navigation = [
      exists(range) &&
        exists(rowCount) &&
        `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !this.state.canGoBack(),
        onclick: () => this.state.goBack(),
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !this.state.canGoForward(),
        onclick: () => this.state.goForward(),
      }),
    ];
    const {selectStatement, columns} = this.state.getCurrentRequest();
    const debugTrackColumns = Object.values(columns).filter(
      (c) => !c.startsWith('__'),
    );
    const addDebugTrack = m(
      Popup,
      {
        trigger: m(Button, {label: 'Show debug track'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        trace: this.state.trace,
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
            addSqlTableTabWithState(this.state.trace, this.state.clone()),
        }),
        m(MenuItem, {
          label: 'Copy SQL query',
          icon: Icons.Copy,
          onclick: () => copyToClipboard(this.state.getNonPaginatedSQLQuery()),
        }),
      ),
    ];
  }

  private tableMenuItems(column: TableColumn) {
    return [
      m(MenuItem, {
        label: 'Pivot',
        onclick: () => {
          const state = new PivotTableState({
            pivots: [column],
            table: this.state.config,
            trace: this.state.trace,
            filters: this.state.filters,
          });
          this.selected = {
            kind: 'pivot',
            state,
          };
          this.pivots.push(state);
        },
      }),
      m(MenuItem, {
        label: 'Add bar chart',
        onclick: () => {
          const state = new SqlBarChartState({
            trace: this.state.trace,
            sqlSource: this.state.config.name,
            column: column.column,
            filters: this.state.filters,
          });
          this.selected = {
            kind: 'bar_chart',
            state,
          };
          this.bar_charts.push(state);
        },
      }),
      m(MenuItem, {
        label: 'Add histogram',
        onclick: () => {
          const state = new SqlHistogramState({
            trace: this.state.trace,
            sqlSource: this.state.config.name,
            column: column.column,
            filters: this.state.filters,
          });
          this.selected = {
            kind: 'histogram',
            state,
          };
          this.histograms.push(state);
        },
      }),
    ];
  }

  render() {
    const showViewButtons =
      this.pivots.length > 0 ||
      this.bar_charts.length > 0 ||
      this.histograms.length > 0;
    const hasFilters = this.state.filters.get().length > 0;

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.getDisplayName(),
        buttons: this.getTableButtons(),
        fillParent: true,
      },
      m('.pf-sql-table', [
        (hasFilters || showViewButtons) &&
          m('.pf-sql-table__toolbar', [
            hasFilters && renderFilters(this.state.filters),
            showViewButtons &&
              m(Stack, {orientation: 'horizontal'}, [
                m(Button, {
                  label: 'Table',
                  active: this.selected.state === this.state,
                  onclick: () => {
                    this.selected = {
                      kind: 'table',
                      state: this.state,
                    };
                  },
                }),
                this.pivots.map((pivot) =>
                  m(Button, {
                    label: `Pivot: ${pivot.getPivots().map(pivotId).join(', ')}`,
                    active: this.selected.state === pivot,
                    onclick: () => {
                      this.selected = {
                        kind: 'pivot',
                        state: pivot,
                      };
                    },
                  }),
                ),
                this.bar_charts.map((chart) =>
                  m(Button, {
                    label: `Bar chart: ${sqlColumnId(chart.args.column)}`,
                    active: this.selected.state === chart,
                    onclick: () => {
                      this.selected = {
                        kind: 'bar_chart',
                        state: chart,
                      };
                    },
                  }),
                ),
                this.histograms.map((chart) =>
                  m(Button, {
                    label: `Histogram: ${sqlColumnId(chart.args.column)}`,
                    active: this.selected.state === chart,
                    onclick: () => {
                      this.selected = {
                        kind: 'histogram',
                        state: chart,
                      };
                    },
                  }),
                ),
              ]),
          ]),
        m('.pf-sql-table__table', [
          this.selected.kind === 'table' &&
            m(SqlTable, {
              state: this.selected.state,
              addColumnMenuItems: this.tableMenuItems.bind(this),
            }),
          this.selected.kind === 'pivot' &&
            m(PivotTable, {
              state: this.selected.state,
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
                      this.state.filters.addFilters(node.getFilters());
                    },
                  }),
                  m(MenuItem, {
                    label: 'Open tab with filters',
                    onclick: () => {
                      const newState = this.state.clone();
                      newState.filters.addFilters(node.getFilters());
                      addSqlTableTabWithState(this.state.trace, newState);
                    },
                  }),
                ),
            }),
          this.selected.kind === 'bar_chart' &&
            m(SqlBarChart, {state: this.selected.state}),
          this.selected.kind === 'histogram' &&
            m(SqlHistogram, {state: this.selected.state}),
        ]),
      ]),
    );
  }

  getTitle(): string {
    const rowCount = this.state.getTotalRowCount();
    const rows = rowCount === undefined ? '' : ` (${rowCount})`;
    return `Table ${this.getDisplayName()}${rows}`;
  }

  private getDisplayName(): string {
    return this.state.config.displayName ?? this.state.config.name;
  }

  isLoading(): boolean {
    return this.state.isLoading();
  }
}
