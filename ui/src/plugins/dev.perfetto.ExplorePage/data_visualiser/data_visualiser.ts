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
import {
  ChartAttrs,
  ChartType,
  renderChart,
} from '../../../components/widgets/charts/chart';
import {Trace} from '../../../public/trace';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {
  SplitPanel,
  SplitPanelDrawerVisibility,
} from '../../../widgets/split_panel';
import {VisViewSource} from './view_source';
import {AddChartMenuItem} from '../../../components/widgets/charts/add_chart_menu';
import {exists} from '../../../base/utils';
import {DetailsShell} from '../../../widgets/details_shell';
import {SqlTable} from '../../../components/widgets/sql/table/table';
import {sqlValueToSqliteString} from '../../../trace_processor/sql_utils';
import {renderFilters} from '../../../components/widgets/sql/table/filters';
import {ExplorePageState} from '../explore_page';

export interface DataVisualiserAttrs {
  trace: Trace;
  readonly state: ExplorePageState;
}

export class DataVisualiser implements m.ClassComponent<DataVisualiserAttrs> {
  private visibility = SplitPanelDrawerVisibility.VISIBLE;

  constructor({attrs}: m.Vnode<DataVisualiserAttrs>) {
    if (attrs.state.selectedNode === undefined) return;

    attrs.state.activeViewSource = new VisViewSource(
      attrs.trace,
      attrs.state.selectedNode,
    );
  }

  private renderSqlTable(state: ExplorePageState) {
    const sqlTableViewState = state.activeViewSource?.visViews?.sqlTableState;

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
      m('div', renderFilters(sqlTableViewState.filters)),
      m(SqlTable, {
        state: sqlTableViewState,
        addColumnMenuItems: (_, columnAlias) => {
          const chartAttrs = {
            data: state.activeViewSource?.data,
            columns: [columnAlias],
          };

          return m(AddChartMenuItem, {
            chartOptions: [
              {
                chartType: ChartType.BAR_CHART,
                ...chartAttrs,
                onIntervalSelection: (value) => {
                  const range = `(${value[columnAlias].map(sqlValueToSqliteString).join(', ')})`;
                  state.activeViewSource?.filters.addFilter({
                    op: (cols) => `${cols[0]} IN ${range}`,
                    columns: [columnAlias],
                  });
                },
                onPointSelection: (item) => {
                  const value = sqlValueToSqliteString(item.datum[columnAlias]);
                  state.activeViewSource?.filters.addFilter({
                    op: (cols) => `${cols[0]} = ${value}`,
                    columns: [columnAlias],
                  });
                },
              },
              {
                chartType: ChartType.HISTOGRAM,
                ...chartAttrs,
                onIntervalSelection: (value) => {
                  const range = `${value[columnAlias][0]} AND ${value[columnAlias][1]}`;
                  state.activeViewSource?.filters.addFilter({
                    op: (cols) => `${cols[0]} BETWEEN ${range}`,
                    columns: [columnAlias],
                  });
                },
                onPointSelection: (item) => {
                  const minValue = item.datum[`bin_maxbins_10_${columnAlias}`];
                  const maxValue =
                    item.datum[`bin_maxbins_10_${columnAlias}_end`];
                  state.activeViewSource?.filters.addFilter({
                    op: (cols) =>
                      `${cols[0]} BETWEEN ${minValue} AND ${maxValue}`,
                    columns: [columnAlias],
                  });
                },
              },
            ],
            addChart: (chart) => state.activeViewSource?.addChart(chart),
          });
        },
      }),
    );
  }

  private renderRemovableChart(chart: ChartAttrs, state: ExplorePageState) {
    return m(
      '.pf-chart-card',
      {
        key: `${chart.chartType}-${chart.columns[0]}`,
      },
      m(Button, {
        className: 'pf-chart-card__button',
        icon: Icons.Close,
        onclick: () => {
          state.activeViewSource?.removeChart(chart);
        },
      }),
      m('.pf-chart-card__chart', renderChart(chart)),
    );
  }

  view({attrs}: m.Vnode<DataVisualiserAttrs>) {
    const {state} = attrs;

    return m(
      SplitPanel,
      {
        visibility: this.visibility,
        onVisibilityChange: (visibility) => {
          this.visibility = visibility;
        },
        drawerContent: m(
          '.pf-chart-container',
          state.activeViewSource?.visViews !== undefined &&
            Array.from(state.activeViewSource?.visViews.charts.values()).map(
              (chart) => this.renderRemovableChart(chart, state),
            ),
        ),
      },
      m('.pf-chart-card', this.renderSqlTable(state)),
    );
  }
}
