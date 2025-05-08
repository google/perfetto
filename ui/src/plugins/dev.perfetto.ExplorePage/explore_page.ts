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
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {NodeType, QueryNode} from './query_node';
import {MenuItem} from '../../widgets/menu';
import {Icons} from '../../base/semantic_icons';
import {VisViewSource} from './data_visualiser/view_source';
import {PopupMenu} from '../../widgets/menu';
import {
  StdlibTableAttrs,
  StdlibTableNode,
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

export const ExplorePageModeToLabel: Record<ExplorePageModes, string> = {
  [ExplorePageModes.QUERY_BUILDER]: 'Query Builder',
  [ExplorePageModes.DATA_VISUALISER]: 'Visualise Data',
};

interface ExplorePageAttrs {
  readonly trace: Trace;
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  renderNodeActionsMenuItems(node: QueryNode, state: ExplorePageState) {
    // TODO: Split into operations on graph (like delete or duplicate) and
    // operations on node (like edit).
    return [
      m(MenuItem, {
        label: 'Visualise Data',
        icon: Icons.Chart,
        onclick: () => {
          state.selectedNode = node;
          state.mode = ExplorePageModes.DATA_VISUALISER;
        },
      }),
      m(MenuItem, {
        label: 'Duplicate',
        onclick: async () => {
          state.rootNodes.push(cloneQueryNode(node));
        },
      }),
      m(MenuItem, {
        label: 'Delete',
        onclick: async () => {
          const idx = state.rootNodes.indexOf(node);
          if (idx !== -1) {
            state.rootNodes.splice(idx, 1);
            state.selectedNode = node;
          }
        },
      }),
    ];
  }

  view({attrs}: m.CVnode<ExplorePageAttrs>) {
    const {trace, state} = attrs;

    return m(
      '.page.explore-page',
      m(
        '.explore-page__header',
        m('h1', `${ExplorePageModeToLabel[state.mode]}`),
        m('span', {style: {flexGrow: 1}}),
        state.mode === ExplorePageModes.QUERY_BUILDER
          ? m(
              '',
              m(
                PopupMenu,
                {
                  trigger: m(Button, {
                    label: 'Add new node',
                    icon: Icons.Add,
                    intent: Intent.Primary,
                    variant: ButtonVariant.Filled,
                  }),
                },
                addSourcePopupMenu(attrs),
              ),
              m(Button, {
                label: 'Clear All Query Nodes',
                intent: Intent.Primary,
                variant: ButtonVariant.Filled,
                onclick: () => {
                  state.rootNodes = [];
                  state.selectedNode = undefined;
                },
                style: {marginLeft: '10px'},
              }),
            )
          : m(Button, {
              label: 'Back to Query Builder',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              onclick: () => {
                state.mode = ExplorePageModes.QUERY_BUILDER;
              },
            }),
      ),

      state.mode === ExplorePageModes.QUERY_BUILDER &&
        m(QueryBuilder, {
          trace,
          sqlModules: attrs.sqlModulesPlugin.getSqlModules(),
          onRootNodeCreated(arg) {
            state.rootNodes.push(arg);
            state.selectedNode = arg;
          },
          onNodeSelected(arg) {
            state.selectedNode = arg;
          },
          renderNodeActionsMenuItems: (node: QueryNode) =>
            this.renderNodeActionsMenuItems(node, state),
          rootNodes: state.rootNodes,
          selectedNode: state.selectedNode,
          addSourcePopupMenu: () => addSourcePopupMenu(attrs),
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

function addSourcePopupMenu(attrs: ExplorePageAttrs): m.Children {
  const {trace, state} = attrs;
  const sqlModules = attrs.sqlModulesPlugin.getSqlModules();
  return [
    m(MenuItem, {
      label: 'Standard library table',
      onclick: async () => {
        const newNode = new StdlibTableNode({
          filters: [],
          sourceCols: [],
          groupByColumns: [],
          aggregations: [],
          trace,
          sqlModules,
        });
        state.rootNodes.push(newNode);
        state.selectedNode = newNode;
      },
    }),
    m(MenuItem, {
      label: 'Custom slices',
      onclick: () => {
        const newNode = new SlicesSourceNode({
          sourceCols: slicesSourceNodeColumns(true),
          filters: [],
          groupByColumns: slicesSourceNodeColumns(false),
          aggregations: [],
        });
        state.rootNodes.push(newNode);
        state.selectedNode = newNode;
      },
    }),
    m(MenuItem, {
      label: 'Custom SQL',
      onclick: () => {
        const newNode = new SqlSourceNode({
          sourceCols: [],
          filters: [],
          groupByColumns: [],
          aggregations: [],
        });
        state.rootNodes.push(newNode);
        state.selectedNode = newNode;
      },
    }),
  ];
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
