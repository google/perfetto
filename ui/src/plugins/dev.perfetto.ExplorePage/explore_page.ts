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
import SqlModulesPlugin from '../dev.perfetto.SqlModules';

import {Builder} from './query_builder/builder';
import {QueryNode, QueryNodeState, singleNodeOperation} from './query_node';
import {Trace} from '../../public/trace';

import {exportStateAsJson, importStateFromJson} from './json_handler';
import {showImportWithStatementModal} from './sql_json_handler';
import {registerCoreNodes} from './query_builder/core_nodes';
import {nodeRegistry} from './query_builder/node_registry';

registerCoreNodes();

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode;
  nodeLayouts: Map<string, {x: number; y: number}>;
  devMode?: boolean;
}

interface ExplorePageAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
  readonly onStateUpdate: (
    update:
      | ExplorePageState
      | ((currentState: ExplorePageState) => ExplorePageState),
  ) => void;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private selectNode(attrs: ExplorePageAttrs, node: QueryNode) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNode: node,
    }));
  }

  private deselectNode(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedNode: undefined,
    }));
  }

  private async handleDevModeChange(attrs: ExplorePageAttrs, enabled: boolean) {
    if (enabled) {
      const {registerDevNodes} = await import('./query_builder/dev_nodes');
      registerDevNodes();
    }
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      devMode: enabled,
    }));
  }

  async handleAddOperationNode(
    attrs: ExplorePageAttrs,
    node: QueryNode,
    derivedNodeId: string,
  ) {
    const {state, onStateUpdate} = attrs;
    const descriptor = nodeRegistry.get(derivedNodeId);
    if (descriptor) {
      let initialState: Partial<QueryNodeState> | null = {};
      if (descriptor.preCreate) {
        const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
        if (!sqlModules) return;
        initialState = await descriptor.preCreate({sqlModules});
      }

      if (initialState === null) {
        return;
      }

      const nodeState: QueryNodeState = {
        ...initialState,
        prevNode: node,
      };

      const newNode = descriptor.factory(nodeState, {
        allNodes: state.rootNodes,
      });

      let lastNodeInChain = node;
      while (
        lastNodeInChain.nextNodes.length === 1 &&
        lastNodeInChain.nextNodes[0] !== undefined &&
        singleNodeOperation(lastNodeInChain.nextNodes[0].type)
      ) {
        lastNodeInChain = lastNodeInChain.nextNodes[0];
      }

      lastNodeInChain.nextNodes = [newNode];

      if ('prevNode' in newNode) {
        newNode.prevNode = lastNodeInChain;
      }

      onStateUpdate((currentState) => ({
        ...currentState,
        selectedNode: newNode,
      }));
    }
  }

  private async handleAddSourceNode(attrs: ExplorePageAttrs, id: string) {
    const descriptor = nodeRegistry.get(id);
    if (!descriptor) return;

    let initialState: Partial<QueryNodeState> | null = {};

    if (descriptor.preCreate) {
      const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
      if (!sqlModules) return;
      initialState = await descriptor.preCreate({sqlModules});
    }

    if (initialState === null) {
      return;
    }

    const newNode = descriptor.factory(
      {
        ...initialState,
        trace: attrs.trace,
      },
      {allNodes: attrs.state.rootNodes},
    );

    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, newNode],
      selectedNode: newNode,
    }));
  }

  handleClearAllNodes(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [],
      selectedNode: undefined,
    }));
  }

  handleDuplicateNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {onStateUpdate} = attrs;
    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: [...currentState.rootNodes, node.clone()],
    }));
  }

  handleDeleteNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;

    let newRootNodes = state.rootNodes.filter((n) => n !== node);
    if (state.rootNodes.includes(node) && node.nextNodes.length > 0) {
      newRootNodes = [...newRootNodes, ...node.nextNodes];
    }

    if ('prevNode' in node && node.prevNode) {
      const childIdx = node.prevNode.nextNodes.indexOf(node);
      if (childIdx !== -1) {
        node.prevNode.nextNodes.splice(childIdx, 1, ...node.nextNodes);
        for (const child of node.nextNodes) {
          if ('prevNode' in child) {
            child.prevNode = node.prevNode;
          }
        }
      }
    } else if ('prevNodes' in node) {
      for (const prevNode of node.prevNodes) {
        if (!prevNode) continue;

        const childIdx = prevNode.nextNodes.indexOf(node);
        if (childIdx !== -1) {
          prevNode.nextNodes.splice(childIdx, 1, ...node.nextNodes);
          for (const child of node.nextNodes) {
            if ('prevNode' in child) {
              child.prevNode = prevNode;
            } else if ('prevNodes' in child) {
              const deletedNodeIdx = child.prevNodes.indexOf(node);
              if (deletedNodeIdx !== -1) {
                child.prevNodes[deletedNodeIdx] = prevNode;
              }
            }
          }
        }
      }
    }

    // If the deleted node was selected, deselect it.
    const newSelectedNode =
      state.selectedNode === node ? undefined : state.selectedNode;

    onStateUpdate((currentState) => ({
      ...currentState,
      rootNodes: newRootNodes,
      selectedNode: newSelectedNode,
    }));
  }

  handleExport(state: ExplorePageState, trace: Trace) {
    exportStateAsJson(state, trace);
  }

  handleImport(attrs: ExplorePageAttrs) {
    const {trace, sqlModulesPlugin, onStateUpdate} = attrs;
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = (event) => {
      const files = (event.target as HTMLInputElement).files;
      if (files && files.length > 0) {
        const file = files[0];
        importStateFromJson(
          file,
          trace,
          sqlModules,
          (newState: ExplorePageState) => {
            onStateUpdate(newState);
          },
        );
      }
    };
    input.click();
  }

  private handleKeyDown(event: KeyboardEvent, attrs: ExplorePageAttrs) {
    const {state} = attrs;
    if (state.selectedNode) {
      return;
    }
    // Do not interfere with text inputs
    if (
      event.target instanceof HTMLInputElement ||
      event.target instanceof HTMLTextAreaElement
    ) {
      return;
    }

    // Handle source node creation shortcuts
    for (const [id, descriptor] of nodeRegistry.list()) {
      if (
        descriptor.type === 'source' &&
        descriptor.hotkey &&
        event.key.toLowerCase() === descriptor.hotkey.toLowerCase()
      ) {
        this.handleAddSourceNode(attrs, id);
        event.preventDefault(); // Prevent default browser actions for this key
        return;
      }
    }

    // Handle other shortcuts
    switch (event.key) {
      case 'i':
        this.handleImport(attrs);
        break;
      case 'e':
        this.handleExport(attrs.state, attrs.trace);
        break;
    }
  }

  private handleImportWithStatement(attrs: ExplorePageAttrs) {
    const {trace, sqlModulesPlugin, onStateUpdate} = attrs;
    const sqlModules = sqlModulesPlugin.getSqlModules();
    if (!sqlModules) return;

    showImportWithStatementModal(trace, sqlModules, onStateUpdate);
  }

  view({attrs}: m.CVnode<ExplorePageAttrs>) {
    const {trace, state} = attrs;

    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();

    if (!sqlModules) {
      return m(
        '.pf-explore-page',
        m(
          '.pf-explore-page__header',
          m('h1', 'Loading SQL Modules, please wait...'),
        ),
      );
    }

    return m(
      '.pf-explore-page',
      {
        onkeydown: (e: KeyboardEvent) => this.handleKeyDown(e, attrs),
        oncreate: (vnode) => {
          (vnode.dom as HTMLElement).focus();
        },
        tabindex: 0,
      },
      m(Builder, {
        trace,
        sqlModules,
        rootNodes: state.rootNodes,
        selectedNode: state.selectedNode,
        nodeLayouts: state.nodeLayouts,
        devMode: state.devMode,
        onDevModeChange: (enabled) => this.handleDevModeChange(attrs, enabled),
        onRootNodeCreated: (node) => {
          attrs.onStateUpdate((currentState) => ({
            ...currentState,
            rootNodes: [...currentState.rootNodes, node],
            selectedNode: node,
          }));
        },
        onNodeSelected: (node) => {
          if (node) this.selectNode(attrs, node);
        },
        onDeselect: () => this.deselectNode(attrs),
        onNodeLayoutChange: (nodeId, layout) => {
          attrs.onStateUpdate((currentState) => {
            const newNodeLayouts = new Map(currentState.nodeLayouts);
            newNodeLayouts.set(nodeId, layout);
            return {
              ...currentState,
              nodeLayouts: newNodeLayouts,
            };
          });
        },
        onAddSourceNode: (id) => {
          this.handleAddSourceNode(attrs, id);
        },
        onAddOperationNode: (id, node) => {
          this.handleAddOperationNode(attrs, node, id);
        },
        onClearAllNodes: () => this.handleClearAllNodes(attrs),
        onDuplicateNode: () => {
          if (state.selectedNode) {
            this.handleDuplicateNode(attrs, state.selectedNode);
          }
        },
        onDeleteNode: () => {
          if (state.selectedNode) {
            this.handleDeleteNode(attrs, state.selectedNode);
          }
        },
        onImport: () => this.handleImport(attrs),
        onImportWithStatement: () => this.handleImportWithStatement(attrs),
        onExport: () => this.handleExport(state, trace),
        onRemoveFilter: (node, filter) => {
          if (node.state.filters) {
            const filterIndex = node.state.filters.indexOf(filter);
            if (filterIndex > -1) {
              node.state.filters.splice(filterIndex, 1);
            }
          }
          attrs.onStateUpdate((currentState) => ({...currentState}));
        },
      }),
    );
  }
}
