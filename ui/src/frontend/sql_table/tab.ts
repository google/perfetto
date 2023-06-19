// Copyright (C) 2023 The Android Open Source Project
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

import {BottomTab, bottomTabRegistry, NewBottomTabArgs} from '../bottom_tab';
import {copyToClipboard} from '../clipboard';
import {Icons} from '../semantic_icons';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {exists} from '../widgets/utils';

import {SqlTableState} from './state';
import {SqlTable} from './table';
import {SqlTableDescription} from './table_description';

interface SqlTableTabConfig {
  table: SqlTableDescription;
  displayName?: string;
  filters?: string[];
}

export class SqlTableTab extends BottomTab<SqlTableTabConfig> {
  static readonly kind = 'dev.perfetto.SqlTableTab';

  private state: SqlTableState;

  constructor(args: NewBottomTabArgs) {
    super(args);

    this.state =
        new SqlTableState(this.engine, this.config.table, this.config.filters);
  }

  static create(args: NewBottomTabArgs): SqlTableTab {
    return new SqlTableTab(args);
  }

  viewTab() {
    const range = this.state.getDisplayedRange();
    const rowCount = this.state.getTotalRowCount();
    const navigation = [
      exists(range) && exists(rowCount) &&
          `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !this.state.canGoBack(),
        onclick: () => this.state.goBack(),
        minimal: true,
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !this.state.canGoForward(),
        onclick: () => this.state.goForward(),
        minimal: true,
      }),
    ];

    return m(
        DetailsShell,
        {
          title: 'Table',
          description: this.config.displayName ?? this.config.table.name,
          buttons: [
            ...navigation,
            m(Button, {
              label: 'Copy SQL query',
              onclick: () =>
                  copyToClipboard(this.state.getNonPaginatedSQLQuery()),
            }),
            m(Button, {
              label: 'Close',
              onclick: () => this.close(),
            }),
          ],
        },
        m(SqlTable, {
          state: this.state,
        }));
  }

  renderTabCanvas() {}

  getTitle(): string {
    const rowCount = this.state.getTotalRowCount();
    const rows = rowCount === undefined ? '' : `(${rowCount})`;
    return `Table ${this.config.displayName ?? this.config.table.name} ${rows}`;
  }

  isLoading(): boolean {
    return this.state.isLoading();
  }
}

bottomTabRegistry.register(SqlTableTab);
