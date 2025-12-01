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
  NodeType,
  createFinalColumns,
  nextNodeId,
} from '../../../query_node';
import {ColumnInfo} from '../../column_info';
import protos from '../../../../../protos';
import {NodeDetailsAttrs} from '../../node_explorer_types';

export class TestNode implements QueryNode {
  readonly nodeId: string;
  readonly state: QueryNodeState;
  isDevNode = true;
  readonly finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(state: QueryNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.finalCols = createFinalColumns([]);
    this.nextNodes = [];
  }

  get type(): NodeType {
    return NodeType.kTable;
  }

  getTitle(): string {
    return 'Test Node';
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: null,
    };
  }

  clone(): QueryNode {
    return new TestNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    return undefined;
  }

  nodeSpecificModify(_onExecute?: () => void): m.Child {
    return m('div', 'Test Node');
  }

  nodeInfo(): m.Children {
    return m('div', 'Test Node Info');
  }

  serializeState(): object {
    return {};
  }

  validate(): boolean {
    return true;
  }
}
