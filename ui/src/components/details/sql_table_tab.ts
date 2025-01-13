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
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {exists} from '../../base/utils';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {Popup, PopupPosition} from '../../widgets/popup';
import {AddDebugTrackMenu} from '../tracks/add_debug_track_menu';
import {Filter} from '../widgets/sql/legacy_table/column';
import {SqlTableState} from '../widgets/sql/legacy_table/state';
import {SqlTable} from '../widgets/sql/legacy_table/table';
import {SqlTableDescription} from '../widgets/sql/legacy_table/table_description';
import {Trace} from '../../public/trace';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {addEphemeralTab} from './add_ephemeral_tab';
import {Tab} from '../../public/tab';
import {addChartTab} from '../widgets/charts/chart_tab';
import {
  ChartOption,
  createChartConfigFromSqlTableState,
} from '../widgets/charts/chart';
import {AddChartMenuItem} from '../widgets/charts/add_chart_menu';

export interface AddSqlTableTabParams {
  table: SqlTableDescription;
  filters?: Filter[];
  imports?: string[];
}

export function addLegacyTableTab(
  trace: Trace,
  config: AddSqlTableTabParams,
): void {
  addSqlTableTabWithState(
    new SqlTableState(trace, config.table, {
      filters: config.filters,
      imports: config.imports,
    }),
  );
}

function addSqlTableTabWithState(state: SqlTableState) {
  addEphemeralTab('sqlTable', new LegacySqlTableTab(state));
}

class LegacySqlTableTab implements Tab {
  constructor(private readonly state: SqlTableState) {}

  render() {
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
        trace: this.state.trace,
        dataSource: {
          sqlSource: `SELECT ${debugTrackColumns.join(', ')} FROM (${selectStatement})`,
          columns: debugTrackColumns,
        },
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
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                icon: Icons.Menu,
              }),
            },
            m(MenuItem, {
              label: 'Duplicate',
              icon: 'tab_duplicate',
              onclick: () => addSqlTableTabWithState(this.state.clone()),
            }),
            m(MenuItem, {
              label: 'Copy SQL query',
              icon: Icons.Copy,
              onclick: () =>
                copyToClipboard(this.state.getNonPaginatedSQLQuery()),
            }),
          ),
        ],
      },
      m(SqlTable, {
        state: this.state,
        addColumnMenuItems: (column, columnAlias) =>
          m(AddChartMenuItem, {
            chartConfig: createChartConfigFromSqlTableState(
              column,
              columnAlias,
              this.state,
            ),
            chartOptions: [ChartOption.HISTOGRAM],
            addChart: (chart) => addChartTab(chart),
          }),
      }),
    );
  }

  getTitle(): string {
    const rowCount = this.state.getTotalRowCount();
    const rows = rowCount === undefined ? '' : ` (${rowCount})`;
    return `Table ${this.getDisplayName()}${rows}`;
  }

  private getDisplayName(): string {
    return this.state.config.displayName ?? this.state.config.name;
  }

  isLoading(): boolean {
    return this.state.isLoading();
  }
}
