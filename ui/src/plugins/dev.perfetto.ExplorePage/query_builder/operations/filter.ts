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
import {ColumnControllerRow} from '../column_controller';
import {Button} from '../../../../widgets/button';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Section} from '../../../../widgets/section';
import protos from '../../../../protos';

export interface Filter {
  filterOp: string;
  columnName: ColumnControllerRow;
  stringsRhs: string[];
  doubleRhs: number[];
  intRhs: number[];
}

export interface FilterAttrs {
  sourceCols: ColumnControllerRow[];
  filters: Filter[];
}

export class FilterOperation implements m.ClassComponent<FilterAttrs> {
  private defaultOp: string = 'EQUAL';
  private availableOperators = [
    'EQUAL',
    'NOT_EQUAL',
    'LESS_THAN',
    'LESS_THAN_EQUAL',
    'GREATER_THAN',
    'GREATER_THAN_EQUAL',
    'IS_NULL',
    'IS_NOT_NULL',
    'GLOB',
  ];

  view({attrs}: m.CVnode<FilterAttrs>) {
    const onAddFilter = (): void => {
      if (attrs.filters === undefined) {
        attrs.filters = [];
      }
      const firstCheckedColumn = attrs.sourceCols?.find((c) => c.checked);
      if (!firstCheckedColumn) {
        return;
      }
      attrs.filters?.push({
        filterOp: this.defaultOp,
        columnName: firstCheckedColumn,
        stringsRhs: [],
        doubleRhs: [],
        intRhs: [],
      });
    };

    const onFilterRemoved = (index: number): void => {
      attrs.filters?.splice(index, 1);
    };

    const filterWidgets = attrs.filters?.map((filter, index): m.Children => {
      const columnOptions = (attrs.sourceCols ?? [])
        .filter((c) => c.checked)
        .map((col) => {
          return m(
            'option',
            {
              value: col.id,
              selected: col.id === filter.columnName.id,
            },
            col.id,
          );
        });

      const operatorOptions: m.Children = this.availableOperators.map((op) => {
        return m(
          'option',
          {
            value: op,
            selected: op === filter.filterOp,
          },
          op,
        );
      });

      return m(
        Section,
        {title: `Filter ${index}`},
        m(Button, {
          label: 'Remove filter',
          onclick: () => onFilterRemoved(index),
        }),
        m(
          '',
          ' Column: ',
          m(
            Select,
            {
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const selectedColumn = attrs.sourceCols?.find(
                  (c) => c.id === target.value,
                );
                if (selectedColumn) {
                  filter.columnName = selectedColumn;
                }
              },
            },
            columnOptions,
          ),
        ),
        m(
          '',
          ' Operator: ',
          m(
            Select,
            {
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                filter.filterOp = target.value;
              },
            },
            operatorOptions,
          ),
        ),
        m(TextInput, {
          placeholder: 'Enter values separated by commas',
          onchange: (e: Event) => {
            const target = e.target as HTMLInputElement;
            const values = target.value
              .split(',')
              .map((s) => s.trim())
              .filter((s) => s !== '');
            filter.stringsRhs = [];
            filter.doubleRhs = [];
            filter.intRhs = [];
            if (values.every((v) => !isNaN(Number(v)))) {
              if (values.every((v) => Number(v) === Math.floor(Number(v)))) {
                filter.intRhs = values.map(Number);
              } else {
                filter.doubleRhs = values.map(Number);
              }
            } else {
              filter.stringsRhs = values;
            }
          },
        }),
      );
    });

    return m(
      '',
      m(Button, {
        label: 'Add Filter',
        onclick: onAddFilter,
      }),
      filterWidgets,
    );
  }
}

function StringToFilterOp(s: string) {
  switch (s) {
    case 'EQUAL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.EQUAL;
    case 'NOT_EQUAL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.NOT_EQUAL;
    case 'GREATER_THAN':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN;
    case 'GREATER_THAN_EQUAL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator
        .GREATER_THAN_EQUAL;
    case 'LESS_THAN':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN;
    case 'LESS_THAN_EQUAL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.LESS_THAN_EQUAL;
    case 'IS_NULL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NULL;
    case 'IS_NOT_NULL':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.IS_NOT_NULL;
    case 'GLOB':
      return protos.PerfettoSqlStructuredQuery.Filter.Operator.GLOB;
    default:
      throw new Error(`Invalid filter operation '${s}'`);
  }
}

export function FilterToProto(
  filter: Filter,
): protos.PerfettoSqlStructuredQuery.Filter {
  const newFilter = new protos.PerfettoSqlStructuredQuery.Filter();
  newFilter.columnName = filter.columnName.id;
  newFilter.op = StringToFilterOp(filter.filterOp);
  newFilter.doubleRhs = filter.doubleRhs;
  newFilter.int64Rhs = filter.intRhs;
  newFilter.stringRhs = filter.stringsRhs;
  return newFilter;
}
