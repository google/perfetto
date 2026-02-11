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

import {QueryNode, QueryNodeState, NodeType} from '../query_node';
import {SqlModules} from '../../../plugins/dev.perfetto.SqlModules/sql_modules';
import {Trace} from '../../../public/trace';

// The context provided to the preCreate hook.
export interface PreCreateContext {
  sqlModules: SqlModules;
}

// The context provided to the node factory.
export interface NodeFactoryContext {
  allNodes: QueryNode[];
}

// The initial state returned by preCreate, which will be merged with
// trace and other runtime properties before node creation.
export type PreCreateState = Record<string, unknown>;

export interface NodeDescriptor {
  // The name of the node, as it appears in the UI.
  name: string;

  // A short description of what the node does.
  description: string;

  // The icon to display for this node.
  icon: string;

  // The keyboard shortcut for this node.
  hotkey?: string;

  // Whether this node is a source, modification or a multi-source node.
  type: 'source' | 'modification' | 'multisource';

  // Optional category for grouping related nodes in the UI.
  // Nodes with the same category will be shown in a submenu.
  category?: string;

  // An optional, async function that runs before the node is created.
  // It can be used for interactive setup, like showing a modal.
  // If it returns null, the creation is aborted.
  // Can return an array to create multiple nodes at once (source nodes only).
  // Note: Operation nodes should only return a single state or null.
  preCreate?: (
    context: PreCreateContext,
  ) => Promise<PreCreateState | PreCreateState[] | null>;

  // A function that creates a new instance of the node.
  factory: (state: QueryNodeState, context?: NodeFactoryContext) => QueryNode;

  /**
   * Whether this node should be shown on the landing page.
   *
   * If false, the node is still available in menus but not on the landing page.
   * This is useful for nodes that are better accessed via commands or menus
   * rather than being a primary entry point.
   *
   * @default true for source nodes
   */
  showOnLandingPage?: boolean;

  // The NodeType enum value for this node (used for serialization lookup).
  nodeType: NodeType;

  // Create a node instance from serialized JSON state.
  deserialize: (
    state: object,
    trace: Trace,
    sqlModules: SqlModules,
  ) => QueryNode;

  // Restore secondary/backward connections after all nodes are created.
  // Primary input is restored automatically based on hasPrimaryInput.
  deserializeConnections?: (
    node: QueryNode,
    state: object,
    allNodes: Map<string, QueryNode>,
  ) => void;

  // Post-deserialization hook (phase 1). Called after all connections are
  // restored. Used for resolving internal references (e.g. column resolution).
  postDeserialize?: (node: QueryNode) => void;

  // Post-deserialization hook (phase 2). Called after all postDeserialize hooks
  // have run. Used for updating derived state that depends on other nodes being
  // fully resolved (e.g. onPrevNodesUpdated).
  postDeserializeLate?: (node: QueryNode) => void;

  // Whether this node has a primary input (vertical connection from above).
  // If true, primaryInputId from serialized state will be auto-restored.
  // Default: true for 'modification' nodes, false for 'source'/'multisource'.
  hasPrimaryInput?: boolean;
}

export class NodeRegistry {
  private nodes: Map<string, NodeDescriptor> = new Map();
  private byNodeType: Map<NodeType, NodeDescriptor> = new Map();

  register(id: string, descriptor: NodeDescriptor) {
    this.nodes.set(id, descriptor);
    this.byNodeType.set(descriptor.nodeType, descriptor);
  }

  get(id: string): NodeDescriptor | undefined {
    return this.nodes.get(id);
  }

  getByNodeType(type: NodeType): NodeDescriptor | undefined {
    return this.byNodeType.get(type);
  }

  list(): [string, NodeDescriptor][] {
    return Array.from(this.nodes.entries());
  }
}

export const nodeRegistry = new NodeRegistry();
