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

import {SqlValue, STR} from '../../../../trace_processor/query_result';
import {
  TableColumn,
  TableColumnSet,
  TableManager,
  SqlColumn,
  sqlColumnId,
  TableColumnParams,
} from './column';
import {
  getStandardContextMenuItems,
  renderStandardCell,
} from './render_cell_utils';
import {Timestamp} from '../../timestamp';
import {Duration, Time} from '../../../../base/time';
import {DurationWidget} from '../../duration';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';
import {renderError} from '../../../../widgets/error';
import {SliceRef} from '../../slice';
import {asSliceSqlId} from '../../../../trace_processor/sql_utils/core_types';
import {sqliteString} from '../../../../base/string_utils';

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

export class SliceIdColumn extends TableColumn {
  constructor(
    private columns: {
      sliceId: SqlColumn;
      trackId: SqlColumn;
      ts: SqlColumn;
      dur: SqlColumn;
    },
    private params?: ColumnParams,
  ) {
    super(params);
  }

  primaryColumn(): SqlColumn {
    return this.columns.sliceId;
  }

  getTitle() {
    return this.params?.title;
  }

  dependentColumns() {
    return {
      trackId: this.columns.trackId,
      ts: this.columns.ts,
      dur: this.columns.dur,
    };
  }

  renderCell(
    value: SqlValue,
    _: TableManager,
    data: {[key: string]: SqlValue},
  ): m.Children {
    const id = value;
    const ts = data['ts'];
    const dur = data['dur'] === null ? -1n : data['dur'];
    const trackId = data['trackId'];

    const columnNotFoundError = (type: string, name: string) =>
      renderError(`${type} column ${name} not found`);
    const wrongTypeError = (type: string, name: SqlColumn, value: SqlValue) =>
      renderError(
        `Wrong type for ${type} column ${sqlColumnId(name)}: bigint expected, ${typeof value} found`,
      );

    if (typeof id !== 'bigint') {
      return sqlValueToReadableString(id);
    }
    if (ts === undefined) {
      return columnNotFoundError('Timestamp', sqlColumnId(this.columns.ts));
    }
    if (typeof ts !== 'bigint') {
      return wrongTypeError('timestamp', this.columns.ts, ts);
    }
    if (dur === undefined) {
      return columnNotFoundError('Duration', sqlColumnId(this.columns.dur));
    }
    if (typeof dur !== 'bigint') {
      return wrongTypeError('duration', this.columns.dur, dur);
    }
    if (trackId === undefined) {
      return columnNotFoundError('Track id', sqlColumnId(this.columns.trackId));
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
        column: new StandardColumn(
          {
            column: 'display_value',
            source: {
              table: 'args',
              joinOn: {
                arg_set_id: this.column,
                key: sqliteString(it.key),
              },
            },
          },
          {
            title: `${sqlColumnId(this.column)}[${it.key}]`,
            alias: it.key.replace(/[^a-zA-Z0-9_]/g, '__'),
          },
        ),
      });
    }
    return result;
  }
}
