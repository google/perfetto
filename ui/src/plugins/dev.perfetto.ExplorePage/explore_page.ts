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

import {DataVisualiser} from './data_visualiser/data_visualiser';
import {QueryBuilder} from './query_builder/builder';
import {NodeType, QueryNode} from './query_node';
import {
  StdlibTableAttrs,
  StdlibTableNode,
  promptForStdlibTableSelection,
} from './query_builder/sources/stdlib_table';
import {
  SlicesSourceAttrs,
  SlicesSourceNode,
  slicesSourceNodeColumns,
} from './query_builder/sources/slices_source';
import {
  SqlSourceAttrs,
  SqlSourceNode,
} from './query_builder/sources/sql_source';
import {Trace} from '../../public/trace';
import {VisViewSource} from './data_visualiser/view_source';

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode; // Selected Query Node on which to perform actions
  activeViewSource?: VisViewSource; // View Source of activeQueryNode
  mode: ExplorePageModes;
}

export enum ExplorePageModes {
  QUERY_BUILDER,
  DATA_VISUALISER,
}

interface ExplorePageAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private addNode(state: ExplorePageState, newNode: QueryNode) {
    state.rootNodes.push(newNode);
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

    const selection = await promptForStdlibTableSelection(trace, sqlModules);

    if (selection) {
      this.addNode(
        state,
        new StdlibTableNode({
          trace,
          sqlModules,
          sqlTable: selection.sqlTable,
          sourceCols: selection.sourceCols,
          filters: [],
          groupByColumns: selection.groupByColumns,
          aggregations: [],
        }),
      );
    }
  }

  handleAddSlicesSource(state: ExplorePageState) {
    this.addNode(
      state,
      new SlicesSourceNode({
        sourceCols: slicesSourceNodeColumns(true),
        filters: [],
        groupByColumns: slicesSourceNodeColumns(false),
        aggregations: [],
      }),
    );
  }

  handleAddSqlSource(state: ExplorePageState) {
    this.addNode(
      state,
      new SqlSourceNode({
        sourceCols: [],
        filters: [],
        groupByColumns: [],
        aggregations: [],
      }),
    );
  }

  handleClearAllNodes(state: ExplorePageState) {
    state.rootNodes = [];
    this.deselectNode(state);
  }

  handleVisualizeNode(state: ExplorePageState, node: QueryNode) {
    this.selectNode(state, node);
    state.mode = ExplorePageModes.DATA_VISUALISER;
  }

  handleDuplicateNode(state: ExplorePageState, node: QueryNode) {
    state.rootNodes.push(cloneQueryNode(node));
  }

  handleDeleteNode(state: ExplorePageState, node: QueryNode) {
    const idx = state.rootNodes.indexOf(node);
    if (idx !== -1) {
      state.rootNodes.splice(idx, 1);
      if (state.selectedNode === node) {
        this.deselectNode(state);
      }
    }
  }

  private handleKeyDown(event: KeyboardEvent, attrs: ExplorePageAttrs) {
    const {state} = attrs;
    if (state.rootNodes.length > 0) {
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
        this.handleAddSqlSource(attrs.state);
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

    return m(
      '.page.explore-page',
      {
        onkeydown: (e: KeyboardEvent) => this.handleKeyDown(e, attrs),
        oncreate: (vnode) => {
          (vnode.dom as HTMLElement).focus();
        },
        tabindex: 0,
      },
      state.mode === ExplorePageModes.QUERY_BUILDER &&
        m(QueryBuilder, {
          trace,
          sqlModules: attrs.sqlModulesPlugin.getSqlModules(),
          onRootNodeCreated: (node: QueryNode) => this.addNode(state, node),
          onNodeSelected: (node?: QueryNode) => {
            if (node) {
              this.selectNode(state, node);
            }
          },
          onDeselect: () => this.deselectNode(state),
          rootNodes: state.rootNodes,
          selectedNode: state.selectedNode,
          onVisualizeNode: (node: QueryNode) =>
            this.handleVisualizeNode(state, node),
          onDuplicateNode: (node: QueryNode) =>
            this.handleDuplicateNode(state, node),
          onDeleteNode: (node: QueryNode) => this.handleDeleteNode(state, node),
          onAddStdlibTableSource: () => this.handleAddStdlibTableSource(attrs),
          onAddSlicesSource: () => this.handleAddSlicesSource(state),
          onAddSqlSource: () => this.handleAddSqlSource(state),
          onClearAllNodes: () => this.handleClearAllNodes(state),
        }),
      state.mode === ExplorePageModes.DATA_VISUALISER &&
        state.rootNodes.length !== 0 &&
        m(DataVisualiser, {
          trace,
          state,
        }),
    );
  }
}

function cloneQueryNode(node: QueryNode): QueryNode {
  const attrsCopy = node.getStateCopy();
  switch (node.type) {
    case NodeType.kStdlibTable:
      return new StdlibTableNode(attrsCopy as StdlibTableAttrs);
    case NodeType.kSimpleSlices:
      return new SlicesSourceNode(attrsCopy as SlicesSourceAttrs);
    case NodeType.kSqlSource:
      return new SqlSourceNode(attrsCopy as SqlSourceAttrs);
  }
}
