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
import {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {TableList} from './table_list';
import {Card, CardStack} from '../../../widgets/card';

export interface ExplorePageHelpAttrs {
  sqlModules: SqlModules;
  onTableClick: (tableName: string, event: MouseEvent) => void;
}

export class ExplorePageHelp implements m.ClassComponent<ExplorePageHelpAttrs> {
  private searchQuery = '';

  view({attrs}: m.CVnode<ExplorePageHelpAttrs>) {
    return m(
      '.pf-explore-page-help',
      this.renderGettingStarted(),
      m(TableList, {
        ...attrs,
        searchQuery: this.searchQuery,
        onSearchQueryChange: (query: string) => {
          this.searchQuery = query;
        },
      }),
    );
  }

  private renderGettingStarted() {
    return m(
      '.pf-exp-getting-started',
      m(
        CardStack,
        {direction: 'horizontal'},
        m(
          Card,
          m('h4', '1. Add a source node'),
          m(
            'p',
            'Begin by adding a new source node from the panel on the left.',
          ),
        ),
        m(
          Card,
          m('h4', '2. Configure the node'),
          m(
            'p',
            'Click on a node to open its configuration options in this panel.',
          ),
        ),
        m(
          Card,
          m('h4', '3. View the results'),
          m(
            'p',
            'The output of the selected node will be shown in the table below.',
          ),
        ),
      ),
    );
  }
}
