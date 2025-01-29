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

import protos from '../../protos';

import {ColumnControllerRows} from './query_builder/column_controller';

export enum NodeType {
  // Sources
  kStdlibTable,
  kSimpleSlices,

  // Operations
  kJoinOperator,
}

export interface QueryNode {
  readonly type: NodeType;
  readonly prevNode?: QueryNode;
  nextNode?: QueryNode;
  finished: boolean;

  dataName?: string;
  columns?: ColumnControllerRows[];

  validate(): boolean;
  getTitle(): string;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
}

export function getLastFinishedNode(node: QueryNode): QueryNode | undefined {
  if (!node.finished) {
    return;
  }
  while (node.nextNode) {
    if (!node.nextNode.finished) {
      return node;
    }
    node = node.nextNode;
  }
  return node;
}

export function getFirstNode(node: QueryNode): QueryNode | undefined {
  if (!node.finished) {
    return;
  }
  while (node.prevNode) {
    if (!node.finished) {
      return;
    }
    node = node.prevNode;
  }
  return node;
}
