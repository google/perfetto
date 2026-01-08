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
import {BarChartData, ColumnDef} from './aggregation';
import {Column} from './widgets/datagrid/model';
import {DataGrid, renderCell, DataGridApi} from './widgets/datagrid/datagrid';
import {defaultValueFormatter} from './widgets/datagrid/export_utils';
import {
  AggregatePivotModel,
  AggregationPanelAttrs,
} from './aggregation_adapter';
import {
  CellRenderer,
  ColumnSchema,
  ColumnType,
  SchemaRegistry,
} from './widgets/datagrid/datagrid_schema';
import {DataSource} from './widgets/datagrid/data_source';

export class AggregationPanel
  implements m.ClassComponent<AggregationPanelAttrs>
{
  view({attrs}: m.CVnode<AggregationPanelAttrs>) {
    const {dataSource, columns, barChartData, onReady} = attrs;

    return m(Stack, {fillHeight: true, spacing: 'none'}, [
      barChartData && m(StackFixed, m(Box, this.renderBarChart(barChartData))),
      m(StackAuto, this.renderTable(dataSource, columns, onReady)),
    ]);
  }

  private renderTable(
    dataSource: DataSource,
    model: ReadonlyArray<ColumnDef> | AggregatePivotModel,
    onReady?: (api: DataGridApi) => void,
  ) {
    if ('groupBy' in model) {
      // Build schema from pivot model columns
      const columnSchema: ColumnSchema = {};
      for (const c of model.columns) {
        columnSchema[c.columnId] = {
          title: c.title,
          titleString: c.title,
          columnType: filterTypeForColumnDef(c.formatHint),
          cellRenderer:
            c.cellRenderer ?? getCellRenderer(c.formatHint, c.columnId),
          cellFormatter: getValueFormatter(c.formatHint),
          dependsOn: c.dependsOn,
        };
      }
      const schema: SchemaRegistry = {data: columnSchema};

      return m(DataGrid, {
        fillHeight: true,
        schema,
        rootSchema: 'data',
        initialPivot: {
          groupBy: model.groupBy,
          aggregates: model.aggregates,
        },
        data: dataSource,
        onReady,
      });
    } else {
      // Build schema from column definitions
      const columnSchema: ColumnSchema = {};
      for (const c of model) {
        columnSchema[c.columnId] = {
          title: c.title,
          titleString: c.title,
          columnType: filterTypeForColumnDef(c.formatHint),
          cellRenderer:
            c.cellRenderer ?? getCellRenderer(c.formatHint, c.columnId),
          cellFormatter: getValueFormatter(c.formatHint),
          dependsOn: c.dependsOn,
        };
      }
      const schema: SchemaRegistry = {data: columnSchema};
      const initialColumns: readonly Column[] = model.map((c) => ({
        field: c.columnId,
        aggregate: c.sum ? 'SUM' : undefined,
        sort: c.sort,
      }));

      return m(DataGrid, {
        fillHeight: true,
        schema,
        rootSchema: 'data',
        initialColumns,
        data: dataSource,
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
