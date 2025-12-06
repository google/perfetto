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
import {SqlValue} from '../trace_processor/query_result';
import {Box} from '../widgets/box';
import {Stack, StackAuto, StackFixed} from '../widgets/stack';
import {BarChartData, ColumnDef, Sorting} from './aggregation';
import {
  CellRenderer,
  ColumnDefinition,
  DataGridDataSource,
} from './widgets/data_grid/common';
import {DataGrid, renderCell, DataGridApi} from './widgets/data_grid/data_grid';
import {defaultValueFormatter} from './widgets/data_grid/export_utils';
import {AggregatePivotModel} from './aggregation_adapter';

export interface AggregationPanelAttrs {
  readonly dataSource: DataGridDataSource;
  readonly sorting: Sorting;
  readonly columns: ReadonlyArray<ColumnDef> | AggregatePivotModel;
  readonly barChartData?: ReadonlyArray<BarChartData>;
  readonly onReady?: (api: DataGridApi) => void;
}

function isColumnDefArray(
  columns: ReadonlyArray<ColumnDef> | AggregatePivotModel,
): columns is ReadonlyArray<ColumnDef> {
  return Array.isArray(columns);
}

export class AggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    const {dataSource, sorting, columns, barChartData, onReady} = attrs;

    return m(Stack, {fillHeight: true, spacing: 'none'}, [
      barChartData && m(StackFixed, m(Box, this.renderBarChart(barChartData))),
      m(StackAuto, this.renderTable(dataSource, sorting, columns, onReady)),
    ]);
  }

  private renderTable(
    dataSource: DataGridDataSource,
    sorting: Sorting,
    model: ReadonlyArray<ColumnDef> | AggregatePivotModel,
    onReady?: (api: DataGridApi) => void,
  ) {
    if (isColumnDefArray(model)) {
      return m(DataGrid, {
        fillHeight: true,
        columns: model.map((c): ColumnDefinition => {
          return {
            name: c.columnId,
            title: c.title,
            aggregation: c.sum ? 'SUM' : undefined,
            filterType: filterTypeForColumnDef(c.formatHint),
            cellRenderer: getCellRenderer(c.formatHint, c.columnId),
            valueFormatter: getValueFormatter(c.formatHint),
          };
        }),
        data: dataSource,
        initialSorting: sorting,
        onReady,
      });
    } else {
      return m(DataGrid, {
        fillHeight: true,
        columns: model.columns.map((c): ColumnDefinition => {
          return {
            name: c.columnId,
            title: c.title,
            filterType: filterTypeForColumnDef(c.formatHint),
            cellRenderer: getCellRenderer(c.formatHint, c.columnId),
            valueFormatter: getValueFormatter(c.formatHint),
          };
        }),
        initialPivot: {
          groupBy: model.groupBy,
          values: model.values,
        },
        data: dataSource,
        initialSorting: sorting,
        onReady,
      });
    }
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

function filterTypeForColumnDef(
  formatHint: string | undefined,
): 'numeric' | 'string' | undefined {
  switch (formatHint) {
    case undefined:
      return undefined;
    case 'NUMERIC':
    case 'DURATION_NS':
    case 'PERCENT':
      return 'numeric';
    case 'STRING':
    default:
      return 'string';
  }
}

function getValueFormatter(
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

function getCellRenderer(
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

function formatDurationValue(value: SqlValue): string {
  if (typeof value === 'bigint') {
    return Duration.humanise(value);
  } else if (typeof value === 'number') {
    return Duration.humanise(BigInt(Math.round(value)));
  } else {
    return String(value);
  }
}

function formatPercentValue(value: SqlValue): string {
  if (typeof value === 'number') {
    return `${(value * 100).toFixed(2)}%`;
  } else {
    return String(value);
  }
}
