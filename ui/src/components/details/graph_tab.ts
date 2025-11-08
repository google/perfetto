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
