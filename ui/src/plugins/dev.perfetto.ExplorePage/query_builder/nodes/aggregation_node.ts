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
  notifyNextNodes,
  ModificationNode,
} from '../../query_node';
import protos from '../../../../protos';
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import {MultiselectInput} from '../../../../widgets/multiselect_input';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import {Card} from '../../../../widgets/card';
import {NodeIssues} from '../node_issues';
import {Icons} from '../../../../base/semantic_icons';
import {
  StructuredQueryBuilder,
  AggregationSpec,
} from '../structured_query_builder';
import {isColumnValidForAggregation} from '../utils';

export interface AggregationSerializedState {
  groupByColumns: {name: string; checked: boolean}[];
  aggregations: {
    column?: ColumnInfo;
    aggregationOp?: string;
    newColumnName?: string;
    percentile?: number;
    isValid?: boolean;
    isEditing?: boolean;
  }[];
  comment?: string;
}

export interface AggregationNodeState extends QueryNodeState {
  prevNode: QueryNode;
  groupByColumns: ColumnInfo[];
  aggregations: Aggregation[];
}

export interface Aggregation {
  column?: ColumnInfo;
  aggregationOp?: string;
  newColumnName?: string;
  percentile?: number;
  isValid?: boolean;
  isEditing?: boolean;
}

export class AggregationNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kAggregation;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
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
      groupByColumns: state.groupByColumns ?? [],
      aggregations: state.aggregations ?? [],
    };
    this.prevNode = state.prevNode;
    this.nextNodes = [];
    if (this.state.groupByColumns.length === 0) {
      this.state.groupByColumns = newColumnInfoList(
        this.prevNode.finalCols ?? [],
        false,
      );
    }
    const userOnChange = this.state.onchange;
    this.state.onchange = () => {
      notifyNextNodes(this);
      userOnChange?.();
    };
  }

  onPrevNodesUpdated() {
    this.updateGroupByColumns();
  }

  updateGroupByColumns() {
    const newGroupByColumns = newColumnInfoList(
      this.prevNode.finalCols ?? [],
      false,
    );
    for (const oldCol of this.state.groupByColumns) {
      if (oldCol.checked) {
        const newCol = newGroupByColumns.find((c) => c.name === oldCol.name);
        if (newCol) {
          newCol.checked = true;
        } else {
          const missingCol = columnInfoFromName(oldCol.name);
          missingCol.checked = true;
          newGroupByColumns.push(missingCol);
        }
      }
    }
    this.state.groupByColumns = newGroupByColumns;
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.prevNode === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }
    if (!this.prevNode.validate()) {
      this.setValidationError('Previous node is invalid');
      return false;
    }
    const sourceColNames = new Set(
      (this.prevNode.finalCols ?? []).map((c) => c.name),
    );
    const missingCols: string[] = [];
    for (const col of this.state.groupByColumns) {
      if (col.checked && !sourceColNames.has(col.name)) {
        missingCols.push(col.name);
      }
    }

    if (missingCols.length > 0) {
      this.setValidationError(
        `Group by columns ['${missingCols.join(', ')}'] not found in input`,
      );
      return false;
    }

    if (!this.state.groupByColumns.find((c) => c.checked)) {
      this.setValidationError(
        'Aggregation node has no group by columns selected',
      );
      return false;
    }
    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Aggregation';
  }

  nodeDetails?(): m.Child | undefined {
    const details: m.Child[] = [];
    const groupByCols = this.state.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.name);
    if (groupByCols.length > 0) {
      details.push(m('div', `Group by: ${groupByCols.join(', ')}`));
    }

    const aggs = this.state.aggregations
      .filter((agg) => agg.isValid)
      .map((agg) => {
        let aggDisplay = '';
        if (agg.aggregationOp === 'COUNT_ALL') {
          aggDisplay = 'COUNT(*)';
        } else if (
          agg.aggregationOp === 'PERCENTILE' &&
          agg.percentile !== undefined
        ) {
          aggDisplay = `PERCENTILE(${agg.column?.name}, ${agg.percentile})`;
        } else {
          aggDisplay = `${agg.aggregationOp}(${agg.column?.name})`;
        }
        return `${aggDisplay} AS ${agg.newColumnName ?? placeholderNewColumnName(agg)}`;
      });

    // Show each aggregation on its own line
    aggs.forEach((agg) => {
      details.push(m('div', agg));
    });

    if (details.length === 0) {
      return m('div', `No aggregation`);
    }
    return m('.pf-aggregation-node-details', details);
  }

  nodeSpecificModify(): m.Child {
    return m(
      '.node-specific-modify',
      m(AggregationOperationComponent, {
        groupByColumns: this.state.groupByColumns,
        aggregations: this.state.aggregations,
        onchange: this.state.onchange,
      }),
    );
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m('p', m('strong', 'Aggregation')),
      m(
        'p',
        'A modification node that ',
        m('strong', 'groups rows'),
        ' by one or more columns and applies ',
        m('strong', 'aggregate functions'),
        ' to compute summary statistics.',
      ),
      m(
        'p',
        m('strong', 'Available functions:'),
        ' You can use aggregation functions like ',
        m('code', 'SUM'),
        ', ',
        m('code', 'COUNT'),
        ', ',
        m('code', 'MIN'),
        ', ',
        m('code', 'MAX'),
        ', ',
        m('code', 'AVG'),
        ', ',
        m('code', 'MEDIAN'),
        ', ',
        m('code', 'PERCENTILE'),
        ', and ',
        m('code', 'DURATION_WEIGHTED_MEAN'),
        ' (useful for time-series data).',
      ),
      m(
        'p',
        m('strong', 'Query type:'),
        ' This node uses the ',
        m('code', 'GroupBy'),
        ' operation from PerfettoSQL structured queries.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Group slices by name and calculate the average duration for each.',
      ),
    );
  }

  clone(): QueryNode {
    const stateCopy: AggregationNodeState = {
      prevNode: this.state.prevNode,
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      onchange: this.state.onchange,
      issues: this.state.issues,
    };
    return new AggregationNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    // Prepare groupByColumns
    const groupByColumns = this.state.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    // Prepare aggregations
    const aggregations: AggregationSpec[] = [];
    for (const agg of this.state.aggregations) {
      agg.isValid = validateAggregation(agg);
      if (agg.isValid) {
        // Map COUNT_ALL to COUNT for the proto (COUNT_ALL is UI-only)
        const protoOp =
          agg.aggregationOp === 'COUNT_ALL' ? 'COUNT' : agg.aggregationOp!;

        const aggSpec: AggregationSpec = {
          columnName: agg.column?.column.name, // Optional for COUNT_ALL
          op: protoOp,
          resultColumnName: agg.newColumnName ?? placeholderNewColumnName(agg),
        };

        // Add percentile if specified
        if (agg.percentile !== undefined) {
          aggSpec.percentile = agg.percentile;
        }

        aggregations.push(aggSpec);
      }
    }

    // Apply group by with this.nodeId (builder handles wrapping if needed)
    const sq = StructuredQueryBuilder.withGroupBy(
      this.prevNode,
      groupByColumns,
      aggregations,
      this.nodeId,
    );
    if (!sq) return undefined;

    // For aggregation, we must always set select_columns to match GROUP BY + aggregates
    // Clear any previous select_columns and set to the correct aggregation output
    sq.selectColumns = [];

    // Add GROUP BY columns
    for (const colName of groupByColumns) {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnName = colName;
      sq.selectColumns.push(selectCol);
    }

    // Add aggregate result columns
    for (const agg of aggregations) {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnName = agg.resultColumnName!;
      sq.selectColumns.push(selectCol);
    }

    return sq;
  }

  resolveColumns() {
    const sourceCols = this.prevNode.finalCols ?? [];
    this.state.groupByColumns.forEach((c) => {
      const sourceCol = sourceCols.find((s) => s.name === c.name);
      if (sourceCol) {
        c.column = sourceCol.column;
      }
    });
    this.state.aggregations.forEach((a) => {
      if (a.column) {
        const sourceCol = sourceCols.find((s) => s.name === a.column?.name);
        if (sourceCol) {
          a.column = sourceCol;
        }
      }
    });
  }

  serializeState(): AggregationSerializedState {
    return {
      groupByColumns: this.state.groupByColumns.map((c) => ({
        name: c.name,
        checked: c.checked,
      })),
      aggregations: this.state.aggregations.map((a) => ({
        column: a.column,
        aggregationOp: a.aggregationOp,
        newColumnName: a.newColumnName,
        percentile: a.percentile,
        isValid: a.isValid,
        isEditing: a.isEditing,
      })),
      comment: this.state.comment,
    };
  }

  static deserializeState(
    state: AggregationSerializedState,
  ): AggregationNodeState {
    const groupByColumns = state.groupByColumns.map((c) => {
      const col = columnInfoFromName(c.name);
      col.checked = c.checked;
      return col;
    });
    const aggregations = state.aggregations.map((a) => {
      return {
        column: a.column,
        aggregationOp: a.aggregationOp,
        newColumnName: a.newColumnName,
        percentile: a.percentile,
        isValid: a.isValid,
        isEditing: a.isEditing,
      };
    });
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
      groupByColumns,
      aggregations,
    };
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
  if (!aggregation.aggregationOp) return false;

  // COUNT_ALL doesn't need a column
  if (aggregation.aggregationOp === 'COUNT_ALL') {
    return true;
  }

  // All other operations require a column
  if (!aggregation.column) return false;

  // Check column type compatibility using utility function
  if (
    !isColumnValidForAggregation(aggregation.column, aggregation.aggregationOp)
  ) {
    return false;
  }

  // PERCENTILE has additional validation requirements
  if (aggregation.aggregationOp === 'PERCENTILE') {
    if (
      aggregation.percentile === undefined ||
      aggregation.percentile < 0 ||
      aggregation.percentile > 100
    ) {
      return false;
    }
  }

  return true;
}

export function GroupByAggregationAttrsToProto(
  agg: Aggregation,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();

  // COUNT_ALL doesn't have a column; all other operations do
  if (agg.column) {
    newAgg.columnName = agg.column.column.name;
  }

  newAgg.op = stringToAggregateOp(agg.aggregationOp!);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);

  // PERCENTILE requires percentile value
  if (agg.aggregationOp === 'PERCENTILE' && agg.percentile !== undefined) {
    newAgg.percentile = agg.percentile;
  }

  return newAgg;
}

export function placeholderNewColumnName(agg: Aggregation) {
  if (!agg.aggregationOp) {
    return 'agg_result';
  }

  // COUNT_ALL doesn't have a column
  if (agg.aggregationOp === 'COUNT_ALL') {
    return 'count';
  }

  if (agg.column) {
    return `${agg.column.name}_${agg.aggregationOp.toLowerCase()}`;
  }

  return `agg_${agg.aggregationOp.toLowerCase()}`;
}

function stringToAggregateOp(
  s: string,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op {
  // COUNT_ALL maps to COUNT in the proto (without a column)
  if (s === 'COUNT_ALL') {
    return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT;
  }

  // Only check ops that exist in the proto (exclude COUNT_ALL)
  const validProtoOps: readonly string[] = AGGREGATION_OPS.filter(
    (op) => op !== 'COUNT_ALL',
  );
  if (validProtoOps.includes(s)) {
    return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op[
      s as keyof typeof protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
    ];
  }
  throw new Error(`Invalid AggregateOp '${s}'`);
}

const AGGREGATION_OPS = [
  'COUNT',
  'COUNT_ALL',
  'SUM',
  'MIN',
  'MAX',
  'MEAN',
  'MEDIAN',
  'DURATION_WEIGHTED_MEAN',
  'PERCENTILE',
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

    // Use the utility function to determine if a column is valid for the given operation
    const isColumnValidForOp = isColumnValidForAggregation;

    const aggregationEditor = (agg: Aggregation): m.Child => {
      const columnOptions = attrs.groupByColumns.map((col) => {
        const isValid = isColumnValidForOp(col, agg.aggregationOp);
        return m(
          'option',
          {
            value: col.name,
            selected: agg.column?.name === col.name,
            disabled: !isValid,
          },
          col.name,
        );
      });

      return m(
        '.pf-exp-aggregation-editor',
        m(
          Select,
          {
            onchange: (e: Event) => {
              agg.aggregationOp = (e.target as HTMLSelectElement).value;
              // Clear percentile when changing operation
              if (agg.aggregationOp !== 'PERCENTILE') {
                agg.percentile = undefined;
              }
              attrs.onchange?.();
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
        // Percentile value input (only for PERCENTILE operation, shown before column)
        agg.aggregationOp === 'PERCENTILE' &&
          m(TextInput, {
            placeholder: 'percentile (0-100)',
            type: 'number',
            min: 0,
            max: 100,
            oninput: (e: InputEvent) => {
              const value = parseFloat((e.target as HTMLInputElement).value);
              agg.percentile = isNaN(value) ? undefined : value;
              attrs.onchange?.();
            },
            value: agg.percentile?.toString() ?? '',
          }),
        // Column selector (not shown for COUNT_ALL)
        agg.aggregationOp !== 'COUNT_ALL' &&
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
          icon: Icons.Check,
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
      let aggDisplay = '';
      if (agg.aggregationOp === 'COUNT_ALL') {
        aggDisplay = 'COUNT(*)';
      } else if (
        agg.aggregationOp === 'PERCENTILE' &&
        agg.percentile !== undefined
      ) {
        aggDisplay = `PERCENTILE(${agg.column?.name}, ${agg.percentile})`;
      } else {
        aggDisplay = `${agg.aggregationOp}(${agg.column?.name})`;
      }

      return m(
        '.pf-exp-aggregation-viewer',
        m(
          'span',
          {
            onclick: () => {
              attrs.aggregations.forEach((a, i) => {
                a.isEditing = i === index;
              });
              m.redraw();
            },
          },
          `${aggDisplay} AS ${agg.newColumnName}`,
        ),
        m(Button, {
          icon: Icons.Close,
          onclick: (e: Event) => {
            e.stopPropagation();
            attrs.aggregations.splice(index, 1);
            attrs.onchange?.();
            m.redraw();
          },
        }),
      );
    };

    const aggregationsList = (): m.Children => {
      if (!hasGroupByColumns) {
        return null;
      }

      const lastAgg = attrs.aggregations[attrs.aggregations.length - 1];
      const showAddButton = lastAgg.isValid;

      return [
        ...attrs.aggregations.map((agg, index) => {
          if (agg.isEditing) {
            return aggregationEditor(agg);
          } else {
            return aggregationViewer(agg, index);
          }
        }),
        showAddButton &&
          m(Button, {
            label: 'Add more aggregations',
            onclick: () => {
              if (!lastAgg.newColumnName) {
                lastAgg.newColumnName = placeholderNewColumnName(lastAgg);
              }
              lastAgg.isEditing = false;
              attrs.aggregations.push({isEditing: true});
              attrs.onchange?.();
            },
          }),
      ];
    };

    return m(
      '.pf-exp-query-operations',
      m(Card, {}, [
        m(
          '.pf-exp-operations-container',
          selectGroupByColumns(),
          m('.pf-exp-aggregations-list', aggregationsList()),
        ),
      ]),
    );
  }
}
