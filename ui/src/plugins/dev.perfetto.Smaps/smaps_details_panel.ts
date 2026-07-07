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
import type {time} from '../../base/time';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import type {Column} from '../../components/widgets/datagrid/model';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {createSimpleSchema} from '../../components/widgets/datagrid/sql_schema';
import {asUpid} from '../../components/sql_utils/core_types';
import {
  getProcessInfo,
  getProcessName,
} from '../../components/sql_utils/process';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {NUM_NULL, type Row} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {Spinner} from '../../widgets/spinner';

const ROOT_SCHEMA = 'mapping';

const COLUMNS: ReadonlyArray<{name: string; title: string}> = [
  {name: 'path', title: 'Path'},
  {name: 'aggregate_count', title: 'Count'},
  {name: 'size_kb', title: 'Size (KB)'},
  {name: 'rss_kb', title: 'RSS (KB)'},
  {name: 'pss_kb', title: 'PSS (KB)'},
  {name: 'pss_dirty_kb', title: 'PSS dirty (KB)'},
  {name: 'shared_clean_kb', title: 'Shared clean (KB)'},
  {name: 'shared_dirty_kb', title: 'Shared dirty (KB)'},
  {name: 'private_clean_kb', title: 'Private clean (KB)'},
  {name: 'private_dirty_kb', title: 'Private dirty (KB)'},
  {name: 'anonymous_kb', title: 'Anonymous (KB)'},
  {name: 'swap_kb', title: 'Swap (KB)'},
  {name: 'swap_pss_kb', title: 'Swap PSS (KB)'},
  {name: 'locked_kb', title: 'Locked (KB)'},
];

function buildGridSchema(): SchemaRegistry {
  const mapping: ColumnSchema = {};
  for (const col of COLUMNS) {
    mapping[col.name] = {
      title: col.title,
      columnType: col.name === 'path' ? 'text' : 'quantitative',
    };
  }
  return {[ROOT_SCHEMA]: mapping};
}

// Datagrid schema with all possible underlying columns.
const SMAPS_SCHEMA = buildGridSchema();

// Computes the columns worth showing in the datagrid, using the entire table
// of memory mapping snapshots. For the memory value columns, which might not
// all be recorded: the sql columns are not nullable, so instead we show any
// column that has non-zero values (which is indistinguishable from truly
// all-zeroes readings). Similarly, the aggregate count is only shown if it's
// ever >1.
export async function computeInitialColumns(
  engine: Engine,
): Promise<ReadonlyArray<Column>> {
  const numeric = COLUMNS.filter((c) => c.name !== 'path');
  const result = await engine.query(`
    SELECT ${numeric.map((c) => `MAX(${c.name}) AS ${c.name}`).join(', ')}
    FROM process_memory_mappings
  `);
  const spec: Row = {};
  for (const c of numeric) {
    spec[c.name] = NUM_NULL;
  }
  const agg = result.firstRow(spec);

  const initialColumns: Column[] = [];
  for (const col of COLUMNS) {
    if (col.name !== 'path') {
      const max = Number(agg[col.name] ?? 0);
      const show = col.name === 'aggregate_count' ? max !== 1 : max !== 0;
      if (!show) continue;
    }
    initialColumns.push({id: col.name, field: col.name});
  }
  return initialColumns;
}

export class SmapsDetailsPanel implements TrackEventDetailsPanel {
  private processName?: string;
  private initialColumns?: ReadonlyArray<Column>;
  private readonly dataSource: SQLDataSource;

  constructor(
    private readonly trace: Trace,
    private readonly upid: number,
    private readonly ts: time,
    private readonly getInitialColumns: () => Promise<ReadonlyArray<Column>>,
  ) {
    // Instead of showing an is_deleted column, use it to reconstruct the
    // original VMA name as reported by the kernel (i.e. suffix it with
    // " (deleted)").
    const selectCols = COLUMNS.map((c) =>
      c.name === 'path'
        ? `iif(is_deleted, path || ' (deleted)', path) AS path`
        : c.name,
    ).join(', ');
    this.dataSource = new SQLDataSource({
      engine: trace.engine,
      sqlSchema: createSimpleSchema(`
        SELECT ${selectCols}
        FROM process_memory_mappings
        WHERE upid = ${upid} AND ts = ${ts}
      `),
      rootSchemaName: 'query',
    });
  }

  async load(): Promise<void> {
    const [info, initialColumns] = await Promise.all([
      getProcessInfo(this.trace.engine, asUpid(this.upid)),
      this.getInitialColumns(),
    ]);
    this.processName = getProcessName(info) ?? `upid: ${this.upid}`;
    this.initialColumns = initialColumns;
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: this.processName ?? 'process',
        description: m('span', [
          'Memory mapping snapshot at ',
          m(Timestamp, {trace: this.trace, ts: this.ts}),
        ]),
        fillHeight: true,
      },
      this.renderContent(),
    );
  }

  private renderContent(): m.Children {
    const {initialColumns} = this;
    if (initialColumns === undefined) {
      return m(Spinner);
    }
    return m(DataGrid, {
      fillHeight: true,
      schema: SMAPS_SCHEMA,
      rootSchema: ROOT_SCHEMA,
      initialColumns,
      data: this.dataSource,
      showExportButton: true,
    });
  }
}
