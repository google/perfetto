// Copyright (C) 2025 The Android Open Source Project
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
import {Duration} from '../../base/time';
import {BarChartData, ColumnDef} from '../../components/aggregation';
import {AggregationPanelAttrs} from '../../components/aggregation_panel';
import {
  ColumnDefinition,
  DataGridDataSource,
  Sorting,
} from '../../components/widgets/data_grid/common';
import {
  DataGrid,
  renderCell,
} from '../../components/widgets/data_grid/data_grid';
import {SqlValue} from '../../trace_processor/query_result';
import {Box} from '../../widgets/box';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';

export class WattsonAggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  private scaleNumericData: boolean = false;

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
    const columnsById = new Map(columns.map((c) => [c.columnId, c]));
    return m(DataGrid, {
      toolbarItemsLeft: m(
        Box,
        m(SegmentedButtons, {
          options: [{label: 'µW'}, {label: 'mW'}],
          selectedOption: this.scaleNumericData ? 0 : 1,
          onOptionSelected: (index) => {
            this.scaleNumericData = index === 0;
          },
          title: 'Select power units',
        }),
      ),
      fillHeight: true,
      showResetButton: false,
      columns: columns.map((c): ColumnDefinition => {
        const displayTitle = this.scaleNumericData
          ? c.title.replace('estimated mW', 'estimated µW')
          : c.title;
        return {
          name: c.columnId,
          title: displayTitle,
          aggregation: c.sum ? 'SUM' : undefined,
        };
      }),
      data: dataSource,
      initialSorting: sorting,
      cellRenderer: (value: SqlValue, columnName: string) => {
        const formatHint = columnsById.get(columnName)?.formatHint;
        return this.renderValue(value, columnName, formatHint);
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

  private renderValue(value: SqlValue, colName: string, formatHint?: string) {
    if (formatHint === 'DURATION_NS' && typeof value === 'bigint') {
      return m('span.pf-data-grid__cell--number', Duration.humanise(value));
    } else if (formatHint === 'PERCENT' && typeof value === 'number') {
      return m(
        'span.pf-data-grid__cell--number',
        `${(value * 100).toFixed(2)}%`,
      );
    } else {
      let v = value;
      if (
        this.scaleNumericData &&
        colName.includes('_mw') &&
        typeof value === 'number'
      ) {
        v = value * 1000;
      }
      return renderCell(v, colName);
    }
  }
}
