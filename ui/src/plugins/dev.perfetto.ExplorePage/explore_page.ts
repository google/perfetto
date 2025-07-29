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
  modalForStdlibTableSelection,
} from './query_builder/sources/stdlib_table';
import {
  SlicesSourceAttrs,
  SlicesSourceNode,
  slicesSourceNodeColumns,
} from './query_builder/sources/slices_source';
import {
  SqlSourceState,
  SqlSourceNode,
} from './query_builder/sources/sql_source';
import {Trace} from '../../public/trace';
import {VisViewSource} from './data_visualiser/view_source';

export interface ExplorePageState {
  rootNodes: QueryNode[];
  selectedNode?: QueryNode;
  activeViewSource?: VisViewSource;
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
    if (!sqlModules) {
      return;
    }

    const selection = await modalForStdlibTableSelection(sqlModules);

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

  handleAddSqlSource(attrs: ExplorePageAttrs) {
    this.addNode(
      attrs.state,
      new SqlSourceNode({
        trace: attrs.trace,
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

  handleDuplicateNode(state: ExplorePageState, node: QueryNode) {
    const attrsCopy = node.getStateCopy();
    switch (node.type) {
      case NodeType.kStdlibTable:
        state.rootNodes.push(
          new StdlibTableNode(attrsCopy as StdlibTableAttrs),
        );
        break;
      case NodeType.kSimpleSlices:
        state.rootNodes.push(
          new SlicesSourceNode(attrsCopy as SlicesSourceAttrs),
        );
        break;
      case NodeType.kSqlSource:
        state.rootNodes.push(new SqlSourceNode(attrsCopy as SqlSourceState));
        break;
    }
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
      '.page.pf-explore-page',
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
