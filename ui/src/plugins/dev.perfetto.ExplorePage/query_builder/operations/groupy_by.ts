// Copyright (C) 2025 The Android Open Source Project
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
  ColumnController,
  ColumnControllerDiff,
  ColumnControllerRow,
} from '../column_controller';
import {Section} from '../../../../widgets/section';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import protos from '../../../../protos';

export interface GroupByAgg {
  column?: ColumnControllerRow;
  aggregationOp: string;
  newColumnName?: string;
}

export interface GroupByAttrs {
  groupByColumns: ColumnControllerRow[];
  aggregations: GroupByAgg[];
}

const AGGREGATION_OPS = [
  'COUNT',
  'SUM',
  'MIN',
  'MAX',
  'MEAN',
  'DURATION_WEIGHTED_MEAN',
] as const;

export class GroupByOperation implements m.ClassComponent<GroupByAttrs> {
  view({attrs}: m.CVnode<GroupByAttrs>) {
    if (attrs.groupByColumns.length === 0) {
      return;
    }

    const selectGroupByColumns = (): m.Child => {
      return m(ColumnController, {
        options: attrs.groupByColumns,
        allowAlias: false,
        onChange: (diffs: ColumnControllerDiff[]) => {
          for (const diff of diffs) {
            const column = attrs.groupByColumns.find((c) => c.id === diff.id);
            if (column) {
              column.checked = diff.checked;
              if (!diff.checked) {
                attrs.aggregations = attrs.aggregations?.filter(
                  (agg) => agg.column?.id !== diff.id,
                );
              }
            }
          }
        },
      });
    };

    const selectAggregationForColumn = (
      agg: GroupByAgg,
      index: number,
    ): m.Child => {
      const columnOptions = attrs.groupByColumns.map((col) =>
        m(
          'option',
          {
            value: col.id,
            selected: agg.column?.id === col.id,
          },
          col.id,
        ),
      );

      return m(
        Section,
        {
          title: `Aggregation ${index + 1}`,
          key: index,
        },
        m(Button, {
          label: 'X',
          onclick: () => {
            attrs.aggregations?.splice(index, 1);
          },
        }),
        m(
          'Column:',
          m(
            Select,
            {
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const selectedColumn = attrs.groupByColumns.find(
                  (c) => c.id === target.value,
                );
                agg.column = selectedColumn;
              },
            },
            m(
              'option',
              {disabled: true, selected: !agg.column},
              'Select a column',
            ),
            columnOptions,
          ),
        ),
        m(
          Select,
          {
            title: 'Aggregation type: ',
            onchange: (e: Event) => {
              agg.aggregationOp = (e.target as HTMLSelectElement).value;
            },
          },
          AGGREGATION_OPS.map((op) =>
            m(
              'option',
              {
                value: op,
                selected: op === agg.aggregationOp,
              },
              op,
            ),
          ),
        ),
        m(TextInput, {
          title: 'New column name',
          placeholder: agg.column
            ? placeholderNewColumnName(agg)
            : 'Enter column name',
          onchange: (e: Event) => {
            agg.newColumnName = (e.target as HTMLInputElement).value.trim();
          },
          value: agg.newColumnName,
        }),
      );
    };

    const onAddAggregation = () => {
      attrs.aggregations.push({
        aggregationOp: AGGREGATION_OPS[0],
        column: undefined,
        newColumnName: undefined,
      });
    };

    const selectAggregations = (): m.Child => {
      return m(
        '',
        attrs.aggregations.map((agg, index) =>
          selectAggregationForColumn(agg, index),
        ),
        m(Button, {
          label: 'Add Aggregation',
          onclick: onAddAggregation,
        }),
      );
    };

    return m(
      '',
      m(Section, {title: 'Columns for group by'}, selectGroupByColumns()),
      m(Section, {title: 'Aggregations'}, selectAggregations()),
    );
  }
}

function stringToAggregateOp(
  s: string,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op {
  if (AGGREGATION_OPS.includes(s as (typeof AGGREGATION_OPS)[number])) {
    return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op[
      s as keyof typeof protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
    ];
  }
  throw new Error(`Invalid AggregateOp '${s}'`);
}

export function GroupByAggregationAttrsToProto(
  agg: GroupByAgg,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();
  newAgg.columnName = agg.column!.column.name;
  newAgg.op = stringToAggregateOp(agg.aggregationOp);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);
  return newAgg;
}

export function placeholderNewColumnName(agg: GroupByAgg) {
  return agg.column
    ? `${agg.column.id}_${agg.aggregationOp}`
    : `agg_${agg.aggregationOp}`;
}
