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
  ColumnController,
  ColumnControllerDiff,
  ColumnControllerRow,
  columnControllerRowFromName,
  newColumnControllerRows,
} from '../column_controller';
import protos from '../../../../protos';
import {Section} from '../../../../widgets/section';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {Button} from '../../../../widgets/button';

export interface GroupByAggregationAttrs {
  column?: ColumnControllerRow;
  aggregationOp: string;
  newColumnName?: string;
}

export interface GroupByAttrs {
  prevNode: QueryNode;
  groupByColumns?: ColumnControllerRow[];
  aggregations?: GroupByAggregationAttrs[];
}

export class GroupByNode implements QueryNode {
  readonly type: NodeType = NodeType.kGroupByOperator;
  prevNode: QueryNode;
  nextNode?: QueryNode;
  readonly dataName = undefined;

  groupByColumns: ColumnControllerRow[];
  aggregations: GroupByAggregationAttrs[];

  constructor(attrs: GroupByAttrs) {
    this.prevNode = attrs.prevNode;
    this.aggregations = attrs.aggregations ?? [];
    this.groupByColumns = attrs.groupByColumns ?? [];
  }

  get columns(): ColumnControllerRow[] {
    const cols = this.groupByColumns.filter((c) => c.checked);
    for (const agg of this.aggregations) {
      if (!agg.column) continue;
      cols.push(
        columnControllerRowFromName(
          agg.newColumnName ?? placeholderNewColumnName(agg),
          true,
        ),
      );
    }
    return cols;
  }

  getTitle(): string {
    const cols = this.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.alias ?? c.id)
      .join(', ');
    return `Group by ${cols}`;
  }

  getDetails(): m.Child {
    const cols = this.groupByColumns
      .filter((c) => c.checked)
      .map((c) => `'${c.alias ?? c.id}'`)
      .join(', ');
    const gbColsStr = cols ? `Group by columns: ${cols}` : '';

    const aggDetails: string[] = this.aggregations.map((agg) => {
      const columnName = agg.column
        ? `'${agg.column.alias ?? agg.column.id}'`
        : '[No column selected]';
      return `\n- Created '${agg.newColumnName ?? placeholderNewColumnName(agg)}' by aggregating ${columnName} with ${agg.aggregationOp}.`;
    });

    return m(TextParagraph, {
      text: [gbColsStr, ...aggDetails].filter(Boolean).join(''),
    });
  }

  validate(): boolean {
    if (!this.groupByColumns.some((c) => c.checked)) {
      return false;
    }

    const newColumnNames = new Set<string>();
    for (const agg of this.aggregations) {
      const name = agg.newColumnName ?? placeholderNewColumnName(agg);
      if (newColumnNames.has(name)) {
        return false;
      }
      newColumnNames.add(name);
    }

    for (const agg of this.aggregations) {
      if (!agg.column) {
        return false;
      }
    }

    return true;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;

    const prevNodeSq = this.prevNode.getStructuredQuery();
    if (!prevNodeSq) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `group_by`;
    sq.innerQuery = prevNodeSq;

    const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
    groupByProto.columnNames = this.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    groupByProto.aggregates = this.aggregations
      .filter((agg) => agg.column)
      .map(GroupByAggregationAttrsToProto);

    sq.groupBy = groupByProto;

    const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] =
      [];
    for (const c of this.columns) {
      if (!c.checked) continue;
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
    if (!attrs.groupByColumns) {
      attrs.groupByColumns = newColumnControllerRows(
        attrs.prevNode.columns?.filter((c) => c.checked) ?? [],
      );
    }

    const selectGroupByColumns = (): m.Child => {
      return m(ColumnController, {
        options: attrs.groupByColumns!,
        allowAlias: false,
        onChange: (diffs: ColumnControllerDiff[]) => {
          for (const diff of diffs) {
            const column = attrs.groupByColumns!.find((c) => c.id === diff.id);
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
      agg: GroupByAggregationAttrs,
      index: number,
    ): m.Child => {
      const columnOptions = (attrs.prevNode.columns ?? [])
        .filter((c) => c.checked)
        .map((col) =>
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
                const selectedColumn = attrs.prevNode.columns?.find(
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
      if (!attrs.aggregations) {
        attrs.aggregations = [];
      }
      attrs.aggregations.push({
        aggregationOp: AGGREGATION_OPS[0],
        column: undefined,
        newColumnName: undefined,
      });
    };

    const selectAggregations = (): m.Child => {
      return m(
        '',
        (attrs.aggregations || []).map((agg, index) =>
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

function GroupByAggregationAttrsToProto(
  agg: GroupByAggregationAttrs,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();
  newAgg.columnName = agg.column!.column.name;
  newAgg.op = stringToAggregateOp(agg.aggregationOp);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);
  return newAgg;
}

function placeholderNewColumnName(agg: GroupByAggregationAttrs) {
  return agg.column
    ? `${agg.column.id}_${agg.aggregationOp}`
    : `agg_${agg.aggregationOp}`;
}
