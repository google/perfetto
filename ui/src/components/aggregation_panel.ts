// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {Duration} from '../base/time';
import type {Trace} from '../public/trace';
import type {SqlValue} from '../trace_processor/query_result';
import {AddDebugTrackMenu} from './tracks/add_debug_track_menu';
import {Box} from '../widgets/box';
import {Button} from '../widgets/button';
import {Popup, PopupPosition} from '../widgets/popup';
import {Stack, StackAuto, StackFixed} from '../widgets/stack';
import type {BarChartData} from './aggregation';
import {
  DataGrid,
  renderCell,
  type DataGridApi,
} from './widgets/datagrid/datagrid';
import {defaultValueFormatter} from './widgets/datagrid/export_utils';
import type {AggregatorGridConfig, DataGridState} from './aggregation_adapter';
import {getDefaultVisibleColumns} from './widgets/datagrid/datagrid_schema';
import type {
  CellRenderer,
  ColumnType,
} from './widgets/datagrid/datagrid_schema';
import type {
  DataSource,
  DataSourceModel,
  FlatModel,
} from './widgets/datagrid/data_source';
import {Icons} from '../base/semantic_icons';

export interface AggregationPanelAttrs {
  readonly dataSource: DataSource;
  readonly gridConfig: AggregatorGridConfig;
  readonly barChartData?: ReadonlyArray<BarChartData>;
  readonly onReady?: (api: DataGridApi) => void;
  readonly dataGridState?: DataGridState;
  readonly onClearGridState?: () => void;
  readonly controls?: m.Children;
  readonly trace?: Trace;
  readonly query?: string;
  /**
   * Called when the grid is ready, providing the data source and a function
   * to retrieve the current model (filters, sort, pagination, pivot state).
   * Use this to build a query that reflects the current filtered result set,
   * e.g. for creating a debug track from filtered data.
   */
  readonly onDataGridReady?: (
    dataSource: DataSource,
    getModel: () => DataSourceModel,
  ) => void;
}

export class AggregationPanel implements m.ClassComponent<AggregationPanelAttrs> {
  // Stores the getModel function from DataGridApi, set when the grid is ready.
  private getModel?: () => DataSourceModel;

  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    const {
      dataSource,
      gridConfig,
      barChartData,
      onReady,
      dataGridState,
      onClearGridState,
      controls,
      trace,
      query,
      onDataGridReady,
    } = attrs;

    return m(Stack, {fillHeight: true, spacing: 'none'}, [
      barChartData && m(StackFixed, m(Box, this.renderBarChart(barChartData))),
      m(
        StackAuto,
        this.renderTable(
          controls,
          dataSource,
          gridConfig,
          onReady,
          dataGridState,
          onClearGridState,
          trace,
          query,
          onDataGridReady,
        ),
      ),
    ]);
  }

  private renderTable(
    controls: m.Children | undefined,
    dataSource: DataSource,
    gridConfig: AggregatorGridConfig,
    onReady?: (api: DataGridApi) => void,
    dataGridState?: DataGridState,
    onClearGridState?: () => void,
    trace?: Trace,
    query?: string,
    onDataGridReady?: (
      dataSource: DataSource,
      getModel: () => DataSourceModel,
    ) => void,
  ) {
    // Use the filtered query if available, otherwise fall back to the static query.
    const filteredQuery = query;

    const gridModel = this.getModel?.();

    const debugModel: FlatModel = {
      mode: 'flat',
      columns: gridModel.map((field) => ({
        field,
        alias: field,
      })),
      filters: gridModel?.filters,
      pagination: undefined,
      sort: undefined,
    };

    const debugTrackButton =
      trace && filteredQuery
        ? m(
            Popup,
            {
              trigger: m(Button, {
                label: 'Add debug track',
                icon: 'add_chart',
              }),
              position: PopupPosition.Top,
            },
            m(AddDebugTrackMenu, {
              trace,
              query: filteredQuery,
              availableColumns: getDefaultVisibleColumns(gridConfig.schema),
              onAdd: () => trace.navigate('#!/viewer'),
            }),
          )
        : undefined;

    return m(DataGrid, {
      fillHeight: true,
      schema: gridConfig.schema,
      data: dataSource,
      onReady: (api: DataGridApi) => {
        onReady?.(api);
        this.getModel = api.getModel;
        onDataGridReady?.(dataSource, api.getModel);
      },
      // Spread controlled state props (columns, filters, pivot and callbacks)
      ...dataGridState,
      toolbarItemsLeft: [
        controls,
        onClearGridState &&
          m(Button, {
            icon: Icons.ResetState,
            tooltip: 'Reset grid state to default for this aggregation',
            onclick: () => onClearGridState(),
          }),
      ],
      toolbarItemsRight: [debugTrackButton],
    });
  }

  private renderBarChart(data: ReadonlyArray<BarChartData>) {
    const summedValues = data.reduce((sum, item) => sum + item.value, 0);
    return m(
      '.pf-aggregation-panel__bar-chart',
      data.map((d) => {
        const width = (d.value / summedValues) * 100;
        return m(
          '.pf-aggregation-panel__bar-chart-bar',
          {
            style: {
              background: d.color.base.cssString,
              color: d.color.textBase.cssString,
              borderColor: d.color.variant.cssString,
              width: `${width}%`,
            },
          },
          d.title,
        );
      }),
    );
  }
}

export function filterTypeForFormatHint(
  formatHint: string | undefined,
): ColumnType | undefined {
  switch (formatHint) {
    case undefined:
      return undefined;
    case 'ID':
      return 'identifier';
    case 'NUMERIC':
    case 'DURATION_NS':
    case 'PERCENT':
      return 'quantitative';
    case 'STRING':
    default:
      return 'text';
  }
}

export function getValueFormatter(
  formatHint: string | undefined,
): (value: SqlValue) => string {
  switch (formatHint) {
    case 'DURATION_NS':
      return formatDurationValue;
    case 'PERCENT':
      return formatPercentValue;
    default:
      return defaultValueFormatter;
  }
}

export function getCellRenderer(
  formatHint: string | undefined,
  columnName: string,
): CellRenderer {
  switch (formatHint) {
    case 'DURATION_NS':
      return formatDurationValue;
    case 'PERCENT':
      return formatPercentValue;
    default:
      return function (value) {
        return renderCell(value, columnName);
      };
  }
}

export function formatDurationValue(value: SqlValue): string {
  if (typeof value === 'bigint') {
    return Duration.humanise(value);
  } else if (typeof value === 'number') {
    return Duration.humanise(BigInt(Math.round(value)));
  } else {
    return String(value);
  }
}

export function formatPercentValue(value: SqlValue): string {
  if (typeof value === 'number') {
    return `${(value * 100).toFixed(2)}%`;
  } else {
    return String(value);
  }
}
