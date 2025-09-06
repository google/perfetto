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
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Button} from '../../../../widgets/button';
import {Callout} from '../../../../widgets/callout';
import {Select} from '../../../../widgets/select';
import {NodeIssues} from '../node_issues';

export interface IntervalIntersectNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  readonly allNodes: QueryNode[];
  readonly intervalNodes: QueryNode[];
  onExecute?: () => void;
}

export class IntervalIntersectNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kIntervalIntersect;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: IntervalIntersectNodeState;
  meterialisedAs?: string;

  get sourceCols(): ColumnInfo[] {
    return this.prevNodes[0]?.finalCols ?? this.prevNodes[0]?.sourceCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return newColumnInfoList(this.sourceCols, true);
  }

  constructor(state: IntervalIntersectNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
    };
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];
  }

  validate(): boolean {
    if (this.prevNodes.length !== 1) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Interval intersect node requires one base source node.',
      );
      return false;
    }

    if (this.state.intervalNodes.length !== 1) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Interval intersect node requires one interval source to be selected.',
      );
      return false;
    }

    // If the basic structure is valid, we can clear any previous validation error.
    if (this.state.issues) {
      this.state.issues.queryError = undefined;
    }

    if (!this.prevNodes[0].validate()) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError =
        this.prevNodes[0].state.issues?.queryError ??
        new Error(`Previous node '${this.prevNodes[0].getTitle()}' is invalid`);
      return false;
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

    if (!checkColumns(this.prevNodes[0], ['id', 'ts', 'dur'])) return false;

    for (const intervalNode of this.state.intervalNodes) {
      if (!intervalNode.validate()) {
        if (!this.state.issues) this.state.issues = new NodeIssues();
        this.state.issues.queryError =
          intervalNode.state.issues?.queryError ??
          new Error(`Interval node '${intervalNode.getTitle()}' is invalid`);
        return false;
      }
      if (!checkColumns(intervalNode, ['id', 'ts', 'dur'])) {
        return false;
      }
    }

    return true;
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Interval Intersect';
  }

  nodeSpecificModify(onExecute?: () => void): m.Child {
    return m(IntervalIntersectComponent, {
      node: this,
      onExecute,
    });
  }

  clone(): QueryNode {
    const stateCopy: IntervalIntersectNodeState = {
      prevNodes: this.state.prevNodes,
      allNodes: this.state.allNodes,
      intervalNodes: [...this.state.intervalNodes],
      filters: [],
      customTitle: this.state.customTitle,
      onchange: this.state.onchange,
      onExecute: this.state.onExecute,
    };
    return new IntervalIntersectNode(stateCopy);
  }

  isMaterialised(): boolean {
    return this.state.isExecuted === true && this.meterialisedAs !== undefined;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const baseSq = this.prevNodes[0].getStructuredQuery();
    if (baseSq === undefined) return undefined;

    const intervalSqs = this.state.intervalNodes.map((node) =>
      node.getStructuredQuery(),
    );
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
}

interface IntervalIntersectComponentAttrs {
  node: IntervalIntersectNode;
  onExecute?: () => void;
}

class IntervalIntersectComponent
  implements m.ClassComponent<IntervalIntersectComponentAttrs>
{
  view({attrs}: m.CVnode<IntervalIntersectComponentAttrs>) {
    const {node, onExecute} = attrs;
    node.validate();
    const availableNodes = node.state.allNodes.filter(
      (n) =>
        n !== node &&
        !node.state.intervalNodes.includes(n) &&
        !node.prevNodes.includes(n),
    );
    const error = node.state.issues?.queryError;

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(
        '.pf-exp-section',
        m(
          '.pf-exp-operations-container',
          m('h2', 'Interval Intersect'),
          node.state.intervalNodes.map((intervalNode, index) =>
            m(
              '.pf-exp-interval-node',
              m('span', `Interval ${index + 1}: ${intervalNode.getTitle()}`),
              m(Button, {
                icon: 'delete',
                onclick: () => {
                  const intervalNode = node.state.intervalNodes[index];
                  const nextNodeIndex = intervalNode.nextNodes.indexOf(node);
                  if (nextNodeIndex > -1) {
                    intervalNode.nextNodes.splice(nextNodeIndex, 1);
                  }
                  node.state.intervalNodes.splice(index, 1);
                  node.state.onchange?.();
                },
              }),
            ),
          ),
          node.state.intervalNodes.length === 0 &&
            m(
              '.pf-exp-add-interval',
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const selectedNodeId = (e.target as HTMLSelectElement)
                      .value;
                    const selectedNode = availableNodes.find(
                      (n) => n.nodeId === selectedNodeId,
                    );
                    if (selectedNode) {
                      node.state.intervalNodes.push(selectedNode);
                      selectedNode.nextNodes.push(node);
                      node.state.onchange?.();
                    }
                  },
                },
                m(
                  'option',
                  {disabled: true, selected: true},
                  'Select a source',
                ),
                availableNodes.map((n) =>
                  m('option', {value: n.nodeId}, n.getTitle()),
                ),
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
}
