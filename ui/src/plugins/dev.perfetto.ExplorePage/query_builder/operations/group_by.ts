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
import {ColumnInfo} from '../column_info';
import {MultiselectInput} from '../../../../widgets/multiselect_input';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import protos from '../../../../protos';

export interface GroupByAgg {
  column?: ColumnInfo;
  aggregationOp?: string;
  newColumnName?: string;
  isValid?: boolean;
}

export interface GroupByAttrs {
  groupByColumns: ColumnInfo[];
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

    if (
      attrs.aggregations.length === 0 ||
      (attrs.aggregations[attrs.aggregations.length - 1].column !== undefined &&
        attrs.aggregations[attrs.aggregations.length - 1].aggregationOp !==
          undefined)
    ) {
      attrs.aggregations.push({});
    }

    const selectGroupByColumns = (): m.Child => {
      return m(
        'div',
        {style: {display: 'flex', alignItems: 'center', gap: '10px'}},
        m('label', 'Group by:'),
        m(MultiselectInput, {
          options: attrs.groupByColumns.map((col) => ({
            key: col.name,
            label: col.name,
          })),
          selectedOptions: attrs.groupByColumns
            .filter((c) => c.checked)
            .map((c) => c.name),
          onOptionAdd: (key: string) => {
            const column = attrs.groupByColumns.find((c) => c.name === key);
            if (column) {
              column.checked = true;
            }
          },
          onOptionRemove: (key: string) => {
            const column = attrs.groupByColumns.find((c) => c.name === key);
            if (column) {
              column.checked = false;
            }
          },
        }),
      );
    };

    const selectAggregationForColumn = (
      agg: GroupByAgg,
      index: number,
    ): m.Child => {
      const columnOptions = attrs.groupByColumns.map((col) =>
        m(
          'option',
          {
            value: col.name,
            selected: agg.column?.name === col.name,
          },
          col.name,
        ),
      );

      return m(
        '',
        m(Icon, {
          icon: agg.isValid ? 'check_circle' : 'warning',
          style: {marginRight: '5px'},
        }),
        m(
          Select,
          {
            title: 'Aggregation: ',
            onchange: (e: Event) => {
              agg.aggregationOp = (e.target as HTMLSelectElement).value;
            },
          },
          m(
            'option',
            {disabled: true, selected: !agg.aggregationOp},
            'Operation',
          ),
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
        m(
          'Column:',
          m(
            Select,
            {
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const selectedColumn = attrs.groupByColumns.find(
                  (c) => c.name === target.value,
                );
                agg.column = selectedColumn;
              },
            },
            m('option', {disabled: true, selected: !agg.column}, 'Column'),
            columnOptions,
          ),
        ),
        ' AS ',
        m(TextInput, {
          title: 'New column name',
          placeholder: agg.column
            ? placeholderNewColumnName(agg)
            : 'Column name',
          onchange: (e: Event) => {
            agg.newColumnName = (e.target as HTMLInputElement).value.trim();
          },
          value: agg.newColumnName,
        }),
        m(Button, {
          label: 'X',
          onclick: () => {
            attrs.aggregations?.splice(index, 1);
          },
        }),
      );
    };

    const selectAggregations = (): m.Child => {
      return m(
        '',
        attrs.aggregations.map((agg, index) =>
          selectAggregationForColumn(agg, index),
        ),
      );
    };

    return m('', selectGroupByColumns(), selectAggregations());
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
  newAgg.op = stringToAggregateOp(agg.aggregationOp!);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);
  return newAgg;
}

export function placeholderNewColumnName(agg: GroupByAgg) {
  return agg.column && agg.aggregationOp
    ? `${agg.column.name}_${agg.aggregationOp}`
    : `agg_${agg.aggregationOp ?? ''}`;
}
