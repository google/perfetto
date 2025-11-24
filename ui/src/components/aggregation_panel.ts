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
import {ColumnDefinition, DataGridDataSource} from './widgets/data_grid/common';
import {DataGrid, renderCell, DataGridApi} from './widgets/data_grid/data_grid';
import {defaultValueFormatter} from './widgets/data_grid/export_utils';

export interface AggregationPanelAttrs {
  readonly dataSource: DataGridDataSource;
  readonly sorting: Sorting;
  readonly columns: ReadonlyArray<ColumnDef>;
  readonly barChartData?: ReadonlyArray<BarChartData>;
  readonly onReady?: (api: DataGridApi) => void;
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
    columns: ReadonlyArray<ColumnDef>,
    onReady?: (api: DataGridApi) => void,
  ) {
    const columnsById = new Map(columns.map((c) => [c.columnId, c]));

    return m(DataGrid, {
      fillHeight: true,
      showResetButton: false,
      columns: columns.map((c): ColumnDefinition => {
        return {
          name: c.columnId,
          title: c.title,
          aggregation: c.sum ? 'SUM' : undefined,
        };
      }),
      data: dataSource,
      initialSorting: sorting,
      onReady,
      cellRenderer: (value: SqlValue, columnName: string) => {
        const formatHint = columnsById.get(columnName)?.formatHint;
        return this.renderCell(value, columnName, formatHint);
      },
      valueFormatter: (value: SqlValue, columnName: string) => {
        const formatHint = columnsById.get(columnName)?.formatHint;
        return valueFormatter(value, formatHint);
      },
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

  private renderCell(value: SqlValue, colName: string, formatHint?: string) {
    if (formatHint === 'DURATION_NS' && typeof value === 'bigint') {
      return Duration.humanise(value);
    } else if (formatHint === 'PERCENT' && typeof value === 'number') {
      return `${(value * 100).toFixed(2)}%`;
    } else {
      return renderCell(value, colName);
    }
  }
}

function valueFormatter(value: SqlValue, formatHint?: string): string {
  if (formatHint === 'DURATION_NS' && typeof value === 'bigint') {
    return Duration.humanise(value);
  } else if (formatHint === 'PERCENT' && typeof value === 'number') {
    return `${(value * 100).toFixed(2)}%`;
  } else {
    return defaultValueFormatter(value);
  }
}
