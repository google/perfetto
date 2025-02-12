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
import {sqliteString} from '../../base/string_utils';
import {Duration, Time} from '../../base/time';
import {SqlValue, STR} from '../../trace_processor/query_result';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../components/sql_utils/core_types';
import {Anchor} from '../../widgets/anchor';
import {renderError} from '../../widgets/error';
import {PopupMenu} from '../../widgets/menu';
import {DurationWidget} from '../../components/widgets/duration';
import {showProcessDetailsMenuItem} from '../../components/widgets/process';
import {SchedRef} from '../../components/widgets/sched';
import {SliceRef} from '../../components/widgets/slice';
import {showThreadDetailsMenuItem} from '../../components/widgets/thread';
import {ThreadStateRef} from '../../components/widgets/thread_state';
import {Timestamp} from '../../components/widgets/timestamp';
import {
  SourceTable,
  SqlColumn,
  sqlColumnId,
  LegacyTableColumn,
  LegacyTableManager,
} from '../../components/widgets/sql/legacy_table/column';
import {
  displayValue,
  getStandardContextMenuItems,
  renderStandardCell,
} from '../../components/widgets/sql/legacy_table/render_cell_utils';

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

export class StandardColumn extends LegacyTableColumn {
  constructor(
    private column: SqlColumn,
    private params?: StandardColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.column;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, tableManager: LegacyTableManager): m.Children {
    return renderStandardCell(value, this.column, tableManager);
  }

  override initialColumns(): LegacyTableColumn[] {
    return this.params?.startsHidden ? [] : [this];
  }
}

export class TimestampColumn extends LegacyTableColumn {
  constructor(
    private column: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.column;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, tableManager: LegacyTableManager): m.Children {
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, tableManager);
    }
    return m(Timestamp, {
      ts: Time.fromRaw(value),
      extraMenuItems: getStandardContextMenuItems(
        value,
        this.column,
        tableManager,
      ),
    });
  }
}

export class DurationColumn extends LegacyTableColumn {
  constructor(
    private column: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.column;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, tableManager: LegacyTableManager): m.Children {
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, tableManager);
    }

    return m(DurationWidget, {
      dur: Duration.fromRaw(value),
      extraMenuItems: getStandardContextMenuItems(
        value,
        this.column,
        tableManager,
      ),
    });
  }
}

export class SliceIdColumn extends LegacyTableColumn {
  constructor(
    private id: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, manager: LegacyTableManager): m.Children {
    const id = value;

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }

    return m(SliceRef, {
      id: asSliceSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }

  override listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, LegacyTableColumn>([
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
        joinOn: {id: this.id},
      },
    };
  }
}

export class SchedIdColumn extends LegacyTableColumn {
  constructor(
    private id: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, manager: LegacyTableManager): m.Children {
    const id = value;

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.id, id);

    return m(SchedRef, {
      id: asSchedSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadStateIdColumn extends LegacyTableColumn {
  constructor(
    private id: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  renderCell(value: SqlValue, manager: LegacyTableManager): m.Children {
    const id = value;

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.id, id);

    return m(ThreadStateRef, {
      id: asThreadStateSqlId(Number(id)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadIdColumn extends LegacyTableColumn {
  constructor(
    private utid: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super();
  }

  primaryColumn(): SqlColumn {
    return this.utid;
  }

  renderCell(value: SqlValue, manager: LegacyTableManager): m.Children {
    const utid = value;

    if (utid === null) {
      return renderStandardCell(utid, this.utid, manager);
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
      getStandardContextMenuItems(utid, this.utid, manager),
    );
  }

  override listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, LegacyTableColumn>([
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

  override initialColumns(): LegacyTableColumn[] {
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
        joinOn: {id: this.utid},
        // If the column is guaranteed not to have null values, we can use an INNER JOIN.
        innerJoin: this.params?.notNull === true,
      },
    };
  }
}

export class ProcessIdColumn extends LegacyTableColumn {
  constructor(
    private upid: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super();
  }

  primaryColumn(): SqlColumn {
    return this.upid;
  }

  renderCell(value: SqlValue, manager: LegacyTableManager): m.Children {
    const upid = value;

    if (upid === null) {
      return renderStandardCell(upid, this.upid, manager);
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
      getStandardContextMenuItems(upid, this.upid, manager),
    );
  }

  override listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, LegacyTableColumn>([
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

  override initialColumns(): LegacyTableColumn[] {
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
        joinOn: {id: this.upid},
        // If the column is guaranteed not to have null values, we can use an INNER JOIN.
        innerJoin: this.params?.notNull === true,
      },
    };
  }
}

class ArgColumn extends LegacyTableColumn {
  private displayValue: SqlColumn;
  private stringValue: SqlColumn;
  private intValue: SqlColumn;
  private realValue: SqlColumn;

  constructor(
    private argSetId: SqlColumn,
    private key: string,
  ) {
    super();

    const argTable: SourceTable = {
      table: 'args',
      joinOn: {
        arg_set_id: argSetId,
        key: sqliteString(key),
      },
    };

    this.displayValue = {
      column: 'display_value',
      source: argTable,
    };
    this.stringValue = {
      column: 'string_value',
      source: argTable,
    };
    this.intValue = {
      column: 'int_value',
      source: argTable,
    };
    this.realValue = {
      column: 'real_value',
      source: argTable,
    };
  }

  override primaryColumn(): SqlColumn {
    return this.displayValue;
  }

  override sortColumns(): SqlColumn[] {
    return [this.stringValue, this.intValue, this.realValue];
  }

  override dependentColumns() {
    return {
      stringValue: this.stringValue,
      intValue: this.intValue,
      realValue: this.realValue,
    };
  }

  getTitle() {
    return `${sqlColumnId(this.argSetId)}[${this.key}]`;
  }

  renderCell(
    value: SqlValue,
    tableManager: LegacyTableManager,
    dependentColumns: {[key: string]: SqlValue},
  ): m.Children {
    const strValue = dependentColumns['stringValue'];
    const intValue = dependentColumns['intValue'];
    const realValue = dependentColumns['realValue'];

    let contextMenuItems: m.Child[] = [];
    if (strValue !== null) {
      contextMenuItems = getStandardContextMenuItems(
        strValue,
        this.stringValue,
        tableManager,
      );
    } else if (intValue !== null) {
      contextMenuItems = getStandardContextMenuItems(
        intValue,
        this.intValue,
        tableManager,
      );
    } else if (realValue !== null) {
      contextMenuItems = getStandardContextMenuItems(
        realValue,
        this.realValue,
        tableManager,
      );
    } else {
      contextMenuItems = getStandardContextMenuItems(
        value,
        this.displayValue,
        tableManager,
      );
    }
    return m(
      PopupMenu,
      {
        trigger: m(Anchor, displayValue(value)),
      },
      ...contextMenuItems,
    );
  }
}

export class ArgSetIdColumn extends LegacyTableColumn {
  constructor(
    private column: SqlColumn,
    private title?: string,
  ) {
    super();
  }

  getTitle(): string {
    return this.title ?? sqlColumnId(this.column);
  }

  primaryColumn(): SqlColumn {
    return this.column;
  }

  override renderCell(
    value: SqlValue,
    tableManager: LegacyTableManager,
  ): m.Children {
    return renderStandardCell(value, this.column, tableManager);
  }

  override listDerivedColumns(manager: LegacyTableManager) {
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

  override initialColumns() {
    return [];
  }
}

export function argSqlColumn(argSetId: SqlColumn, key: string): SqlColumn {
  return {
    column: 'display_value',
    source: {
      table: 'args',
      joinOn: {
        arg_set_id: argSetId,
        key: sqliteString(key),
      },
    },
  };
}

export function argTableColumn(
  argSetId: SqlColumn,
  key: string,
): LegacyTableColumn {
  return new ArgColumn(argSetId, key);
}
