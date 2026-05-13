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

import {QueryNode} from './query_node';
import {getAllNodes} from './query_builder/graph_utils';

/**
 * Gets the primary selected node for showing details.
 * When only one node is selected, returns that node.
 * When multiple nodes are selected, returns the first one (by insertion order).
 * Returns undefined if no nodes are selected.
 */
export function getPrimarySelectedNode(
  selectedNodeIds: ReadonlySet<string>,
  rootNodes: QueryNode[],
): QueryNode | undefined {
  if (selectedNodeIds.size === 0) return undefined;

  // Get first selected node (Set maintains insertion order in JS)
  const firstId = selectedNodeIds.values().next().value;
  const allNodes = getAllNodes(rootNodes);
  return allNodes.find((n) => n.nodeId === firstId);
}
