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
import {copyToClipboard} from '../base/clipboard';
import {Icons} from '../base/semantic_icons';
import {exists} from '../base/utils';
import {uuidv4} from '../base/uuid';
import {addBottomTab} from '../common/add_ephemeral_tab';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {Popup, PopupPosition} from '../widgets/popup';
import {BottomTab, NewBottomTabArgs} from './bottom_tab';
import {AddDebugTrackMenu} from './debug_tracks/add_debug_track_menu';
import {getEngine} from './get_engine';
import {Filter} from './widgets/sql/table/column';
import {SqlTableState} from './widgets/sql/table/state';
import {SqlTable} from './widgets/sql/table/table';
import {SqlTableDescription} from './widgets/sql/table/table_description';

export interface SqlTableTabConfig {
  table: SqlTableDescription;
  filters?: Filter[];
  imports?: string[];
}

export function addSqlTableTabImpl(config: SqlTableTabConfig): void {
  const queryResultsTab = new SqlTableTab({
    config,
    engine: getEngine('QueryResult'),
    uuid: uuidv4(),
  });

  addBottomTab(queryResultsTab, 'sqlTable');
}

export class SqlTableTab extends BottomTab<SqlTableTabConfig> {
  static readonly kind = 'dev.perfetto.SqlTableTab';

  private state: SqlTableState;

  constructor(args: NewBottomTabArgs<SqlTableTabConfig>) {
    super(args);

    this.state = new SqlTableState(this.engine, this.config.table, {
      filters: this.config.filters,
      imports: this.config.imports,
    });
  }

  static create(args: NewBottomTabArgs<SqlTableTabConfig>): SqlTableTab {
    return new SqlTableTab(args);
  }

  viewTab() {
    const range = this.state.getDisplayedRange();
    const rowCount = this.state.getTotalRowCount();
    const navigation = [
      exists(range) &&
        exists(rowCount) &&
        `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !this.state.canGoBack(),
        onclick: () => this.state.goBack(),
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !this.state.canGoForward(),
        onclick: () => this.state.goForward(),
      }),
    ];
    const {selectStatement, columns} = this.state.getCurrentRequest();
    const debugTrackColumns = Object.values(columns).filter(
      (c) => !c.startsWith('__'),
    );
    const addDebugTrack = m(
      Popup,
      {
        trigger: m(Button, {label: 'Show debug track'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        dataSource: {
          sqlSource: `SELECT ${debugTrackColumns.join(', ')} FROM (${selectStatement})`,
          columns: debugTrackColumns,
        },
        engine: this.engine,
      }),
    );

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.getDisplayName(),
        buttons: [
          ...navigation,
          addDebugTrack,
          m(Button, {
            label: 'Copy SQL query',
            onclick: () =>
              copyToClipboard(this.state.getNonPaginatedSQLQuery()),
          }),
        ],
      },
      m(SqlTable, {
        state: this.state,
      }),
    );
  }

  getTitle(): string {
    const rowCount = this.state.getTotalRowCount();
    const rows = rowCount === undefined ? '' : ` (${rowCount})`;
    return `Table ${this.getDisplayName()}${rows}`;
  }

  private getDisplayName(): string {
    return this.config.table.displayName ?? this.config.table.name;
  }

  isLoading(): boolean {
    return this.state.isLoading();
  }
}
