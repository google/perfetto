// Copyright (C) 2026 The Android Open Source Project
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
import {singleNodeOperation} from '../../../query_node';
import {
  NodeGraph,
  Node as GraphNode,
  Connection,
  NodeGraphAPI,
} from '../../../../../widgets/nodegraph';
import {buildReadOnlyNodeConfig} from '../../graph/node_config';
import type {GroupNode} from '.';

interface InnerGraphPreviewAttrs {
  readonly groupNode: GroupNode;
}

/**
 * A read-only NodeGraph preview of a GroupNode's inner subgraph.
 * Owns its own view-layer state (recenter tracking) so that GroupNode
 * remains a pure data model.
 */
export class InnerGraphPreview
  implements m.ClassComponent<InnerGraphPreviewAttrs>
{
  private recentered = false;
  private lastInnerNodeCount = 0;

  view({attrs}: m.CVnode<InnerGraphPreviewAttrs>): m.Children {
    const {groupNode} = attrs;
    if (groupNode.innerNodes.length === 0) return null;

    // Reset recenter flag when inner node count changes (e.g. after
    // a connection is removed and the group is rebuilt).
    if (groupNode.innerNodes.length !== this.lastInnerNodeCount) {
      this.lastInnerNodeCount = groupNode.innerNodes.length;
      this.recentered = false;
    }

    const innerSet = new Set(groupNode.innerNodes.map((n) => n.nodeId));
    const connections: Connection[] = [];

    // Collect docked node IDs (rendered via `next`, not as top-level).
    const dockedIds = new Set<string>();
    for (const n of groupNode.innerNodes) {
      if (
        n.nextNodes.length === 1 &&
        singleNodeOperation(n.nextNodes[0].type) &&
        n.nextNodes[0].primaryInput === n &&
        innerSet.has(n.nextNodes[0].nodeId)
      ) {
        dockedIds.add(n.nextNodes[0].nodeId);
      }
    }

    // Build connections first so we know which ports are used.
    // Use groupPort as suffix (not loop index) so IDs stay stable if the
    // externalConnections array is ever reordered.
    const inputNodeIds = new Map<number, string>();
    for (let i = 0; i < groupNode.externalConnections.length; i++) {
      const conn = groupNode.externalConnections[i];
      const inputId = `__input_${conn.groupPort}`;
      inputNodeIds.set(conn.groupPort, inputId);

      const offset = singleNodeOperation(conn.innerTargetNode.type) ? 1 : 0;
      const toPort =
        conn.innerTargetPort !== undefined ? conn.innerTargetPort + offset : 0;
      connections.push({
        fromNode: inputId,
        fromPort: 0,
        toNode: conn.innerTargetNode.nodeId,
        toPort,
      });
    }

    for (const n of groupNode.innerNodes) {
      // Add primaryInput connections for undocked inner nodes.
      // Docked nodes render this via the `next` property instead.
      if (
        n.primaryInput !== undefined &&
        innerSet.has(n.primaryInput.nodeId) &&
        !dockedIds.has(n.nodeId)
      ) {
        connections.push({
          fromNode: n.primaryInput.nodeId,
          fromPort: 0,
          toNode: n.nodeId,
          toPort: 0,
        });
      }

      if (n.secondaryInputs) {
        const offset = singleNodeOperation(n.type) ? 1 : 0;
        for (const [port, src] of n.secondaryInputs.connections) {
          if (src !== undefined && innerSet.has(src.nodeId)) {
            connections.push({
              fromNode: src.nodeId,
              fromPort: 0,
              toNode: n.nodeId,
              toPort: port + offset,
            });
          }
        }
      }
    }

    // Collect connected ports from all connections.
    const connectedInputs = new Set<string>();
    const connectedOutputs = new Set<string>();
    for (const c of connections) {
      connectedInputs.add(`${c.toNode}:${c.toPort}`);
      connectedOutputs.add(`${c.fromNode}:${c.fromPort}`);
    }

    // Docked nodes' primary input (port 0) is rendered via `next`, not as
    // a connection line. Register it as "connected" so the port isn't
    // filtered out — otherwise secondary input port indices shift and
    // connections target the wrong port.
    for (const dockedId of dockedIds) {
      connectedInputs.add(`${dockedId}:0`);
    }

    // Build nodes with only connected ports.
    const roots = groupNode.innerNodes.filter((n) => !dockedIds.has(n.nodeId));
    const nodeSpacingX = 250;
    const nodes: GraphNode[] = [];

    for (let i = 0; i < roots.length; i++) {
      nodes.push({
        ...buildReadOnlyNodeConfig(
          roots[i],
          innerSet,
          connectedInputs,
          connectedOutputs,
        ),
        x: i * nodeSpacingX,
        y: 0,
      });
    }

    // Add "Input N" nodes for external connections.
    // Grey out disconnected inputs.
    for (let i = 0; i < groupNode.externalConnections.length; i++) {
      const conn = groupNode.externalConnections[i];
      const isConnected = groupNode.secondaryInputs.connections.has(
        conn.groupPort,
      );
      const inputId = inputNodeIds.get(conn.groupPort);
      if (inputId === undefined) continue;
      nodes.push({
        id: inputId,
        x: i * nodeSpacingX,
        y: -80,
        titleBar: {title: `Input ${conn.groupPort + 1}`},
        outputs: [{direction: 'bottom'}],
        className: isConnected ? undefined : 'pf-node--disconnected',
      });
    }

    return m(
      '.pf-group-inner-graph',
      {
        oncreate: () => {
          this.recentered = false;
        },
      },
      [
        m(NodeGraph, {
          nodes,
          connections,
          hideControls: true,
          fillHeight: true,
          onReady: (api: NodeGraphAPI) => {
            if (!this.recentered) {
              this.recentered = true;
              requestAnimationFrame(() => api.recenter());
            }
          },
        }),
      ],
    );
  }
}
