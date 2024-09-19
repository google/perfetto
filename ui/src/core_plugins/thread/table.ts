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
import {SqlTableDescription} from '../../frontend/widgets/sql/table/table_description';
import {SqlValue} from '../../trace_processor/query_result';
import {PopupMenu2} from '../../widgets/menu';
import {Anchor} from '../../widgets/anchor';
import {getStandardContextMenuItems} from '../../frontend/widgets/sql/table/render_cell_utils';
import {
  ProcessColumn,
  StandardColumn,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';
import {showThreadDetailsMenuItem} from '../../frontend/widgets/thread';
import {asUtid} from '../../trace_processor/sql_utils/core_types';

// ThreadIdColumn is a column type for displaying primary key of the `thread` table.
// All other references (foreign keys) should use `ThreadColumn` instead.
class ThreadIdColumn extends TableColumn {
  private columns: {tid: SqlColumn};

  constructor(private utid: SqlColumn) {
    super({});

    const threadTable: SourceTable = {
      table: 'thread',
      joinOn: {id: this.utid},
      innerJoin: true,
    };

    this.columns = {
      tid: {
        column: 'tid',
        source: threadTable,
      },
    };
  }

  primaryColumn(): SqlColumn {
    return this.utid;
  }

  getTitle() {
    return 'utid';
  }

  dependentColumns() {
    return {
      tid: this.columns.tid,
    };
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const utid = value;
    const rawTid = data['tid'];

    if (typeof utid !== 'bigint') {
      throw new Error(
        `thread.upid is expected to be bigint, got ${typeof utid}`,
      );
    }

    return m(
      PopupMenu2,
      {
        trigger: m(Anchor, `${utid}`),
      },

      showThreadDetailsMenuItem(
        asUtid(Number(utid)),
        rawTid === null ? undefined : Number(rawTid),
      ),
      getStandardContextMenuItems(utid, this.utid, manager),
    );
  }

  aggregation(): AggregationConfig {
    return {dataType: 'nominal'};
  }
}

export function getThreadTable(): SqlTableDescription {
  return {
    name: 'thread',
    columns: [
      new ThreadIdColumn('utid'),
      new StandardColumn('tid', {aggregationType: 'nominal'}),
      new StandardColumn('name'),
      new TimestampColumn('start_ts'),
      new TimestampColumn('end_ts'),
      new ProcessColumn('upid', {notNull: true}),
      new StandardColumn('is_main_thread', {
        aggregationType: 'nominal',
      }),
    ],
  };
}
