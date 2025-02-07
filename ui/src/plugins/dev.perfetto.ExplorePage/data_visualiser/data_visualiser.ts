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
import {QueryNode} from '../query_state';
import {VisViewSource} from './view_source';
import {AddChartMenuItem} from '../../../components/widgets/charts/add_chart_menu';
import {exists} from '../../../base/utils';
import {DetailsShell} from '../../../widgets/details_shell';
import {SqlTable} from '../../../components/widgets/sql/legacy_table/table';
import {VisFilterOptions} from './filters';
import {SqlTableState} from '../../../components/widgets/sql/legacy_table/state';

export interface DataVisualiserState {
  queryNode?: QueryNode;
}

export interface DataVisualiserAttrs {
  trace: Trace;
  readonly state: DataVisualiserState;
}

export class DataVisualiser implements m.ClassComponent<DataVisualiserAttrs> {
  private visibility = SplitPanelDrawerVisibility.VISIBLE;
  private viewSource?: VisViewSource;

  constructor({attrs}: m.Vnode<DataVisualiserAttrs>) {
    const queryNode = attrs.state.queryNode;
    if (queryNode === undefined) return;

    this.viewSource = new VisViewSource(attrs.trace, queryNode);
  }

  private renderSqlTable(sqlTableViewState?: SqlTableState) {
    const viewSource = this.viewSource;

    if (viewSource === undefined || sqlTableViewState === undefined) return;

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
        addColumnMenuItems: (_, columnAlias) => {
          const chartAttrs = {
            data: sqlTableViewState.nonPaginatedData?.rows,
            columns: [columnAlias],
          };

          return m(AddChartMenuItem, {
            chartOptions: [
              {
                chartType: ChartType.BAR_CHART,
                ...chartAttrs,
                onIntervalSelection: (value) => {
                  this.viewSource?.addFilterFromChart(
                    VisFilterOptions['in'],
                    columnAlias,
                    value[columnAlias],
                  );
                },
                onPointSelection: (item) => {
                  this.viewSource?.addFilterFromChart(
                    VisFilterOptions['equals to'],
                    columnAlias,
                    item.datum[columnAlias],
                  );
                },
              },
              {
                chartType: ChartType.HISTOGRAM,
                ...chartAttrs,
                onIntervalSelection: (value) => {
                  this.viewSource?.addFilterFromChart(
                    VisFilterOptions['between'],
                    columnAlias,
                    value[columnAlias],
                  );
                },
                onPointSelection: (item) => {
                  this.viewSource?.addFilterFromChart(
                    VisFilterOptions['between'],
                    columnAlias,
                    [
                      item.datum[`bin_maxbins_10_${columnAlias}`],
                      item.datum[`bin_maxbins_10_${columnAlias}_end`],
                    ],
                  );
                },
              },
            ],
            addChart: (chart) => this.viewSource?.addChart(chart),
          });
        },
        extraAddFilterActions: (op, column, value) => {
          this.viewSource?.addFilter({
            filterOption: VisFilterOptions[op],
            columnName: column,
            value,
          });
        },
        extraRemoveFilterActions: (filterSqlStr) => {
          this.viewSource?.removeFilter(filterSqlStr);
        },
      }),
    );
  }

  private renderRemovableChart(chart: ChartAttrs) {
    return m(
      '.pf-chart-card',
      {
        key: `${chart.chartType}-${chart.columns[0]}`,
      },
      m(Button, {
        className: 'pf-chart-card__button',
        icon: Icons.Close,
        onclick: () => {
          this.viewSource?.removeChart(chart);
        },
      }),
      m('.pf-chart-card__chart', renderChart(chart)),
    );
  }

  view() {
    return m(
      SplitPanel,
      {
        visibility: this.visibility,
        onVisibilityChange: (visibility) => {
          this.visibility = visibility;
        },
        drawerContent:
          this.viewSource?.visViews &&
          Array.from(this.viewSource?.visViews.charts.values()).map((chart) => {
            return this.renderRemovableChart(chart);
          }),
      },
      m(
        '.pf-chart-card',
        this.renderSqlTable(this.viewSource?.visViews?.sqlTableState),
      ),
    );
  }
}
