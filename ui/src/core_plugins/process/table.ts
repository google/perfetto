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

import {
  AggregationConfig,
  SourceTable,
  SqlColumn,
  TableColumn,
  TableManager,
} from '../../frontend/widgets/sql/table/column';
import {getStandardContextMenuItems} from '../../frontend/widgets/sql/table/render_cell_utils';
import {SqlTableDescription} from '../../frontend/widgets/sql/table/table_description';
import {
  ArgSetColumnSet,
  ProcessColumn,
  StandardColumn,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';
import {SqlValue} from '../../trace_processor/sql_utils';
import {PopupMenu2} from '../../widgets/menu';
import {Anchor} from '../../widgets/anchor';
import {showProcessDetailsMenuItem} from '../../frontend/widgets/process';
import {asUpid} from '../../trace_processor/sql_utils/core_types';

// ProcessIdColumn is a column type for displaying primary key of the `process` table.
// All other references (foreign keys) should use `ProcessColumn` instead.
class ProcessIdColumn extends TableColumn {
  private columns: {pid: SqlColumn};

  constructor(private upid: SqlColumn) {
    super({});

    const processTable: SourceTable = {
      table: 'process',
      joinOn: {id: this.upid},
      innerJoin: true,
    };

    this.columns = {
      pid: {
        column: 'pid',
        source: processTable,
      },
    };
  }

  primaryColumn(): SqlColumn {
    return this.upid;
  }

  getTitle() {
    return 'upid';
  }

  dependentColumns() {
    return {
      pid: this.columns.pid,
    };
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const upid = value;
    const rawPid = data['pid'];

    if (typeof upid !== 'bigint') {
      throw new Error(
        `process.upid is expected to be bigint, got ${typeof upid}`,
      );
    }

    return m(
      PopupMenu2,
      {
        trigger: m(Anchor, `${upid}`),
      },

      showProcessDetailsMenuItem(
        asUpid(Number(upid)),
        rawPid === null ? undefined : Number(rawPid),
      ),
      getStandardContextMenuItems(upid, this.upid, manager),
    );
  }

  aggregation(): AggregationConfig {
    return {dataType: 'nominal'};
  }
}

export function getProcessTable(): SqlTableDescription {
  return {
    name: 'process',
    columns: [
      new ProcessIdColumn('upid'),
      new StandardColumn('pid', {aggregationType: 'nominal'}),
      new StandardColumn('name'),
      new TimestampColumn('start_ts'),
      new TimestampColumn('end_ts'),
      new ProcessColumn('parent_upid'),
      new StandardColumn('uid', {aggregationType: 'nominal'}),
      new StandardColumn('android_appid', {aggregationType: 'nominal'}),
      new StandardColumn('cmdline', {startsHidden: true}),
      new StandardColumn('machine_id', {aggregationType: 'nominal'}),
      new ArgSetColumnSet('arg_set_id'),
    ],
  };
}
