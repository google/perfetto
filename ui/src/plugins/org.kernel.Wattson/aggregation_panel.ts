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
import {Column} from '../../components/widgets/datagrid/model';
import {DataGrid, renderCell} from '../../components/widgets/datagrid/datagrid';
import {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {Box} from '../../widgets/box';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Stack, StackAuto, StackFixed} from '../../widgets/stack';
import {
  AggregatePivotModel,
  AggregationPanelAttrs,
} from '../../components/aggregation_adapter';
import {DataSource} from '../../components/widgets/datagrid/data_source';

export class WattsonAggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  private scaleNumericData: boolean = false;

  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    const {dataSource, columns, barChartData} = attrs;

    return m(Stack, {fillHeight: true}, [
      barChartData && m(StackFixed, m(Box, this.renderBarChart(barChartData))),
      m(StackAuto, this.renderTable(dataSource, columns)),
    ]);
  }

  private renderTable(
    dataSource: DataSource,
    model: ReadonlyArray<ColumnDef> | AggregatePivotModel,
  ) {
    // TODO: Support pivot tables
    if ('groupBy' in model) {
      return undefined;
    }

    const initialColumns: readonly Column[] = model.map((c) => ({
      field: c.columnId,
      aggregate: c.sum ? 'SUM' : undefined,
      sort: c.sort,
    }));

    // Build schema directly
    const columnSchema: ColumnSchema = {};
    for (const c of model) {
      const displayTitle = this.scaleNumericData
        ? c.title.replace('estimated mW', 'estimated µW')
        : c.title;
      columnSchema[c.columnId] = {
        title: displayTitle,
        titleString: displayTitle,
        columnType: filterTypeForColumnDef(c.formatHint),
        cellRenderer: (value) => {
          const formatHint = c.formatHint;
          if (formatHint === 'DURATION_NS' && typeof value === 'bigint') {
            return m(
              'span.pf-data-grid__cell--number',
              Duration.humanise(value),
            );
          } else if (formatHint === 'PERCENT' && typeof value === 'number') {
            return m(
              'span.pf-data-grid__cell--number',
              `${(value * 100).toFixed(2)}%`,
            );
          } else {
            let v = value;
            if (
              this.scaleNumericData &&
              c.columnId.includes('_mw') &&
              typeof value === 'number'
            ) {
              v = value * 1000;
            }
            return renderCell(v, c.columnId);
          }
        },
      };
    }
    const schema: SchemaRegistry = {data: columnSchema};

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
      initialColumns,
      fillHeight: true,
      schema,
      rootSchema: 'data',
      data: dataSource,
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

function filterTypeForColumnDef(
  formatHint: string | undefined,
): 'quantitative' | 'text' | undefined {
  switch (formatHint) {
    case 'UNDEFINED':
      return undefined;
    case 'NUMERIC':
    case 'DURATION_NS':
      return 'quantitative';
    case 'STRING':
    default:
      return 'text';
  }
}
