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
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../components/widgets/datagrid/sql_schema';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {shortUuid} from '../../base/uuid';

const UI_SCHEMA: SchemaRegistry = {
  memory_snapshot: {
    path: {
      title: 'Path',
      columnType: 'text',
    },
    size: {
      title: 'Size',
      columnType: 'quantitative',
    },
    effective_size: {
      title: 'Effective Size',
      columnType: 'quantitative',
    },
  },
};

class MemorySnapshotsTab implements m.ClassComponent<{trace: Trace}> {
  private dataSource?: SQLDataSource;

  view({attrs}: m.CVnode<{trace: Trace}>) {
    const {trace} = attrs;

    // Create data source lazily
    if (!this.dataSource) {
      this.dataSource = new SQLDataSource({
        engine: trace.engine,
        sqlSchema: createSimpleSchema('memory_snapshot_node'),
        rootSchemaName: 'query',
      });
    }

    return m(
      DetailsShell,
      {
        title: 'Memory Snapshots',
        description: 'Hierarchical view of memory snapshot nodes',
        fillHeight: true,
      },
      m(DataGrid, {
        schema: UI_SCHEMA,
        rootSchema: 'memory_snapshot',
        data: this.dataSource,
        fillHeight: true,
        initialPivot: {
          groupBy: [
            {
              id: shortUuid(),
              field: 'path',
              tree: {delimiter: '/'},
            },
          ],
          aggregates: [
            {
              id: shortUuid(),
              function: 'SUM',
              field: 'size',
            },
            {
              id: shortUuid(),
              function: 'SUM',
              field: 'effective_size',
            },
          ],
        },
      }),
    );
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.MemorySnapshots';
  static readonly description =
    'Displays memory snapshot nodes in a hierarchical tree view.';

  async onTraceLoad(trace: Trace) {
    trace.tabs.registerTab({
      uri: 'dev.perfetto.MemorySnapshots#MemorySnapshotsTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Memory Snapshots',
        render: () => m(MemorySnapshotsTab, {trace}),
      },
    });

    // Show the tab immediately on trace load
    trace.tabs.showTab('dev.perfetto.MemorySnapshots#MemorySnapshotsTab');
  }
}
