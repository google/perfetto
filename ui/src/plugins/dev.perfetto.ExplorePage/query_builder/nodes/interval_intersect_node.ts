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
  removeConnection,
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
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    // Check for undefined entries (disconnected inputs)
    const validPrevNodes = this.prevNodes.filter(
      (node): node is QueryNode => node !== undefined,
    );

    if (validPrevNodes.length < this.prevNodes.length) {
      this.setValidationError(
        'Interval intersect node has disconnected inputs. Please connect all inputs or remove this node.',
      );
      return false;
    }

    if (this.prevNodes.length < 2) {
      this.setValidationError(
        'Interval intersect node requires one base source and at least one interval source.',
      );
      return false;
    }

    for (const prevNode of this.prevNodes) {
      // Skip undefined entries (already handled above)
      if (prevNode === undefined) continue;

      if (!prevNode.validate()) {
        this.setValidationError(
          prevNode.state.issues?.queryError?.message ??
            `Previous node '${prevNode.getTitle()}' is invalid`,
        );
        return false;
      }
    }

    const checkColumns = (node: QueryNode, required: string[]) => {
      const cols = new Set(node.finalCols.map((c) => c.name));
      const missing = required.filter((r) => !cols.has(r));
      if (missing.length > 0) {
        this.setValidationError(
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

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Interval Intersect';
  }

  nodeSpecificModify(): m.Child {
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
                  // Remove the connection between intervalNode and this node
                  removeConnection(intervalNode, this);
                  this.state.onchange?.();
                },
              }),
            ),
          ),
        ),
      ),
    );
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      prevNodes: [...this.state.prevNodes],
      filters: this.state.filters ? [...this.state.filters] : undefined,
      onchange: this.state.onchange,
    };
    return new IntervalIntersectNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    // Validate returns false if any prevNodes are undefined, so this is safe
    const baseSq = this.prevNodes[0]?.getStructuredQuery();
    if (baseSq === undefined) return undefined;

    const intervalSqs = this.prevNodes
      .slice(1)
      .filter((node): node is QueryNode => node !== undefined)
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
      intervalNodes: this.prevNodes
        .slice(1)
        .filter((n): n is QueryNode => n !== undefined)
        .map((n) => n.nodeId),
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
