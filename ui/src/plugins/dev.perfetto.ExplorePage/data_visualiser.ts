// Copyright (C) 2024 The Android Open Source Project
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
import {SqlTableState} from '../../components/widgets/sql/legacy_table/state';
import {
  Chart,
  ChartOption,
  createChartConfigFromSqlTableState,
  renderChartComponent,
} from '../../components/widgets/charts/chart';
import {Trace} from '../../public/trace';
import {Menu, MenuItem, MenuItemAttrs} from '../../widgets/menu';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {exists} from '../../base/utils';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {DetailsShell} from '../../widgets/details_shell';
import {SqlTable} from '../../components/widgets/sql/legacy_table/table';
import {AddChartMenuItem} from '../../components/widgets/charts/add_chart_menu';
import {
  SplitPanel,
  SplitPanelDrawerVisibility,
} from '../../widgets/split_panel';
import {VerticalSplitContainer} from './vertical_split_container';

export interface DataVisualiserState {
  sqlTableViewState?: SqlTableState;
  selectedTableName?: string;
}

export interface DataVisualiserAttrs {
  trace: Trace;
  readonly state: DataVisualiserState;
  charts: Set<Chart>;
}

export class DataVisualiser implements m.ClassComponent<DataVisualiserAttrs> {
  private visibility = SplitPanelDrawerVisibility.VISIBLE;

  // Show menu with standard library tables
  private renderSelectableTablesMenuItems(
    trace: Trace,
    state: DataVisualiserState,
  ): m.Vnode<MenuItemAttrs, unknown>[] {
    const sqlModules = trace.plugins
      .getPlugin(SqlModulesPlugin)
      .getSqlModules();
    return sqlModules.listTables().map((tableName) => {
      const sqlTable = sqlModules
        .getModuleForTable(tableName)
        ?.getTable(tableName);
      const sqlTableViewDescription = sqlModules
        .getModuleForTable(tableName)
        ?.getSqlTableDescription(tableName);

      return m(MenuItem, {
        label: tableName,
        onclick: () => {
          if (
            (state.selectedTableName &&
              tableName === state.selectedTableName) ||
            sqlTable === undefined ||
            sqlTableViewDescription === undefined
          ) {
            return;
          }

          state.selectedTableName = sqlTable.name;
          state.sqlTableViewState = new SqlTableState(
            trace,
            {
              name: tableName,
              columns: sqlTable.getTableColumns(),
            },
            {imports: sqlTableViewDescription.imports},
          );
        },
      });
    });
  }

  private renderSqlTable(state: DataVisualiserState, charts: Set<Chart>) {
    const sqlTableViewState = state.sqlTableViewState;

    if (sqlTableViewState === undefined) return;

    const range = sqlTableViewState.getDisplayedRange();
    const rowCount = sqlTableViewState.getTotalRowCount();

    const navigation = [
      exists(range) &&
        exists(rowCount) &&
        `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !sqlTableViewState.canGoBack(),
        onclick: () => sqlTableViewState!.goBack(),
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !sqlTableViewState.canGoForward(),
        onclick: () => sqlTableViewState!.goForward(),
      }),
    ];

    return m(
      DetailsShell,
      {
        title: 'Explore Table',
        buttons: navigation,
        fillParent: false,
      },
      m(SqlTable, {
        state: sqlTableViewState,
        addColumnMenuItems: (column, columnAlias) =>
          m(AddChartMenuItem, {
            chartConfig: createChartConfigFromSqlTableState(
              column,
              columnAlias,
              sqlTableViewState,
            ),
            chartOptions: [ChartOption.HISTOGRAM],
            addChart: (chart) => charts.add(chart),
          }),
      }),
    );
  }

  private renderRemovableChart(chart: Chart, charts: Set<Chart>) {
    return m(
      '.chart-card',
      {
        key: `${chart.option}-${chart.config.columnTitle}`,
      },
      m(Button, {
        icon: Icons.Close,
        onclick: () => {
          charts.delete(chart);
        },
      }),
      renderChartComponent(chart),
    );
  }

  view({attrs}: m.CVnode<DataVisualiserAttrs>) {
    const {trace, state, charts} = attrs;

    return m(
      SplitPanel,
      {
        visibility: this.visibility,
        onVisibilityChange: (visibility) => {
          this.visibility = visibility;
        },
        drawerContent: this.renderSqlTable(state, charts),
      },
      m(VerticalSplitContainer, {
        leftPane: m(Menu, this.renderSelectableTablesMenuItems(trace, state)),
        rightPane: Array.from(attrs.charts.values()).map((chart) =>
          this.renderRemovableChart(chart, attrs.charts),
        ),
      }),
    );
  }
}
