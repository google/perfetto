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
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  createSelectColumnsProto,
} from '../../query_node';
import protos from '../../../../protos';
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import {createFiltersProto} from '../operations/operation_component';
import {MultiselectInput} from '../../../../widgets/multiselect_input';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import {NodeIssues} from '../node_issues';

export interface AggregationNodeState extends QueryNodeState {
  readonly prevNode: QueryNode;
  readonly sourceCols: ColumnInfo[];
  readonly groupByColumns: ColumnInfo[];
  readonly aggregations: Aggregation[];
}

export interface Aggregation {
  column?: ColumnInfo;
  aggregationOp?: string;
  newColumnName?: string;
  isValid?: boolean;
  isEditing?: boolean;
}

export class AggregationNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kAggregation;
  readonly prevNode?: QueryNode;
  nextNodes: QueryNode[];
  readonly sourceCols: ColumnInfo[];
  readonly state: AggregationNodeState;

  get finalCols(): ColumnInfo[] {
    const selected = this.state.groupByColumns.filter((c) => c.checked);
    for (const agg of this.state.aggregations) {
      selected.push(
        columnInfoFromName(agg.newColumnName ?? placeholderNewColumnName(agg)),
      );
    }
    return newColumnInfoList(selected, true);
  }

  constructor(state: AggregationNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      groupByColumns: newColumnInfoList(state.sourceCols, false),
    };
    this.prevNode = state.prevNode;
    this.sourceCols = state.sourceCols;
    this.nextNodes = [];
  }

  validate(): boolean {
    if (this.state.issues) {
      this.state.issues.queryError = undefined;
    }
    if (this.prevNode === undefined) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Aggregation node has no previous node',
      );
      return false;
    }
    if (!this.prevNode.validate()) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error('Previous node is invalid');
      return false;
    }
    return true;
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Aggregation';
  }

  nodeSpecificModify(): m.Child {
    return m(AggregationOperationComponent, {
      groupByColumns: this.state.groupByColumns,
      aggregations: this.state.aggregations,
      onchange: this.state.onchange,
    });
  }

  clone(): QueryNode {
    const stateCopy: AggregationNodeState = {
      prevNode: this.state.prevNode,
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      filters: [],
      customTitle: this.state.customTitle,
      onchange: this.state.onchange,
      issues: this.state.issues,
    };
    return new AggregationNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const prevSq = this.prevNode!.getStructuredQuery();
    if (!prevSq) return undefined;

    const groupByProto = createGroupByProto(
      this.state.groupByColumns,
      this.state.aggregations,
    );
    const filtersProto = createFiltersProto(
      this.state.filters,
      this.sourceCols,
    );

    // If the previous node already has an aggregation, we need to create a
    // subquery.
    if (prevSq.groupBy) {
      const sq = new protos.PerfettoSqlStructuredQuery();
      sq.id = this.nodeId;
      sq.innerQuery = prevSq;
      if (filtersProto) sq.filters = filtersProto;
      if (groupByProto) sq.groupBy = groupByProto;
      const selectedColumns = createSelectColumnsProto(this);
      if (selectedColumns) sq.selectColumns = selectedColumns;
      return sq;
    }

    // Otherwise, we can just add the aggregation and filters to the previous
    // query.
    if (filtersProto) {
      if (prevSq.filters) {
        prevSq.filters.push(...filtersProto);
      } else {
        prevSq.filters = filtersProto;
      }
    }
    if (groupByProto) {
      prevSq.groupBy = groupByProto;
    }
    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) {
      prevSq.selectColumns = selectedColumns;
    }
    return prevSq;
  }
}

export function createGroupByProto(
  groupByColumns: ColumnInfo[],
  aggregations: Aggregation[],
): protos.PerfettoSqlStructuredQuery.GroupBy | undefined {
  if (!groupByColumns.find((c) => c.checked)) return;

  const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
  groupByProto.columnNames = groupByColumns
    .filter((c) => c.checked)
    .map((c) => c.column.name);

  for (const agg of aggregations) {
    agg.isValid = validateAggregation(agg);
  }
  groupByProto.aggregates = aggregations
    .filter((agg) => agg.isValid)
    .map(GroupByAggregationAttrsToProto);
  return groupByProto;
}

function validateAggregation(aggregation: Aggregation): boolean {
  if (!aggregation.column || !aggregation.aggregationOp) return false;
  return true;
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

const AGGREGATION_OPS = [
  'COUNT',
  'SUM',
  'MIN',
  'MAX',
  'MEAN',
  'DURATION_WEIGHTED_MEAN',
] as const;

interface AggregationOperationComponentAttrs {
  groupByColumns: ColumnInfo[];
  aggregations: Aggregation[];
  onchange?: () => void;
}

class AggregationOperationComponent
  implements m.ClassComponent<AggregationOperationComponentAttrs>
{
  view({attrs}: m.CVnode<AggregationOperationComponentAttrs>) {
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

    return m('.pf-exp-query-operations', [
      m(
        '.pf-exp-section',
        m(
          '.pf-exp-operations-container',
          selectGroupByColumns(),
          m('.pf-exp-aggregations-list', aggregationsList()),
        ),
      ),
    ]);
  }
}
