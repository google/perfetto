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
import {sqliteString} from '../../../../base/string_utils';
import {Duration, Time} from '../../../../base/time';
import {SqlValue, STR} from '../../../../trace_processor/query_result';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../../sql_utils/core_types';
import {Anchor} from '../../../../widgets/anchor';
import {renderError} from '../../../../widgets/error';
import {PopupMenu} from '../../../../widgets/menu';
import {DurationWidget} from '../../duration';
import {showProcessDetailsMenuItem} from '../../process';
import {SchedRef} from '../../sched';
import {SliceRef} from '../../slice';
import {showThreadDetailsMenuItem} from '../../thread';
import {ThreadStateRef} from '../../thread_state';
import {Timestamp} from '../../timestamp';
import {TableColumn, TableManager} from './table_column';
import {
  getStandardContextMenuItems,
  renderStandardCell,
} from './render_cell_utils';
import {SqlColumn, sqlColumnId, SqlExpression} from './sql_column';

function wrongTypeError(type: string, name: SqlColumn, value: SqlValue) {
  return renderError(
    `Wrong type for ${type} column ${sqlColumnId(name)}: bigint expected, ${typeof value} found`,
  );
}

export type ColumnParams = {
  alias?: string;
  startsHidden?: boolean;
  title?: string;
};

export type StandardColumnParams = ColumnParams;

export interface IdColumnParams {
  // Whether this column is a primary key (ID) for this table or whether it's a reference
  // to another table's primary key.
  type?: 'id' | 'joinid';
  // Whether the column is guaranteed not to have null values.
  // (this will allow us to upgrage the joins on this column to more performant INNER JOINs).
  notNull?: boolean;
}

export class StandardColumn implements TableColumn {
  constructor(
    public readonly column: SqlColumn,
    private params?: StandardColumnParams,
  ) {}

  renderCell(value: SqlValue, tableManager?: TableManager): m.Children {
    return renderStandardCell(value, this.column, tableManager);
  }

  initialColumns(): TableColumn[] {
    return this.params?.startsHidden ? [] : [this];
  }
}

export class TimestampColumn implements TableColumn {
  constructor(public readonly column: SqlColumn) {}

  renderCell(value: SqlValue, tableManager?: TableManager): m.Children {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, tableManager);
    }
    return m(Timestamp, {
      ts: Time.fromRaw(value),
      extraMenuItems:
        tableManager &&
        getStandardContextMenuItems(value, this.column, tableManager),
    });
  }
}

export class DurationColumn implements TableColumn {
  constructor(public column: SqlColumn) {}

  renderCell(value: SqlValue, tableManager?: TableManager): m.Children {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, tableManager);
    }

    return m(DurationWidget, {
      dur: Duration.fromRaw(value),
      extraMenuItems:
        tableManager &&
        getStandardContextMenuItems(value, this.column, tableManager),
    });
  }
}

export class SliceIdColumn implements TableColumn {
  constructor(
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {}

  renderCell(value: SqlValue, manager?: TableManager): m.Children {
    const id = value;

    if (!manager || id === null) {
      return renderStandardCell(id, this.column, manager);
    }

    return m(SliceRef, {
      id: asSliceSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        ['ts', new TimestampColumn(this.getChildColumn('ts'))],
        ['dur', new DurationColumn(this.getChildColumn('dur'))],
        ['name', new StandardColumn(this.getChildColumn('name'))],
        ['parent_id', new SliceIdColumn(this.getChildColumn('parent_id'))],
      ]);
  }

  private getChildColumn(name: string): SqlColumn {
    return {
      column: name,
      source: {
        table: 'slice',
        joinOn: {id: this.column},
      },
    };
  }
}

export class SchedIdColumn implements TableColumn {
  constructor(public readonly column: SqlColumn) {}

  renderCell(value: SqlValue, manager?: TableManager): m.Children {
    const id = value;

    if (!manager || id === null) {
      return renderStandardCell(id, this.column, manager);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.column, id);

    return m(SchedRef, {
      id: asSchedSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadStateIdColumn implements TableColumn {
  constructor(public readonly column: SqlColumn) {}

  renderCell(value: SqlValue, manager?: TableManager): m.Children {
    const id = value;

    if (!manager || id === null) {
      return renderStandardCell(id, this.column, manager);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.column, id);

    return m(ThreadStateRef, {
      id: asThreadStateSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadIdColumn implements TableColumn {
  constructor(
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {}

  renderCell(value: SqlValue, manager?: TableManager): m.Children {
    const utid = value;

    if (!manager || utid === null) {
      return renderStandardCell(utid, this.column, manager);
    }

    if (typeof utid !== 'bigint') {
      throw new Error(
        `thread.utid is expected to be bigint, got ${typeof utid}`,
      );
    }

    return m(
      PopupMenu,
      {
        trigger: m(Anchor, `${utid}`),
      },

      showThreadDetailsMenuItem(asUtid(Number(utid))),
      getStandardContextMenuItems(utid, this.column, manager),
    );
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        ['tid', new StandardColumn(this.getChildColumn('tid'))],
        ['name', new StandardColumn(this.getChildColumn('name'))],
        ['start_ts', new TimestampColumn(this.getChildColumn('start_ts'))],
        ['end_ts', new TimestampColumn(this.getChildColumn('end_ts'))],
        ['upid', new ProcessIdColumn(this.getChildColumn('upid'))],
        [
          'is_main_thread',
          new StandardColumn(this.getChildColumn('is_main_thread')),
        ],
      ]);
  }

  initialColumns(): TableColumn[] {
    return [
      this,
      new StandardColumn(this.getChildColumn('tid')),
      new StandardColumn(this.getChildColumn('name')),
    ];
  }

  private getChildColumn(name: string): SqlColumn {
    return {
      column: name,
      source: {
        table: 'thread',
        joinOn: {id: this.column},
        // If the column is guaranteed not to have null values, we can use an INNER JOIN.
        innerJoin: this.params?.notNull === true,
      },
    };
  }
}

export class ProcessIdColumn implements TableColumn {
  constructor(
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {}

  renderCell(value: SqlValue, manager?: TableManager): m.Children {
    const upid = value;

    if (!manager || upid === null) {
      return renderStandardCell(upid, this.column, manager);
    }

    if (typeof upid !== 'bigint') {
      throw new Error(
        `thread.upid is expected to be bigint, got ${typeof upid}`,
      );
    }

    return m(
      PopupMenu,
      {
        trigger: m(Anchor, `${upid}`),
      },

      showProcessDetailsMenuItem(asUpid(Number(upid))),
      getStandardContextMenuItems(upid, this.column, manager),
    );
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        ['pid', new StandardColumn(this.getChildColumn('pid'))],
        ['name', new StandardColumn(this.getChildColumn('name'))],
        ['start_ts', new TimestampColumn(this.getChildColumn('start_ts'))],
        ['end_ts', new TimestampColumn(this.getChildColumn('end_ts'))],
        [
          'parent_upid',
          new ProcessIdColumn(this.getChildColumn('parent_upid')),
        ],
        [
          'is_main_thread',
          new StandardColumn(this.getChildColumn('is_main_thread')),
        ],
      ]);
  }

  initialColumns(): TableColumn[] {
    return [
      this,
      new StandardColumn(this.getChildColumn('pid')),
      new StandardColumn(this.getChildColumn('name')),
    ];
  }

  private getChildColumn(name: string): SqlColumn {
    return {
      column: name,
      source: {
        table: 'process',
        joinOn: {id: this.column},
        // If the column is guaranteed not to have null values, we can use an INNER JOIN.
        innerJoin: this.params?.notNull === true,
      },
    };
  }
}

class ArgColumn implements TableColumn<{type: SqlColumn}> {
  public readonly column: SqlColumn;
  private id: string;

  constructor(
    private argSetId: SqlColumn,
    private key: string,
  ) {
    this.id = `${sqlColumnId(this.argSetId)}[${this.key}]`;
    this.column = new SqlExpression(
      (cols: string[]) => `COALESCE(${cols[0]}, ${cols[1]}, ${cols[2]})`,
      [
        this.getRawColumn('string_value'),
        this.getRawColumn('int_value'),
        this.getRawColumn('real_value'),
      ],
      this.id,
    );
  }

  supportingColumns() {
    return {type: this.getRawColumn('value_type')};
  }

  private getRawColumn(
    type: 'string_value' | 'int_value' | 'real_value' | 'id' | 'value_type',
  ): SqlColumn {
    return {
      column: type,
      source: {
        table: 'args',
        joinOn: {
          arg_set_id: this.argSetId,
          key: `${sqliteString(this.key)}`,
        },
      },
      id: `${this.id}.${type.replace(/_value$/g, '')}`,
    };
  }

  renderCell(
    value: SqlValue,
    tableManager?: TableManager,
    values?: {type: SqlValue},
  ): m.Children {
    // If the value is NULL, then filters can check for id column for better performance.
    if (value === null) {
      return renderStandardCell(
        value,
        this.getRawColumn('value_type'),
        tableManager,
      );
    }
    if (values?.type === 'int') {
      return renderStandardCell(
        value,
        this.getRawColumn('int_value'),
        tableManager,
      );
    }
    if (values?.type === 'string') {
      return renderStandardCell(
        value,
        this.getRawColumn('string_value'),
        tableManager,
      );
    }
    if (values?.type === 'real') {
      return renderStandardCell(
        value,
        this.getRawColumn('real_value'),
        tableManager,
      );
    }
    return renderStandardCell(value, this.column, tableManager);
  }
}

export class ArgSetIdColumn implements TableColumn {
  constructor(public readonly column: SqlColumn) {}

  renderCell(value: SqlValue, tableManager: TableManager): m.Children {
    return renderStandardCell(value, this.column, tableManager);
  }

  listDerivedColumns(manager: TableManager) {
    return async () => {
      const queryResult = await manager.trace.engine.query(`
        SELECT
          DISTINCT args.key
        FROM (${manager.getSqlQuery({arg_set_id: this.column})}) data
        JOIN args USING (arg_set_id)
      `);
      const result = new Map();
      const it = queryResult.iter({key: STR});
      for (; it.valid(); it.next()) {
        result.set(it.key, argTableColumn(this.column, it.key));
      }
      return result;
    };
  }

  initialColumns() {
    return [];
  }
}

export function argTableColumn(argSetId: SqlColumn, key: string): TableColumn {
  return new ArgColumn(argSetId, key);
}
