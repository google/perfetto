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
} from '../../../query_node';
import {ColumnInfo} from '../../column_info';
import protos from '../../../../../protos';
import {Card} from '../../../../../widgets/card';
import {TextInput} from '../../../../../widgets/text_input';

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

  constructor(state: LimitAndOffsetNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
    this.state.limit = this.state.limit ?? 10;
    this.state.offset = this.state.offset ?? 0;
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Limit and Offset';
  }

  nodeDetails(): m.Child {
    const hasLimit = this.state.limit !== undefined && this.state.limit > 0;
    const hasOffset = this.state.offset !== undefined && this.state.offset > 0;
    if (!hasLimit && !hasOffset) {
      return m('.pf-aggregation-node-details', `No limit set`);
    }

    const limitMessage = hasLimit ? `Limit: ${this.state.limit}` : undefined;
    const offsetMessage = hasOffset
      ? `Offset: ${this.state.offset}`
      : undefined;

    return m(
      '.pf-aggregation-node-details',
      [limitMessage, offsetMessage].filter(Boolean).join(', '),
    );
  }

  nodeSpecificModify(): m.Child {
    return m(Card, [
      m('label', 'Limit '),
      m(TextInput, {
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          this.state.limit = Number(target.value);
          m.redraw();
        },
        value: this.state.limit?.toString() ?? '10',
      }),
      m('label', 'Offset '),
      m(TextInput, {
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          this.state.offset = Number(target.value);
          m.redraw();
        },
        value: this.state.offset?.toString() ?? undefined,
      }),
    ]);
  }

  validate(): boolean {
    return this.prevNode !== undefined;
  }

  clone(): QueryNode {
    return new LimitAndOffsetNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // TODO(mayzner): Implement this.
    return this.prevNode.getStructuredQuery();
  }

  serializeState(): object {
    return this.state;
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
