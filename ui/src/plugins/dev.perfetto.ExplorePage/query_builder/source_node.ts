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
  createFinalColumns,
  NodeType,
} from '../query_node';
import {ColumnInfo} from './column_info';
import protos from '../../../protos';

export abstract class SourceNode implements QueryNode {
  readonly nodeId: string;
  prevNodes: QueryNode[] = [];
  nextNodes: QueryNode[];
  meterialisedAs?: string;

  abstract readonly sourceCols: ColumnInfo[];
  finalCols: ColumnInfo[];

  readonly state: QueryNodeState;

  constructor(state: QueryNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.finalCols = createFinalColumns(this);
    this.nextNodes = [];
  }

  abstract get type(): NodeType;
  abstract getTitle(): string;
  abstract clone(): QueryNode;
  abstract getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  abstract nodeSpecificModify(onExecute?: () => void): m.Child;
  abstract isMaterialised(): boolean;
  abstract serializeState(): object;

  validate(): boolean {
    return true;
  }
}
