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
import {Icons} from '../../../../base/semantic_icons';
import {sqliteString} from '../../../../base/string_utils';
import {Duration, Time} from '../../../../base/time';
import {SqlValue, STR} from '../../../../trace_processor/query_result';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../../../trace_processor/sql_utils/core_types';
import {getProcessName} from '../../../../trace_processor/sql_utils/process';
import {getThreadName} from '../../../../trace_processor/sql_utils/thread';
import {Anchor} from '../../../../widgets/anchor';
import {renderError} from '../../../../widgets/error';
import {MenuDivider, MenuItem, PopupMenu2} from '../../../../widgets/menu';
import {DurationWidget} from '../../duration';
import {processRefMenuItems, showProcessDetailsMenuItem} from '../../process';
import {SchedRef} from '../../sched';
import {SliceRef} from '../../slice';
import {showThreadDetailsMenuItem, threadRefMenuItems} from '../../thread';
import {ThreadStateRef} from '../../thread_state';
import {Timestamp} from '../../timestamp';
import {
  AggregationConfig,
  SourceTable,
  SqlColumn,
  sqlColumnId,
  TableColumn,
  TableColumnSet,
  TableManager,
} from './column';
import {
  displayValue,
  getStandardContextMenuItems,
  getStandardFilters,
  renderStandardCell,
} from './render_cell_utils';

export type ColumnParams = {
  alias?: string;
  startsHidden?: boolean;
  title?: string;
};

export type StandardColumnParams = ColumnParams & {
  aggregationType?: 'nominal' | 'quantitative';
};

export interface IdColumnParams {
  // Whether the column is guaranteed not to have null values.
  // (this will allow us to upgrage the joins on this column to more performant INNER JOINs).
  notNull?: boolean;
}

type ColumnSetParams = {
  title: string;
  startsHidden?: boolean;
};

export class StandardColumn extends TableColumn {
  constructor(
    private column: SqlColumn,
    private params?: StandardColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.column;
  }

  aggregation(): AggregationConfig {
    return {dataType: this.params?.aggregationType};
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
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);

    const sliceTable: SourceTable = {
      table: 'slice',
      joinOn: {id: this.id},
      // If the column is guaranteed not to have null values, we can use an INNER JOIN.
      innerJoin: this.params?.notNull === true,
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
      switchToCurrentSelectionTab: false,
    });
  }
}

export class SliceColumnSet extends TableColumnSet {
  constructor(
    private id: SqlColumn,
    private params?: ColumnSetParams,
  ) {
    super();
  }

  getTitle(): string {
    return this.params?.title ?? `${sqlColumnId(this.id)} (slice)`;
  }

  async discover(): Promise<
    {key: string; column: TableColumn | TableColumnSet}[]
  > {
    const column: (name: string) => SqlColumn = (name) => {
      return {
        column: name,
        source: {
          table: 'slice',
          joinOn: {id: this.id},
        },
      };
    };

    return [
      {
        key: 'id',
        column: new SliceIdColumn(this.id),
      },
      {
        key: 'ts',
        column: new TimestampColumn(column('ts')),
      },
      {
        key: 'dur',
        column: new DurationColumn(column('dur')),
      },
      {
        key: 'name',
        column: new StandardColumn(column('name')),
      },
      {
        key: 'thread_dur',
        column: new StandardColumn(column('thread_dur')),
      },
      {
        key: 'parent_id',
        column: new SliceColumnSet(column('parent_id')),
      },
    ];
  }

  initialColumns(): TableColumn[] {
    if (this.params?.startsHidden) return [];
    return [new SliceIdColumn(this.id)];
  }
}

export class SchedIdColumn extends TableColumn {
  private columns: {ts: SqlColumn; dur: SqlColumn; cpu: SqlColumn};

  constructor(
    private id: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);

    const schedTable: SourceTable = {
      table: 'sched',
      joinOn: {id: this.id},
      // If the column is guaranteed not to have null values, we can use an INNER JOIN.
      innerJoin: this.params?.notNull === true,
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
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadStateIdColumn extends TableColumn {
  private columns: {ts: SqlColumn; dur: SqlColumn; utid: SqlColumn};

  constructor(
    private id: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    super(params);

    const threadStateTable: SourceTable = {
      table: 'thread_state',
      joinOn: {id: this.id},
      // If the column is guaranteed not to have null values, we can use an INNER JOIN.
      innerJoin: this.params?.notNull === true,
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
      name: `${id}`,
      switchToCurrentSelectionTab: false,
    });
  }
}

export class ThreadColumn extends TableColumn {
  private columns: {name: SqlColumn; tid: SqlColumn};

  constructor(
    private utid: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    // Both ThreadColumn and ThreadIdColumn are referencing the same underlying SQL column as primary,
    // so we have to use tag to distinguish them.
    super({tag: 'thread', ...params});

    const threadTable: SourceTable = {
      table: 'thread',
      joinOn: {id: this.utid},
      // If the column is guaranteed not to have null values, we can use an INNER JOIN.
      innerJoin: this.params?.notNull === true,
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
    if (this.params?.title !== undefined) return this.params.title;
    return `${sqlColumnId(this.utid)} (thread)`;
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

  aggregation(): AggregationConfig {
    return {
      dataType: 'nominal',
    };
  }
}

// ThreadIdColumn is a column type for displaying primary key of the `thread` table.
// All other references (foreign keys) should use `ThreadColumn` instead.
export class ThreadIdColumn extends TableColumn {
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

    if (utid === null) {
      return renderStandardCell(utid, this.utid, manager);
    }

    if (typeof utid !== 'bigint') {
      throw new Error(
        `thread.utid is expected to be bigint, got ${typeof utid}`,
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

export class ThreadColumnSet extends TableColumnSet {
  constructor(
    private id: SqlColumn,
    private params: ColumnSetParams & {
      notNull?: boolean;
    },
  ) {
    super();
  }

  getTitle(): string {
    return `${this.params.title} (thread)`;
  }

  initialColumns(): TableColumn[] {
    if (this.params.startsHidden === true) return [];
    return [new ThreadColumn(this.id)];
  }

  async discover() {
    const column: (name: string) => SqlColumn = (name) => ({
      column: name,
      source: {
        table: 'thread',
        joinOn: {id: this.id},
      },
      innerJoin: this.params.notNull === true,
    });

    return [
      {
        key: 'thread',
        column: new ThreadColumn(this.id),
      },
      {
        key: 'utid',
        column: new ThreadIdColumn(this.id),
      },
      {
        key: 'tid',
        column: new StandardColumn(column('tid'), {aggregationType: 'nominal'}),
      },
      {
        key: 'name',
        column: new StandardColumn(column('name')),
      },
      {
        key: 'start_ts',
        column: new TimestampColumn(column('start_ts')),
      },
      {
        key: 'end_ts',
        column: new TimestampColumn(column('end_ts')),
      },
      {
        key: 'upid',
        column: new ProcessColumnSet(column('upid'), {title: 'upid'}),
      },
      {
        key: 'is_main_thread',
        column: new StandardColumn(column('is_main_thread'), {
          aggregationType: 'nominal',
        }),
      },
    ];
  }
}

export class ProcessColumn extends TableColumn {
  private columns: {name: SqlColumn; pid: SqlColumn};

  constructor(
    private upid: SqlColumn,
    private params?: ColumnParams & IdColumnParams,
  ) {
    // Both ProcessColumn and ProcessIdColumn are referencing the same underlying SQL column as primary,
    // so we have to use tag to distinguish them.
    super({tag: 'process', ...params});

    const processTable: SourceTable = {
      table: 'process',
      joinOn: {id: this.upid},
      // If the column is guaranteed not to have null values, we can use an INNER JOIN.
      innerJoin: this.params?.notNull === true,
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
    if (this.params?.title !== undefined) return this.params.title;
    return `${sqlColumnId(this.upid)} (process)`;
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

  aggregation(): AggregationConfig {
    return {
      dataType: 'nominal',
    };
  }
}

// ProcessIdColumn is a column type for displaying primary key of the `process` table.
// All other references (foreign keys) should use `ProcessColumn` instead.
export class ProcessIdColumn extends TableColumn {
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

    if (upid === null) {
      return renderStandardCell(upid, this.upid, manager);
    }

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

export class ProcessColumnSet extends TableColumnSet {
  constructor(
    private id: SqlColumn,
    private params: ColumnSetParams & {
      notNull?: boolean;
    },
  ) {
    super();
  }

  getTitle(): string {
    return `${this.params.title} (process)`;
  }

  initialColumns(): TableColumn[] {
    if (this.params.startsHidden === true) return [];
    return [new ProcessColumn(this.id)];
  }

  async discover() {
    const column: (name: string) => SqlColumn = (name) => ({
      column: name,
      source: {
        table: 'process',
        joinOn: {id: this.id},
      },
      innerJoin: this.params.notNull === true,
    });

    return [
      {
        key: 'process',
        column: new ProcessColumn(this.id),
      },
      {
        key: 'upid',
        column: new ProcessIdColumn(this.id),
      },
      {
        key: 'pid',
        column: new StandardColumn(column('pid'), {aggregationType: 'nominal'}),
      },
      {
        key: 'name',
        column: new StandardColumn(column('name')),
      },
      {
        key: 'start_ts',
        column: new TimestampColumn(column('start_ts')),
      },
      {
        key: 'end_ts',
        column: new TimestampColumn(column('end_ts')),
      },
      {
        key: 'parent_upid',
        column: new ProcessColumnSet(column('parent_upid'), {
          title: 'parent_upid',
        }),
      },
      {
        key: 'uid',
        column: new StandardColumn(column('uid'), {aggregationType: 'nominal'}),
      },
      {
        key: 'android_appid',
        column: new StandardColumn(column('android_appid'), {
          aggregationType: 'nominal',
        }),
      },
      {
        key: 'cmdline',
        column: new StandardColumn(column('cmdline')),
      },
      {
        key: 'arg_set_id (args)',
        column: new ArgSetColumnSet(column('arg_set_id')),
      },
    ];
  }
}

class ArgColumn extends TableColumn {
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
    tableManager: TableManager,
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
      PopupMenu2,
      {
        trigger: m(Anchor, displayValue(value)),
      },
      ...contextMenuItems,
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
    const queryResult = await manager.trace.engine.query(`
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
  return new ArgColumn(argSetId, key);
}
