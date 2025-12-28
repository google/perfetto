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
  SecondaryInputSpec,
} from '../../query_node';
import {getSecondaryInput} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {Switch} from '../../../../widgets/switch';
import {
  StructuredQueryBuilder,
  JoinCondition,
} from '../structured_query_builder';
import {loadNodeDoc} from '../node_doc_loader';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeTitle} from '../node_styling_widgets';
import {JoinConditionSelector, JoinConditionDisplay} from '../join_widgets';
import {ResizableSqlEditor} from '../widgets';

export interface JoinSerializedState {
  leftNodeId: string;
  rightNodeId: string;
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  joinType?: 'INNER' | 'LEFT';
  leftColumn?: string;
  rightColumn?: string;
  sqlExpression?: string;
  comment?: string;
  leftColumns?: {
    name: string;
    type: string;
    checked: boolean;
    alias?: string;
  }[];
  rightColumns?: {
    name: string;
    type: string;
    checked: boolean;
    alias?: string;
  }[];
}

export interface JoinNodeState extends QueryNodeState {
  leftNode?: QueryNode;
  rightNode?: QueryNode;
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  joinType: 'INNER' | 'LEFT';
  leftColumn: string;
  rightColumn: string;
  sqlExpression: string;
  // Column selections from left and right sources with checked/alias state
  leftColumns?: ColumnInfo[];
  rightColumns?: ColumnInfo[];
}

export class JoinNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kJoin;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: JoinNodeState;

  get leftNode(): QueryNode | undefined {
    return getSecondaryInput(this, 0);
  }

  get rightNode(): QueryNode | undefined {
    return getSecondaryInput(this, 1);
  }

  get finalCols(): ColumnInfo[] {
    // Return only checked columns from both left and right sources
    const result: ColumnInfo[] = [];

    // Add checked columns from left
    for (const col of this.state.leftColumns ?? []) {
      if (col.checked) {
        result.push(col);
      }
    }

    // Add checked columns from right
    for (const col of this.state.rightColumns ?? []) {
      if (col.checked) {
        result.push(col);
      }
    }

    return result;
  }

  constructor(state: JoinNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
      leftQueryAlias: state.leftQueryAlias ?? 'left',
      rightQueryAlias: state.rightQueryAlias ?? 'right',
      conditionType: state.conditionType ?? 'equality',
      joinType: state.joinType ?? 'INNER',
      leftColumn: state.leftColumn ?? '',
      rightColumn: state.rightColumn ?? '',
      sqlExpression: state.sqlExpression ?? '',
      leftColumns: state.leftColumns ?? [],
      rightColumns: state.rightColumns ?? [],
    };
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 2,
      portNames: (portIndex: number) =>
        portIndex === 0
          ? this.state.leftQueryAlias
          : this.state.rightQueryAlias,
    };
    // Initialize connections from state
    if (state.leftNode) {
      this.secondaryInputs.connections.set(0, state.leftNode);
    }
    if (state.rightNode) {
      this.secondaryInputs.connections.set(1, state.rightNode);
    }
    this.nextNodes = [];

    // Initialize column arrays from connected nodes if empty
    this.updateColumnArrays();
  }

  onPrevNodesUpdated() {
    // Update column arrays when input nodes change
    this.updateColumnArrays();
  }

  // Update column arrays when nodes change or on initialization
  private updateColumnArrays() {
    // Update left columns if left node is connected
    if (this.leftNode) {
      const sourceCols = this.leftNode.finalCols;
      const newLeftColumns = newColumnInfoList(sourceCols);

      // Preserve checked status and aliases for columns that still exist
      const existingLeftColumns = this.state.leftColumns ?? [];
      for (const oldCol of existingLeftColumns) {
        const newCol = newLeftColumns.find(
          (c) => c.column.name === oldCol.column.name,
        );
        if (newCol) {
          newCol.checked = oldCol.checked;
          newCol.alias = oldCol.alias;
        }
      }

      // Default all to unchecked if this is first initialization
      if (existingLeftColumns.length === 0) {
        for (const col of newLeftColumns) {
          col.checked = false;
        }
      }

      this.state.leftColumns = newLeftColumns;
    } else {
      this.state.leftColumns = [];
    }

    // Update right columns if right node is connected
    if (this.rightNode) {
      const sourceCols = this.rightNode.finalCols;
      const newRightColumns = newColumnInfoList(sourceCols);

      // Preserve checked status and aliases for columns that still exist
      const existingRightColumns = this.state.rightColumns ?? [];
      for (const oldCol of existingRightColumns) {
        const newCol = newRightColumns.find(
          (c) => c.column.name === oldCol.column.name,
        );
        if (newCol) {
          newCol.checked = oldCol.checked;
          newCol.alias = oldCol.alias;
        }
      }

      // Default all to unchecked if this is first initialization
      if (existingRightColumns.length === 0) {
        for (const col of newRightColumns) {
          col.checked = false;
        }
      }

      this.state.rightColumns = newRightColumns;
    } else {
      this.state.rightColumns = [];
    }
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (
      this.secondaryInputs.connections.size !== 2 ||
      !this.leftNode ||
      !this.rightNode
    ) {
      this.setValidationError(
        'Join node requires exactly two sources (left and right).',
      );
      return false;
    }

    if (!this.state.leftQueryAlias || !this.state.rightQueryAlias) {
      this.setValidationError(
        'Both left and right query aliases are required.',
      );
      return false;
    }

    if (this.state.conditionType === 'equality') {
      if (!this.state.leftColumn || !this.state.rightColumn) {
        this.setValidationError(
          'Both left and right columns are required for equality join.',
        );
        return false;
      }
    } else {
      if (!this.state.sqlExpression) {
        this.setValidationError(
          'SQL expression for join condition is required.',
        );
        return false;
      }
    }

    if (!this.leftNode.validate()) {
      this.setValidationError(
        this.leftNode.state.issues?.queryError?.message ??
          `Left node '${this.leftNode.getTitle()}' is invalid`,
      );
      return false;
    }

    if (!this.rightNode.validate()) {
      this.setValidationError(
        this.rightNode.state.issues?.queryError?.message ??
          `Right node '${this.rightNode.getTitle()}' is invalid`,
      );
      return false;
    }

    // Check if there are any columns selected
    const leftColumns = this.state.leftColumns ?? [];
    const rightColumns = this.state.rightColumns ?? [];
    const hasCheckedColumns =
      leftColumns.some((c) => c.checked) || rightColumns.some((c) => c.checked);

    if (!hasCheckedColumns) {
      this.setValidationError(
        'No columns selected. Select at least one column from either source.',
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
    return 'Join';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('join');
  }

  nodeDetails(): NodeDetailsAttrs {
    let content: m.Children;

    if (this.state.conditionType === 'equality') {
      if (this.state.leftColumn && this.state.rightColumn) {
        content = m(JoinConditionDisplay, {
          leftAlias: this.state.leftQueryAlias,
          rightAlias: this.state.rightQueryAlias,
          leftColumn: this.state.leftColumn,
          rightColumn: this.state.rightColumn,
        });
      } else {
        content = m('.pf-exp-node-details-message', 'No condition set');
      }
    } else {
      if (this.state.sqlExpression) {
        content = m('code.pf-exp-sql-expression', this.state.sqlExpression);
      } else {
        content = m('.pf-exp-node-details-message', 'No condition set');
      }
    }

    return {
      content: [NodeTitle(this.getTitle()), content],
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.state.issues?.queryError;

    const sections: NodeModifyAttrs['sections'] = [];
    const bottomRightButtons: NodeModifyAttrs['bottomRightButtons'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Join type section
    sections.push({
      title: 'Join Type',
      content: m(Switch, {
        checked: this.state.joinType === 'LEFT',
        label: 'Left Join',
        onchange: (e: Event) => {
          const target = e.target as HTMLInputElement;
          this.state.joinType = target.checked ? 'LEFT' : 'INNER';
          this.state.onchange?.();
        },
      }),
    });

    // Join condition section with integrated column selection
    sections.push({
      content:
        this.state.conditionType === 'equality'
          ? m(JoinConditionSelector, {
              leftLabel: 'Left',
              rightLabel: 'Right',
              leftColumns: this.state.leftColumns ?? [],
              rightColumns: this.state.rightColumns ?? [],
              leftColumn: this.state.leftColumn,
              rightColumn: this.state.rightColumn,
              onLeftColumnChange: (columnName: string) => {
                this.state.leftColumn = columnName;
                this.state.onchange?.();
              },
              onRightColumnChange: (columnName: string) => {
                this.state.rightColumn = columnName;
                this.state.onchange?.();
              },
              onLeftColumnToggle: (index: number, checked: boolean) => {
                if (this.state.leftColumns) {
                  this.state.leftColumns[index].checked = checked;
                  this.state.onchange?.();
                }
              },
              onRightColumnToggle: (index: number, checked: boolean) => {
                if (this.state.rightColumns) {
                  this.state.rightColumns[index].checked = checked;
                  this.state.onchange?.();
                }
              },
              onLeftColumnAlias: (index: number, alias: string) => {
                if (this.state.leftColumns) {
                  this.state.leftColumns[index].alias =
                    alias.trim() === '' ? undefined : alias;
                  this.state.onchange?.();
                }
              },
              onRightColumnAlias: (index: number, alias: string) => {
                if (this.state.rightColumns) {
                  this.state.rightColumns[index].alias =
                    alias.trim() === '' ? undefined : alias;
                  this.state.onchange?.();
                }
              },
            })
          : m(ResizableSqlEditor, {
              sql: this.state.sqlExpression,
              onUpdate: (text: string) => {
                this.state.sqlExpression = text;
                this.state.onchange?.();
              },
            }),
    });

    // Mode switch button
    bottomRightButtons.push({
      label:
        this.state.conditionType === 'equality'
          ? 'Switch to freeform SQL'
          : 'Switch to equality',
      icon: this.state.conditionType === 'equality' ? 'code' : 'view_column',
      onclick: () => {
        this.state.conditionType =
          this.state.conditionType === 'equality' ? 'freeform' : 'equality';
        // Disable auto-execute in freeform SQL mode
        this.state.autoExecute = this.state.conditionType === 'equality';
        this.state.onchange?.();
      },
      compact: true,
    });

    return {
      info: 'Combines rows from exactly two inputs side-by-side by matching on a join key. Each row from the first input is matched with rows from the second input where the join column values are equal.',
      sections,
      bottomRightButtons,
    };
  }

  clone(): QueryNode {
    const stateCopy: JoinNodeState = {
      leftNode: this.leftNode,
      rightNode: this.rightNode,
      onchange: this.state.onchange,
      leftQueryAlias: this.state.leftQueryAlias,
      rightQueryAlias: this.state.rightQueryAlias,
      conditionType: this.state.conditionType,
      joinType: this.state.joinType,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      sqlExpression: this.state.sqlExpression,
      leftColumns: this.state.leftColumns
        ? newColumnInfoList(this.state.leftColumns)
        : undefined,
      rightColumns: this.state.rightColumns
        ? newColumnInfoList(this.state.rightColumns)
        : undefined,
    };
    return new JoinNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate() || !this.leftNode || !this.rightNode) return;

    const condition: JoinCondition =
      this.state.conditionType === 'equality'
        ? {
            type: 'equality',
            leftColumn: this.state.leftColumn,
            rightColumn: this.state.rightColumn,
          }
        : {
            type: 'freeform',
            leftQueryAlias: this.state.leftQueryAlias,
            rightQueryAlias: this.state.rightQueryAlias,
            sqlExpression: this.state.sqlExpression,
          };

    const sq = StructuredQueryBuilder.withJoin(
      this.leftNode,
      this.rightNode,
      this.state.joinType,
      condition,
      this.nodeId,
    );

    if (sq === undefined) return undefined;

    // Add select_columns to explicitly specify which columns to return
    // Include aliases if specified
    sq.selectColumns = this.finalCols.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.column.name;
      if (col.alias) {
        selectCol.alias = col.alias;
      }
      return selectCol;
    });

    return sq;
  }

  serializeState(): JoinSerializedState {
    return {
      leftNodeId: this.leftNode?.nodeId ?? '',
      rightNodeId: this.rightNode?.nodeId ?? '',
      leftQueryAlias: this.state.leftQueryAlias,
      rightQueryAlias: this.state.rightQueryAlias,
      conditionType: this.state.conditionType,
      joinType: this.state.joinType,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      sqlExpression: this.state.sqlExpression,
      leftColumns: (this.state.leftColumns ?? []).map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        alias: c.alias,
      })),
      rightColumns: (this.state.rightColumns ?? []).map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        alias: c.alias,
      })),
    };
  }

  static deserializeState(state: JoinSerializedState): JoinNodeState {
    return {
      leftQueryAlias: state.leftQueryAlias,
      rightQueryAlias: state.rightQueryAlias,
      conditionType: state.conditionType ?? 'equality',
      joinType: state.joinType ?? 'INNER',
      leftColumn: state.leftColumn ?? '',
      rightColumn: state.rightColumn ?? '',
      sqlExpression: state.sqlExpression ?? '',
      leftColumns:
        state.leftColumns?.map((c) => ({
          name: c.name,
          type: c.type,
          checked: c.checked,
          column: {name: c.name},
          alias: c.alias,
        })) ?? [],
      rightColumns:
        state.rightColumns?.map((c) => ({
          name: c.name,
          type: c.type,
          checked: c.checked,
          column: {name: c.name},
          alias: c.alias,
        })) ?? [],
    };
  }

  static deserializeConnections(
    nodes: Map<string, QueryNode>,
    state: JoinSerializedState,
  ): {
    leftNode?: QueryNode;
    rightNode?: QueryNode;
  } {
    return {
      leftNode: nodes.get(state.leftNodeId),
      rightNode: nodes.get(state.rightNodeId),
    };
  }
}
