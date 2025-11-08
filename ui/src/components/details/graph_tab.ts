// Copyright (C) 2024 The Android Open Source Project
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
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';
import {DetailsShell} from '../../widgets/details_shell';
import {Spinner} from '../../widgets/spinner';
import {raf} from '../../core/raf_scheduler';
import {addEphemeralTab} from './add_ephemeral_tab';
import {STR} from '../../trace_processor/query_result';
import {
  Connection,
  Node,
  NodeGraph,
  NodeGraphAttrs,
} from '../../widgets/nodegraph';
import {Button} from '../../widgets/button';

export interface GraphTabConfig {
  sqlQuery: string;
}

export function addGraphTab(trace: Trace, config: GraphTabConfig) {
  addEphemeralTab(trace, 'graph', new GraphTab(trace, config));
}

export class GraphTab implements Tab {
  constructor(
    private trace: Trace,
    private config: GraphTabConfig,
  ) {}

  private data?: Array<{source: string; dest: string}>;
  private loading = true;
  private error?: string;
  private nodes: Map<string, Node> = new Map();
  private connections: Connection[] = [];

  async loadData() {
    try {
      const result = await this.trace.engine.query(this.config.sqlQuery);

      const data = [];
      for (
        const it = result.iter({source: STR, dest: STR});
        it.valid();
        it.next()
      ) {
        data.push({source: it.source, dest: it.dest});
      }

      this.data = data;
      this.buildGraph(data);
      this.loading = false;
    } catch (e) {
      this.error = String(e);
      this.loading = false;
    }
    raf.scheduleFullRedraw();
  }

  private buildGraph(edges: Array<{source: string; dest: string}>) {
    // Clear existing nodes and connections
    this.nodes.clear();
    this.connections = [];

    // Create a map to track node positions
    const nodeMap = new Map<string, {x: number; y: number; index: number}>();
    let nodeIndex = 0;

    // Collect all unique nodes
    const uniqueNodes = new Set<string>();
    for (const edge of edges) {
      uniqueNodes.add(edge.source);
      uniqueNodes.add(edge.dest);
    }

    // Create nodes in a grid layout
    const nodesPerRow = Math.ceil(Math.sqrt(uniqueNodes.size));
    let row = 0;
    let col = 0;

    for (const nodeId of uniqueNodes) {
      const x = 150 + col * 200;
      const y = 100 + row * 150;

      nodeMap.set(nodeId, {x, y, index: nodeIndex++});

      this.nodes.set(nodeId, {
        id: nodeId,
        x,
        y,
        content: m('div', {style: 'padding: 8px; text-align: center;'}, nodeId),
        inputs: [{content: '', direction: 'top'}],
        outputs: [{content: '', direction: 'bottom'}],
        hue: Math.floor(Math.random() * 360),
        titleBar: {title: nodeId},
      });

      col++;
      if (col >= nodesPerRow) {
        col = 0;
        row++;
      }
    }

    // Create connections between nodes
    for (const edge of edges) {
      const sourceInfo = nodeMap.get(edge.source);
      const destInfo = nodeMap.get(edge.dest);

      if (sourceInfo && destInfo) {
        this.connections.push({
          fromNode: edge.source,
          fromPort: 0,
          toNode: edge.dest,
          toPort: 0,
        });
      }
    }
  }

  private getNodeDimensions(nodeId: string): {width: number; height: number} {
    const nodeElement = document.querySelector(`[data-node="${nodeId}"]`);
    if (nodeElement) {
      const rect = nodeElement.getBoundingClientRect();
      // Get the canvas zoom level if available
      const canvas = nodeElement.closest('.pf-canvas') as HTMLElement;
      const zoom = canvas?.style?.transform
        ? parseFloat(
            canvas.style.transform.match(/scale\(([\d.]+)\)/)?.[1] || '1',
          )
        : 1;

      // Divide by zoom to get actual dimensions
      return {
        width: rect.width / zoom,
        height: rect.height / zoom,
      };
    }
    // Fallback if DOM element not found
    return {width: 180, height: 100};
  }

  private autoLayoutDFS() {
    // Build adjacency list for the graph
    const adjacencyList = new Map<string, Set<string>>();
    const incomingEdges = new Map<string, Set<string>>();

    // Initialize all nodes
    for (const node of this.nodes.keys()) {
      adjacencyList.set(node, new Set());
      incomingEdges.set(node, new Set());
    }

    // Build the graph structure
    for (const conn of this.connections) {
      adjacencyList.get(conn.fromNode)?.add(conn.toNode);
      incomingEdges.get(conn.toNode)?.add(conn.fromNode);
    }

    // Find root nodes (nodes with no incoming edges)
    const rootNodes = [];
    for (const [nodeId, incoming] of incomingEdges) {
      if (incoming.size === 0) {
        rootNodes.push(nodeId);
      }
    }

    // If no root nodes, start with any node
    if (rootNodes.length === 0 && this.nodes.size > 0) {
      rootNodes.push(this.nodes.keys().next().value);
    }

    // DFS traversal to assign positions
    const visited = new Set<string>();
    const layers = new Map<string, number>(); // Node to layer mapping
    let maxLayer = 0;
    const nodesPerLayer = new Map<number, string[]>();

    const dfs = (nodeId: string, layer: number) => {
      if (visited.has(nodeId)) return;
      visited.add(nodeId);

      layers.set(nodeId, layer);
      maxLayer = Math.max(maxLayer, layer);

      if (!nodesPerLayer.has(layer)) {
        nodesPerLayer.set(layer, []);
      }
      nodesPerLayer.get(layer)!.push(nodeId);

      // Visit children
      const children = adjacencyList.get(nodeId) || new Set();
      for (const child of children) {
        if (!visited.has(child)) {
          dfs(child, layer + 1);
        }
      }
    };

    // Perform DFS from each root
    for (const root of rootNodes) {
      dfs(root, 0);
    }

    // Position nodes based on DFS layers using actual node dimensions
    const horizontalSpacing = 30; // Space between nodes horizontally (same as Auto Layout)
    const verticalSpacing = 50; // Space between layers vertically (same as Auto Layout)

    let currentY = 50; // Start position Y

    for (let layer = 0; layer <= maxLayer; layer++) {
      const nodesInLayer = nodesPerLayer.get(layer) || [];

      // Find the tallest node in this layer
      let maxHeight = 0;
      nodesInLayer.forEach((nodeId) => {
        const dims = this.getNodeDimensions(nodeId);
        maxHeight = Math.max(maxHeight, dims.height);
      });

      // Position each node in this layer
      let currentX = 50; // Start position X
      nodesInLayer.forEach((nodeId) => {
        const node = this.nodes.get(nodeId);
        if (node) {
          node.x = currentX;
          node.y = currentY;

          // Get actual width and move to next position
          const dims = this.getNodeDimensions(nodeId);
          currentX += dims.width + horizontalSpacing;
        }
      });

      // Move to next layer
      currentY += maxHeight + verticalSpacing;
    }

    raf.scheduleFullRedraw();
  }

  render(): m.Children {
    if (this.loading && !this.data) {
      this.loadData();
      return m(
        DetailsShell,
        {
          title: 'Graph',
        },
        m('.pf-graph-container', {style: 'padding: 20px;'}, m(Spinner)),
      );
    }

    if (this.error) {
      return m(
        DetailsShell,
        {
          title: 'Graph',
        },
        m(
          '.pf-graph-container',
          {style: 'padding: 20px;'},
          m('.pf-error', `Error loading graph data: ${this.error}`),
        ),
      );
    }

    const nodeGraphAttrs: NodeGraphAttrs = {
      nodes: Array.from(this.nodes.values()),
      connections: this.connections,
      toolbarItems: [
        m(Button, {
          label: 'Auto Layout (DFS)',
          icon: 'account_tree',
          onclick: () => this.autoLayoutDFS(),
        }),
      ],
      onNodeMove: (nodeId: string, x: number, y: number) => {
        const node = this.nodes.get(nodeId);
        if (node) {
          node.x = x;
          node.y = y;
        }
      },
      onConnect: (conn: Connection) => {
        this.connections.push(conn);
        raf.scheduleFullRedraw();
      },
      onConnectionRemove: (index: number) => {
        this.connections.splice(index, 1);
        raf.scheduleFullRedraw();
      },
    };

    return m(
      DetailsShell,
      {
        title: 'Graph',
        description: `Showing ${this.data?.length ?? 0} edges with ${this.nodes.size} nodes`,
        fillHeight: true,
      },
      m(
        '.pf-graph-container',
        {
          style:
            'width: 100%; height: 100%; display: flex; flex-direction: column;',
        },
        m(NodeGraph, {
          ...nodeGraphAttrs,
          style: 'flex: 1; width: 100%; height: 100%;',
        }),
      ),
    );
  }

  getTitle(): string {
    return `Graph`;
  }

  isLoading(): boolean {
    return this.loading;
  }
}
