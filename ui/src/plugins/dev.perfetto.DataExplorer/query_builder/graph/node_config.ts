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

import {Node, NodePort} from '../../../../widgets/nodegraph';
import {QueryNode, NodeType, singleNodeOperation} from '../../query_node';

function getPortName(
  portNames: string[] | ((portIndex: number) => string),
  portIndex: number,
): string {
  if (typeof portNames === 'function') {
    return portNames(portIndex);
  }
  return portNames[portIndex] ?? `Input ${portIndex}`;
}

export function getNodeHue(node: QueryNode): number {
  switch (node.type) {
    case NodeType.kTable:
      return 354; // Red (#ffcdd2)
    case NodeType.kSimpleSlices:
      return 122; // Green (#c8e6c9)
    case NodeType.kSqlSource:
      return 199; // Cyan/Light Blue (#b3e5fc)
    case NodeType.kTimeRangeSource:
      return 33; // Orange (#ffe0b2)
    case NodeType.kAggregation:
      return 339; // Pink (#f8bbd0)
    case NodeType.kModifyColumns:
      return 261; // Purple (#d1c4e9)
    case NodeType.kAddColumns:
      return 232; // Indigo (#c5cae9)
    case NodeType.kFilterDuring:
      return 88; // Light Green (#dcedc8)
    case NodeType.kLimitAndOffset:
      return 175; // Teal (#b2dfdb)
    case NodeType.kSort:
      return 54; // Yellow (#fff9c4)
    case NodeType.kFilter:
      return 207; // Blue (#bbdefb)
    case NodeType.kIntervalIntersect:
      return 45; // Amber/Orange (#ffecb3)
    case NodeType.kUnion:
      return 187; // Cyan (#b2ebf2)
    case NodeType.kJoin:
      return 14; // Deep Orange (#ffccbc)
    case NodeType.kCreateSlices:
      return 100; // Green (#c8e6c9)
    case NodeType.kMetrics:
      return 280; // Violet (#e1bee7)
    case NodeType.kVisualisation:
      return 30; // Orange (#ffe0b2)
    case NodeType.kDashboard:
      return 160; // Teal (#b2dfdb)
    case NodeType.kGroup:
      return 260; // Purple (#d1c4e9)
    default:
      return 65; // Lime (#f0f4c3)
  }
}

/**
 * Set of "nodeId:portIndex" strings identifying ports that have connections.
 * Used to hide unused ports in read-only previews.
 */
export type ConnectedPorts = ReadonlySet<string>;

function portKey(nodeId: string, portIndex: number): string {
  return `${nodeId}:${portIndex}`;
}

/**
 * Builds a read-only Node config for preview purposes (e.g. group node
 * inner graph). Only depends on QueryNode and nodegraph — no graph_utils
 * dependency to avoid circular imports.
 */
export function buildReadOnlyNodeConfig(
  qnode: QueryNode,
  innerSet: ReadonlySet<string>,
  connectedInputs?: ConnectedPorts,
  connectedOutputs?: ConnectedPorts,
): Omit<Node, 'x' | 'y'> {
  const isSingle = singleNodeOperation(qnode.type);
  const inputs: NodePort[] = [];
  let portIdx = 0;

  if (isSingle) {
    // Top port (primary input) — only show if connected or no filter provided.
    if (
      connectedInputs === undefined ||
      connectedInputs.has(portKey(qnode.nodeId, portIdx))
    ) {
      inputs.push({direction: 'top'});
    }
    portIdx++;
  }
  if (qnode.secondaryInputs) {
    const portNames = qnode.secondaryInputs.portNames;
    let secIdx = 0;
    for (const [,] of qnode.secondaryInputs.connections) {
      if (
        connectedInputs === undefined ||
        connectedInputs.has(portKey(qnode.nodeId, portIdx))
      ) {
        inputs.push({
          label: getPortName(portNames, secIdx),
          direction: 'left',
        });
      }
      portIdx++;
      secIdx++;
    }
  }

  const outputs: NodePort[] = [];
  if (
    connectedOutputs === undefined ||
    connectedOutputs.has(portKey(qnode.nodeId, 0))
  ) {
    outputs.push({direction: 'bottom'});
  }

  // Find docked child — the nodegraph widget can route connection lines
  // to docked children, so we always dock single-op successors.
  // Note: we only dock when the parent has exactly one child. If a node has
  // multiple children (even if only one is in innerSet), we skip docking to
  // avoid ambiguity — the outer graph's docking logic handles multi-child
  // cases differently via layout positions.
  let next: Omit<Node, 'x' | 'y'> | undefined;
  if (
    qnode.nextNodes.length === 1 &&
    singleNodeOperation(qnode.nextNodes[0].type) &&
    qnode.nextNodes[0].primaryInput === qnode &&
    innerSet.has(qnode.nextNodes[0].nodeId)
  ) {
    next = buildReadOnlyNodeConfig(
      qnode.nextNodes[0],
      innerSet,
      connectedInputs,
      connectedOutputs,
    );
  }

  return {
    id: qnode.nodeId,
    titleBar: undefined,
    inputs,
    outputs,
    canDockTop: isSingle,
    canDockBottom: true,
    hue: getNodeHue(qnode),
    accentBar: true,
    content: qnode.nodeDetails().content,
    next,
  };
}
