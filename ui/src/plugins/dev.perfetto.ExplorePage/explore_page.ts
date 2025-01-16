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
import {SqlTableState as SqlTableViewState} from '../../components/widgets/sql/legacy_table/state';
import {Chart} from '../../components/widgets/charts/chart';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {DataVisualiser} from './data_visualiser';
import {QueryBuilder} from './query_builder/query_builder';
import {DataSourceViewer} from './query_builder/data_source_viewer';
import {QueryNode} from './query_state';

export interface ExploreTableState {
  sqlTableViewState?: SqlTableViewState;
  queryNode?: QueryNode;
}

interface ExplorePageAttrs extends PageWithTraceAttrs {
  readonly sqlModulesPlugin: SqlModulesPlugin;
  readonly state: ExploreTableState;
  readonly charts: Set<Chart>;
}

enum ExplorePageModes {
  QUERY_BUILDER,
  DATA_VISUALISER,
}

const ExplorePageModeToLabel: Record<ExplorePageModes, string> = {
  [ExplorePageModes.QUERY_BUILDER]: 'Query Builder',
  [ExplorePageModes.DATA_VISUALISER]: 'Data Visualiser',
};

export class ExplorePage implements m.ClassComponent<ExplorePageAttrs> {
  private selectedMode = ExplorePageModes.QUERY_BUILDER;

  view({attrs}: m.CVnode<ExplorePageAttrs>) {
    const {trace, state, charts} = attrs;

    return m(
      '.page.explore-page',
      m(
        '.explore-page__header',
        m('h1', 'Exploration Mode: '),
        m(SegmentedButtons, {
          options: [
            {label: ExplorePageModeToLabel[ExplorePageModes.QUERY_BUILDER]},
            {label: ExplorePageModeToLabel[ExplorePageModes.DATA_VISUALISER]},
          ],
          selectedOption: this.selectedMode,
          onOptionSelected: (i) => (this.selectedMode = i),
        }),
      ),

      this.selectedMode === ExplorePageModes.QUERY_BUILDER &&
        m(
          'div',
          m(QueryBuilder, {
            trace: attrs.trace,
            sqlModules: attrs.sqlModulesPlugin.getSqlModules(),
            onQueryNodeCreated(arg) {
              attrs.state.queryNode = arg;
            },
            queryNode: attrs.state.queryNode,
          }),
          attrs.state.queryNode?.finished &&
            m(DataSourceViewer, {
              trace: attrs.trace,
              plugin: attrs.sqlModulesPlugin,
              queryNode: attrs.state.queryNode,
            }),
        ),
      this.selectedMode === ExplorePageModes.DATA_VISUALISER &&
        m(DataVisualiser, {
          trace,
          state,
          charts,
        }),
    );
  }
}
