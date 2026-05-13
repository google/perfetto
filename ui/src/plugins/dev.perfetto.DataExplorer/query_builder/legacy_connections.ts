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

// Backward compatibility: restores secondary input connections for old saved
// graphs that stored connection IDs inside individual node state instead of
// at the graph level. Each entry maps a NodeType to the old field name pattern.
//
// New graphs use SerializedNode.secondaryInputIds and never reach this code.

import {NodeType, QueryNode} from '../query_node';

// Describes how secondary input IDs were stored in old node state.
type LegacyConnectionSpec =
  // string[] field → sequential ports 0, 1, 2, ...
  | {type: 'array'; field: string}
  // single string field → one specific port
  | {type: 'singleField'; field: string; port: number}
  // named string fields → specific ports (e.g. leftNodeId→0, rightNodeId→1)
  | {type: 'namedFields'; fields: Array<{field: string; port: number}>};

// Map from NodeType to the legacy field(s) that held secondary input IDs.
const LEGACY_SECONDARY_INPUT_SPECS: Partial<
  Record<NodeType, LegacyConnectionSpec>
> = {
  [NodeType.kSqlSource]: {type: 'array', field: 'inputNodeIds'},
  [NodeType.kAddColumns]: {
    type: 'singleField',
    field: 'secondaryInputNodeId',
    port: 0,
  },
  [NodeType.kFilterDuring]: {type: 'array', field: 'secondaryInputNodeIds'},
  [NodeType.kFilterIn]: {type: 'array', field: 'secondaryInputNodeIds'},
  [NodeType.kIntervalIntersect]: {type: 'array', field: 'intervalNodes'},
  [NodeType.kJoin]: {
    type: 'namedFields',
    fields: [
      {field: 'leftNodeId', port: 0},
      {field: 'rightNodeId', port: 1},
    ],
  },
  [NodeType.kCreateSlices]: {
    type: 'namedFields',
    fields: [
      {field: 'startsNodeId', port: 0},
      {field: 'endsNodeId', port: 1},
    ],
  },
  [NodeType.kUnion]: {type: 'array', field: 'unionNodes'},
  [NodeType.kTraceSummary]: {type: 'array', field: 'secondaryInputNodeIds'},
};

/**
 * Restores secondary input connections from old-format node state.
 * Called during deserialization when no graph-level secondaryInputIds are
 * present (i.e. the graph was saved before the new format was introduced).
 */
export function restoreLegacySecondaryInputs(
  node: QueryNode,
  state: object,
  allNodes: Map<string, QueryNode>,
): void {
  if (!node.secondaryInputs) return;
  const spec = LEGACY_SECONDARY_INPUT_SPECS[node.type];
  if (spec === undefined) return;

  const s = state as Record<string, unknown>;
  node.secondaryInputs.connections.clear();

  if (spec.type === 'array') {
    const ids = s[spec.field] as string[] | undefined;
    if (!ids) return;
    for (let i = 0; i < ids.length; i++) {
      const n = allNodes.get(ids[i]);
      if (n) node.secondaryInputs.connections.set(i, n);
    }
  } else if (spec.type === 'singleField') {
    const id = s[spec.field] as string | undefined;
    if (!id) return;
    const n = allNodes.get(id);
    if (n) node.secondaryInputs.connections.set(spec.port, n);
  } else {
    for (const {field, port} of spec.fields) {
      const id = s[field] as string | undefined;
      if (!id) continue;
      const n = allNodes.get(id);
      if (n) node.secondaryInputs.connections.set(port, n);
    }
  }
}
