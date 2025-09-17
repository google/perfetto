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
import {QueryNode} from './query_node';
import {
  TableSourceNode,
  modalForTableSelection,
} from './query_builder/nodes/sources/table_source';
import {SlicesSourceNode} from './query_builder/nodes/sources/slices_source';
import {SqlSourceNode} from './query_builder/nodes/sources/sql_source';
import {AggregationNode} from './query_builder/nodes/aggregation_node';
import {ModifyColumnsNode} from './query_builder/nodes/modify_columns_node';
import {Trace} from '../../public/trace';
import {IntervalIntersectNode} from './query_builder/nodes/interval_intersect_node';
import {NodeBoxLayout} from './query_builder/node_box';
import {exportStateAsJson, importStateFromJson} from './json_handler';
import {showImportWithStatementModal} from './sql_json_handler';

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode;
  nodeLayouts: Map<string, NodeBoxLayout>;
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
    attrs.onStateUpdate({
      ...attrs.state,
      selectedNode: node,
    });
  }

  private deselectNode(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate({
      ...attrs.state,
      selectedNode: undefined,
    });
  }

  async handleAddStdlibTableSource(attrs: ExplorePageAttrs) {
    const {trace, state, onStateUpdate} = attrs;
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      return;
    }

    const selection = await modalForTableSelection(sqlModules);

    if (selection) {
      const newNode = new TableSourceNode({
        trace,
        sqlModules,
        sqlTable: selection.sqlTable,
        filters: [],
      });
      onStateUpdate({
        ...state,
        rootNodes: [...state.rootNodes, newNode],
        selectedNode: newNode,
      });
    }
  }

  handleAddAggregation(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;
    const newNode = new AggregationNode({
      prevNodes: [node],
      groupByColumns: [],
      aggregations: [],
      filters: [],
    });
    node.nextNodes.push(newNode);
    onStateUpdate({
      ...state,
      selectedNode: newNode,
    });
  }

  handleAddModifyColumns(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;
    const newNode = new ModifyColumnsNode({
      prevNodes: [node],
      newColumns: [],
      selectedColumns: [],
      filters: [],
    });
    node.nextNodes.push(newNode);
    onStateUpdate({
      ...state,
      selectedNode: newNode,
    });
  }

  handleAddIntervalIntersect(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;
    const newNode = new IntervalIntersectNode({
      prevNodes: [node],
      allNodes: state.rootNodes,
      intervalNodes: [],
      filters: [],
    });
    node.nextNodes.push(newNode);
    onStateUpdate({
      ...state,
      selectedNode: newNode,
    });
  }

  handleAddSlicesSource(attrs: ExplorePageAttrs) {
    const {state, onStateUpdate} = attrs;
    const newNode = new SlicesSourceNode({
      filters: [],
    });
    onStateUpdate({
      ...state,
      rootNodes: [...state.rootNodes, newNode],
      selectedNode: newNode,
    });
  }

  handleAddSqlSource(attrs: ExplorePageAttrs) {
    const {state, onStateUpdate} = attrs;
    const newNode = new SqlSourceNode({
      trace: attrs.trace,
      filters: [],
    });
    onStateUpdate({
      ...state,
      rootNodes: [...state.rootNodes, newNode],
      selectedNode: newNode,
    });
  }

  handleClearAllNodes(attrs: ExplorePageAttrs) {
    attrs.onStateUpdate({
      ...attrs.state,
      rootNodes: [],
      selectedNode: undefined,
    });
  }

  handleDuplicateNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;
    onStateUpdate({
      ...state,
      rootNodes: [...state.rootNodes, node.clone()],
    });
  }

  handleDeleteNode(attrs: ExplorePageAttrs, node: QueryNode) {
    const {state, onStateUpdate} = attrs;

    // If the node is a root node, remove it from the root nodes array.
    const newRootNodes = state.rootNodes.filter((n) => n !== node);

    // If the node is a child of another node, remove it from the parent's
    // nextNodes array.
    if (node.prevNodes) {
      for (const prevNode of node.prevNodes) {
        const childIdx = prevNode.nextNodes.indexOf(node);
        if (childIdx !== -1) {
          prevNode.nextNodes.splice(childIdx, 1);
        }
      }
    }

    // If the deleted node was selected, deselect it.
    const newSelectedNode =
      state.selectedNode === node ? undefined : state.selectedNode;

    onStateUpdate({
      ...state,
      rootNodes: newRootNodes,
      selectedNode: newSelectedNode,
    });
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
    switch (event.key) {
      case 'q':
        this.handleAddSqlSource(attrs);
        break;
      case 't':
        this.handleAddStdlibTableSource(attrs);
        break;
      case 's':
        this.handleAddSlicesSource(attrs);
        break;
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
        onRootNodeCreated: (node) => {
          attrs.onStateUpdate({
            ...state,
            rootNodes: [...state.rootNodes, node],
          });
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
        onAddStdlibTableSource: () => this.handleAddStdlibTableSource(attrs),
        onAddSlicesSource: () => this.handleAddSlicesSource(attrs),
        onAddSqlSource: () => this.handleAddSqlSource(attrs),
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
        onAddAggregationNode: () => {
          if (state.selectedNode) {
            this.handleAddAggregation(attrs, state.selectedNode);
          }
        },
        onAddModifyColumnsNode: () => {
          if (state.selectedNode) {
            this.handleAddModifyColumns(attrs, state.selectedNode);
          }
        },
        onAddIntervalIntersectNode: () => {
          if (state.selectedNode) {
            this.handleAddIntervalIntersect(attrs, state.selectedNode);
          }
        },
        onImport: () => this.handleImport(attrs),
        onImportWithStatement: () => this.handleImportWithStatement(attrs),
        onExport: () => this.handleExport(state, trace),
        onRemoveFilter: (node, filter) => {
          const filterIndex = node.state.filters.indexOf(filter);
          if (filterIndex > -1) {
            node.state.filters.splice(filterIndex, 1);
          }
          attrs.onStateUpdate({...state});
        },
      }),
    );
  }
}
