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
import protos from '../../../../../protos';
import {
  QueryNode,
  QueryNodeState,
  NodeType,
  nextNodeId,
  SecondaryInputSpec,
} from '../../../query_node';
import {ColumnInfo} from '../../column_info';
import {NodeDetailsAttrs, NodeModifyAttrs} from '../../../node_types';
import {TextInput} from '../../../../../widgets/text_input';
import {StructuredQueryBuilder} from '../../structured_query_builder';
import {InnerGraphPreview} from './inner_graph_preview';
import {perfettoSqlTypeToString} from '../../../../../trace_processor/perfetto_sql_type';

/**
 * Serialized shape of a GroupNode's state (used for JSON round-tripping).
 */
export interface GroupSerializedState {
  readonly name?: string;
  readonly innerNodeIds?: string[];
}

/**
 * Describes an external connection flowing into the group.
 * The inner nodes still reference their original inputs for SQL generation;
 * this structure is used for visual port display and graph rewiring.
 */
export interface ExternalGroupConnection {
  readonly sourceNode: QueryNode;
  readonly innerTargetNode: QueryNode;
  // Port on the inner target node (undefined = primary input)
  readonly innerTargetPort: number | undefined;
  // Port index on the GroupNode's secondaryInputs
  readonly groupPort: number;
}

/**
 * A GroupNode encapsulates a subgraph (set of inner nodes) as a single proxy
 * node. It exposes:
 *   - One input port per external connection flowing into the subgraph
 *   - One output port representing the result of the single end node
 *
 * SQL generation delegates to the end node's getStructuredQuery(), which
 * recursively includes all inner nodes' queries through their unchanged
 * primaryInput / secondaryInputs references.
 */
export class GroupNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kGroup;
  nextNodes: QueryNode[] = [];
  primaryInput?: QueryNode = undefined;
  readonly state: QueryNodeState;

  // The nodes contained inside this group.
  innerNodes: QueryNode[];
  // The single inner node with no outgoing connections inside the group.
  // May be undefined during deserialization before connections are restored.
  endNode: QueryNode | undefined;
  // External connections (sources outside the group feeding into inner nodes).
  // Not readonly because postDeserialize rebuilds this array after restoring
  // inner node connections.
  externalConnections: ExternalGroupConnection[];

  // User-editable display name for the group.
  name: string;

  // Not readonly because postDeserialize rebuilds it after restoring
  // external connections.
  secondaryInputs: SecondaryInputSpec;

  constructor(
    innerNodes: QueryNode[],
    endNode: QueryNode | undefined,
    externalConnections: ExternalGroupConnection[],
    state: QueryNodeState = {},
    name = 'Group',
  ) {
    this.nodeId = nextNodeId();
    this.innerNodes = innerNodes;
    this.endNode = endNode;
    this.externalConnections = externalConnections;
    this.state = state;
    this.name = name;

    const connections = new Map<number, QueryNode>();
    for (const conn of externalConnections) {
      connections.set(conn.groupPort, conn.sourceNode);
    }
    this.secondaryInputs = {
      connections,
      min: 0,
      max: externalConnections.length,
      portNames: (i: number) => `Input ${i + 1}`,
    };
  }

  /**
   * Called when a connection to this group is added or removed.
   * Syncs inner node references: if a group port was disconnected,
   * clear the corresponding inner node's input so that validation and
   * serialization reflect the real state.
   */
  onPrevNodesUpdated(): void {
    for (const conn of this.externalConnections) {
      const source = this.secondaryInputs.connections.get(conn.groupPort);
      const inner = conn.innerTargetNode;
      if (source === undefined) {
        // Port was disconnected — clear the inner node's reference.
        if (conn.innerTargetPort === undefined) {
          inner.primaryInput = undefined;
        } else if (inner.secondaryInputs) {
          inner.secondaryInputs.connections.delete(conn.innerTargetPort);
        }
      } else {
        // Port was (re)connected — update the inner node's reference
        // so SQL generation uses the new source.
        if (conn.innerTargetPort === undefined) {
          inner.primaryInput = source;
        } else if (inner.secondaryInputs) {
          inner.secondaryInputs.connections.set(conn.innerTargetPort, source);
        }
      }
    }
  }

  get finalCols(): ColumnInfo[] {
    return this.endNode?.finalCols ?? [];
  }

  validate(): boolean {
    if (this.innerNodes.length === 0 || this.endNode === undefined) {
      return false;
    }

    // Check that all expected external connections are actually wired up.
    // When a connection to the group is removed, the group's
    // secondaryInputs.connections entry is cleared, but the inner node
    // still holds a stale reference — so inner validate() alone is not
    // sufficient.
    for (const conn of this.externalConnections) {
      const source = this.secondaryInputs.connections.get(conn.groupPort);
      if (source === undefined) {
        return false;
      }
    }

    return this.innerNodes.every((n) => n.validate());
  }

  getTitle(): string {
    return this.name;
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: this.name,
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const cols = this.finalCols;

    const sections: NodeModifyAttrs['sections'] = [
      {
        title: 'Name',
        content: m(TextInput, {
          value: this.name,
          onInput: (value: string) => {
            this.name = value;
            this.state.onchange?.();
          },
        }),
      },
      {
        title: `Inner Graph (${this.innerNodes.length} nodes)`,
        content: m(InnerGraphPreview, {groupNode: this}),
      },
      {
        title: `Output Columns (${cols.length})`,
        content: m(
          '.pf-column-list',
          cols.map((col) =>
            m('.pf-exp-draggable-item.pf-group-column-readonly', [
              m('span.pf-checkbox-label', col.name),
              m(
                'span.pf-column-type',
                perfettoSqlTypeToString(col.column.type),
              ),
            ]),
          ),
        ),
      },
    ];

    return {
      info: 'A group encapsulates a set of nodes as a single unit. Its output represents the result of the end node inside the group.',
      sections,
    };
  }

  nodeInfo(): m.Children {
    return null;
  }

  clone(): GroupNode {
    // Deep clone: create copies of all inner nodes with new IDs.
    const oldToNew = new Map<string, QueryNode>();
    const clonedInnerNodes: QueryNode[] = [];

    for (const inner of this.innerNodes) {
      const cloned = inner.clone();
      oldToNew.set(inner.nodeId, cloned);
      clonedInnerNodes.push(cloned);
    }

    // Remap internal connections to use cloned nodes.
    for (const cloned of clonedInnerNodes) {
      if (cloned.primaryInput !== undefined) {
        const mapped = oldToNew.get(cloned.primaryInput.nodeId);
        if (mapped !== undefined) {
          cloned.primaryInput = mapped;
        }
      }
      cloned.nextNodes = cloned.nextNodes.map(
        (n) => oldToNew.get(n.nodeId) ?? n,
      );
      if (cloned.secondaryInputs !== undefined) {
        for (const [port, src] of cloned.secondaryInputs.connections) {
          const mapped = oldToNew.get(src.nodeId);
          if (mapped !== undefined) {
            cloned.secondaryInputs.connections.set(port, mapped);
          }
        }
      }
    }

    // Find the cloned end node.
    const clonedEndNode =
      this.endNode !== undefined
        ? oldToNew.get(this.endNode.nodeId)
        : undefined;

    // Remap external connections to use cloned inner targets.
    const clonedExternalConnections: ExternalGroupConnection[] =
      this.externalConnections.map((conn) => ({
        ...conn,
        innerTargetNode:
          oldToNew.get(conn.innerTargetNode.nodeId) ?? conn.innerTargetNode,
      }));

    return new GroupNode(
      clonedInnerNodes,
      clonedEndNode,
      clonedExternalConnections,
      {...this.state},
      this.name,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // Create a passthrough query with the GroupNode's own ID that references
    // the end node. This ensures TP knows about the group node ID so results
    // can be looked up by it.
    if (this.endNode === undefined) return undefined;
    return StructuredQueryBuilder.passthrough(this.endNode, this.nodeId);
  }

  serializeState(): object {
    return {
      name: this.name,
      innerNodeIds: this.innerNodes.map((n) => n.nodeId),
    };
  }
}
