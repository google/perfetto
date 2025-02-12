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
import {NodeType, QueryNode} from '../../query_state';
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
import {assertExists} from '../../../../base/logging';
import {TextParagraph} from '../../../../widgets/text_paragraph';

export interface GroupByAggregationAttrs {
  column: ColumnControllerRow;
  aggregationOp?: string;
  newColumnName?: string;
}

export interface GroupByAttrs {
  prevNode: QueryNode;

  groupByColumns?: ColumnControllerRow[];
  aggregations?: GroupByAggregationAttrs[];
}

export class GroupByNode implements QueryNode {
  type: NodeType = NodeType.kGroupByOperator;
  prevNode: QueryNode;
  nextNode?: QueryNode;

  dataName = undefined;
  columns: ColumnControllerRow[];

  groupByColumns: ColumnControllerRow[];
  aggregations: GroupByAggregationAttrs[];

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
    const gbColsStr = cols && `Group by columns: ${cols}`;

    const aggDetails: string[] = [];
    for (const agg of this.aggregations) {
      aggDetails.push(
        `\n- Created '${agg.newColumnName}' by aggregating `,
        `'${agg.column.alias ?? agg.column.id}' with `,
        `${agg.aggregationOp?.toString()}.`,
      );
    }

    return m(TextParagraph, {
      text: gbColsStr + aggDetails.join(''),
    });
  }

  constructor(attrs: GroupByAttrs) {
    this.prevNode = attrs.prevNode;
    if (
      attrs.aggregations === undefined ||
      attrs.groupByColumns === undefined
    ) {
      throw new Error('GroupByNode: missing required attributes');
    }
    attrs.aggregations.forEach((agg) => validateAggregation(agg));

    this.aggregations = attrs.aggregations;
    this.groupByColumns = attrs.groupByColumns;

    // Columns consists of all columns used for group by and all new columns.
    this.columns = attrs.groupByColumns.filter((c) => c.checked);
    for (const agg of attrs.aggregations) {
      this.columns.push(
        columnControllerRowFromName(assertExists(agg.newColumnName), true),
      );
    }
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
    sq.id = `group_by`;
    sq.innerQuery = prevNodeSq;
    const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
    groupByProto.columnNames = this.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.column.name);
    groupByProto.aggregates = this.aggregations.map((agg) =>
      GroupByAggregationAttrsToProto(agg),
    );
    sq.groupBy = groupByProto;

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

export class GroupByOperation implements m.ClassComponent<GroupByAttrs> {
  private selectForAggregationColumns?: ColumnControllerRow[];
  private defaultOp: string = 'COUNT';

  view({attrs}: m.CVnode<GroupByAttrs>) {
    const selectGroupByColumns = (): m.Child => {
      if (attrs.groupByColumns === undefined) {
        attrs.groupByColumns = newColumnControllerRows(
          attrs.prevNode.columns?.filter((c) => c.checked) ?? [],
        );
      }
      if (this.selectForAggregationColumns === undefined) {
        this.selectForAggregationColumns = newColumnControllerRows(
          attrs.groupByColumns,
        );
      }

      return m(ColumnController, {
        options: attrs.groupByColumns,
        allowAlias: false,
        onChange: (diffs: ColumnControllerDiff[]) => {
          if (attrs.groupByColumns === undefined) return;

          if (this.selectForAggregationColumns === undefined) {
            this.selectForAggregationColumns = newColumnControllerRows(
              attrs.groupByColumns,
            );
          }
          for (const diff of diffs) {
            const column = attrs.groupByColumns?.find((c) => c.id === diff.id);
            if (column) column.checked = diff.checked;
          }
        },
      });
    };

    const selectAggregationColumns = (): m.Child => {
      if (this.selectForAggregationColumns === undefined) return;

      return m(ColumnController, {
        options: this.selectForAggregationColumns,
        allowAlias: false,
        onChange: (diffs: ColumnControllerDiff[]) => {
          if (this.selectForAggregationColumns === undefined) {
            return;
          }
          for (const diff of diffs) {
            const column = this.selectForAggregationColumns.find(
              (c) => c.id === diff.id,
            );
            if (column) column.checked = diff.checked;
          }
        },
      });
    };

    const selectAggregationForColumn = (col: ColumnControllerRow): m.Child => {
      if (attrs.aggregations === undefined) {
        attrs.aggregations = [];
      }
      let agg = attrs.aggregations.find((agg) => agg.column.id === col.id);
      if (agg === undefined) {
        agg = {
          column: col,
          aggregationOp: this.defaultOp,
        };
        attrs.aggregations.push(agg);
      }
      // TODO(mayzner):
      // Add `median` operation after we start to suport it in the backend.
      const optionNames = [
        'Count',
        'Sum',
        'Min',
        'Max',
        'Mean',
        'Duration weighted mean',
      ];
      return m(
        Section,
        {title: `Column: ${col.id}`},
        m(
          ``,
          m(
            Select,
            {
              title: 'Aggregation type: ',
              id: `col.${col.id}`,
              oninput: (e: Event) => {
                if (!e.target || agg === undefined) return;
                agg.aggregationOp = (e.target as HTMLSelectElement).value;
              },
            },
            optionNames.map((name) =>
              m('option', {
                value: name.toUpperCase().replace(' ', '_'),
                label: name,
                selected: name === this.defaultOp,
              }),
            ),
          ),
          m(
            '',
            m(TextInput, {
              title: 'New column name',
              id: `newColName.${col.id}`,
              oninput: (e: KeyboardEvent) => {
                if (!e.target || agg === undefined) return;
                agg.newColumnName = (e.target as HTMLInputElement).value.trim();
              },
            }),
          ),
        ),
      );
    };

    const selectAggregations = (): m.Child => {
      if (this.selectForAggregationColumns === undefined) return;
      return m(
        '',
        this.selectForAggregationColumns
          .filter((c) => c.checked)
          .map((c) => selectAggregationForColumn(c)),
      );
    };

    return m(
      '',
      m(
        '.explore-page__rowish',
        m(Section, {title: 'Columns for group by'}, selectGroupByColumns()),
        m(
          Section,
          {title: 'Columns for aggregation'},
          selectAggregationColumns(),
        ),
      ),
      m(
        Section,
        {title: 'Select aggregation type and new column name'},
        selectAggregations(),
      ),
    );
  }
}

function StringToAggregateOp(s: string) {
  switch (s) {
    case 'COUNT':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT;
    case 'SUM':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.SUM;
    case 'MIN':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MIN;
    case 'MAX':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MAX;
    case 'MEAN':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEAN;
    case 'MEDIAN':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.MEDIAN;
    case 'DURATION_WEIGHTED_MEAN':
      return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
        .DURATION_WEIGHTED_MEAN;
    default:
      throw new Error(`Invalid AggregateOp '${s}'`);
  }
}

function GroupByAggregationAttrsToProto(
  agg: GroupByAggregationAttrs,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  validateAggregation(agg);

  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();
  newAgg.columnName = agg.column.column.name;
  newAgg.op = StringToAggregateOp(assertExists(agg.aggregationOp));
  newAgg.resultColumnName = assertExists(agg.newColumnName);
  return newAgg;
}

function validateAggregation(agg: GroupByAggregationAttrs): void {
  if (agg.aggregationOp === undefined) {
    throw new Error(
      `GroupByAggregationAttrs: missing aggregation operation on column ${agg.column.id}`,
    );
  }
  if (agg.newColumnName === undefined) {
    throw new Error(
      `GroupByAggregationAttrs: missing aggregation new column name on column ${agg.column.id}`,
    );
  }

  return;
}
