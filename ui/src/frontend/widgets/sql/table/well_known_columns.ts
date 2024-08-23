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
  TableColumn,
  TableColumnSet,
  TableManager,
  SqlColumn,
  sqlColumnId,
  TableColumnParams,
  SourceTable,
} from './column';
import {
  getStandardContextMenuItems,
  getStandardFilters,
  renderStandardCell,
} from './render_cell_utils';
import {Timestamp} from '../../timestamp';
import {Duration, Time} from '../../../../base/time';
import {DurationWidget} from '../../duration';
import {renderError} from '../../../../widgets/error';
import {SliceRef} from '../../slice';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../../../trace_processor/sql_utils/core_types';
import {sqliteString} from '../../../../base/string_utils';
import {ThreadStateRef} from '../../thread_state';
import {MenuDivider, MenuItem, PopupMenu2} from '../../../../widgets/menu';
import {getThreadName} from '../../../../trace_processor/sql_utils/thread';
import {Anchor} from '../../../../widgets/anchor';
import {threadRefMenuItems} from '../../thread';
import {Icons} from '../../../../base/semantic_icons';
import {SqlValue, STR} from '../../../../trace_processor/query_result';
import {getProcessName} from '../../../../trace_processor/sql_utils/process';
import {processRefMenuItems} from '../../process';
import {SchedRef} from '../../sched';

type ColumnParams = TableColumnParams & {
  title?: string;
};

export class StandardColumn extends TableColumn {
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

  renderCell(value: SqlValue, tableManager: TableManager): m.Children {
    return renderStandardCell(value, this.column, tableManager);
  }
}

export class TimestampColumn extends TableColumn {
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

  renderCell(value: SqlValue, tableManager: TableManager): m.Children {
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

export class DurationColumn extends TableColumn {
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

  renderCell(value: SqlValue, tableManager: TableManager): m.Children {
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

function wrongTypeError(type: string, name: SqlColumn, value: SqlValue) {
  return renderError(
    `Wrong type for ${type} column ${sqlColumnId(name)}: bigint expected, ${typeof value} found`,
  );
}

export class SliceIdColumn extends TableColumn {
  private columns: {ts: SqlColumn; dur: SqlColumn; trackId: SqlColumn};

  constructor(
    private id: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);

    const sliceTable: SourceTable = {
      table: 'slice',
      joinOn: {id: this.id},
    };

    this.columns = {
      ts: {
        column: 'ts',
        source: sliceTable,
      },
      dur: {
        column: 'dur',
        source: sliceTable,
      },
      trackId: {
        column: 'track_id',
        source: sliceTable,
      },
    };
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  dependentColumns() {
    return this.columns;
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const id = value;
    const ts = data['ts'];
    const dur = data['dur'] === null ? -1n : data['dur'];
    const trackId = data['trackId'];

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }
    if (ts === null || trackId === null) {
      return renderError(`Slice with id ${id} not found`);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.id, id);
    if (typeof ts !== 'bigint') {
      return wrongTypeError('timestamp', this.columns.ts, ts);
    }
    if (typeof dur !== 'bigint') {
      return wrongTypeError('duration', this.columns.dur, dur);
    }
    if (typeof trackId !== 'bigint') {
      return wrongTypeError('track id', this.columns.trackId, trackId);
    }

    return m(SliceRef, {
      id: asSliceSqlId(Number(id)),
      name: `${id}`,
      ts: Time.fromRaw(ts),
      dur: dur,
      sqlTrackId: Number(trackId),
      switchToCurrentSelectionTab: false,
    });
  }
}

export class SchedIdColumn extends TableColumn {
  private columns: {ts: SqlColumn; dur: SqlColumn; cpu: SqlColumn};

  constructor(
    private id: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);

    const schedTable: SourceTable = {
      table: 'sched',
      joinOn: {id: this.id},
    };

    this.columns = {
      ts: {
        column: 'ts',
        source: schedTable,
      },
      dur: {
        column: 'dur',
        source: schedTable,
      },
      cpu: {
        column: 'cpu',
        source: schedTable,
      },
    };
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  dependentColumns() {
    return {
      ts: this.columns.ts,
      dur: this.columns.dur,
      cpu: this.columns.cpu,
    };
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const id = value;
    const ts = data['ts'];
    const dur = data['dur'] === null ? -1n : data['dur'];
    const cpu = data['cpu'];

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }
    if (ts === null || cpu === null) {
      return renderError(`Sched with id ${id} not found`);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.id, id);
    if (typeof ts !== 'bigint') {
      return wrongTypeError('timestamp', this.columns.ts, ts);
    }
    if (typeof dur !== 'bigint') {
      return wrongTypeError('duration', this.columns.dur, dur);
    }
    if (typeof cpu !== 'bigint') {
      return wrongTypeError('track id', this.columns.cpu, cpu);
    }

    return m(SchedRef, {
      id: asSchedSqlId(Number(id)),
      ts: Time.fromRaw(ts),
      dur: dur,
      cpu: Number(cpu),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadStateIdColumn extends TableColumn {
  private columns: {ts: SqlColumn; dur: SqlColumn; utid: SqlColumn};

  constructor(
    private id: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);

    const threadStateTable: SourceTable = {
      table: 'thread_state',
      joinOn: {id: this.id},
    };

    this.columns = {
      ts: {
        column: 'ts',
        source: threadStateTable,
      },
      dur: {
        column: 'dur',
        source: threadStateTable,
      },
      utid: {
        column: 'utid',
        source: threadStateTable,
      },
    };
  }

  primaryColumn(): SqlColumn {
    return this.id;
  }

  getTitle() {
    return this.params?.title;
  }

  dependentColumns() {
    return {
      ts: this.columns.ts,
      dur: this.columns.dur,
      utid: this.columns.utid,
    };
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const id = value;
    const ts = data['ts'];
    const dur = data['dur'] === null ? -1n : data['dur'];
    const utid = data['utid'];

    if (id === null) {
      return renderStandardCell(id, this.id, manager);
    }
    if (ts === null || utid === null) {
      return renderError(`Thread state with id ${id} not found`);
    }
    if (typeof id !== 'bigint') return wrongTypeError('id', this.id, id);
    if (typeof ts !== 'bigint') {
      return wrongTypeError('timestamp', this.columns.ts, ts);
    }
    if (typeof dur !== 'bigint') {
      return wrongTypeError('duration', this.columns.dur, dur);
    }
    if (typeof utid !== 'bigint') {
      return wrongTypeError('track id', this.columns.utid, utid);
    }

    return m(ThreadStateRef, {
      id: asThreadStateSqlId(Number(id)),
      ts: Time.fromRaw(ts),
      dur: dur,
      utid: asUtid(Number(utid)),
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadColumn extends TableColumn {
  private columns: {name: SqlColumn; tid: SqlColumn};

  constructor(
    private utid: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);

    const threadTable: SourceTable = {
      table: 'thread',
      joinOn: {id: this.utid},
    };

    this.columns = {
      name: {
        column: 'name',
        source: threadTable,
      },
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
    return this.params?.title;
  }

  dependentColumns() {
    return {
      tid: this.columns.tid,
      name: this.columns.name,
    };
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const utid = value;
    const rawTid = data['tid'];
    const rawName = data['name'];

    if (utid === null) {
      return renderStandardCell(utid, this.utid, manager);
    }
    if (typeof utid !== 'bigint') {
      return wrongTypeError('utid', this.utid, utid);
    }
    if (rawTid !== null && typeof rawTid !== 'bigint') {
      return wrongTypeError('tid', this.columns.tid, rawTid);
    }
    if (rawName !== null && typeof rawName !== 'string') {
      return wrongTypeError('name', this.columns.name, rawName);
    }

    const name: string | undefined = rawName ?? undefined;
    const tid: number | undefined =
      rawTid !== null ? Number(rawTid) : undefined;

    return m(
      PopupMenu2,
      {
        trigger: m(
          Anchor,
          getThreadName({
            name: name ?? undefined,
            tid: tid !== null ? Number(tid) : undefined,
          }),
        ),
      },
      threadRefMenuItems({utid: asUtid(Number(utid)), name, tid}),
      m(MenuDivider),
      m(
        MenuItem,
        {
          label: 'Add filter',
          icon: Icons.Filter,
        },
        m(
          MenuItem,
          {
            label: 'utid',
          },
          getStandardFilters(utid, this.utid, manager),
        ),
        m(
          MenuItem,
          {
            label: 'thread name',
          },
          getStandardFilters(rawName, this.columns.name, manager),
        ),
        m(
          MenuItem,
          {
            label: 'tid',
          },
          getStandardFilters(rawTid, this.columns.tid, manager),
        ),
      ),
    );
  }
}

export class ProcessColumn extends TableColumn {
  private columns: {name: SqlColumn; pid: SqlColumn};

  constructor(
    private upid: SqlColumn,
    private params?: ColumnParams,
  ) {
    super(params);

    const processTable: SourceTable = {
      table: 'process',
      joinOn: {id: this.upid},
    };

    this.columns = {
      name: {
        column: 'name',
        source: processTable,
      },
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
    return this.params?.title;
  }

  dependentColumns() {
    return this.columns;
  }

  renderCell(
    value: SqlValue,
    manager: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const upid = value;
    const rawPid = data['pid'];
    const rawName = data['name'];

    if (upid === null) {
      return renderStandardCell(upid, this.upid, manager);
    }
    if (typeof upid !== 'bigint') {
      return wrongTypeError('upid', this.upid, upid);
    }
    if (rawPid !== null && typeof rawPid !== 'bigint') {
      return wrongTypeError('pid', this.columns.pid, rawPid);
    }
    if (rawName !== null && typeof rawName !== 'string') {
      return wrongTypeError('name', this.columns.name, rawName);
    }

    const name: string | undefined = rawName ?? undefined;
    const pid: number | undefined =
      rawPid !== null ? Number(rawPid) : undefined;

    return m(
      PopupMenu2,
      {
        trigger: m(
          Anchor,
          getProcessName({
            name: name ?? undefined,
            pid: pid !== null ? Number(pid) : undefined,
          }),
        ),
      },
      processRefMenuItems({upid: asUpid(Number(upid)), name, pid}),
      m(MenuDivider),
      m(
        MenuItem,
        {
          label: 'Add filter',
          icon: Icons.Filter,
        },
        m(
          MenuItem,
          {
            label: 'upid',
          },
          getStandardFilters(upid, this.upid, manager),
        ),
        m(
          MenuItem,
          {
            label: 'process name',
          },
          getStandardFilters(rawName, this.columns.name, manager),
        ),
        m(
          MenuItem,
          {
            label: 'tid',
          },
          getStandardFilters(rawPid, this.columns.pid, manager),
        ),
      ),
    );
  }
}

export class ArgSetColumnSet extends TableColumnSet {
  constructor(
    private column: SqlColumn,
    private title?: string,
  ) {
    super();
  }

  getTitle(): string {
    return this.title ?? sqlColumnId(this.column);
  }

  async discover(
    manager: TableManager,
  ): Promise<{key: string; column: TableColumn}[]> {
    const queryResult = await manager.engine.query(`
      -- Encapsulate the query in a CTE to avoid clashes between filters
      -- and columns of the 'args' table.
      SELECT
        DISTINCT args.key
      FROM (${manager.getSqlQuery({arg_set_id: this.column})}) data
      JOIN args USING (arg_set_id)
    `);
    const result = [];
    const it = queryResult.iter({key: STR});
    for (; it.valid(); it.next()) {
      result.push({
        key: it.key,
        column: argTableColumn(this.column, it.key),
      });
    }
    return result;
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

export function argTableColumn(argSetId: SqlColumn, key: string) {
  return new StandardColumn(argSqlColumn(argSetId, key), {
    title: `${sqlColumnId(argSetId)}[${key}]`,
    alias: `arg_${key.replace(/[^a-zA-Z0-9_]/g, '__')}`,
  });
}
