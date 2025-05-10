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
import {DataGridDataSource} from '../../components/widgets/data_grid/common';
import {DataGrid} from '../../components/widgets/data_grid/data_grid';
import {SQLDataSource} from '../../components/widgets/data_grid/sql_data_source';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {QueryPage} from './query_page';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.QueryPage';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.pages.registerPage({
      route: '/query',
      render: () => m(QueryPage, {trace}),
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      text: 'Query (SQL)',
      href: '#!/query',
      icon: 'database',
      sortOrder: 1,
    });

    const datasource = new SQLDataSource(
      trace.engine,
      'select * from ftrace_event',
    );

    trace.tabs.registerTab({
      uri: 'testtab',
      content: {
        getTitle: () => 'Test',
        render: () => renderTab(datasource),
      },
      isEphemeral: false,
    });
  }
}

function renderTab(datasource: DataGridDataSource) {
  return m(DataGrid, {
    dataSource: datasource,
    columns: [{name: 'id'}, {name: 'ts'}, {name: 'name'}, {name: 'dur'}],
  });
}
