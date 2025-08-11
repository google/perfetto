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
import protos from '../../../../protos';

export interface Aggregation {
  column?: ColumnInfo;
  aggregationOp?: string;
  newColumnName?: string;
  isValid?: boolean;
  isEditing?: boolean;
}

export interface AggregationsOperatorAttrs {
  groupByColumns: ColumnInfo[];
  aggregations: Aggregation[];
  onchange?: () => void;
}

const AGGREGATION_OPS = [
  'COUNT',
  'SUM',
  'MIN',
  'MAX',
  'MEAN',
  'DURATION_WEIGHTED_MEAN',
] as const;

export class AggregationsOperator
  implements m.ClassComponent<AggregationsOperatorAttrs>
{
  view({attrs}: m.CVnode<AggregationsOperatorAttrs>) {
    if (attrs.groupByColumns.length === 0) {
      return;
    }

    const hasGroupByColumns = attrs.groupByColumns.some((c) => c.checked);

    if (hasGroupByColumns && attrs.aggregations.length === 0) {
      attrs.aggregations.push({isEditing: true});
    }

    if (!hasGroupByColumns && attrs.aggregations.length > 0) {
      // Clear aggregations if no group by columns are selected
      attrs.aggregations.length = 0;
    }

    const selectGroupByColumns = (): m.Child => {
      return m(
        '.pf-exp-multi-select-container',
        m('label', 'GROUP BY columns'),
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
              attrs.onchange?.();
              m.redraw();
            }
          },
          onOptionRemove: (key: string) => {
            const column = attrs.groupByColumns.find((c) => c.name === key);
            if (column) {
              column.checked = false;
              attrs.onchange?.();
              m.redraw();
            }
          },
        }),
      );
    };

    const aggregationEditor = (agg: Aggregation, index: number): m.Child => {
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
        '.pf-exp-aggregation-editor',
        m(
          Select,
          {
            onchange: (e: Event) => {
              agg.aggregationOp = (e.target as HTMLSelectElement).value;
              m.redraw();
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
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              agg.column = attrs.groupByColumns.find(
                (c) => c.name === target.value,
              );
              attrs.onchange?.();
              m.redraw();
            },
          },
          m('option', {disabled: true, selected: !agg.column}, 'Column'),
          columnOptions,
        ),
        'AS',
        m(TextInput, {
          placeholder: placeholderNewColumnName(agg),
          oninput: (e: Event) => {
            agg.newColumnName = (e.target as HTMLInputElement).value.trim();
          },
          value: agg.newColumnName,
        }),
        m(Button, {
          className: 'delete-button',
          icon: 'delete',
          onclick: () => {
            attrs.aggregations.splice(index, 1);
            attrs.onchange?.();
          },
        }),
        m(Button, {
          label: 'Done',
          className: 'is-primary',
          disabled: !agg.isValid,
          onclick: () => {
            if (!agg.newColumnName) {
              agg.newColumnName = placeholderNewColumnName(agg);
            }
            agg.isEditing = false;
            attrs.onchange?.();
          },
        }),
      );
    };

    const aggregationViewer = (agg: Aggregation, index: number): m.Child => {
      return m(
        '.pf-exp-aggregation-viewer',
        {
          onclick: () => {
            attrs.aggregations.forEach((a, i) => {
              a.isEditing = i === index;
            });
            m.redraw();
          },
        },
        `${agg.aggregationOp}(${agg.column?.name}) AS ${agg.newColumnName}`,
      );
    };

    const aggregationsList = (): m.Children => {
      if (!hasGroupByColumns) {
        return null;
      }

      const lastAgg = attrs.aggregations[attrs.aggregations.length - 1];
      const showAddButton = lastAgg.isValid && !lastAgg.isEditing;

      return [
        ...attrs.aggregations.map((agg, index) => {
          if (agg.isEditing) {
            return aggregationEditor(agg, index);
          } else {
            return aggregationViewer(agg, index);
          }
        }),
        showAddButton &&
          m(Button, {
            label: 'Add more aggregations',
            onclick: () => {
              attrs.aggregations.push({isEditing: true});
              attrs.onchange?.();
            },
          }),
      ];
    };

    return m(
      '.pf-exp-section',
      m(
        '.pf-exp-operations-container',
        selectGroupByColumns(),
        m('.pf-exp-aggregations-list', aggregationsList()),
      ),
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
  agg: Aggregation,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();
  newAgg.columnName = agg.column!.column.name;
  newAgg.op = stringToAggregateOp(agg.aggregationOp!);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);
  return newAgg;
}

export function placeholderNewColumnName(agg: Aggregation) {
  return agg.column && agg.aggregationOp
    ? `${agg.column.name}_${agg.aggregationOp}`
    : `agg_${agg.aggregationOp ?? ''}`;
}
