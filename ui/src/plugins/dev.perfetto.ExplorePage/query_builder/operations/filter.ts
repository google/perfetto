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
import {NodeType, QueryNode} from '../../query_node';
import {
  ColumnControllerRow,
  newColumnControllerRows,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {Button} from '../../../../widgets/button';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Section} from '../../../../widgets/section';

export interface Filter {
  filterOp: string;
  columnName: ColumnControllerRow;
  stringsRhs: string[];
  doubleRhs: number[];
  intRhs: number[];
}

export interface FilterAttrs {
  prevNode: QueryNode;

  filters?: Filter[];
}

export class FilterNode implements QueryNode {
  type: NodeType = NodeType.kFilterOperator;
  prevNode: QueryNode;
  nextNode?: QueryNode;

  dataName = undefined;
  columns: ColumnControllerRow[];

  filters: Filter[];

  getTitle(): string {
    const cols = this.filters
      .map((f) => f.columnName)
      .map((c) => c.alias ?? c.id)
      .join(', ');
    return `Filter ${cols}`;
  }

  getDetails(): m.Child {
    const filterStrs: string[] = [];
    for (const f of this.filters) {
      filterStrs.push(
        `'${f.columnName.id}' ${f.filterOp} ${
          f.stringsRhs.join(', ') + f.doubleRhs.join(', ') + f.intRhs.join(', ')
        }`,
      );
    }

    return m(TextParagraph, {
      text: filterStrs.join('\nOR '),
    });
  }

  constructor(attrs: FilterAttrs) {
    this.prevNode = attrs.prevNode;
    this.filters = attrs.filters ?? [];

    // Columns consists of all columns from previous node.
    this.columns = newColumnControllerRows(
      this.prevNode.columns?.filter((c) => c.checked) ?? [],
      true,
    );
  }

  validate(): boolean {
    return true;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;
    const prevNodeSq = this.prevNode.getStructuredQuery();
    if (prevNodeSq === undefined) {
      return;
    }

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `filter`;
    sq.innerQuery = prevNodeSq;
    sq.filters = this.filters.map((f) => FilterToProto(f));

    const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] =
      [];
    for (const c of this.columns) {
      if (c.checked === false) continue;
      const newC = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      newC.columnName = c.column.name;
      if (c.alias) {
        newC.alias = c.alias;
      }
      selectedColumns.push(newC);
    }
    sq.selectColumns = selectedColumns;
    return sq;
  }
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
      const firstCheckedColumn = attrs.prevNode.columns?.find((c) => c.checked);
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
      const columnOptions = (attrs.prevNode.columns ?? [])
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
                const selectedColumn = attrs.prevNode.columns?.find(
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

function FilterToProto(
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
