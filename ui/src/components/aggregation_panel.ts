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
import {SqlValue} from '../trace_processor/query_result';
import {Box} from '../widgets/box';
import {Stack, StackAuto, StackFixed} from '../widgets/stack';
import {ColumnDefinition, DataGridDataSource} from './widgets/data_grid/common';
import {DataGrid, renderCell} from './widgets/data_grid/data_grid';
import {BarChartData, ColumnDef, Sorting} from './aggregation';

export interface AggregationPanelAttrs {
  readonly dataSource: DataGridDataSource;
  readonly sorting: Sorting;
  readonly columns: ReadonlyArray<ColumnDef>;
  readonly barChartData?: ReadonlyArray<BarChartData>;
}

export class AggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    const {dataSource, sorting, columns, barChartData} = attrs;

    return m(Stack, {fillHeight: true}, [
      barChartData && m(StackFixed, m(Box, this.renderBarChart(barChartData))),
      m(StackAuto, this.renderTable(dataSource, sorting, columns)),
    ]);
  }

  private renderTable(
    dataSource: DataGridDataSource,
    sorting: Sorting,
    columns: ReadonlyArray<ColumnDef>,
  ) {
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
      cellRenderer: (value: SqlValue, columnName: string) => {
        const kind = columns.find((c) => c.columnId === columnName)?.kind ?? '';
        return colKindToRenderer(kind, value, columnName);
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
}

function colKindToRenderer(kind: string, value: SqlValue, colName: string) {
  if (kind === 'TIMESTAMP_NS' && typeof value === 'bigint') {
    return m(
      'span.pf-data-grid__cell--number',
      (Number(value) / 1_000_000).toFixed(3),
    );
  } else {
    return renderCell(value, colName);
  }
}
