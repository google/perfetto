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
import {
  PopupMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../../../../widgets/multiselect';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import {Card} from '../../../../widgets/card';
import {Form} from '../../../../widgets/form';
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
    // When there's no prevNode, aggregation doesn't make sense
    // Return empty array to indicate no output columns
    if (this.prevNode === undefined) {
      return [];
    }
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
    if (this.state.groupByColumns.length === 0 && this.prevNode !== undefined) {
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
    if (this.prevNode === undefined) {
      return;
    }
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

    // Must have at least one of: group by columns OR aggregation functions
    const hasGroupBy = this.state.groupByColumns.find((c) => c.checked);

    // Validate aggregations first
    for (const agg of this.state.aggregations) {
      agg.isValid = validateAggregation(agg);
    }
    const hasAggregations =
      this.state.aggregations.filter((a) => a.isValid).length > 0;

    if (!hasGroupBy && !hasAggregations) {
      this.setValidationError(
        'Aggregation node requires at least one group by column or aggregation function',
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
    const groupByOptions: MultiSelectOption[] = this.state.groupByColumns.map(
      (col) => ({
        id: col.name,
        name: col.name,
        checked: col.checked,
      }),
    );

    const selectedGroupBy = this.state.groupByColumns.filter((c) => c.checked);
    const label =
      selectedGroupBy.length > 0
        ? selectedGroupBy.map((c) => c.name).join(', ')
        : 'None';

    const details: m.Child[] = [
      m(
        '.pf-group-by-selector',
        {
          style: {
            display: 'flex',
            alignItems: 'center',
            gap: '8px',
            marginBottom: this.state.aggregations.length > 0 ? '8px' : '0',
          },
        },
        m('label', 'Group by:'),
        m(PopupMultiSelect, {
          label,
          options: groupByOptions,
          showNumSelected: false,
          compact: true,
          onChange: (diffs: MultiSelectDiff[]) => {
            for (const diff of diffs) {
              const column = this.state.groupByColumns.find(
                (c) => c.name === diff.id,
              );
              if (column) {
                column.checked = diff.checked;
              }
            }
            this.state.onchange?.();
          },
        }),
      ),
    ];

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
      m(
        'p',
        'Compute summary statistics like ',
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
        ', or ',
        m('code', 'PERCENTILE'),
        '. Optionally group rows by one or more columns.',
      ),
      m(
        'p',
        'Add aggregation functions to create new columns. Optionally select GROUP BY columns to group the results.',
      ),
      m(
        'p',
        m('strong', 'Example 1:'),
        ' Aggregate without grouping: ',
        m('code', 'COUNT(*)'),
        ' to count all rows, or ',
        m('code', 'AVG(dur)'),
        ' to get average duration across all slices.',
      ),
      m(
        'p',
        m('strong', 'Example 2:'),
        ' Group slices by ',
        m('code', 'name'),
        ' and compute ',
        m('code', 'AVG(dur)'),
        ' to find average duration per slice name.',
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

    // Defensive check: prevNode must exist for aggregation to work
    if (this.prevNode === undefined) return undefined;

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

    // Only use GROUP BY if we have group by columns
    // Otherwise, aggregations apply to the entire result set
    const sq =
      groupByColumns.length > 0
        ? StructuredQueryBuilder.withGroupBy(
            this.prevNode,
            groupByColumns,
            aggregations,
            this.nodeId,
          )
        : StructuredQueryBuilder.withGroupBy(
            this.prevNode,
            [], // Empty group by columns means aggregate entire result set
            aggregations,
            this.nodeId,
          );
    if (!sq) return undefined;

    // For aggregation, we must always set select_columns to match GROUP BY + aggregates
    // Clear any previous select_columns and set to the correct aggregation output
    sq.selectColumns = [];

    // Add GROUP BY columns (if any)
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
    if (this.prevNode === undefined) {
      return;
    }
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
  // Allow group by with empty column names (aggregates entire result set)
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

  // Only return undefined if we have no aggregates at all
  if (groupByProto.aggregates.length === 0) return undefined;

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
  // COUNT_ALL doesn't have a column
  if (agg.aggregationOp === 'COUNT_ALL') {
    return 'count';
  }

  if (agg.column && agg.aggregationOp) {
    return `${agg.column.name}_${agg.aggregationOp.toLowerCase()}`;
  }

  // Fallback for incomplete aggregations
  return agg.aggregationOp?.toLowerCase() ?? 'result';
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
    // Initialize with an aggregation editor if we don't have any aggregations yet
    if (attrs.aggregations.length === 0) {
      attrs.aggregations.push({isEditing: true});
    }

    // Use the utility function to determine if a column is valid for the given operation
    const isColumnValidForOp = isColumnValidForAggregation;

    const aggregationEditor = (agg: Aggregation, index: number): m.Child => {
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

      // Validation function that checks if the aggregation is complete and valid
      const isAggregationValid = (): boolean => {
        return validateAggregation(agg);
      };

      return m(
        Form,
        {
          submitLabel: 'Apply',
          submitIcon: Icons.Check,
          cancelLabel: 'Cancel',
          required: true,
          validation: isAggregationValid,
          onSubmit: (e: Event) => {
            e.preventDefault();
            if (!agg.newColumnName) {
              agg.newColumnName = placeholderNewColumnName(agg);
            }
            agg.isEditing = false;
            attrs.onchange?.();
          },
          onCancel: () => {
            // If this is a new aggregation that hasn't been confirmed yet, remove it
            if (!agg.isValid) {
              attrs.aggregations.splice(index, 1);
            } else {
              // Otherwise just stop editing
              agg.isEditing = false;
            }
            m.redraw();
          },
        },
        m(
          '.pf-exp-aggregation-editor',
          m(
            Select,
            {
              required: true,
              onchange: (e: Event) => {
                agg.aggregationOp = (e.target as HTMLSelectElement).value;
                // Clear percentile when changing operation
                if (agg.aggregationOp !== 'PERCENTILE') {
                  agg.percentile = undefined;
                }
                // Clear column when switching to COUNT_ALL
                if (agg.aggregationOp === 'COUNT_ALL') {
                  agg.column = undefined;
                }
                m.redraw();
              },
            },
            m(
              'option',
              {disabled: true, selected: !agg.aggregationOp, value: ''},
              'Select operation',
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
              required: true,
              oninput: (e: InputEvent) => {
                const value = parseFloat((e.target as HTMLInputElement).value);
                agg.percentile = isNaN(value) ? undefined : value;
                m.redraw();
              },
              value: agg.percentile?.toString() ?? '',
            }),
          // Column selector (not shown for COUNT_ALL)
          agg.aggregationOp &&
            agg.aggregationOp !== 'COUNT_ALL' &&
            m(
              Select,
              {
                required: true,
                onchange: (e: Event) => {
                  const target = e.target as HTMLSelectElement;
                  agg.column = attrs.groupByColumns.find(
                    (c) => c.name === target.value,
                  );
                  m.redraw();
                },
              },
              m(
                'option',
                {disabled: true, selected: !agg.column, value: ''},
                'Select column',
              ),
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
        ),
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
      const lastAgg = attrs.aggregations[attrs.aggregations.length - 1];
      const showAddButton = lastAgg.isValid;

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

    const selectGroupByColumns = (): m.Child => {
      const groupByOptions: MultiSelectOption[] = attrs.groupByColumns.map(
        (col) => ({
          id: col.name,
          name: col.name,
          checked: col.checked,
        }),
      );

      const selectedGroupBy = attrs.groupByColumns.filter((c) => c.checked);
      const label =
        selectedGroupBy.length > 0
          ? selectedGroupBy.map((c) => c.name).join(', ')
          : 'None';

      return m(
        '.pf-exp-multi-select-container',
        m('label', 'GROUP BY columns'),
        m(PopupMultiSelect, {
          label,
          options: groupByOptions,
          showNumSelected: false,
          onChange: (diffs: MultiSelectDiff[]) => {
            for (const diff of diffs) {
              const column = attrs.groupByColumns.find(
                (c) => c.name === diff.id,
              );
              if (column) {
                column.checked = diff.checked;
              }
            }
            attrs.onchange?.();
          },
        }),
      );
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
