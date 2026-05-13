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
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {Select} from '../../widgets/select';
import {NUM, STR_NULL} from '../../trace_processor/query_result';

interface SnapshotInfo {
  id: number;
  name: string;
}

function renderSize(value: unknown): string {
  if (value === null || value === undefined) return '';
  return Number(value).toLocaleString();
}

const UI_SCHEMA: SchemaRegistry = {
  memory_snapshot_node: {
    path: {
      title: 'Path',
      columnType: 'text',
    },
    size: {
      title: 'Size',
      columnType: 'quantitative',
      cellRenderer: (value) => renderSize(value),
    },
    effective_size: {
      title: 'Effective Size',
      columnType: 'quantitative',
      cellRenderer: (value) => renderSize(value),
    },
    all_args: {
      title: 'All Args',
      columnType: 'text',
    },
    args: {
      title: 'Args',
      parameterized: true,
    },
  },
};

// SQL schema for memory_snapshot_node with args support
function createMemorySnapshotSchema(snapshotId: number): SQLSchemaRegistry {
  const query = `(
    SELECT
      SUBSTR(path, LENGTH(RTRIM(path, REPLACE(path, '/', ''))) + 1) AS path,
      id,
      parent_node_id,
      size,
      effective_size
    FROM memory_snapshot_node
    WHERE process_snapshot_id = ${snapshotId}
  )`;
  return {
    memory_snapshot_node: {
      table: query,
      columns: {
        id: {},
        parent_node_id: {},
        path: {},
        size: {},
        effective_size: {},
        all_args: {
          expression: (alias) =>
            `__intrinsic_arg_set_to_json(${alias}.arg_set_id)`,
        },
        args: {
          expression: (alias, key) =>
            `extract_arg(${alias}.arg_set_id, '${key}')`,
          parameterized: true,
          parameterKeysQuery: (baseTable, baseAlias) => `
            SELECT DISTINCT args.key
            FROM ${baseTable} AS ${baseAlias}
            JOIN args ON args.arg_set_id = ${baseAlias}.arg_set_id
            WHERE args.key IS NOT NULL
            ORDER BY args.key
            LIMIT 1000
          `,
        },
      },
    },
  };
}

interface SnapshotTabAttrs {
  trace: Trace;
  snapshotId: number;
}

class SnapshotTab implements m.ClassComponent<SnapshotTabAttrs> {
  private dataSource?: SQLDataSource;

  view({attrs}: m.CVnode<SnapshotTabAttrs>) {
    const {trace, snapshotId} = attrs;

    // Create data source lazily
    if (!this.dataSource) {
      this.dataSource = new SQLDataSource({
        engine: trace.engine,
        sqlSchema: createMemorySnapshotSchema(snapshotId),
        rootSchemaName: 'memory_snapshot_node',
      });
    }

    return m(DataGrid, {
      schema: UI_SCHEMA,
      rootSchema: 'memory_snapshot_node',
      data: this.dataSource,
      fillHeight: true,
      initialTree: {
        idField: 'id',
        parentIdField: 'parent_node_id',
        treeColumn: 'path',
      },
      initialColumns: [
        {id: 'path', field: 'path'},
        {id: 'size', field: 'size'},
        {id: 'effective_size', field: 'effective_size'},
      ],
    });
  }
}

class MemorySnapshotsTab implements m.ClassComponent<{trace: Trace}> {
  private snapshots?: SnapshotInfo[];
  private selectedSnapshotId?: number;

  async loadSnapshots(trace: Trace) {
    // Query snapshot IDs with process names
    const result = await trace.engine.query(`
      SELECT
        pms.id AS process_snapshot_id,
        p.name AS process_name
      FROM process_memory_snapshot pms
      LEFT JOIN process p ON pms.upid = p.id
      WHERE pms.id IN (SELECT DISTINCT process_snapshot_id FROM memory_snapshot_node)
      ORDER BY pms.id
    `);
    this.snapshots = [];
    for (
      const it = result.iter({
        process_snapshot_id: NUM,
        process_name: STR_NULL,
      });
      it.valid();
      it.next()
    ) {
      const id = it.process_snapshot_id;
      const processName = it.process_name ?? 'Unknown';
      this.snapshots.push({
        id,
        name: `${processName} (${id})`,
      });
    }
    // Select first snapshot by default
    if (this.snapshots.length > 0) {
      this.selectedSnapshotId = this.snapshots[0].id;
    }
  }

  view({attrs}: m.CVnode<{trace: Trace}>) {
    const {trace} = attrs;

    // Load snapshots if not yet loaded
    if (this.snapshots === undefined) {
      this.loadSnapshots(trace);
      return m(DetailsShell, {
        title: 'Memory Snapshots',
        description: 'Loading...',
        fillHeight: true,
      });
    }

    if (this.snapshots.length === 0) {
      return m(DetailsShell, {
        title: 'Memory Snapshots',
        description: 'No memory snapshots found',
        fillHeight: true,
      });
    }

    return m(
      DetailsShell,
      {
        title: 'Memory Snapshots',
        description: m(
          Select,
          {
            value: String(this.selectedSnapshotId),
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              this.selectedSnapshotId = Number(target.value);
            },
          },
          this.snapshots.map((snapshot) =>
            m('option', {value: String(snapshot.id)}, snapshot.name),
          ),
        ),
        fillHeight: true,
      },
      this.selectedSnapshotId !== undefined &&
        m(SnapshotTab, {
          key: this.selectedSnapshotId,
          trace,
          snapshotId: this.selectedSnapshotId,
        }),
    );
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.MemorySnapshots';
  static readonly description =
    'Displays memory snapshot nodes in a hierarchical tree view.';

  async onTraceLoad(trace: Trace) {
    trace.tabs.registerTab({
      uri: 'org.chromium.MemorySnapshotsTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Memory Snapshots',
        render: () => m(MemorySnapshotsTab, {trace}),
      },
    });

    // Show the tab immediately on trace load
    trace.tabs.showTab('org.chromium.MemorySnapshotsTab');
  }
}
