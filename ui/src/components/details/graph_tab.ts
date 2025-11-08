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
import {Icons} from '../../base/semantic_icons';
import {Section} from '../../widgets/section';
import {TextInput} from '../../widgets/text_input';

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
  private selectedNodeId?: string;
  private showSidePanel = true;
  private sidePanelWidth = 300;
  private isDraggingPanel = false;
  private showLeftPanel = true;
  private leftPanelWidth = 250;
  private isDraggingLeftPanel = false;
  private nodeSearchQuery = '';

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

  private getNodeDetails(nodeId: string) {
    // Get all edges where this node is source or destination
    const outgoingEdges =
      this.data?.filter((edge) => edge.source === nodeId) || [];
    const incomingEdges =
      this.data?.filter((edge) => edge.dest === nodeId) || [];

    return {
      id: nodeId,
      outgoingEdges,
      incomingEdges,
      totalConnections: outgoingEdges.length + incomingEdges.length,
    };
  }

  private renderLeftPanel() {
    if (!this.showLeftPanel) return null;

    // Get sorted list of nodes
    const nodeList = Array.from(this.nodes.keys()).sort();

    // Filter nodes based on search query
    const filteredNodes = this.nodeSearchQuery
      ? nodeList.filter(nodeId =>
          nodeId.toLowerCase().includes(this.nodeSearchQuery.toLowerCase())
        )
      : nodeList;

    // Count connections for each node
    const nodeStats = new Map<string, {incoming: number; outgoing: number}>();
    for (const nodeId of nodeList) {
      const details = this.getNodeDetails(nodeId);
      nodeStats.set(nodeId, {
        incoming: details.incomingEdges.length,
        outgoing: details.outgoingEdges.length,
      });
    }

    return m('.pf-graph-left-panel',
      {
        style: {
          width: `${this.leftPanelWidth}px`,
          position: 'absolute',
          left: 0,
          top: 0,
          bottom: 0,
          background: 'var(--pf-color-background-secondary)',
          borderRight: '1px solid var(--pf-color-border)',
          display: 'flex',
          flexDirection: 'column',
          zIndex: 10,
          overflow: 'hidden',
        },
      },
      // Resize handle
      m('.pf-graph-panel-resize-handle-left',
        {
          style: {
            position: 'absolute',
            right: 0,
            top: 0,
            bottom: 0,
            width: '5px',
            cursor: 'ew-resize',
            background: this.isDraggingLeftPanel
              ? 'var(--pf-color-accent)'
              : 'transparent',
          },
          onmousedown: (e: MouseEvent) => {
            e.preventDefault();
            this.isDraggingLeftPanel = true;
            const startX = e.clientX;
            const startWidth = this.leftPanelWidth;

            const handleMouseMove = (e: MouseEvent) => {
              const delta = e.clientX - startX;
              this.leftPanelWidth = Math.max(
                150,
                Math.min(400, startWidth + delta),
              );
              raf.scheduleFullRedraw();
            };

            const handleMouseUp = () => {
              this.isDraggingLeftPanel = false;
              document.removeEventListener('mousemove', handleMouseMove);
              document.removeEventListener('mouseup', handleMouseUp);
              raf.scheduleFullRedraw();
            };

            document.addEventListener('mousemove', handleMouseMove);
            document.addEventListener('mouseup', handleMouseUp);
          },
        },
      ),
      // Panel header
      m('.pf-graph-panel-header',
        {
          style: {
            padding: '12px',
            borderBottom: '1px solid var(--pf-color-border)',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
          },
        },
        m('h3', {style: {margin: 0, fontSize: '16px'}}, `Nodes (${filteredNodes.length})`),
        m(Button, {
          icon: Icons.Close,
          minimal: true,
          compact: true,
          onclick: () => {
            this.showLeftPanel = false;
            raf.scheduleFullRedraw();
          },
        }),
      ),
      // Search input
      m('.pf-graph-search',
        {
          style: {
            padding: '8px 12px',
            borderBottom: '1px solid var(--pf-color-border)',
          },
        },
        m(TextInput, {
          placeholder: 'Search nodes...',
          value: this.nodeSearchQuery,
          oninput: (e: InputEvent) => {
            this.nodeSearchQuery = (e.target as HTMLInputElement).value;
            raf.scheduleFullRedraw();
          },
        }),
      ),
      // Node list
      m('.pf-graph-node-list',
        {
          style: {
            flex: 1,
            overflowY: 'auto',
            padding: '8px',
          },
        },
        filteredNodes.length === 0
          ? m('.pf-empty-state',
              {
                style: {
                  padding: '20px',
                  textAlign: 'center',
                  color: 'var(--pf-color-text-secondary)',
                },
              },
              'No nodes found'
            )
          : filteredNodes.map((nodeId) => {
              const stats = nodeStats.get(nodeId);
              const isSelected = this.selectedNodeId === nodeId;

              return m('.pf-graph-node-item',
                {
                  key: nodeId,
                  style: {
                    padding: '8px 12px',
                    marginBottom: '4px',
                    background: isSelected
                      ? 'var(--pf-color-accent-light)'
                      : 'var(--pf-color-background)',
                    border: isSelected
                      ? '2px solid var(--pf-color-accent)'
                      : '1px solid var(--pf-color-border)',
                    borderRadius: '4px',
                    cursor: 'pointer',
                    transition: 'all 0.2s',
                  },
                  onclick: () => {
                    this.selectedNodeId = nodeId;
                    this.showSidePanel = true;

                    // Focus on the selected node in the graph
                    const node = this.nodes.get(nodeId);
                    if (node) {
                      // Center the view on the selected node
                      const canvas = document.querySelector('.pf-canvas') as HTMLElement;
                      if (canvas) {
                        // Trigger redraw to update selection
                        raf.scheduleFullRedraw();
                      }
                    }
                  },
                },
                m('.pf-node-item-header',
                  {
                    style: {
                      display: 'flex',
                      justifyContent: 'space-between',
                      alignItems: 'center',
                      marginBottom: '4px',
                    },
                  },
                  m('strong',
                    {
                      style: {
                        fontSize: '13px',
                        wordBreak: 'break-all',
                      },
                    },
                    nodeId
                  ),
                ),
                m('.pf-node-item-stats',
                  {
                    style: {
                      display: 'flex',
                      gap: '12px',
                      fontSize: '11px',
                      color: 'var(--pf-color-text-secondary)',
                    },
                  },
                  m('span', `↓ ${stats?.incoming || 0}`),
                  m('span', `↑ ${stats?.outgoing || 0}`),
                ),
              );
            }),
      ),
    );
  }

  private renderSidePanel() {
    if (!this.selectedNodeId || !this.showSidePanel) return null;

    const nodeDetails = this.getNodeDetails(this.selectedNodeId);

    return m(
      '.pf-graph-side-panel',
      {
        style: {
          width: `${this.sidePanelWidth}px`,
          position: 'absolute',
          right: 0,
          top: 0,
          bottom: 0,
          background: 'var(--pf-color-background-secondary)',
          borderLeft: '1px solid var(--pf-color-border)',
          display: 'flex',
          flexDirection: 'column',
          zIndex: 10,
          overflow: 'hidden',
        },
      },
      // Resize handle
      m('.pf-graph-panel-resize-handle', {
        style: {
          position: 'absolute',
          left: 0,
          top: 0,
          bottom: 0,
          width: '5px',
          cursor: 'ew-resize',
          background: this.isDraggingPanel
            ? 'var(--pf-color-accent)'
            : 'transparent',
        },
        onmousedown: (e: MouseEvent) => {
          e.preventDefault();
          this.isDraggingPanel = true;
          const startX = e.clientX;
          const startWidth = this.sidePanelWidth;

          const handleMouseMove = (e: MouseEvent) => {
            const delta = startX - e.clientX;
            this.sidePanelWidth = Math.max(
              200,
              Math.min(600, startWidth + delta),
            );
            raf.scheduleFullRedraw();
          };

          const handleMouseUp = () => {
            this.isDraggingPanel = false;
            document.removeEventListener('mousemove', handleMouseMove);
            document.removeEventListener('mouseup', handleMouseUp);
            raf.scheduleFullRedraw();
          };

          document.addEventListener('mousemove', handleMouseMove);
          document.addEventListener('mouseup', handleMouseUp);
        },
      }),
      // Panel header
      m(
        '.pf-graph-panel-header',
        {
          style: {
            padding: '12px',
            borderBottom: '1px solid var(--pf-color-border)',
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
          },
        },
        m('h3', {style: {margin: 0, fontSize: '16px'}}, 'Node Details'),
        m(Button, {
          icon: Icons.Close,
          minimal: true,
          compact: true,
          onclick: () => {
            this.showSidePanel = false;
            raf.scheduleFullRedraw();
          },
        }),
      ),
      // Panel content
      m(
        '.pf-graph-panel-content',
        {
          style: {
            padding: '12px',
            overflowY: 'auto',
            flex: 1,
          },
        },
        m(
          Section,
          {title: 'Node Information'},
          m(
            '.pf-details-table',
            m(
              'table',
              {style: {width: '100%'}},
              m(
                'tr',
                m(
                  'td',
                  {style: {fontWeight: 'bold', paddingRight: '12px'}},
                  'ID:',
                ),
                m('td', nodeDetails.id),
              ),
              m(
                'tr',
                m(
                  'td',
                  {style: {fontWeight: 'bold', paddingRight: '12px'}},
                  'Total Connections:',
                ),
                m('td', nodeDetails.totalConnections),
              ),
            ),
          ),
        ),
        nodeDetails.incomingEdges.length > 0 &&
          m(
            Section,
            {title: `Incoming Edges (${nodeDetails.incomingEdges.length})`},
            m(
              '.pf-edge-list',
              {style: {maxHeight: '200px', overflowY: 'auto'}},
              nodeDetails.incomingEdges.map((edge) =>
                m(
                  '.pf-edge-item',
                  {
                    style: {
                      padding: '4px 8px',
                      marginBottom: '4px',
                      background: 'var(--pf-color-background)',
                      borderRadius: '4px',
                      cursor: 'pointer',
                    },
                    onclick: () => {
                      this.selectedNodeId = edge.source;
                      raf.scheduleFullRedraw();
                    },
                  },
                  m(
                    'span',
                    {style: {color: 'var(--pf-color-text-secondary)'}},
                    'From: ',
                  ),
                  m('span', edge.source),
                ),
              ),
            ),
          ),
        nodeDetails.outgoingEdges.length > 0 &&
          m(
            Section,
            {title: `Outgoing Edges (${nodeDetails.outgoingEdges.length})`},
            m(
              '.pf-edge-list',
              {style: {maxHeight: '200px', overflowY: 'auto'}},
              nodeDetails.outgoingEdges.map((edge) =>
                m(
                  '.pf-edge-item',
                  {
                    style: {
                      padding: '4px 8px',
                      marginBottom: '4px',
                      background: 'var(--pf-color-background)',
                      borderRadius: '4px',
                      cursor: 'pointer',
                    },
                    onclick: () => {
                      this.selectedNodeId = edge.dest;
                      raf.scheduleFullRedraw();
                    },
                  },
                  m(
                    'span',
                    {style: {color: 'var(--pf-color-text-secondary)'}},
                    'To: ',
                  ),
                  m('span', edge.dest),
                ),
              ),
            ),
          ),
      ),
    );
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
      selectedNodeIds: this.selectedNodeId
        ? new Set([this.selectedNodeId])
        : new Set(),
      toolbarItems: [
        m(Button, {
          label: 'Nodes List',
          icon: Icons.List,
          onclick: () => {
            this.showLeftPanel = !this.showLeftPanel;
            raf.scheduleFullRedraw();
          },
        }),
        m(Button, {
          label: 'Auto Layout (DFS)',
          icon: 'account_tree',
          onclick: () => this.autoLayoutDFS(),
        }),
        this.selectedNodeId &&
          m(Button, {
            label: 'Show Details',
            icon: Icons.Info,
            onclick: () => {
              this.showSidePanel = true;
              raf.scheduleFullRedraw();
            },
          }),
      ],
      onNodeMove: (nodeId: string, x: number, y: number) => {
        const node = this.nodes.get(nodeId);
        if (node) {
          node.x = x;
          node.y = y;
        }
      },
      onNodeSelect: (nodeId: string) => {
        this.selectedNodeId = nodeId;
        this.showSidePanel = true;
        raf.scheduleFullRedraw();
      },
      onSelectionClear: () => {
        this.selectedNodeId = undefined;
        raf.scheduleFullRedraw();
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
            'width: 100%; height: 100%; display: flex; flex-direction: column; position: relative;',
        },
        m(NodeGraph, {
          ...nodeGraphAttrs,
          style: `flex: 1; width: 100%; height: 100%;
            padding-left: ${this.showLeftPanel ? this.leftPanelWidth : 0}px;
            padding-right: ${this.showSidePanel && this.selectedNodeId ? this.sidePanelWidth : 0}px;`,
        }),
        this.renderLeftPanel(),
        this.renderSidePanel(),
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
