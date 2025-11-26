// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may a copy of the License at
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
  ModificationNode,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {TextInput} from '../../../../widgets/text_input';
import {Button} from '../../../../widgets/button';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {setValidationError} from '../node_issues';

export interface LimitAndOffsetNodeState extends QueryNodeState {
  prevNode: QueryNode;
  limit?: number;
  offset?: number;
}
export class LimitAndOffsetNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kLimitAndOffset;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: LimitAndOffsetNodeState;
  private showOffset = false;

  constructor(state: LimitAndOffsetNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
    this.state.limit = this.state.limit ?? 10;
    this.state.offset = this.state.offset ?? 0;
    // Show offset if it's already set to a non-zero value
    this.showOffset = this.state.offset !== undefined && this.state.offset > 0;
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Limit and Offset';
  }

  nodeDetails(): m.Child {
    const hasOffset = this.state.offset !== undefined && this.state.offset > 0;

    return m('div', [
      m(
        '.limit-row',
        {
          style: {
            display: 'flex',
            alignItems: 'center',
            gap: '8px',
            marginBottom: '8px',
          },
        },
        [
          m('label', 'Limit'),
          m(TextInput, {
            style: {width: '40px'},
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              this.state.limit = Number(target.value);
              m.redraw();
            },
            onblur: () => {
              this.state.onchange?.();
            },
            onkeydown: (e: KeyboardEvent) => {
              if (e.key === 'Enter') {
                this.state.onchange?.();
              }
            },
            value: this.state.limit?.toString() ?? '10',
          }),
          !this.showOffset &&
            m(Button, {
              icon: 'edit',
              minimal: true,
              onclick: () => {
                this.showOffset = true;
                // Set offset to 10 when showing for the first time
                if (
                  this.state.offset === 0 ||
                  this.state.offset === undefined
                ) {
                  this.state.offset = 10;
                }
                this.state.onchange?.();
                m.redraw();
              },
            }),
        ],
      ),
      (this.showOffset || hasOffset) &&
        m(
          '.offset-row',
          {
            style: {
              display: 'flex',
              alignItems: 'center',
              gap: '8px',
            },
          },
          [
            m('label', 'Offset'),
            m(TextInput, {
              style: {width: '40px'},
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                const value = Number(target.value);
                this.state.offset = value;
                // Hide offset when set to 0
                if (value === 0) {
                  this.showOffset = false;
                }
                m.redraw();
              },
              onblur: () => {
                this.state.onchange?.();
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter') {
                  this.state.onchange?.();
                }
              },
              value: this.state.offset?.toString() ?? '10',
            }),
          ],
        ),
    ]);
  }

  nodeSpecificModify(): m.Child {
    return null;
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Limit the number of rows returned and optionally skip rows. Useful for sampling data or pagination.',
      ),
      m(
        'p',
        m('strong', 'Tip:'),
        ' Combine with Sort to get meaningful results like "top 10 longest slices" or "rows 100-150".',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Set limit to 10 to see first 10 rows, or set offset to 100 and limit to 50 to see rows 100-150.',
      ),
    );
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.prevNode === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.prevNode.validate()) {
      setValidationError(this.state, 'Previous node is invalid');
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new LimitAndOffsetNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNode === undefined) return undefined;

    const hasLimit = this.state.limit !== undefined && this.state.limit >= 0;
    const hasOffset = this.state.offset !== undefined && this.state.offset > 0;

    if (!hasLimit && !hasOffset) {
      return this.prevNode.getStructuredQuery();
    }

    return StructuredQueryBuilder.withLimitOffset(
      this.prevNode,
      this.state.limit,
      this.state.offset,
      this.nodeId,
    );
  }

  serializeState(): object {
    // Only return serializable fields, excluding callbacks and objects
    // that might contain circular references
    return {
      limit: this.state.limit,
      offset: this.state.offset,
      comment: this.state.comment,
    };
  }

  static deserializeState(
    state: LimitAndOffsetNodeState,
  ): LimitAndOffsetNodeState {
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
