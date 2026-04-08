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

import {
  NodeGraphDockedNode,
  NodeGraphPort,
} from '../../../../widgets/nodegraph';
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

// Port ID helpers for read-only preview nodes.
// The '|' separator is safe since node IDs are UUIDs (hex and '-' only).
export const getReadOnlyOutputPortId = (nodeId: string) => `${nodeId}|out`;
export const getReadOnlyTopPortId = (nodeId: string) => `${nodeId}|in-top`;
export const getReadOnlyLeftPortId = (nodeId: string, i: number) =>
  `${nodeId}|in-left-${i}`;

/**
 * Set of port ID strings identifying ports that have connections.
 * Used to hide unused ports in read-only previews.
 */
export type ConnectedPorts = ReadonlySet<string>;

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
): NodeGraphDockedNode {
  const isSingle = singleNodeOperation(qnode.type);
  const inputs: NodeGraphPort[] = [];

  if (isSingle) {
    // North port (primary input) — only show if connected or no filter provided.
    const portId = getReadOnlyTopPortId(qnode.nodeId);
    if (connectedInputs === undefined || connectedInputs.has(portId)) {
      inputs.push({id: portId, direction: 'north'});
    }
  }
  if (qnode.secondaryInputs) {
    const portNames = qnode.secondaryInputs.portNames;
    let secIdx = 0;
    for (const [,] of qnode.secondaryInputs.connections) {
      const portId = getReadOnlyLeftPortId(qnode.nodeId, secIdx);
      if (connectedInputs === undefined || connectedInputs.has(portId)) {
        inputs.push({
          id: portId,
          label: getPortName(portNames, secIdx),
          direction: 'west',
        });
      }
      secIdx++;
    }
  }

  const outputs: NodeGraphPort[] = [];
  const outPortId = getReadOnlyOutputPortId(qnode.nodeId);
  if (connectedOutputs === undefined || connectedOutputs.has(outPortId)) {
    outputs.push({id: outPortId, direction: 'south'});
  }

  // Find docked child — the nodegraph widget can route connection lines
  // to docked children, so we always dock single-op successors.
  // Note: we only dock when the parent has exactly one child. If a node has
  // multiple children (even if only one is in innerSet), we skip docking to
  // avoid ambiguity — the outer graph's docking logic handles multi-child
  // cases differently via layout positions.
  let next: NodeGraphDockedNode | undefined;
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
    headerBar: undefined,
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
