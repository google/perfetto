// Copyright (C) 2026 The Android Open Source Project
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
import z from 'zod';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {shortUuid} from '../../base/uuid';
import {
  DataGrid,
  DataGridAttrs,
} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../components/widgets/datagrid/sql_schema';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.PivotTable';

  private readonly dataSourceMap = new Map<string, SQLDataSource>();

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.commands.registerCommand({
      id: 'dev.perfetto.PivotTable.openExamplePivotTableTab',
      name: 'Open example Pivot Table Tab',
      callback: () => {
        trace.tabs.openGenericTab({
          id: shortUuid(),
          tabId: 'dev.perfetto.PivotTableTab',
          config: {query: 'SELECT * FROM slice LIMIT 1000'},
        });
      },
    });

    trace.tabs.registerGenericTab({
      id: 'dev.perfetto.PivotTableTab',
      schema: z.object({
        query: z.string(),
      }),
      render: (id, config) => {
        console.log('Rendering Pivot Table Tab with config:', config);
        const tableName = 'query';
        let dataSource = this.dataSourceMap.get(id);
        if (!dataSource) {
          const sqlSchema = createSimpleSchema(config.query, tableName);
          dataSource = new SQLDataSource({
            engine: trace.engine,
            sqlSchema: sqlSchema,
            rootSchemaName: tableName,
          });
          this.dataSourceMap.set(id, dataSource);
        }
        return m(DataGrid, {
          fillHeight: true,
          data: dataSource,
          schema: {
            [tableName]: {
              id: {},
              name: {},
              ts: {},
              dur: {},
            },
          },
          rootSchema: tableName,
        } satisfies DataGridAttrs);
      },
    });
  }
}
