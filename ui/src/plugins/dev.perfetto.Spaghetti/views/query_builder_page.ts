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
import {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {DurationWidget} from '../../../components/widgets/duration';
import {Timestamp} from '../../../components/widgets/timestamp';
import {Trace} from '../../../public/trace';
import {isIdType} from '../../../trace_processor/perfetto_sql_type';
import {Row} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {HotkeyContext} from '../../../widgets/hotkey_context';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  Connection,
  Node,
  NodeGraph,
  NodeGraphApi,
} from '../../../widgets/nodegraph';
import {Popup} from '../../../widgets/popup';
import {SplitPanel} from '../../../widgets/split_panel';
import {Tabs} from '../../../widgets/tabs';
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {ManifestPort, NodeData, NodeQueryBuilderStore} from '../graph_model';
import {
  findConnectedInputs,
  findDockedParent,
  getColumnsForNode,
  getManifest,
  getManifestInputs,
  getOutputColumnsForNode,
  getRootNodeIds,
} from '../graph_utils';
import type NodeQueryBuilderPlugin from '../index';
import type {QueryBuilderDelegate} from '../index';
import {buildIR} from '../ir';
import {MaterializationService} from '../materialization';
import {DetailsContext, RenderContext} from '../node_types';
import {renderCacheTab} from './cache_tab';
import {renderColumnsTab} from './columns_tab';
import {GraphTab} from './graph_tab';
import {renderIrTab} from './ir_tab';
import {renderSqlTab} from './sql_tab';

export interface QueryBuilderPageAttrs {
  readonly trace: Trace;
  readonly sqlModules: SqlModules | undefined;
  readonly plugin?: NodeQueryBuilderPlugin;
}

export function QueryBuilderPage(
  _initialVnode: m.Vnode<QueryBuilderPageAttrs>,
): m.Component<QueryBuilderPageAttrs> {
  let graphApi: NodeGraphApi | undefined;

  // Initialize store
  let store: NodeQueryBuilderStore = {
    nodes: {},
    connections: [],
    labels: [],
  };

  // History management
  const history: NodeQueryBuilderStore[] = [store];
  let historyIndex = 0;

  // Selection state (separate from undo/redo history)
  const selectedNodeIds = new Set<string>();

  // Pinned node: when set, results panel always shows this node's query
  let pinnedNodeId: string | undefined;

  // Helpers to get the nodes/connections for the currently active view.
  function getActiveNodes(): Record<string, NodeData> {
    return store.nodes;
  }

  function getActiveConnections(): Connection[] {
    return store.connections;
  }

  let matService: MaterializationService | undefined;

  const STORAGE_KEY = 'perfettoSpaghetti';

  function serializeStore(s: NodeQueryBuilderStore): string {
    return JSON.stringify({
      nodes: s.nodes,
      connections: s.connections,
      labels: s.labels,
    });
  }

  function deserializeStore(json: string): NodeQueryBuilderStore {
    const obj = JSON.parse(json);
    return {
      nodes: obj.nodes ?? {},
      connections: obj.connections ?? [],
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
      saveGraph();
    }
  };

  const redo = () => {
    if (historyIndex < history.length - 1) {
      historyIndex++;
      store = history[historyIndex];
      saveGraph();
    }
  };

  const canUndo = () => historyIndex > 0;
  const canRedo = () => historyIndex < history.length - 1;

  const updateNode = (
    nodeId: string,
    updates: Partial<Omit<NodeData, 'id'>>,
  ) => {
    updateStore((draft) => {
      const node = draft.nodes[nodeId];
      if (node) {
        Object.assign(node, updates);
      }
    });
  };

  const removeNode = (nodeId: string) => {
    updateStore((draft) => {
      const nodeToDelete = draft.nodes[nodeId];
      if (!nodeToDelete) return;

      for (const parent of Object.values(draft.nodes)) {
        if (parent.nextId === nodeId) {
          parent.nextId = nodeToDelete.nextId;
        }
      }

      for (let i = draft.connections.length - 1; i >= 0; i--) {
        const c = draft.connections[i];
        if (c.fromNode === nodeId || c.toNode === nodeId) {
          draft.connections.splice(i, 1);
        }
      }

      delete draft.nodes[nodeId];
    });

    selectedNodeIds.delete(nodeId);
    if (pinnedNodeId === nodeId) pinnedNodeId = undefined;
  };

  // Load the graph from localStorage on initialization
  loadGraph();

  // --- Clipboard support ---

  interface ClipboardEntry {
    node: NodeData;
    relativeX: number;
    relativeY: number;
  }

  interface ClipboardConnection {
    fromIndex: number;
    toIndex: number;
    fromPort: number;
    toPort: number;
  }

  interface ClipboardDock {
    parentIndex: number;
    childIndex: number;
  }

  let clipboard:
    | {
        nodes: ClipboardEntry[];
        connections: ClipboardConnection[];
        docks: ClipboardDock[];
      }
    | undefined;

  function copySelectedNodes() {
    if (selectedNodeIds.size === 0) return;

    const activeNodes = getActiveNodes();
    const activeConns = getActiveConnections();

    const selected: NodeData[] = [];
    const idToIndex = new Map<string, number>();
    for (const id of selectedNodeIds) {
      const node = activeNodes[id];
      if (node) {
        idToIndex.set(id, selected.length);
        selected.push(node);
      }
    }
    if (selected.length === 0) return;

    const minX = Math.min(...selected.map((n) => n.x));
    const minY = Math.min(...selected.map((n) => n.y));

    const clipNodes: ClipboardEntry[] = selected.map((n) => ({
      node: structuredClone(n),
      relativeX: n.x - minX,
      relativeY: n.y - minY,
    }));

    const clipConns: ClipboardConnection[] = [];
    for (const conn of activeConns) {
      const fi = idToIndex.get(conn.fromNode);
      const ti = idToIndex.get(conn.toNode);
      if (fi !== undefined && ti !== undefined) {
        clipConns.push({
          fromIndex: fi,
          toIndex: ti,
          fromPort: conn.fromPort,
          toPort: conn.toPort,
        });
      }
    }

    const clipDocks: ClipboardDock[] = [];
    for (const node of selected) {
      if (node.nextId && idToIndex.has(node.nextId)) {
        clipDocks.push({
          parentIndex: idToIndex.get(node.id)!,
          childIndex: idToIndex.get(node.nextId)!,
        });
      }
    }

    clipboard = {nodes: clipNodes, connections: clipConns, docks: clipDocks};
  }

  function pasteNodes() {
    if (!clipboard || clipboard.nodes.length === 0) return;

    const pasteOffset = 50;
    const newNodes: NodeData[] = clipboard.nodes.map((entry) => {
      const newId = shortUuid();
      return {
        ...structuredClone(entry.node),
        id: newId,
        x: entry.relativeX + pasteOffset,
        y: entry.relativeY + pasteOffset,
        nextId: undefined as string | undefined,
      };
    });

    for (const dock of clipboard.docks) {
      newNodes[dock.parentIndex].nextId = newNodes[dock.childIndex].id;
    }

    updateStore((draft) => {
      for (const node of newNodes) {
        draft.nodes[node.id] = node;
      }
      for (const conn of clipboard!.connections) {
        draft.connections.push({
          fromNode: newNodes[conn.fromIndex].id,
          fromPort: conn.fromPort,
          toNode: newNodes[conn.toIndex].id,
          toPort: conn.toPort,
        });
      }
    });

    selectedNodeIds.clear();
    for (const node of newNodes) {
      selectedNodeIds.add(node.id);
    }
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
      const placement = graphApi.findPlacementForNode({
        id,
        inputs: manifest.defaultInputs?.() ?? manifest.inputs,
        outputs: manifest.outputs,
        content: m('span', type),
        canDockBottom: manifest.canDockBottom,
        canDockTop: manifest.canDockTop,
        titleBar: {title: manifest.title, icon: manifest.icon},
        hue: manifest.hue,
      });
      x = placement.x;
      y = placement.y;
    } else {
      x = 100 + Math.random() * 200;
      y = 50 + Math.random() * 200;
    }

    const newNode: NodeData = {
      type,
      id,
      x,
      y,
      inputs: manifest.defaultInputs?.(),
      config: manifest.defaultConfig(),
    };

    updateStore((draft) => {
      draft.nodes[newNode.id] = newNode;

      if (toNodeId) {
        const parentNode = draft.nodes[toNodeId];
        if (parentNode) {
          newNode.nextId = parentNode.nextId;
          parentNode.nextId = id;
        }

        const bottomConnectionIdx = draft.connections.findIndex(
          (c: Connection) => c.fromNode === toNodeId && c.fromPort === 0,
        );
        if (bottomConnectionIdx > -1) {
          draft.connections[bottomConnectionIdx] = {
            ...draft.connections[bottomConnectionIdx],
            fromNode: id,
            fromPort: 0,
          };
        }
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
    const activeNodes = getActiveNodes();
    const activeConns = getActiveConnections();
    const manifest = getManifest(nodeData.type);

    // Compute available columns (input columns from upstream parent).
    const availableColumns = getColumnsForNode(
      activeNodes,
      activeConns,
      nodeData.id,
      sqlModules,
    );

    // Build a map from port label → connected input node for column resolution.
    const portInputs = new Map<string, NodeData>();
    const parent = findDockedParent(activeNodes, nodeData.id);
    const connected = findConnectedInputs(
      activeNodes,
      activeConns,
      nodeData.id,
    );
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
        return (
          getOutputColumnsForNode(
            activeNodes,
            activeConns,
            input.id,
            sqlModules,
          ) ?? []
        );
      },
      addInput: manifest.defaultInputs
        ? (port: ManifestPort) => {
            updateStore((draft) => {
              const node = draft.nodes[nodeData.id];
              if (node) {
                node.inputs = [...(node.inputs ?? []), port];
              }
            });
          }
        : undefined,
      removeLastInput: manifest.defaultInputs
        ? () => {
            updateStore((draft) => {
              const node = draft.nodes[nodeData.id];
              if (!node?.inputs || node.inputs.length <= 1) return;
              const portIdx = node.inputs.length - 1;
              for (let i = draft.connections.length - 1; i >= 0; i--) {
                const c = draft.connections[i];
                if (c.toNode === nodeData.id && c.toPort === portIdx) {
                  draft.connections.splice(i, 1);
                }
              }
              node.inputs = node.inputs.slice(0, portIdx);
            });
          }
        : undefined,
    };

    const updateConfig = (updates: {}) => {
      updateStore((draft) => {
        const node = draft.nodes[nodeData.id];
        if (node) {
          node.config = {...node.config, ...updates};
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
  ): Omit<Node, 'x' | 'y'> {
    const activeNodes = getActiveNodes();
    const nextModel = nodeData.nextId
      ? activeNodes[nodeData.nextId]
      : undefined;

    const manifest = getManifest(nodeData.type);

    return {
      id: nodeData.id,
      inputs: getManifestInputs(manifest, nodeData),
      outputs: manifest.outputs,
      content: renderNodeContentWithContext(
        nodeData,
        tableNames,
        sqlModules,
        trace,
      ),
      canDockBottom: manifest.canDockBottom,
      canDockTop: manifest.canDockTopDynamic
        ? manifest.canDockTopDynamic(nodeData)
        : manifest.canDockTop,
      next: nextModel
        ? buildNodeModel(nextModel, tableNames, trace, sqlModules)
        : undefined,
      titleBar: {
        title: manifest.title,
        icon: manifest.icon,
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
            updateStore((draft) => {
              draft.nodes[newId] = {
                ...structuredClone(nodeData),
                id: newId,
                x: nodeData.x + 50,
                y: nodeData.y + 50,
                nextId: undefined,
              };
            });
          },
        }),
        m(MenuItem, {
          label: 'Delete',
          icon: 'delete',
          onclick: () => removeNode(nodeData.id),
        }),
      ],
      collapsed: nodeData.collapsed,
      className: matService?.materializingNodeIds.has(nodeData.id)
        ? 'pf-materializing'
        : matService?.fadingOutNodeIds.has(nodeData.id)
          ? 'pf-materializing-fade-out'
          : undefined,
    };
  }

  // Render a node and its docked chain
  function renderNodeChain(
    nodeData: NodeData,
    tableNames: string[],
    trace: Trace,
    sqlModules: SqlModules | undefined,
  ): Node {
    const model = buildNodeModel(nodeData, tableNames, trace, sqlModules);
    return {
      ...model,
      x: nodeData.x,
      y: nodeData.y,
    };
  }

  return {
    oncreate({attrs}: m.VnodeDOM<QueryBuilderPageAttrs>) {
      if (attrs.plugin) {
        const delegate: QueryBuilderDelegate = {
          getStore: () => store,
          setStore: (newStore) => {
            store = newStore;
            history.splice(0, history.length, store);
            historyIndex = 0;
            selectedNodeIds.clear();
            pinnedNodeId = undefined;
            saveGraph();
            m.redraw();
          },
          serializeStore: () => serializeStore(store),
          deserializeAndSetStore: (json: string) => {
            store = deserializeStore(json);
            history.splice(0, history.length, store);
            historyIndex = 0;
            selectedNodeIds.clear();
            pinnedNodeId = undefined;
            saveGraph();
            m.redraw();
          },
          selectNode: (nodeId: string) => {
            selectedNodeIds.clear();
            selectedNodeIds.add(nodeId);
            m.redraw();
          },
          pinNode: (nodeId: string | undefined) => {
            pinnedNodeId = nodeId;
            if (nodeId) {
              selectedNodeIds.clear();
              selectedNodeIds.add(nodeId);
            }
            m.redraw();
          },
        };
        attrs.plugin.registerDelegate(delegate);
      }
    },
    onremove({attrs}: m.VnodeDOM<QueryBuilderPageAttrs>) {
      matService?.dispose();
      attrs.plugin?.unregisterDelegate();
    },
    view({attrs}: m.Vnode<QueryBuilderPageAttrs>) {
      const {trace, sqlModules} = attrs;

      // Get table names from SqlModules, falling back to a basic list
      const tableNames = sqlModules
        ? sqlModules.listTablesNames().sort()
        : ['slice', 'sched', 'thread', 'process'];

      // Build the rendered nodes list from the active view.
      const activeNodes = getActiveNodes();
      const activeConns = getActiveConnections();

      const rootIds = getRootNodeIds(activeNodes);
      const renderedNodes: Node[] = rootIds
        .map((id) => activeNodes[id])
        .filter((n): n is NodeData => n !== undefined)
        .map((n) => renderNodeChain(n, tableNames, trace, sqlModules));

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
        const pinnedNode = activeNodes[pinnedNodeId];
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
                      nodes: {},
                      connections: [],
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
            draft.connections.push(conn);
          });
        },
        onConnectionRemove: (index: number) => {
          updateStore((draft) => {
            draft.connections.splice(index, 1);
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
            const target = draft.nodes[targetId];
            const child = draft.nodes[childNode.id];
            if (target && child) {
              target.nextId = child.id;
            }
            for (let i = draft.connections.length - 1; i >= 0; i--) {
              const conn = draft.connections[i];
              if (
                (conn.fromNode === targetId && conn.toNode === childNode.id) ||
                (conn.fromNode === childNode.id && conn.toNode === targetId)
              ) {
                draft.connections.splice(i, 1);
              }
            }
          });
        },
        onUndock: (parentId: string, nodeId: string, x: number, y: number) => {
          updateStore((draft) => {
            const parent = draft.nodes[parentId];
            const child = draft.nodes[nodeId];
            if (parent && child) {
              child.x = x;
              child.y = y;
              parent.nextId = undefined;
            }
          });
        },
        onNodeCollapse: (nodeId: string, collapsed: boolean) => {
          updateNode(nodeId, {collapsed});
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
        ? getOutputColumnsForNode(
            activeNodes,
            activeConns,
            activeNodeId,
            sqlModules,
          )
        : undefined;

      const sqlText = activeNodeId
        ? displaySql ?? 'Incomplete query — fill in all required fields'
        : 'Select a node to preview its SQL';

      // Build IR entries for the IR tab (pure function of graph, always fresh).
      const irEntries = activeNodeId
        ? buildIR(activeNodes, activeConns, activeNodeId, sqlModules) ?? []
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
              content: renderSqlTab({displaySql, sqlText}),
            },
            {
              key: 'columns',
              title: 'Columns',
              content: renderColumnsTab({outputColumns, activeNodeId}),
            },
            {
              key: 'ir',
              title: 'IR',
              content: renderIrTab({irEntries, reportByHash, activeNodeId}),
            },
            {
              key: 'cache',
              title: `Cache (${cacheEntries.length})`,
              content: renderCacheTab({
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

      const activeNode = activeNodeId ? activeNodes[activeNodeId] : undefined;
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
