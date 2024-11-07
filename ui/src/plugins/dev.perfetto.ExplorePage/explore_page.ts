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
import {PageWithTraceAttrs} from '../../public/page';
import {Trace} from '../../public/trace';
import {
  DurationColumn,
  ProcessColumnSet,
  StandardColumn,
  ThreadColumnSet,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';
import {SqlTableState} from '../../frontend/widgets/sql/table/state';
import {SqlTable} from '../../frontend/widgets/sql/table/table';
import {exists} from '../../base/utils';
import {Menu, MenuItem, MenuItemAttrs} from '../../widgets/menu';
import {
  TableColumn,
  TableColumnSet,
} from '../../frontend/widgets/sql/table/column';
import {Button} from '../../widgets/button';
import {Icons} from '../../base/semantic_icons';
import {DetailsShell} from '../../widgets/details_shell';

interface ExplorePageState {
  sqlTableState?: SqlTableState;
  selectedTable?: ExplorableTable;
}

interface ExplorableTable {
  name: string;
  module: string;
  columns: (TableColumn | TableColumnSet)[];
}

export class ExplorePage implements m.ClassComponent<PageWithTraceAttrs> {
  private readonly state: ExplorePageState;

  constructor() {
    this.state = {
      sqlTableState: undefined,
      selectedTable: undefined,
    };
  }

  // Show menu with standard library tables
  private renderSelectableTablesMenuItems(
    trace: Trace,
  ): m.Vnode<MenuItemAttrs, unknown>[] {
    // TODO (lydiatse@): The following is purely for prototyping and
    // should be derived from the actual stdlib itself rather than
    // being hardcoded.
    const explorableTables: ExplorableTable[] = [
      {
        name: 'android_binder_txns',
        module: 'android.binder',
        columns: [
          new StandardColumn('aidl_name'),
          new StandardColumn('aidl_ts'),
          new StandardColumn('aidl_dur'),
          new StandardColumn('binder_txn_id', {startsHidden: true}),
          new ProcessColumnSet('client_upid', {title: 'client_upid'}),
          new ThreadColumnSet('client_utid', {title: 'client_utid'}),
          new StandardColumn('is_main_thread'),
          new TimestampColumn('client_ts'),
          new DurationColumn('client_dur'),
          new StandardColumn('binder_reply_id', {startsHidden: true}),
          new ProcessColumnSet('server_upid', {title: 'server_upid'}),
          new ThreadColumnSet('server_utid', {title: 'server_utid'}),
          new TimestampColumn('server_ts'),
          new DurationColumn('server_dur'),
          new StandardColumn('client_oom_score', {aggregationType: 'nominal'}),
          new StandardColumn('server_oom_score', {aggregationType: 'nominal'}),
          new StandardColumn('is_sync', {startsHidden: true}),
          new StandardColumn('client_monotonic_dur', {startsHidden: true}),
          new StandardColumn('server_monotonic_dur', {startsHidden: true}),
          new StandardColumn('client_package_version_code', {
            startsHidden: true,
          }),
          new StandardColumn('server_package_version_code', {
            startsHidden: true,
          }),
          new StandardColumn('is_client_package_debuggable', {
            startsHidden: true,
          }),
          new StandardColumn('is_server_package_debuggable', {
            startsHidden: true,
          }),
        ],
      },
    ];

    return explorableTables.map((table) => {
      return m(MenuItem, {
        label: table.name,
        onclick: () => {
          if (
            this.state.selectedTable &&
            table.name === this.state.selectedTable.name
          ) {
            return;
          }

          this.state.selectedTable = table;

          const sqlTableState = new SqlTableState(
            trace,
            {
              name: table.name,
              columns: table.columns,
            },
            {imports: [table.module]},
          );
          this.state.sqlTableState = sqlTableState;
        },
      });
    });
  }

  private renderSqlTable() {
    const sqlTableState = this.state.sqlTableState;

    if (sqlTableState === undefined) return;

    const range = sqlTableState.getDisplayedRange();
    const rowCount = sqlTableState.getTotalRowCount();

    const navigation = [
      exists(range) &&
        exists(rowCount) &&
        `Showing rows ${range.from}-${range.to} of ${rowCount}`,
      m(Button, {
        icon: Icons.GoBack,
        disabled: !sqlTableState.canGoBack(),
        onclick: () => sqlTableState!.goBack(),
      }),
      m(Button, {
        icon: Icons.GoForward,
        disabled: !sqlTableState.canGoForward(),
        onclick: () => sqlTableState!.goForward(),
      }),
    ];

    return m(
      DetailsShell,
      {
        title: 'Explore Table',
        buttons: navigation,
        fillParent: false,
      },
      m(SqlTable, {
        state: sqlTableState,
      }),
    );
  }

  view({attrs}: m.CVnode<PageWithTraceAttrs>) {
    return m(
      '.explore-page',
      m(Menu, this.renderSelectableTablesMenuItems(attrs.trace)),
      this.state.selectedTable && this.renderSqlTable(),
    );
  }
}
