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
import {Trace} from '../../public/trace';
import {IntervalIntersectNode} from './query_builder/nodes/interval_intersect_node';

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode;
}

interface ExplorePageAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private addNode(
    state: ExplorePageState,
    newNode: QueryNode,
    prevNode?: QueryNode,
  ) {
    if (prevNode) {
      prevNode.nextNodes.push(newNode);
    } else {
      state.rootNodes.push(newNode);
    }
    this.selectNode(state, newNode);
  }

  private selectNode(state: ExplorePageState, node: QueryNode) {
    state.selectedNode = node;
  }

  private deselectNode(state: ExplorePageState) {
    state.selectedNode = undefined;
  }

  async handleAddStdlibTableSource(attrs: ExplorePageAttrs) {
    const {trace, state} = attrs;
    const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
    if (!sqlModules) {
      return;
    }

    const selection = await modalForTableSelection(sqlModules);

    if (selection) {
      this.addNode(
        state,
        new TableSourceNode({
          trace,
          sqlModules,
          sqlTable: selection.sqlTable,
          filters: [],
        }),
      );
    }
  }

  handleAddAggregation(state: ExplorePageState, node: QueryNode) {
    const newNode = new AggregationNode({
      prevNodes: [node],
      groupByColumns: [],
      aggregations: [],
      filters: [],
    });
    this.addNode(state, newNode, node);
  }

  handleAddIntervalIntersect(state: ExplorePageState, node: QueryNode) {
    const newNode = new IntervalIntersectNode({
      prevNodes: [node],
      allNodes: state.rootNodes,
      intervalNodes: [],
      filters: [],
    });
    this.addNode(state, newNode, node);
  }

  handleAddSlicesSource(state: ExplorePageState) {
    this.addNode(
      state,
      new SlicesSourceNode({
        filters: [],
      }),
    );
  }

  handleAddSqlSource(attrs: ExplorePageAttrs) {
    this.addNode(
      attrs.state,
      new SqlSourceNode({
        trace: attrs.trace,
        filters: [],
      }),
    );
  }

  handleClearAllNodes(state: ExplorePageState) {
    state.rootNodes = [];
    this.deselectNode(state);
  }

  handleDuplicateNode(state: ExplorePageState, node: QueryNode) {
    state.rootNodes.push(node.clone());
  }

  handleDeleteNode(state: ExplorePageState, node: QueryNode) {
    // If the node is a root node, remove it from the root nodes array.
    const rootIdx = state.rootNodes.indexOf(node);
    if (rootIdx !== -1) {
      state.rootNodes.splice(rootIdx, 1);
    }

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
    if (state.selectedNode === node) {
      this.deselectNode(state);
    }
  }

  private handleKeyDown(event: KeyboardEvent, attrs: ExplorePageAttrs) {
    const {state} = attrs;
    if (state.selectedNode !== undefined) {
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
        this.handleAddSlicesSource(attrs.state);
        break;
    }
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
        onRootNodeCreated: (node) => this.addNode(state, node),
        onNodeSelected: (node) => (state.selectedNode = node),
        onDeselect: () => this.deselectNode(state),
        onAddStdlibTableSource: () => this.handleAddStdlibTableSource(attrs),
        onAddSlicesSource: () => this.handleAddSlicesSource(state),
        onAddSqlSource: () => this.handleAddSqlSource(attrs),
        onClearAllNodes: () => this.handleClearAllNodes(state),
        onDuplicateNode: (node) => this.handleDuplicateNode(state, node),
        onDeleteNode: (node) => this.handleDeleteNode(state, node),
        onAddAggregationNode: (node) => this.handleAddAggregation(state, node),
        onAddIntervalIntersectNode: (node) =>
          this.handleAddIntervalIntersect(state, node),
      }),
    );
  }
}
