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

import m from 'mithril';
import {z} from 'zod';
import {Icons} from '../../../base/semantic_icons';
import {Duration, Time} from '../../../base/time';
import {shortUuid} from '../../../base/uuid';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
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
import {
  MenuDivider,
  MenuItem,
  MenuTitle,
  PopupMenu,
} from '../../../widgets/menu';
import {
  type Connection,
  type Node,
  NodeGraph,
  type NodeGraphApi,
  type NodeGraphViewport,
} from '../../../widgets/nodegraph';
// Connection is only used as the widget's wire type (for visual rendering
// and event callbacks). Connections are NOT stored in the graph model —
// they are derived from node.inputs arrays on each render.
import {Popup} from '../../../widgets/popup';
import {RadioGroup} from '../../../widgets/radio_group';
import {Stack} from '../components/stack';
import {SplitPanel} from '../../../widgets/split_panel';
import {Tabs} from '../../../widgets/tabs';
import type {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import type {
  NodeData,
  NodeQueryBuilderStore,
  RootNodeData,
} from '../graph_model';
import {SpaghettiGraphState} from '../graph_state';
import {
  chainTail,
  deserializeStore,
  extractNodesForCopy,
  findConnectedInputs,
  findDockedParent,
  flattenNodes,
  getColumnsForNode,
  getManifest,
  getManifestInputs,
  getOutputColumnsForNode,
  instantiateCopiedNodes,
  serializeStore,
  trimInputs,
  type GraphIndex,
} from '../graph_utils';
import {buildDisplayQuery} from '../compose';
import {buildPlan} from '../plan';
import {MaterializationService} from '../materialization';
import type {DetailsContext, RenderContext} from '../node_types';
import {CacheTab} from './cache_tab';
import {ColumnsTab} from './columns_tab';
import {GraphTab} from './graph_tab';
import {MaterializationTab} from './materialization_tab';
import {SqlTab} from './sql_tab';
import {TextInput} from '../../../widgets/text_input';
import {findRef} from '../../../base/dom_utils';
import {assertExists, assertIsInstance} from '../../../base/assert';
import {classNames} from '../../../base/classnames';
import './spaghetti_page.scss';

type LoadingStyle = 'beam' | 'shimmer' | 'pulse' | 'scanline' | 'none';

const ViewportSchema = z.object({
  panOffset: z.object({
    x: z.number().finite(),
    y: z.number().finite(),
  }),
  zoom: z.number().finite().min(0.1).max(5.0),
});

const LOADING_STYLE_OPTIONS: ReadonlyArray<{
  readonly value: LoadingStyle;
  readonly label: string;
}> = [
  {value: 'shimmer', label: 'Header bar'},
  {value: 'beam', label: 'Border beam'},
  {value: 'pulse', label: 'Breathing halo'},
  {value: 'scanline', label: 'Data scanline'},
  {value: 'none', label: 'Off'},
];

export interface SpaghettiPage {
  readonly trace: Trace;
  readonly sqlModules: SqlModules | undefined;
  readonly state?: SpaghettiGraphState;
  readonly matService?: MaterializationService;
}

export function SpaghettiPage(): m.Component<SpaghettiPage> {
  let graphApi: NodeGraphApi | undefined;
  let fallbackState: SpaghettiGraphState | undefined;
  let activeState: SpaghettiGraphState | undefined;
  let suppressHistoryOnMove = false;

  function getState(): SpaghettiGraphState {
    if (activeState) return activeState;
    fallbackState ??= new SpaghettiGraphState();
    return fallbackState;
  }

  // Visual style for the node loading animation while materializing.
  let loadingStyle: LoadingStyle = 'shimmer';

  // When enabled, sleeps 10x each node's query execution time to preview loading UX.
  let emulateSlowLoads = false;

  // Controlled open state for the toolbar "Add Node" popup menu.
  let addMenuOpen = false;

  function getFlatNodes(): GraphIndex {
    return flattenNodes(getState().store.nodes);
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

  let fallbackMatService: MaterializationService | undefined;
  let matService: MaterializationService | undefined;

  const VIEWPORT_STORAGE_KEY = 'perfettoSpaghettiViewport';
  const VIEWPORT_SAVE_DEBOUNCE_MS = 250;

  let pendingViewport: NodeGraphViewport | undefined;
  let viewportSaveTimer: ReturnType<typeof setTimeout> | undefined;

  function loadViewport(): NodeGraphViewport | undefined {
    try {
      const raw = localStorage.getItem(VIEWPORT_STORAGE_KEY);
      if (!raw) return undefined;
      const parsed = ViewportSchema.safeParse(JSON.parse(raw));
      return parsed.success ? parsed.data : undefined;
    } catch {
      return undefined;
    }
  }

  function flushViewportSave() {
    if (viewportSaveTimer !== undefined) {
      clearTimeout(viewportSaveTimer);
      viewportSaveTimer = undefined;
    }
    if (!pendingViewport) return;
    const toSave = pendingViewport;
    pendingViewport = undefined;
    try {
      localStorage.setItem(VIEWPORT_STORAGE_KEY, JSON.stringify(toSave));
    } catch (e) {
      console.error('[QueryBuilder] Failed to save viewport:', e);
    }
  }

  function scheduleSaveViewport(viewport: NodeGraphViewport) {
    pendingViewport = viewport;
    if (viewportSaveTimer !== undefined) {
      clearTimeout(viewportSaveTimer);
    }
    viewportSaveTimer = setTimeout(
      flushViewportSave,
      VIEWPORT_SAVE_DEBOUNCE_MS,
    );
  }

  const initialViewport = loadViewport();

  const updateStore = (updater: (draft: NodeQueryBuilderStore) => void) => {
    getState().updateStore(updater);
  };

  const undo = () => getState().undo();
  const redo = () => getState().redo();
  const canUndo = () => getState().canUndo();
  const canRedo = () => getState().canRedo();

  const updateNode = (
    nodeId: string,
    updates: Partial<Omit<RootNodeData, 'id'>>,
  ) => {
    getState().updateNode(nodeId, updates, {
      skipHistory: suppressHistoryOnMove,
    });
  };

  const removeNode = (nodeId: string) => {
    getState().removeNode(nodeId);
  };

  function flushPendingLayout() {
    if (!graphApi) return;
    const pending = getState().consumePendingLayout();
    if (!pending) return;
    requestAnimationFrame(() => {
      if (!graphApi) return;
      if (pending.autoLayout) {
        suppressHistoryOnMove = true;
        try {
          graphApi.autoLayout();
        } finally {
          suppressHistoryOnMove = false;
        }
      }
      if (pending.recenter) {
        graphApi.recenter();
      }
    });
  }

  // --- Clipboard support ---

  let lastCopiedJson: string | undefined;
  let pasteCount = 0;

  function copySelectedNodes() {
    const state = getState();
    const copied = extractNodesForCopy(
      state.store.nodes,
      state.selectedNodeIds,
    );
    if (copied.length === 0) return;

    const json = JSON.stringify(
      JSON.parse(serializeStore({nodes: copied, labels: []})),
      null,
      2,
    );
    lastCopiedJson = json;
    pasteCount = 0;
    navigator.clipboard.writeText(json).catch(() => {
      // Fall back silently to in-memory lastCopiedJson if clipboard access fails.
    });
  }

  function applyPastedJson(json: string) {
    let parsed: NodeQueryBuilderStore;
    try {
      parsed = deserializeStore(json);
    } catch {
      return;
    }
    if (parsed.nodes.length === 0) return;

    if (json !== lastCopiedJson) {
      lastCopiedJson = json;
      pasteCount = 1;
    } else {
      pasteCount++;
    }

    const step = 40 * pasteCount;
    const newNodes = instantiateCopiedNodes(parsed.nodes, {
      dx: step,
      dy: step,
    });

    updateStore((draft) => {
      for (const node of newNodes) draft.nodes.push(node);
    });

    const selectedNodeIds = getState().selectedNodeIds;
    selectedNodeIds.clear();
    for (const node of newNodes) selectedNodeIds.add(node.id);
  }

  function pasteNodes() {
    navigator.clipboard
      .readText()
      .then((text) => {
        applyPastedJson(text !== '' ? text : (lastCopiedJson ?? ''));
        m.redraw();
      })
      .catch(() => {
        if (lastCopiedJson !== undefined) {
          applyPastedJson(lastCopiedJson);
          m.redraw();
        }
      });
  }

  function cutSelectedNodes() {
    copySelectedNodes();
    for (const id of [...getState().selectedNodeIds]) {
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
    for (const [i, port] of ports.entries()) {
      const input = i === 0 && parent ? parent : connected.get(i);
      if (input) {
        portInputs.set(port.name, input);
      }
    }

    const ctx: RenderContext = {
      availableColumns,
      tableNames,
      trace,
      isSelected: getState().selectedNodeIds.has(nodeData.id),
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
          label:
            getState().pinnedNodeId === nodeData.id ? 'Unpin node' : 'Pin node',
          icon: 'push_pin',
          onclick: () => {
            const state = getState();
            state.pinnedNodeId =
              state.pinnedNodeId === nodeData.id ? undefined : nodeData.id;
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
      graphApi = undefined;
      flushViewportSave();
      matService = undefined;
      fallbackMatService?.dispose();
      fallbackMatService = undefined;
    },
    view({attrs}: m.Vnode<SpaghettiPage>) {
      const {trace, sqlModules} = attrs;
      activeState = attrs.state;
      matService =
        attrs.matService ??
        (fallbackMatService ??= new MaterializationService(trace.engine));
      const state = getState();
      const {store, selectedNodeIds, pinnedNodeId} = state;
      const materializationDialect = state.dialect;

      flushPendingLayout();

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

      toolbarItems.push(
        m(
          PopupMenu,
          {
            isOpen: addMenuOpen,
            onChange: (open: boolean) => {
              addMenuOpen = open;
            },
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
              label: 'Add Node',
              icon: 'add',
              onclick: () => {
                addMenuOpen = !addMenuOpen;
              },
            }),
          },
          m(AddNodeMenu, {
            onAddNode: (type: string) => {
              addMenuOpen = false;
              addNode(type);
            },
          }),
        ),
      );

      if (pinnedNodeId !== undefined) {
        const pinnedNode = flatNodes.nodes[pinnedNodeId];
        const pinnedLabel = pinnedNode
          ? (getManifest(pinnedNode.type)?.title ?? pinnedNode.type)
          : pinnedNodeId;
        toolbarItems.push(
          m(Button, {
            variant: ButtonVariant.Filled,
            icon: 'push_pin',
            label: pinnedLabel,
            title: 'Unpin node',
            onclick: () => {
              state.pinnedNodeId = undefined;
            },
          }),
        );
      }

      toolbarItems.push(
        m('.pf-spag__toolbar-spacer'),
        m(
          RadioGroup,
          {
            intent: Intent.Primary,
            selectedValue: materializationDialect,
            title: 'Query backend used for materialization',
            onValueChange: (value: string) => {
              state.dialect = value === 'pipe' ? 'pipe' : 'sql';
            },
          },
          [
            m(RadioGroup.Button, {value: 'sql'}, 'SQL'),
            m(RadioGroup.Button, {value: 'pipe'}, 'PerfettoSQL Next'),
          ],
        ),
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'build',
              intent: emulateSlowLoads ? Intent.Primary : undefined,
              title: 'Developer options',
            }),
          },
          [
            m(MenuTitle, {label: 'Developer Options'}),
            m(MenuItem, {
              label: 'Emulate slow loads (10x)',
              icon: emulateSlowLoads ? Icons.Checkbox : Icons.BlankCheckbox,
              closePopupOnClick: false,
              onclick: () => {
                emulateSlowLoads = !emulateSlowLoads;
              },
            }),
            m(MenuDivider),
            m(MenuTitle, {label: 'Loading Effect'}),
            ...LOADING_STYLE_OPTIONS.map((opt) =>
              m(MenuItem, {
                label: opt.label,
                icon:
                  loadingStyle === opt.value
                    ? Icons.RadioChecked
                    : Icons.RadioUnchecked,
                closePopupOnClick: false,
                onclick: () => {
                  loadingStyle = opt.value;
                },
              }),
            ),
          ],
        ),
        m(
          Popup,
          {
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'delete_sweep',
              title: 'Clear workspace',
            }),
          },
          m(Stack, [
            m('span', 'Are you sure you want to clear everything?'),
            m(`.pf-spag-confirm-actions.${Popup.DISMISS_POPUP_GROUP_CLASS}`, [
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
                  state.clear();
                },
              }),
            ]),
          ]),
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
        initialViewport,
        onViewportChange: scheduleSaveViewport,
        selectedNodeIds,
        fillHeight: true,
        contextMenuOnHover: true,
        toolbarItems,
        onReady: (api: NodeGraphApi) => {
          graphApi = api;
          flushPendingLayout();
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
      const activeNodeId = state.getActiveNodeId();

      matService.emulateSlowLoads = emulateSlowLoads;

      // Schedule materialization on every render — the AsyncLimiter
      // ensures only the latest invocation actually runs.
      matService.scheduleUpdate(
        store,
        activeNodeId,
        sqlModules,
        materializationDialect,
      );

      const dataSource = matService.dataSource;
      const matError = matService.error;
      const cacheEntries = matService.cacheEntries;

      // Build DataGrid schema from the node's output columns.
      const outputColumns = activeNodeId
        ? getOutputColumnsForNode(flatNodes, activeNodeId, sqlModules)
        : undefined;

      // Build plan + display query (pure functions of the graph).
      const plan = activeNodeId
        ? buildPlan(flatNodes, activeNodeId, sqlModules)
        : undefined;
      const matPlan = plan ? matService.planFor(plan) : undefined;
      const displaySql = buildDisplayQuery(plan, materializationDialect);

      const liveJson = JSON.stringify(
        JSON.parse(serializeStore(store)),
        null,
        2,
      );

      const sqlPanel = m(
        '.pf-spag__sql-panel',
        m(Tabs, {
          tabs: [
            {
              key: 'sql',
              title: 'SQL',
              content: m(SqlTab, {
                displaySql,
                activeNodeId,
                language:
                  materializationDialect === 'pipe'
                    ? 'PerfettoSQL Next'
                    : 'SQL',
              }),
            },
            {
              key: 'columns',
              title: 'Columns',
              content: m(ColumnsTab, {outputColumns, activeNodeId}),
            },
            {
              key: 'materialization',
              title: 'Materialization',
              content: m(MaterializationTab, {
                matPlan,
                dialect: materializationDialect,
                lastRunOutcomes: matService.lastRunOutcomes,
                lastIncludesTimeMs: matService.lastIncludesTimeMs,
                activeNodeId,
              }),
            },
            {
              key: 'cache',
              title: `Cache (${cacheEntries.length})`,
              content: m(CacheTab, {
                cacheEntries,
                onPruneOrphans: () => matService?.pruneOrphans(),
                onClearCache: () => matService?.clearCache(),
              }),
            },
            {
              key: 'json',
              title: 'JSON',
              content: m(GraphTab, {
                liveJson,
                onApply: (json: string) => {
                  state.replaceStore(deserializeStore(json));
                },
              }),
            },
          ],
        }),
      );

      const datagridSchema: ColumnSchema = Object.fromEntries(
        (outputColumns ?? []).map((col) => {
          if (col.type && isIdType(col.type)) {
            const tableName = col.type.source.table;
            return [
              col.name,
              {
                cellRenderer: (value: Row[string]) => {
                  const cell = renderCell(value, col.name);
                  if (typeof value !== 'bigint' && typeof value !== 'number') {
                    return cell;
                  }
                  const id = typeof value === 'bigint' ? Number(value) : value;
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
      );

      function renderEmptyState(): m.Children {
        if (!activeNodeId) {
          return m(
            EmptyState,
            {
              fillHeight: true,
              title: 'No node selected',
            },
            'Click on a node to see its query results.',
          );
        }
        if (!displaySql) {
          return m(
            EmptyState,
            {
              icon: 'warning',
              fillHeight: true,
              title: 'Incomplete query',
            },
            'Fill in all required fields to see results.',
          );
        }
        if (matError) {
          return m(
            EmptyState,
            {
              icon: 'warning',
              fillHeight: true,
              title: 'Query error',
            },
            m('pre.pf-spag__error', matError),
          );
        }
        return m(EmptyState, {
          title: 'Query results will appear here',
          fillHeight: true,
        });
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
        : activeNodeId && displaySql && dataSource
          ? m(DataGrid, {
              key: matService.materializedTable ?? '',
              data: dataSource,
              schema: datagridSchema,
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
        '.pf-spag',
        {
          className: classNames(`pf-spag--loading-${loadingStyle}`),
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
                {
                  hotkey: 'Mod+C',
                  callback: () => copySelectedNodes(),
                },
                {
                  hotkey: 'Mod+V',
                  callback: () => pasteNodes(),
                },
                {
                  hotkey: 'Mod+X',
                  callback: () => cutSelectedNodes(),
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

interface AddNodeMenuAttrs {
  readonly onAddNode?: (type: string) => void;
}

interface NodeCategory {
  readonly title: string;
  readonly types: readonly string[];
}

const NODE_CATEGORIES: readonly NodeCategory[] = [
  {
    title: 'Sources',
    types: ['from', 'data_table', 'time_range'],
  },
  {
    title: 'Columns',
    types: ['select', 'extend', 'set', 'rename', 'drop', 'extract_arg'],
  },
  {
    title: 'Rows',
    types: ['filter', 'distinct', 'sort', 'limit'],
  },
  {
    title: 'Combine & Aggregate',
    types: ['groupby', 'join', 'union'],
  },
  {
    title: 'Intervals',
    types: [
      'interval_intersect',
      'interval_flatten',
      'interval_merge',
      'interval_fill_gaps',
      'intervals_from_events',
    ],
  },
  {
    title: 'Graphs & Trees',
    types: ['graph_tree', 'tree_accumulate'],
  },
  {
    title: 'Visualize & Custom',
    types: ['chart'],
  },
  {
    title: 'Advanced',
    types: ['sql'],
  },
];

function AddNodeMenu(): m.Component<AddNodeMenuAttrs> {
  const SEARCH_BOX_REF = 'SearchBox';
  let searchTerm = '';

  return {
    view({attrs}) {
      const {onAddNode} = attrs;
      const query = searchTerm.trim().toLowerCase();

      function nodeMenuItem(type: string, isHighlighted: boolean): m.Children {
        const manifest = getManifest(type);
        return m(MenuItem, {
          label: manifest.title,
          icon: manifest.icon,
          active: isHighlighted,
          className: 'pf-spag-add-menu-item',
          style: `--pf-node-hue: ${manifest.hue}`,
          onclick: () => onAddNode?.(type),
        });
      }

      const filteredNodeTypes: string[] = [];
      const menuChildren: m.Children[] = [];

      for (const category of NODE_CATEGORIES) {
        const matching = category.types.filter((type) => {
          if (query === '') return true;
          const manifest = getManifest(type);
          return (
            manifest.title.toLowerCase().includes(query) ||
            type.toLowerCase().includes(query) ||
            category.title.toLowerCase().includes(query)
          );
        });
        if (matching.length === 0) continue;

        if (menuChildren.length > 0) {
          menuChildren.push(m(MenuDivider));
        }
        menuChildren.push(m(MenuTitle, {label: category.title}));
        for (const type of matching) {
          const isFirst = filteredNodeTypes.length === 0;
          filteredNodeTypes.push(type);
          menuChildren.push(nodeMenuItem(type, query !== '' && isFirst));
        }
      }

      if (menuChildren.length === 0) {
        menuChildren.push(m('.pf-spag-add-menu-empty', 'No matching nodes'));
      }

      // Build the popup content: fixed search box + scrollable categorized list.
      const searchBox = m(TextInput, {
        ref: SEARCH_BOX_REF,
        placeholder: 'Filter nodes…',
        leftIcon: 'search',
        value: searchTerm,
        oninput: (e: InputEvent) => {
          searchTerm = (e.target as HTMLInputElement).value;
        },
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter') {
            const first = filteredNodeTypes[0];
            if (first !== undefined) {
              e.preventDefault();
              onAddNode?.(first);
            }
          }
        },
      });

      return [searchBox, m('.pf-spag-add-menu-list', menuChildren)];
    },
    oncreate({dom}) {
      const textBoxElement = findRef(dom, SEARCH_BOX_REF);
      assertExists(textBoxElement);
      assertIsInstance(textBoxElement, HTMLInputElement);
      textBoxElement.focus();
    },
  };
}
