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
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Button} from '../../../../widgets/button';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {UIFilter} from '../operations/filter';

export interface IntervalIntersectSerializedState {
  intervalNodes: string[];
  filters?: UIFilter[];
  comment?: string;
}

export interface IntervalIntersectNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  readonly allNodes: QueryNode[];
  onExecute?: () => void;
}

export class IntervalIntersectNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly type = NodeType.kIntervalIntersect;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: IntervalIntersectNodeState;

  get finalCols(): ColumnInfo[] {
    return newColumnInfoList(this.prevNodes[0]?.finalCols ?? [], true);
  }

  constructor(state: IntervalIntersectNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
    };
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];
  }

  validate(): boolean {
    if (this.prevNodes.length < 2) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Interval intersect node requires one base source and at least one interval source.',
      );
      return false;
    }

    // If the basic structure is valid, we can clear any previous validation error.
    if (this.state.issues) {
      this.state.issues.queryError = undefined;
    }

    for (const prevNode of this.prevNodes) {
      if (!prevNode.validate()) {
        if (!this.state.issues) this.state.issues = new NodeIssues();
        this.state.issues.queryError =
          prevNode.state.issues?.queryError ??
          new Error(`Previous node '${prevNode.getTitle()}' is invalid`);
        return false;
      }
    }

    const checkColumns = (node: QueryNode, required: string[]) => {
      const cols = new Set(node.finalCols.map((c) => c.name));
      const missing = required.filter((r) => !cols.has(r));
      if (missing.length > 0) {
        if (!this.state.issues) this.state.issues = new NodeIssues();
        this.state.issues.queryError = new Error(
          `Node '${node.getTitle()}' is missing required columns: ${missing.join(
            ', ',
          )}`,
        );
        return false;
      }
      return true;
    };

    for (const prevNode of this.prevNodes) {
      if (!checkColumns(prevNode, ['id', 'ts', 'dur'])) return false;
    }

    return true;
  }

  getTitle(): string {
    return 'Interval Intersect';
  }

  nodeSpecificModify(onExecute?: () => void): m.Child {
    this.validate();
    const error = this.state.issues?.queryError;

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(
        '.pf-exp-section',
        m(
          '.pf-exp-operations-container',
          m('h2', 'Interval Intersect'),
          this.prevNodes.slice(1).map((intervalNode, index) =>
            m(
              '.pf-exp-interval-node',
              m('span', `Interval ${index + 1}: ${intervalNode.getTitle()}`),
              m(Button, {
                icon: 'delete',
                onclick: () => {
                  const nextNodeIndex = intervalNode.nextNodes.indexOf(this);
                  if (nextNodeIndex > -1) {
                    intervalNode.nextNodes.splice(nextNodeIndex, 1);
                  }
                  this.prevNodes.splice(index + 1, 1);
                  this.state.onchange?.();
                },
              }),
            ),
          ),
          m(Button, {
            label: 'Run',
            onclick: onExecute,
          }),
        ),
      ),
    );
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      prevNodes: [...this.state.prevNodes],
      allNodes: this.state.allNodes,
      filters: this.state.filters ? [...this.state.filters] : undefined,
      onchange: this.state.onchange,
      onExecute: this.state.onExecute,
    };
    return new IntervalIntersectNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const baseSq = this.prevNodes[0].getStructuredQuery();
    if (baseSq === undefined) return undefined;

    const intervalSqs = this.prevNodes
      .slice(1)
      .map((node) => node.getStructuredQuery());
    if (intervalSqs.some((sq) => sq === undefined)) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.intervalIntersect =
      new protos.PerfettoSqlStructuredQuery.IntervalIntersect();
    sq.intervalIntersect.base = baseSq;
    sq.intervalIntersect.intervalIntersect =
      intervalSqs as protos.PerfettoSqlStructuredQuery[];
    return sq;
  }

  serializeState(): IntervalIntersectSerializedState {
    return {
      intervalNodes: this.prevNodes.slice(1).map((n) => n.nodeId),
      filters: this.state.filters,
      comment: this.state.comment,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: IntervalIntersectSerializedState,
    baseNode: QueryNode,
  ): {prevNodes: QueryNode[]} {
    const intervalNodes = state.intervalNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      prevNodes: [baseNode, ...intervalNodes],
    };
  }
}
