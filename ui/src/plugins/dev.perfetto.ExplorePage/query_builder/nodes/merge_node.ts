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
  getSecondaryInput,
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {Card, CardStack} from '../../../../widgets/card';
import {TextInput} from '../../../../widgets/text_input';
import {TabStrip} from '../../../../widgets/tabs';
import {Select} from '../../../../widgets/select';
import {Editor} from '../../../../widgets/editor';
import {
  StructuredQueryBuilder,
  JoinCondition,
} from '../structured_query_builder';
import {FormRow} from '../widgets';

export interface MergeSerializedState {
  leftNodeId: string;
  rightNodeId: string;
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  leftColumn?: string;
  rightColumn?: string;
  sqlExpression?: string;
  comment?: string;
}

export interface MergeNodeState extends QueryNodeState {
  leftNode?: QueryNode;
  rightNode?: QueryNode;
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  leftColumn: string;
  rightColumn: string;
  sqlExpression: string;
}

export class MergeNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kMerge;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: MergeNodeState;

  get leftNode(): QueryNode | undefined {
    return getSecondaryInput(this, 0);
  }

  get rightNode(): QueryNode | undefined {
    return getSecondaryInput(this, 1);
  }

  get finalCols(): ColumnInfo[] {
    // Both nodes must be connected for merge to produce columns
    if (!this.leftNode || !this.rightNode) {
      return [];
    }

    const leftCols = this.leftNode.finalCols;
    const rightCols = this.rightNode.finalCols;

    const result: ColumnInfo[] = [];
    const seenColumns = new Set<string>();

    // Handle equality condition: if joining on same column name (e.g., id = id),
    // include it once. Otherwise, handle equality columns separately.
    if (this.state.conditionType === 'equality') {
      if (this.state.leftColumn && this.state.rightColumn) {
        if (this.state.leftColumn === this.state.rightColumn) {
          // Same column name on both sides (e.g., id = id)
          // Include it once in the output
          const equalityCol = leftCols.find(
            (c) => c.name === this.state.leftColumn,
          );
          if (equalityCol) {
            result.push({...equalityCol, checked: true});
            seenColumns.add(this.state.leftColumn);
          }
        }
        // If different column names (e.g., id = parent_id), don't add to seenColumns yet
        // They'll be handled in the deduplication logic below
      }
    }

    // Identify which columns are duplicated across inputs
    const columnCounts = new Map<string, number>();
    for (const col of leftCols) {
      if (!seenColumns.has(col.name)) {
        columnCounts.set(col.name, (columnCounts.get(col.name) ?? 0) + 1);
      }
    }
    for (const col of rightCols) {
      if (!seenColumns.has(col.name)) {
        columnCounts.set(col.name, (columnCounts.get(col.name) ?? 0) + 1);
      }
    }

    // Add only non-duplicated columns from left
    for (const col of leftCols) {
      if (!seenColumns.has(col.name) && columnCounts.get(col.name) === 1) {
        result.push({...col, checked: true});
        seenColumns.add(col.name);
      }
    }

    // Add only non-duplicated columns from right
    for (const col of rightCols) {
      if (!seenColumns.has(col.name) && columnCounts.get(col.name) === 1) {
        result.push({...col, checked: true});
        seenColumns.add(col.name);
      }
    }

    return result;
  }

  constructor(state: MergeNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
      leftQueryAlias: state.leftQueryAlias ?? 'left',
      rightQueryAlias: state.rightQueryAlias ?? 'right',
      conditionType: state.conditionType ?? 'equality',
      leftColumn: state.leftColumn ?? '',
      rightColumn: state.rightColumn ?? '',
      sqlExpression: state.sqlExpression ?? '',
    };
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 2,
    };
    // Initialize connections from state
    if (state.leftNode) {
      this.secondaryInputs.connections.set(0, state.leftNode);
    }
    if (state.rightNode) {
      this.secondaryInputs.connections.set(1, state.rightNode);
    }
    this.nextNodes = [];
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
        'Merge node requires exactly two sources (left and right).',
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

    // Check if there are any columns to expose after deduplication
    if (this.finalCols.length === 0) {
      this.setValidationError(
        'No columns to expose. All columns are duplicated across both inputs. Use a Modify Columns node to alias columns in one of the sources.',
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
    return 'Merge';
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Combine two data sources by matching rows based on a condition. Connect sources to the two top ports.',
      ),
      m(
        'p',
        'Choose equality mode to join on matching column values, or custom SQL mode for complex conditions.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Join process info with thread info where ',
        m('code', 'process.id = thread.upid'),
        ' to see which threads belong to each process.',
      ),
    );
  }

  getInputLabels(): string[] {
    return [this.state.leftQueryAlias, this.state.rightQueryAlias];
  }

  nodeDetails(): m.Child | undefined {
    const wrapperStyle = {paddingTop: '5px'};
    const textStyle = {
      fontSize: 'var(--pf-exp-font-size-sm)',
      color: 'var(--pf-color-text-lighter)',
    };

    let content: string;

    if (this.state.conditionType === 'equality') {
      if (this.state.leftColumn && this.state.rightColumn) {
        content = `${this.state.leftQueryAlias}.${this.state.leftColumn} = ${this.state.rightQueryAlias}.${this.state.rightColumn}`;
      } else {
        content = 'No condition set';
      }
    } else {
      if (this.state.sqlExpression) {
        content = this.state.sqlExpression;
      } else {
        content = 'No condition set';
      }
    }

    return m(
      'div',
      {style: wrapperStyle},
      m(Card, m('small', {style: textStyle}, content)),
    );
  }

  nodeSpecificModify(): m.Child {
    this.validate();
    const error = this.state.issues?.queryError;

    // Get available columns from left and right nodes
    const leftCols = this.leftNode?.finalCols ?? [];
    const rightCols = this.rightNode?.finalCols ?? [];

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(
        CardStack,
        m(
          Card,
          m(
            FormRow,
            {label: 'Left Alias:'},
            m(TextInput, {
              value: this.state.leftQueryAlias,
              placeholder: 'e.g., left, t1, base',
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                this.state.leftQueryAlias = target.value;
                this.state.onchange?.();
              },
            }),
          ),
          m(
            FormRow,
            {label: 'Right Alias:'},
            m(TextInput, {
              value: this.state.rightQueryAlias,
              placeholder: 'e.g., right, t2, other',
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                this.state.rightQueryAlias = target.value;
                this.state.onchange?.();
              },
            }),
          ),
        ),
        m(
          Card,
          m(TabStrip, {
            tabs: [
              {key: 'equality', title: 'Equality'},
              {key: 'freeform', title: 'Freeform SQL'},
            ],
            currentTabKey: this.state.conditionType,
            onTabChange: (key: string) => {
              this.state.conditionType = key as 'equality' | 'freeform';
              this.state.onchange?.();
            },
          }),
          m(
            'div',
            {style: {paddingTop: '10px'}},
            this.state.conditionType === 'equality'
              ? [
                  m(
                    FormRow,
                    {label: 'Left Column:'},
                    m(
                      Select,
                      {
                        onchange: (e: Event) => {
                          const target = e.target as HTMLSelectElement;
                          this.state.leftColumn = target.value;
                          this.state.onchange?.();
                        },
                      },
                      m(
                        'option',
                        {disabled: true, selected: !this.state.leftColumn},
                        'Select column',
                      ),
                      leftCols.map((col) =>
                        m(
                          'option',
                          {
                            value: col.column.name,
                            selected: col.column.name === this.state.leftColumn,
                          },
                          col.column.name,
                        ),
                      ),
                    ),
                  ),
                  m(
                    FormRow,
                    {label: 'Right Column:'},
                    m(
                      Select,
                      {
                        onchange: (e: Event) => {
                          const target = e.target as HTMLSelectElement;
                          this.state.rightColumn = target.value;
                          this.state.onchange?.();
                        },
                      },
                      m(
                        'option',
                        {disabled: true, selected: !this.state.rightColumn},
                        'Select column',
                      ),
                      rightCols.map((col) =>
                        m(
                          'option',
                          {
                            value: col.column.name,
                            selected:
                              col.column.name === this.state.rightColumn,
                          },
                          col.column.name,
                        ),
                      ),
                    ),
                  ),
                ]
              : m(Editor, {
                  text: this.state.sqlExpression,
                  language: 'perfetto-sql',
                  onUpdate: (text: string) => {
                    this.state.sqlExpression = text;
                    this.state.onchange?.();
                  },
                }),
          ),
        ),
      ),
    );
  }

  clone(): QueryNode {
    const stateCopy: MergeNodeState = {
      leftNode: this.leftNode,
      rightNode: this.rightNode,
      onchange: this.state.onchange,
      leftQueryAlias: this.state.leftQueryAlias,
      rightQueryAlias: this.state.rightQueryAlias,
      conditionType: this.state.conditionType,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      sqlExpression: this.state.sqlExpression,
    };
    return new MergeNode(stateCopy);
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
      'INNER',
      condition,
      this.nodeId,
    );

    if (!sq) return undefined;

    // Add select_columns to explicitly specify which columns to return
    // This ensures we only expose the clean, well-defined columns from finalCols
    sq.selectColumns = this.finalCols.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.name;
      return selectCol;
    });

    return sq;
  }

  serializeState(): MergeSerializedState {
    return {
      leftNodeId: this.leftNode?.nodeId ?? '',
      rightNodeId: this.rightNode?.nodeId ?? '',
      leftQueryAlias: this.state.leftQueryAlias,
      rightQueryAlias: this.state.rightQueryAlias,
      conditionType: this.state.conditionType,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      sqlExpression: this.state.sqlExpression,
      comment: this.state.comment,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: MergeSerializedState,
  ): {
    leftNode?: QueryNode;
    rightNode?: QueryNode;
    leftQueryAlias: string;
    rightQueryAlias: string;
    conditionType: 'equality' | 'freeform';
    leftColumn: string;
    rightColumn: string;
    sqlExpression: string;
  } {
    const leftNode = nodes.get(state.leftNodeId);
    const rightNode = nodes.get(state.rightNodeId);

    return {
      leftNode,
      rightNode,
      leftQueryAlias: state.leftQueryAlias,
      rightQueryAlias: state.rightQueryAlias,
      conditionType: state.conditionType ?? 'equality',
      leftColumn: state.leftColumn ?? '',
      rightColumn: state.rightColumn ?? '',
      sqlExpression: state.sqlExpression ?? '',
    };
  }
}
