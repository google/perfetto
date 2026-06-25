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

import {produce} from 'immer';
import m from 'mithril';
import {Icons} from '../../../base/semantic_icons';
import {Duration, Time} from '../../../base/time';
import {shortUuid} from '../../../base/uuid';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {DurationWidget} from '../../../components/widgets/duration';
import {Timestamp} from '../../../components/widgets/timestamp';
import type {Trace} from '../../../public/trace';
import {isIdType} from '../../../trace_processor/perfetto_sql_type';
import type {Row} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {HotkeyContext} from '../../../widgets/hotkey_context';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  type Connection,
  type Node,
  NodeGraph,
  type NodeGraphApi,
} from '../../../widgets/nodegraph';
// Connection is only used as the widget's wire type (for visual rendering
// and event callbacks). Connections are NOT stored in the graph model —
// they are derived from node.inputs arrays on each render.
import {Popup} from '../../../widgets/popup';
import {SplitPanel} from '../../../widgets/split_panel';
import {Tabs} from '../../../widgets/tabs';
import type {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import type {
  NodeData,
  NodeQueryBuilderStore,
  RootNodeData,
} from '../graph_model';
import {
  chainTail,
  findConnectedInputs,
  findDockedParent,
  flattenNodes,
  getColumnsForNode,
  getManifest,
  getManifestInputs,
  getOutputColumnsForNode,
  type GraphIndex,
} from '../graph_utils';
import {buildIR} from '../ir';
import {MaterializationService} from '../materialization';
import type {DetailsContext, RenderContext} from '../node_types';
import {CacheTab} from './cache_tab';
import {ColumnsTab} from './columns_tab';
import {GraphTab} from './graph_tab';
import {IrTab} from './materialization_tab';
import {SqlTab} from './sql_tab';

export interface SpaghettiPage {
  readonly trace: Trace;
  readonly sqlModules: SqlModules | undefined;
}

export function SpaghettiPage(): m.Component<SpaghettiPage> {
  let graphApi: NodeGraphApi | undefined;

  // Initialize store
  let store: NodeQueryBuilderStore = {
    nodes: [],
    labels: [],
  };

  // Cached index of root nodes by ID, rebuilt whenever store changes.
  let nodesIndex: Record<string, RootNodeData> = {};
  function rebuildIndex() {
    nodesIndex = {};
    for (const n of store.nodes) nodesIndex[n.id] = n;
  }

  // History management
  const history: NodeQueryBuilderStore[] = [store];
  let historyIndex = 0;

  // Selection state (separate from undo/redo history)
  const selectedNodeIds = new Set<string>();

  // Pinned node: when set, results panel always shows this node's query
  let pinnedNodeId: string | undefined;

  // O(1) root lookup via cached index. Rebuild only happens when store changes.
  function getActiveNodes(): Record<string, RootNodeData> {
    return nodesIndex;
  }

  function getFlatNodes(): GraphIndex {
    return flattenNodes(store.nodes);
  }

  // Trim trailing nulls from an inputs array so length reflects actual wiring.
  function trimInputs(inputs: (string | null)[]): (string | null)[] {
    let end = inputs.length;
    while (end > 0 && inputs[end - 1] === null) end--;
    return inputs.slice(0, end);
  }

  // Derive Connection[] from node inputs for the nodegraph widget.
  // All nodes have a single output (fromPort always 0); toPort is the index
  // into the destination node's inputs array.
  function getActiveConnections(): Connection[] {
    const result: Connection[] = [];
    for (const node of Object.values(getFlatNodes().nodes)) {
      const inputs = node.inputs ?? [];
      for (let i = 0; i < inputs.length; i++) {
        const fromId = inputs[i];
        if (fromId !== null && fromId !== undefined) {
          result.push({
            fromNode: fromId,
            fromPort: 0,
            toNode: node.id,
            toPort: i,
          });
        }
      }
    }
    return result;
  }

  let matService: MaterializationService | undefined;

  const STORAGE_KEY = 'perfettoSpaghetti';

  function serializeStore(s: NodeQueryBuilderStore): string {
    return JSON.stringify({
      nodes: s.nodes,
      labels: s.labels,
    });
  }

  function deserializeStore(json: string): NodeQueryBuilderStore {
    const obj = JSON.parse(json);
    return {
      nodes: Array.isArray(obj.nodes) ? obj.nodes : [],
      labels: obj.labels ?? [],
    };
  }

  function saveGraph() {
    try {
      localStorage.setItem(STORAGE_KEY, serializeStore(store));
      console.log('[QueryBuilder] Graph saved');
    } catch (e) {
      console.error('[QueryBuilder] Failed to save graph:', e);
    }
  }

  function loadGraph() {
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (!saved) {
        console.log('[QueryBuilder] No saved graph found');
        return;
      }
      store = deserializeStore(saved);
      rebuildIndex();
      history.splice(0, history.length, store);
      historyIndex = 0;
      selectedNodeIds.clear();
      pinnedNodeId = undefined;
      console.log('[QueryBuilder] Graph loaded');
    } catch (e) {
      console.error('[QueryBuilder] Failed to load graph:', e);
    }
  }

  // Update store with history
  const updateStore = (updater: (draft: NodeQueryBuilderStore) => void) => {
    const newStore = produce(store, updater);
    store = newStore;
    rebuildIndex();

    if (historyIndex < history.length - 1) {
      history.splice(historyIndex + 1);
    }

    history.push(store);
    historyIndex = history.length - 1;

    if (history.length > 50) {
      history.shift();
      historyIndex--;
    }

    saveGraph();
  };

  const undo = () => {
    if (historyIndex > 0) {
      historyIndex--;
      store = history[historyIndex];
      rebuildIndex();
      saveGraph();
    }
  };

  const redo = () => {
    if (historyIndex < history.length - 1) {
      historyIndex++;
      store = history[historyIndex];
      rebuildIndex();
      saveGraph();
    }
  };

  const canUndo = () => historyIndex > 0;
  const canRedo = () => historyIndex < history.length - 1;

  const updateNode = (
    nodeId: string,
    updates: Partial<Omit<RootNodeData, 'id'>>,
  ) => {
    updateStore((draft) => {
      const node = draft.nodes.find((n) => n.id === nodeId);
      if (node) {
        Object.assign(node, updates);
      }
    });
  };

  const removeNode = (nodeId: string) => {
    updateStore((draft) => {
      const idx = draft.nodes.findIndex((n) => n.id === nodeId);
      if (idx === -1) return;

      // Clear any inputs referencing this node across all nodes.
      for (const node of Object.values(flattenNodes(draft.nodes).nodes)) {
        if (!node.inputs) continue;
        for (let i = 0; i < node.inputs.length; i++) {
          if (node.inputs[i] === nodeId) node.inputs[i] = null;
        }
        node.inputs = trimInputs(node.inputs);
      }

      draft.nodes.splice(idx, 1);
    });

    selectedNodeIds.delete(nodeId);
    if (pinnedNodeId === nodeId) pinnedNodeId = undefined;
  };

  // Load the graph from localStorage on initialization
  loadGraph();

  // --- Clipboard support ---

  interface ClipboardEntry {
    node: RootNodeData;
    relativeX: number;
    relativeY: number;
  }

  let clipboard: ClipboardEntry[] | undefined;

  function copySelectedNodes() {
    if (selectedNodeIds.size === 0) return;

    const activeNodes = getActiveNodes();
    const selected: RootNodeData[] = [];
    for (const id of selectedNodeIds) {
      const node = activeNodes[id];
      if (node) selected.push(node);
    }
    if (selected.length === 0) return;

    const minX = Math.min(...selected.map((n) => n.x));
    const minY = Math.min(...selected.map((n) => n.y));

    clipboard = selected.map((n) => ({
      node: structuredClone(n),
      relativeX: n.x - minX,
      relativeY: n.y - minY,
    }));
  }

  function pasteNodes() {
    if (!clipboard || clipboard.length === 0) return;

    const pasteOffset = 50;
    const oldToNew = new Map<string, string>();

    // Build new nodes with fresh IDs, collecting old→new ID mapping.
    const newNodes: RootNodeData[] = clipboard.map((entry) => {
      const newId = shortUuid();
      oldToNew.set(entry.node.id, newId);
      // Also register chain node IDs so inputs within chains get remapped.
      let cur = entry.node.next;
      while (cur) {
        const chainId = shortUuid();
        oldToNew.set(cur.id, chainId);
        cur = cur.next;
      }
      return {
        ...structuredClone(entry.node),
        id: newId,
        x: entry.relativeX + pasteOffset,
        y: entry.relativeY + pasteOffset,
      };
    });

    // Remap chain node IDs and inputs throughout each new root.
    function remapNode(node: NodeData): void {
      const newId = oldToNew.get(node.id);
      if (newId) (node as {id: string}).id = newId;
      if (node.inputs) {
        node.inputs = node.inputs.map((id) =>
          id !== null ? oldToNew.get(id) ?? id : null,
        );
      }
      if (node.next) remapNode(node.next);
    }
    for (const node of newNodes) remapNode(node);

    updateStore((draft) => {
      for (const node of newNodes) draft.nodes.push(node);
    });

    selectedNodeIds.clear();
    for (const node of newNodes) selectedNodeIds.add(node.id);
  }

  function cutSelectedNodes() {
    copySelectedNodes();
    for (const id of [...selectedNodeIds]) {
      removeNode(id);
    }
  }

  const addNode = (type: string, toNodeId?: string) => {
    const manifest = getManifest(type);
    const id = shortUuid();

    let x: number;
    let y: number;

    if (graphApi && !toNodeId) {
      const defaultConfig = manifest.defaultConfig();
      const defaultInputs = (manifest.getInputs?.(defaultConfig) ?? []).map(
        (p) => ({...p, direction: 'left' as const}),
      );
      const placement = graphApi.findPlacementForNode({
        id,
        inputs: defaultInputs,
        outputs: [{content: 'Output', direction: 'right' as const}],
        content: m('span', type),
        canDockBottom: true,
        canDockTop: defaultInputs.length > 0,
        titleBar: {
          title: manifest.title,
          icon: manifest.resolveIcon?.(defaultConfig) ?? manifest.icon,
        },
        hue: manifest.hue,
      });
      x = placement.x;
      y = placement.y;
    } else {
      x = 100 + Math.random() * 200;
      y = 50 + Math.random() * 200;
    }

    updateStore((draft) => {
      if (toNodeId) {
        // Insert new chain node right after toNodeId in its chain.
        const parentNode = draft.nodes.find((n) => n.id === toNodeId);
        if (parentNode) {
          const chainNode: NodeData = {
            type,
            id,
            config: manifest.defaultConfig(),
          };
          chainNode.next = parentNode.next;
          parentNode.next = chainNode;

          // Any node that had toNodeId as a wired input now routes through id.
          for (const node of Object.values(flattenNodes(draft.nodes).nodes)) {
            if (!node.inputs) continue;
            for (let i = 0; i < node.inputs.length; i++) {
              if (node.inputs[i] === toNodeId) node.inputs[i] = id;
            }
          }
        }
      } else {
        draft.nodes.push({
          type,
          id,
          x,
          y,
          config: manifest.defaultConfig(),
        });
      }
    });
  };

  function renderTitleBarActions(): m.Children {
    return [];
  }

  // Build RenderContext and dispatch to manifest.render.
  function renderNodeContentWithContext(
    nodeData: NodeData,
    tableNames: string[],
    sqlModules: SqlModules | undefined,
    trace: Trace,
  ): m.Children {
    const flatNodes = getFlatNodes();
    const manifest = getManifest(nodeData.type);

    // Compute available columns (input columns from upstream parent).
    const availableColumns = getColumnsForNode(
      flatNodes,
      nodeData.id,
      sqlModules,
    );

    // Build a map from port label → connected input node for column resolution.
    const portInputs = new Map<string, NodeData>();
    const parent = findDockedParent(flatNodes, nodeData.id);
    const connected = findConnectedInputs(flatNodes, nodeData.id);
    const ports = getManifestInputs(manifest, nodeData);
    for (let i = 0; i < ports.length; i++) {
      const input = i === 0 && parent ? parent : connected.get(i);
      if (input) {
        portInputs.set(ports[i].name, input);
      }
    }

    const ctx: RenderContext = {
      availableColumns,
      tableNames,
      trace,
      isSelected: selectedNodeIds.has(nodeData.id),
      inputPorts: ports,
      getInputColumns(portLabel: string) {
        const input = portInputs.get(portLabel);
        if (!input) return [];
        return getOutputColumnsForNode(flatNodes, input.id, sqlModules) ?? [];
      },
    };

    const updateConfig = (updates: {}) => {
      updateStore((draft) => {
        // Search root nodes and their chains for the node to update.
        function updateInChain(node: NodeData): boolean {
          if (node.id === nodeData.id) {
            node.config = {...node.config, ...updates};
            return true;
          }
          return node.next ? updateInChain(node.next) : false;
        }
        for (const root of draft.nodes) {
          if (updateInChain(root)) return;
        }
      });
    };

    return manifest.render(nodeData.config, updateConfig, ctx);
  }

  function buildNodeModel(
    nodeData: NodeData,
    tableNames: string[],
    trace: Trace,
    sqlModules: SqlModules | undefined,
    isRoot = false,
  ): Omit<Node, 'x' | 'y'> {
    const manifest = getManifest(nodeData.type);

    return {
      id: nodeData.id,
      inputs: getManifestInputs(manifest, nodeData).map((p) => ({
        ...p,
        direction: 'left' as const,
      })),
      outputs: [{content: 'Output', direction: 'right' as const}],
      content: renderNodeContentWithContext(
        nodeData,
        tableNames,
        sqlModules,
        trace,
      ),
      canDockBottom: true,
      canDockTop: getManifestInputs(manifest, nodeData).length > 0,
      next: nodeData.next
        ? buildNodeModel(nodeData.next, tableNames, trace, sqlModules, false)
        : undefined,
      titleBar: {
        title: manifest.title,
        icon: manifest.resolveIcon?.(nodeData.config) ?? manifest.icon,
        actions: renderTitleBarActions(),
      },
      hue: manifest.hue,
      contextMenuItems: [
        m(MenuItem, {
          label: pinnedNodeId === nodeData.id ? 'Unpin node' : 'Pin node',
          icon: 'push_pin',
          onclick: () => {
            pinnedNodeId =
              pinnedNodeId === nodeData.id ? undefined : nodeData.id;
          },
        }),
        m(MenuItem, {
          label: 'Duplicate',
          icon: 'content_copy',
          onclick: () => {
            const newId = shortUuid();
            const rootData = nodeData as Partial<RootNodeData>;
            updateStore((draft) => {
              draft.nodes.push({
                type: nodeData.type,
                id: newId,
                x: (rootData.x ?? 100) + 50,
                y: (rootData.y ?? 100) + 50,
                config: structuredClone(nodeData.config),
              });
            });
          },
        }),
        m(MenuItem, {
          label: 'Delete',
          icon: 'delete',
          onclick: () => removeNode(nodeData.id),
        }),
      ],
      collapsed: isRoot ? nodeData.collapsed : undefined,
      collapsible: isRoot,
      className: matService?.materializingNodeIds.has(nodeData.id)
        ? 'pf-materializing'
        : matService?.fadingOutNodeIds.has(nodeData.id)
          ? 'pf-materializing-fade-out'
          : undefined,
    };
  }

  // Render a node and its docked chain
  function renderNodeChain(
    nodeData: RootNodeData,
    tableNames: string[],
    trace: Trace,
    sqlModules: SqlModules | undefined,
  ): Node {
    const model = buildNodeModel(nodeData, tableNames, trace, sqlModules, true);
    return {
      ...model,
      x: nodeData.x,
      y: nodeData.y,
    };
  }

  return {
    onremove(_: m.VnodeDOM<SpaghettiPage>) {
      matService?.dispose();
    },
    view({attrs}: m.Vnode<SpaghettiPage>) {
      const {trace, sqlModules} = attrs;

      // Get table names from SqlModules, falling back to a basic list
      const tableNames = sqlModules
        ? sqlModules.listTablesNames().sort()
        : ['slice', 'sched', 'thread', 'process'];

      // Build the rendered nodes list from the active view.
      const flatNodes = getFlatNodes();
      const activeConns = getActiveConnections();

      const renderedNodes: Node[] = store.nodes.map((n) =>
        renderNodeChain(n, tableNames, trace, sqlModules),
      );

      const toolbarItems: m.Children[] = [];

      function nodeMenuItem(type: string): m.Children {
        const manifest = getManifest(type);
        return m(MenuItem, {
          label: manifest.title,
          icon: manifest.icon,
          style: {borderLeft: `3px solid hsl(${manifest.hue}, 60%, 65%)`},
          onclick: () => addNode(type),
        });
      }

      const nodeTypes: string[] = [
        'from',
        'time_range',
        'select',
        'extend',
        'drop',
        'filter',
        'extract_arg',
        'sort',
        'limit',
        'groupby',
        'join',
        'union',
        'interval_intersect',
        'chart',
        'sql',
      ];
      const addNodeMenuItems = nodeTypes.map(nodeMenuItem);

      toolbarItems.push(
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
              label: 'Add Node',
              icon: 'add',
            }),
          },
          addNodeMenuItems,
        ),
      );

      if (pinnedNodeId !== undefined) {
        const pinnedNode = flatNodes.nodes[pinnedNodeId];
        const pinnedLabel = pinnedNode
          ? getManifest(pinnedNode.type)?.title ?? pinnedNode.type
          : pinnedNodeId;
        toolbarItems.push(
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'push_pin',
            label: pinnedLabel,
            title: 'Unpin node',
            onclick: () => {
              pinnedNodeId = undefined;
            },
          }),
        );
      }

      toolbarItems.push(
        m('div', {style: {flex: '1'}}),
        m(
          Popup,
          {
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'delete_sweep',
              title: 'Clear workspace',
            }),
          },
          m('.pf-qb-stack', [
            m('span', 'Are you sure you want to clear everything?'),
            m(
              `.${Popup.DISMISS_POPUP_GROUP_CLASS}`,
              {
                style: {
                  display: 'flex',
                  gap: '4px',
                  justifyContent: 'flex-end',
                },
              },
              [
                m(Button, {
                  label: 'Cancel',
                  className: Popup.DISMISS_POPUP_GROUP_CLASS,
                }),
                m(Button, {
                  label: 'Clear',
                  variant: ButtonVariant.Filled,
                  intent: Intent.Danger,
                  className: Popup.DISMISS_POPUP_GROUP_CLASS,
                  onclick: () => {
                    store = {
                      nodes: [],
                      labels: [],
                    };
                    history.splice(0, history.length, store);
                    historyIndex = 0;
                    selectedNodeIds.clear();
                    pinnedNodeId = undefined;
                  },
                }),
              ],
            ),
          ]),
        ),
        m(
          ButtonGroup,
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'save',
            onclick: saveGraph,
          }),
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'folder_open',
            onclick: loadGraph,
          }),
        ),
        m(
          ButtonGroup,
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'undo',
            disabled: !canUndo(),
            onclick: undo,
          }),
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'redo',
            disabled: !canRedo(),
            onclick: redo,
          }),
        ),
      );

      const graphPanel = m(NodeGraph, {
        nodes: renderedNodes,
        connections: activeConns,
        labels: store.labels,
        selectedNodeIds,
        fillHeight: true,
        contextMenuOnHover: true,
        toolbarItems,
        onReady: (api: NodeGraphApi) => {
          graphApi = api;
        },
        onCopy: () => copySelectedNodes(),
        onPaste: () => pasteNodes(),
        onCut: () => cutSelectedNodes(),
        onConnect: (conn: Connection) => {
          updateStore((draft) => {
            const flat = flattenNodes(draft.nodes);
            const target = flat.nodes[conn.toNode];
            if (!target) return;
            if (!target.inputs) target.inputs = [];
            while (target.inputs.length <= conn.toPort) {
              target.inputs.push(null);
            }
            target.inputs[conn.toPort] = conn.fromNode;
            target.inputs = trimInputs(target.inputs);
          });
        },
        onConnectionRemove: (index: number) => {
          const conn = activeConns[index];
          if (!conn) return;
          updateStore((draft) => {
            const flat = flattenNodes(draft.nodes);
            const target = flat.nodes[conn.toNode];
            if (target?.inputs && conn.toPort < target.inputs.length) {
              target.inputs[conn.toPort] = null;
              target.inputs = trimInputs(target.inputs);
            }
          });
        },
        onNodeMove: (nodeId: string, x: number, y: number) => {
          updateNode(nodeId, {x, y});
        },
        onNodeRemove: (nodeId: string) => {
          removeNode(nodeId);
        },
        onNodeSelect: (nodeId: string) => {
          selectedNodeIds.clear();
          selectedNodeIds.add(nodeId);
        },
        onNodeAddToSelection: (nodeId: string) => {
          selectedNodeIds.add(nodeId);
        },
        onNodeRemoveFromSelection: (nodeId: string) => {
          selectedNodeIds.delete(nodeId);
        },
        onSelectionClear: () => {
          selectedNodeIds.clear();
        },
        onDock: (targetId: string, childNode: Omit<Node, 'x' | 'y'>) => {
          updateStore((draft) => {
            const child = draft.nodes.find((n) => n.id === childNode.id);
            if (!child) return;

            // Target may be a root or a chain node — search all chains.
            let targetNode: NodeData | undefined;
            outer: for (const root of draft.nodes) {
              let cur: NodeData = root;
              while (true) {
                if (cur.id === targetId) {
                  targetNode = cur;
                  break outer;
                }
                if (!cur.next) break;
                cur = cur.next;
              }
            }

            if (targetNode) {
              // Strip x/y — chain nodes don't have canvas positions.
              const {x: _x, y: _y, ...chainNode} = child as RootNodeData;
              chainTail(targetNode).next = chainNode;
              const idx = draft.nodes.findIndex((n) => n.id === childNode.id);
              if (idx >= 0) draft.nodes.splice(idx, 1);
            }

            // Remove any wired inputs between target and child (docking is
            // now the implicit connection, so explicit wires are redundant).
            const flat = flattenNodes(draft.nodes);
            for (const id of [targetId, childNode.id]) {
              const n = flat.nodes[id];
              if (!n?.inputs) continue;
              const otherId = id === targetId ? childNode.id : targetId;
              for (let i = 0; i < n.inputs.length; i++) {
                if (n.inputs[i] === otherId) n.inputs[i] = null;
              }
            }
          });
        },
        onUndock: (parentId: string, nodeId: string, x: number, y: number) => {
          updateStore((draft) => {
            // Parent may be a root or chain node — search all chains.
            let parentNode: NodeData | undefined;
            outer: for (const root of draft.nodes) {
              let cur: NodeData = root;
              while (true) {
                if (cur.id === parentId) {
                  parentNode = cur;
                  break outer;
                }
                if (!cur.next) break;
                cur = cur.next;
              }
            }
            if (!parentNode) return;

            // Find the child in parentNode's chain and split there.
            let cur: NodeData = parentNode;
            while (cur.next) {
              if (cur.next.id === nodeId) {
                const detached = cur.next;
                cur.next = undefined;
                // Re-add detached node as a new root.
                draft.nodes.push({
                  ...detached,
                  x,
                  y,
                } as RootNodeData);
                return;
              }
              cur = cur.next;
            }
          });
        },
        onNodeCollapse: (nodeId: string, collapsed: boolean) => {
          updateStore((draft) => {
            for (const root of draft.nodes) {
              let cur: NodeData = root;
              while (true) {
                if (cur.id === nodeId) {
                  cur.collapsed = collapsed || undefined;
                  return;
                }
                if (!cur.next) break;
                cur = cur.next;
              }
            }
          });
        },
        onLabelMove: (labelId: string, x: number, y: number) => {
          updateStore((draft) => {
            const label = draft.labels.find((l) => l.id === labelId);
            if (label) {
              label.x = x;
              label.y = y;
            }
          });
        },
        onLabelResize: (labelId: string, width: number) => {
          updateStore((draft) => {
            const label = draft.labels.find((l) => l.id === labelId);
            if (label) {
              label.width = width;
            }
          });
        },
        onLabelRemove: (labelId: string) => {
          updateStore((draft) => {
            const idx = draft.labels.findIndex((l) => l.id === labelId);
            if (idx !== -1) {
              draft.labels.splice(idx, 1);
            }
          });
          selectedNodeIds.delete(labelId);
        },
      });

      // Use pinned node if set, otherwise fall back to selected node.
      const activeNodeId =
        pinnedNodeId ??
        (selectedNodeIds.size === 1
          ? (selectedNodeIds.values().next().value as string)
          : undefined);

      // Lazily create the materialization service.
      if (!matService) {
        matService = new MaterializationService(trace.engine);
      }

      // Schedule materialization on every render — the AsyncLimiter
      // ensures only the latest invocation actually runs.
      matService.scheduleUpdate(store, activeNodeId, sqlModules);

      const displaySql = matService.displaySql;
      const dataSource = matService.dataSource;
      const matError = matService.error;
      const queryReport = matService.queryReport;
      const cacheEntries = matService.cacheEntries;

      // Build DataGrid schema from the node's output columns.
      const outputColumns = activeNodeId
        ? getOutputColumnsForNode(flatNodes, activeNodeId, sqlModules)
        : undefined;

      const sqlText = activeNodeId
        ? displaySql ?? 'Incomplete query — fill in all required fields'
        : 'Select a node to preview its SQL';

      // Build IR entries for the IR tab (pure function of graph, always fresh).
      const irEntries = activeNodeId
        ? buildIR(flatNodes, activeNodeId, sqlModules) ?? []
        : [];

      // Index report entries by hash for O(1) lookup per IR block.
      const reportByHash = new Map(
        (queryReport?.entries ?? []).map((e) => [e.hash, e]),
      );

      const liveJson = JSON.stringify(
        JSON.parse(serializeStore(store)),
        null,
        2,
      );

      const sqlPanel = m(
        '',
        {
          style: {
            display: 'flex',
            flexDirection: 'column',
            height: '100%',
            background: 'var(--surface)',
          },
        },
        m(Tabs, {
          tabs: [
            {
              key: 'sql',
              title: 'SQL',
              content: m(SqlTab, {displaySql, sqlText}),
            },
            {
              key: 'columns',
              title: 'Columns',
              content: m(ColumnsTab, {outputColumns, activeNodeId}),
            },
            {
              key: 'materialization',
              title: 'Materialization',
              content: m(IrTab, {irEntries, reportByHash, activeNodeId}),
            },
            {
              key: 'cache',
              title: `Cache (${cacheEntries.length})`,
              content: m(CacheTab, {
                cacheEntries,
                onClearCache: () => matService?.clearCache(),
              }),
            },
            {
              key: 'graph',
              title: 'Graph',
              content: m(GraphTab, {
                liveJson,
                onApply: (json: string) => {
                  store = deserializeStore(json);
                  history.splice(0, history.length, store);
                  historyIndex = 0;
                  selectedNodeIds.clear();
                  pinnedNodeId = undefined;
                  saveGraph();
                },
              }),
            },
          ],
        }),
      );

      const datagridSchema: SchemaRegistry = {
        query: Object.fromEntries(
          (outputColumns ?? []).map((col) => {
            if (col.type && isIdType(col.type)) {
              const tableName = col.type.source.table;
              return [
                col.name,
                {
                  cellRenderer: (value: Row[string]) => {
                    const cell = renderCell(value, col.name);
                    if (
                      typeof value !== 'bigint' &&
                      typeof value !== 'number'
                    ) {
                      return cell;
                    }
                    const id =
                      typeof value === 'bigint' ? Number(value) : value;
                    return m(
                      Anchor,
                      {
                        title: `Go to ${tableName} on the timeline`,
                        icon: Icons.UpdateSelection,
                        onclick: () => {
                          trace.navigate('#!/viewer');
                          trace.selection.selectSqlEvent(tableName, id, {
                            switchToCurrentSelectionTab: false,
                            scrollToSelection: true,
                          });
                        },
                      },
                      cell,
                    );
                  },
                },
              ];
            }
            if (col.type?.kind === 'timestamp') {
              return [
                col.name,
                {
                  cellRenderer: (value: Row[string]) => {
                    if (typeof value === 'bigint') {
                      return m(Timestamp, {trace, ts: Time.fromRaw(value)});
                    }
                    return renderCell(value, col.name);
                  },
                },
              ];
            }
            if (col.type?.kind === 'duration') {
              return [
                col.name,
                {
                  cellRenderer: (value: Row[string]) => {
                    if (typeof value === 'bigint') {
                      return m(DurationWidget, {
                        trace,
                        dur: Duration.fromRaw(value),
                      });
                    }
                    return renderCell(value, col.name);
                  },
                },
              ];
            }
            return [col.name, {}];
          }),
        ),
      };

      function renderEmptyState(): m.Children {
        if (matError) {
          return m(
            EmptyState,
            {
              icon: 'warning',
              fillHeight: true,
              title: 'Query error',
            },
            m('pre.pf-node-query-builder-page__error', matError),
          );
        } else if (activeNodeId) {
          return m(
            EmptyState,
            {
              icon: 'warning',
              fillHeight: true,
              title: 'Incomplete query',
            },
            'Fill in all required fields to see results.',
          );
        } else {
          return m(
            EmptyState,
            {
              fillHeight: true,
              title: 'No node selected',
            },
            'Click on a node to see its query results.',
          );
        }
      }

      const activeNode = activeNodeId
        ? flatNodes.nodes[activeNodeId]
        : undefined;
      const activeManifest = activeNode
        ? getManifest(activeNode.type)
        : undefined;

      const detailsCtx: DetailsContext = {
        outputColumns,
        error: matError,
        trace,
        materializedTable: matService?.materializedTable,
      };

      const resultsPanel = activeManifest?.renderDetails
        ? activeManifest.renderDetails(activeNode!.config, detailsCtx)
        : dataSource
          ? m(DataGrid, {
              key: displaySql,
              data: dataSource,
              schema: datagridSchema,
              rootSchema: 'query',
              fillHeight: true,
            })
          : renderEmptyState();

      const bottomPanel = m(SplitPanel, {
        direction: 'vertical',
        controlledPanel: 'first',
        initialSplit: {percent: 30},
        minSize: 50,
        firstPanel: sqlPanel,
        secondPanel: resultsPanel,
      });

      return m(
        '.pf-node-query-builder-page',
        {
          style: {
            display: 'flex',
            flexDirection: 'column',
            height: '100%',
          },
        },
        m(SplitPanel, {
          direction: 'horizontal',
          controlledPanel: 'second',
          initialSplit: {percent: 40},
          minSize: 100,
          firstPanel: m(
            HotkeyContext,
            {
              fillHeight: true,
              showFocusRing: true,
              hotkeys: [
                {
                  hotkey: 'Mod+Z',
                  callback: () => undo(),
                },
                {
                  hotkey: 'Mod+Shift+Z',
                  callback: () => redo(),
                },
              ],
            },
            graphPanel,
          ),
          secondPanel: bottomPanel,
        }),
      );
    },
  };
}
