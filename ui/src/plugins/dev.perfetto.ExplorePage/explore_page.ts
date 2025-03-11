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

import {PageWithTraceAttrs} from '../../public/page';
import {DataVisualiser} from './data_visualiser/data_visualiser';
import {QueryBuilder} from './query_builder/builder';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {QueryNode} from './query_node';
import {MenuItem} from '../../widgets/menu';
import {Icons} from '../../base/semantic_icons';
import {VisViewSource} from './data_visualiser/view_source';

export interface ExplorePageState {
  rootNode?: QueryNode; // Root Query Node
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

interface ExplorePageAttrs extends PageWithTraceAttrs {
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExplorePageState;
}

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  renderVisualiseDataMenuItems(node: QueryNode, state: ExplorePageState) {
    return m(MenuItem, {
      label: 'Visualise Data',
      icon: Icons.Chart,
      onclick: () => {
        state.selectedNode = node;
        state.mode = ExplorePageModes.DATA_VISUALISER;
      },
    });
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
          ? m(Button, {
              label: 'Clear All Query Nodes',
              intent: Intent.Primary,
              onclick: () => {
                state.rootNode = undefined;
                state.selectedNode = undefined;
              },
            })
          : m(Button, {
              label: 'Back to Query Builder',
              intent: Intent.Primary,
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
            state.rootNode = arg;
            state.selectedNode = arg;
          },
          onNodeSelected(arg) {
            state.selectedNode = arg;
          },
          visualiseDataMenuItems: (node: QueryNode) =>
            this.renderVisualiseDataMenuItems(node, state),
          rootNode: state.rootNode,
          selectedNode: state.selectedNode,
        }),
      state.mode === ExplorePageModes.DATA_VISUALISER &&
        state.rootNode &&
        m(DataVisualiser, {
          trace,
          state,
        }),
    );
  }
}
