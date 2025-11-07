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
  MultiSourceNode,
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {UIFilter} from '../operations/filter';
import {Card, CardStack} from '../../../../widgets/card';
import {TextInput} from '../../../../widgets/text_input';
import {TabStrip} from '../../../../widgets/tabs';
import {Select} from '../../../../widgets/select';
import {Editor} from '../../../../widgets/editor';
import {
  StructuredQueryBuilder,
  JoinCondition,
} from '../structured_query_builder';

export interface MergeSerializedState {
  leftNodeId: string;
  rightNodeId: string;
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  leftColumn?: string;
  rightColumn?: string;
  sqlExpression?: string;
  filters?: UIFilter[];
  comment?: string;
}

export interface MergeNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  leftColumn: string;
  rightColumn: string;
  sqlExpression: string;
}

export class MergeNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly type = NodeType.kMerge;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: MergeNodeState;

  get finalCols(): ColumnInfo[] {
    if (this.prevNodes.length !== 2) return [];

    // Combine columns from both sources, prefixed with their aliases
    const leftCols = this.prevNodes[0]?.finalCols ?? [];
    const rightCols = this.prevNodes[1]?.finalCols ?? [];

    const leftAlias = this.state.leftQueryAlias || 'left';
    const rightAlias = this.state.rightQueryAlias || 'right';

    const result: ColumnInfo[] = [];

    // Add left columns with prefix
    for (const col of leftCols) {
      result.push({
        ...col,
        column: {
          ...col.column,
          name: `${leftAlias}.${col.column.name}`,
        },
      });
    }

    // Add right columns with prefix (only for INNER join, or nullable for LEFT join)
    for (const col of rightCols) {
      result.push({
        ...col,
        column: {
          ...col.column,
          name: `${rightAlias}.${col.column.name}`,
        },
      });
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
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.prevNodes.length !== 2) {
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

    for (const prevNode of this.prevNodes) {
      if (!prevNode.validate()) {
        this.setValidationError(
          prevNode.state.issues?.queryError?.message ??
            `Previous node '${prevNode.getTitle()}' is invalid`,
        );
        return false;
      }
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
    const leftCols = this.prevNodes[0]?.finalCols ?? [];
    const rightCols = this.prevNodes[1]?.finalCols ?? [];

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(
        CardStack,
        m(
          Card,
          m(
            '.pf-form-row',
            m('label', 'Left Alias:'),
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
            '.pf-form-row',
            m('label', 'Right Alias:'),
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
                    '.pf-form-row',
                    m('label', 'Left Column:'),
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
                    '.pf-form-row',
                    m('label', 'Right Column:'),
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
      prevNodes: [...this.state.prevNodes],
      filters: this.state.filters ? [...this.state.filters] : undefined,
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
    if (!this.validate()) return;

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

    return StructuredQueryBuilder.withJoin(
      this.prevNodes[0],
      this.prevNodes[1],
      'INNER',
      condition,
      this.nodeId,
    );
  }

  serializeState(): MergeSerializedState {
    return {
      leftNodeId: this.prevNodes[0]?.nodeId ?? '',
      rightNodeId: this.prevNodes[1]?.nodeId ?? '',
      leftQueryAlias: this.state.leftQueryAlias,
      rightQueryAlias: this.state.rightQueryAlias,
      conditionType: this.state.conditionType,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      sqlExpression: this.state.sqlExpression,
      filters: this.state.filters?.map((f) => {
        // Explicitly extract only serializable fields to avoid circular references
        if ('value' in f) {
          return {
            column: f.column,
            op: f.op,
            value: f.value,
            enabled: f.enabled,
          };
        } else {
          return {
            column: f.column,
            op: f.op,
            enabled: f.enabled,
          };
        }
      }),
      comment: this.state.comment,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: MergeSerializedState,
  ): {
    prevNodes: QueryNode[];
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
      prevNodes: [leftNode, rightNode].filter(
        (node): node is QueryNode => node !== undefined,
      ),
      leftQueryAlias: state.leftQueryAlias,
      rightQueryAlias: state.rightQueryAlias,
      conditionType: state.conditionType ?? 'equality',
      leftColumn: state.leftColumn ?? '',
      rightColumn: state.rightColumn ?? '',
      sqlExpression: state.sqlExpression ?? '',
    };
  }
}
