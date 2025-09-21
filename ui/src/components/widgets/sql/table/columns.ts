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
import {Trace} from '../../../../public/trace';
import {SqlValue, STR} from '../../../../trace_processor/query_result';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../../sql_utils/core_types';
import {renderError} from '../../../../widgets/error';
import {DurationWidget} from '../../duration';
import {showProcessDetailsMenuItem} from '../../process';
import {SchedRef} from '../../sched';
import {SliceRef} from '../../slice';
import {showThreadDetailsMenuItem} from '../../thread';
import {ThreadStateRef} from '../../thread_state';
import {Timestamp} from '../../timestamp';
import {
  RenderedCell,
  TableColumn,
  RenderCellContext,
  ListColumnsContext,
} from './table_column';
import {
  getStandardContextMenuItems,
  renderStandardCell,
} from './render_cell_utils';
import {SqlColumn, sqlColumnId, SqlExpression} from './sql_column';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../../trace_processor/perfetto_sql_type';
import {parseJsonWithBigints} from '../../../../base/json_utils';
import {Anchor} from '../../../../widgets/anchor';
import {MenuItem, PopupMenu} from '../../../../widgets/menu';
import {Icons} from '../../../../base/semantic_icons';
import {copyToClipboard} from '../../../../base/clipboard';
import {Args} from '../../../sql_utils/args';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';

function wrongTypeError(type: string, name: SqlColumn, value: SqlValue) {
  return renderError(
    `Wrong type for ${type} column ${sqlColumnId(
      name,
    )}: bigint expected, ${typeof value} found`,
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
    public readonly type: PerfettoSqlType | undefined,
    private params?: StandardColumnParams,
  ) {}

  renderCell(value: SqlValue, context?: RenderCellContext) {
    return renderStandardCell(value, this.column, context);
  }

  initialColumns(): TableColumn[] {
    return this.params?.startsHidden ? [] : [this];
  }
}

export class TimestampColumn implements TableColumn {
  public readonly type = PerfettoSqlTypes.TIMESTAMP;

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
  ) {}

  renderCell(value: SqlValue, context?: RenderCellContext) {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, context);
    }
    return {
      content: m(Timestamp, {
        trace: this.trace,
        ts: Time.fromRaw(value),
      }),
      menu: [
        context && getStandardContextMenuItems(value, this.column, context),
      ],
      isNumerical: true,
    };
  }
}

export class DurationColumn implements TableColumn {
  public readonly type = PerfettoSqlTypes.DURATION;

  constructor(
    public readonly trace: Trace,
    public column: SqlColumn,
  ) {}

  renderCell(value: SqlValue, context?: RenderCellContext) {
    if (typeof value === 'number') {
      value = BigInt(Math.round(value));
    }
    if (typeof value !== 'bigint') {
      return renderStandardCell(value, this.column, context);
    }

    return {
      content: m(DurationWidget, {
        trace: this.trace,
        dur: Duration.fromRaw(value),
      }),
      menu: [
        context && getStandardContextMenuItems(value, this.column, context),
      ],
      isNumerical: true,
    };
  }
}

export class SliceIdColumn implements TableColumn {
  public readonly type: PerfettoSqlType;

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {
    this.type = {
      kind: params?.type === 'id' ? 'id' : 'joinid',
      source: {table: 'slice', column: 'id'},
    };
  }

  renderCell(value: SqlValue, context?: RenderCellContext): RenderedCell {
    const id = value;

    if (!context || id === null) {
      return renderStandardCell(id, this.column, context);
    }

    return {
      content: m(SliceRef, {
        trace: this.trace,
        id: asSliceSqlId(Number(id)),
        name: `${id}`,
        switchToCurrentSelectionTab: false,
      }),
      menu: getStandardContextMenuItems(id, this.column, context),
      isNumerical: true,
    };
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        ['ts', new TimestampColumn(this.trace, this.getChildColumn('ts'))],
        ['dur', new DurationColumn(this.trace, this.getChildColumn('dur'))],
        [
          'name',
          new StandardColumn(
            this.getChildColumn('name'),
            PerfettoSqlTypes.STRING,
          ),
        ],
        [
          'parent_id',
          new SliceIdColumn(this.trace, this.getChildColumn('parent_id')),
        ],
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
  public readonly type: PerfettoSqlType = {
    kind: 'joinid',
    source: {table: 'sched', column: 'id'},
  };

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
  ) {}

  renderCell(value: SqlValue, context?: RenderCellContext) {
    const id = value;

    if (!context || id === null) {
      return renderStandardCell(id, this.column, context);
    }
    if (typeof id !== 'bigint') {
      return {content: wrongTypeError('id', this.column, id)};
    }

    return {
      content: m(SchedRef, {
        trace: this.trace,
        id: asSchedSqlId(Number(id)),
        name: `${id}`,
        switchToCurrentSelectionTab: false,
      }),
      menu: getStandardContextMenuItems(id, this.column, context),
      isNumerical: true,
    };
  }
}

export class ThreadStateIdColumn implements TableColumn {
  public readonly type: PerfettoSqlType = {
    kind: 'joinid',
    source: {table: 'thread_state', column: 'id'},
  };

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
  ) {}

  renderCell(value: SqlValue, context?: RenderCellContext) {
    const id = value;

    if (!context || id === null) {
      return renderStandardCell(id, this.column, context);
    }
    if (typeof id !== 'bigint') {
      return {content: wrongTypeError('id', this.column, id)};
    }

    return {
      content: m(ThreadStateRef, {
        trace: this.trace,
        id: asThreadStateSqlId(Number(id)),
        name: `${id}`,
        switchToCurrentSelectionTab: false,
      }),
      menu: getStandardContextMenuItems(id, this.column, context),
      isNumerical: true,
    };
  }
}

export class ThreadIdColumn implements TableColumn {
  public readonly type: PerfettoSqlType;

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {
    this.type = {
      kind: params?.type === 'id' ? 'id' : 'joinid',
      source: {table: 'thread', column: 'id'},
    };
  }

  renderCell(value: SqlValue, context?: RenderCellContext) {
    const utid = value;

    if (context === undefined || utid === null) {
      return renderStandardCell(utid, this.column, context);
    }

    if (typeof utid !== 'bigint') {
      throw new Error(
        `thread.utid is expected to be bigint, got ${typeof utid}`,
      );
    }

    return {
      content: `${utid}`,
      menu: [
        showThreadDetailsMenuItem(this.trace, asUtid(Number(utid))),
        getStandardContextMenuItems(utid, this.column, context),
      ],
      isNumerical: true,
    };
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        [
          'tid',
          new StandardColumn(this.getChildColumn('tid'), PerfettoSqlTypes.INT),
        ],
        [
          'name',
          new StandardColumn(
            this.getChildColumn('name'),
            PerfettoSqlTypes.STRING,
          ),
        ],
        [
          'start_ts',
          new TimestampColumn(this.trace, this.getChildColumn('start_ts')),
        ],
        [
          'end_ts',
          new TimestampColumn(this.trace, this.getChildColumn('end_ts')),
        ],
        ['upid', new ProcessIdColumn(this.trace, this.getChildColumn('upid'))],
        [
          'is_main_thread',
          new StandardColumn(
            this.getChildColumn('is_main_thread'),
            PerfettoSqlTypes.BOOLEAN,
          ),
        ],
      ]);
  }

  initialColumns(): TableColumn[] {
    return [
      this,
      new StandardColumn(this.getChildColumn('tid'), PerfettoSqlTypes.INT),
      new StandardColumn(this.getChildColumn('name'), PerfettoSqlTypes.STRING),
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
  public readonly type: PerfettoSqlType;

  constructor(
    public readonly trace: Trace,
    public readonly column: SqlColumn,
    private params?: IdColumnParams,
  ) {
    this.type = {
      kind: params?.type === 'id' ? 'id' : 'joinid',
      source: {table: 'process', column: 'id'},
    };
  }

  renderCell(value: SqlValue, context?: RenderCellContext) {
    const upid = value;

    if (context === undefined || upid === null) {
      return renderStandardCell(upid, this.column, context);
    }

    if (typeof upid !== 'bigint') {
      throw new Error(
        `thread.upid is expected to be bigint, got ${typeof upid}`,
      );
    }

    return {
      content: `${upid}`,
      menu: [
        showProcessDetailsMenuItem(this.trace, asUpid(Number(upid))),
        getStandardContextMenuItems(upid, this.column, context),
      ],
      isNumerical: true,
    };
  }

  listDerivedColumns() {
    if (this.params?.type === 'id') return undefined;
    return async () =>
      new Map<string, TableColumn>([
        [
          'pid',
          new StandardColumn(this.getChildColumn('pid'), PerfettoSqlTypes.INT),
        ],
        [
          'name',
          new StandardColumn(
            this.getChildColumn('name'),
            PerfettoSqlTypes.STRING,
          ),
        ],
        [
          'start_ts',
          new TimestampColumn(this.trace, this.getChildColumn('start_ts')),
        ],
        [
          'end_ts',
          new TimestampColumn(this.trace, this.getChildColumn('end_ts')),
        ],
        [
          'parent_upid',
          new ProcessIdColumn(this.trace, this.getChildColumn('parent_upid')),
        ],
        [
          'is_main_thread',
          new StandardColumn(
            this.getChildColumn('is_main_thread'),
            PerfettoSqlTypes.BOOLEAN,
          ),
        ],
      ]);
  }

  initialColumns(): TableColumn[] {
    return [
      this,
      new StandardColumn(this.getChildColumn('pid'), PerfettoSqlTypes.INT),
      new StandardColumn(this.getChildColumn('name'), PerfettoSqlTypes.STRING),
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

class ArgColumn implements TableColumn {
  public readonly column: SqlColumn;
  public readonly display: SqlColumn;
  public readonly type: PerfettoSqlType | undefined = undefined;
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
    this.display = new SqlExpression(
      (cols: string[]) => `json_object(
          'id', ${cols[0]},
          'int_value', ${cols[1]},
          'real_value', ${cols[2]},
          'string_value', ${cols[3]},
          'display_value', ${cols[4]}
      )`,
      (
        [
          'id',
          'int_value',
          'real_value',
          'string_value',
          'display_value',
        ] as const
      ).map((c) => this.getRawColumn(c)),
    );
  }

  private getRawColumn(
    type:
      | 'string_value'
      | 'int_value'
      | 'real_value'
      | 'id'
      | 'type'
      | 'display_value',
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

  renderCell(value: SqlValue, context?: RenderCellContext): RenderedCell {
    if (context === undefined) {
      return renderStandardCell(value, this.column, context);
    }
    if (typeof value !== 'string') {
      return {
        content: renderError(
          `Wrong type: expected string, ${typeof value} found`,
        ),
      };
    }
    const argValue = parseJsonWithBigints(value);
    if (argValue['id'] === null) {
      return renderStandardCell(null, this.getRawColumn('id'), context);
    }
    if (argValue['int_value'] !== null) {
      return renderStandardCell(
        argValue['int_value'],
        this.getRawColumn('int_value'),
        context,
      );
    } else if (argValue['real_value'] !== null) {
      return renderStandardCell(
        argValue['real_value'],
        this.getRawColumn('real_value'),
        context,
      );
    } else {
      return renderStandardCell(
        argValue['string_value'],
        this.getRawColumn('string_value'),
        context,
      );
    }
  }
}

export class ArgSetIdColumn implements TableColumn {
  public readonly type = PerfettoSqlTypes.ARG_SET_ID;

  constructor(public readonly column: SqlColumn) {}

  renderCell(value: SqlValue, context: RenderCellContext) {
    return renderStandardCell(value, this.column, context);
  }

  listDerivedColumns(context: ListColumnsContext) {
    return async () => {
      const queryResult = await context.trace.engine.query(`
        SELECT
          DISTINCT args.key
        FROM (${context.getSqlQuery({arg_set_id: this.column})}) data
        JOIN args USING (arg_set_id)
      `);
      const it = queryResult.iter({key: STR});
      const result = new Map();
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

export class PrintArgsColumn implements TableColumn {
  public readonly column: SqlColumn;
  public readonly type = undefined;

  constructor(public readonly argSetIdColumn: SqlColumn) {
    this.column = new SqlExpression(
      (cols: string[]) => `__intrinsic_arg_set_to_json(${cols[0]})`,
      [argSetIdColumn],
      `print_args(${sqlColumnId(argSetIdColumn)})`,
    );
  }

  renderCell(value: SqlValue, context?: RenderCellContext): RenderedCell {
    if (value === null) {
      return {
        content: '{}',
      };
    }
    if (typeof value !== 'string') {
      return {
        content: renderError(
          `Unexpected type: expected string, got ${typeof value}`,
        ),
      };
    }

    let data: Args;
    try {
      data = parseJsonWithBigints(value) as Args;
    } catch (e) {
      return {
        content: renderError(`Failed to parse JSON: ${e}`),
      };
    }
    const content: m.Children[] = [];

    // Condense single-key nested objects into a flattened key: {a: {b: 1}} becomes {a.b: 1}.
    const condense = (key: string, value: Args): {key: string; value: Args} => {
      if (
        value !== null &&
        typeof value === 'object' &&
        !Array.isArray(value)
      ) {
        const entries = Object.entries(value);
        if (entries.length === 1) {
          const [nestedKey, nestedValue] = entries[0];
          return condense(`${key}.${nestedKey}`, nestedValue);
        }
      }
      return {key, value};
    };

    const renderJsonValue = (value: Args, prefix?: string) => {
      if (value === null) {
        content.push(m('i', 'null'));
      } else if (typeof value === 'object' && !Array.isArray(value)) {
        content.push('{');
        Object.entries(value).forEach(([rawKey, rawVal], idx) => {
          if (idx > 0) {
            content.push(', ');
          }
          const {key, value: val} = condense(rawKey, rawVal);

          const isLeaf = typeof val !== 'object' || val === null;
          const fullKey = prefix === undefined ? key : `${prefix}.${key}`;
          const argColumn = argTableColumn(this.argSetIdColumn, fullKey);
          const keyElement =
            isLeaf && context
              ? m(
                  PopupMenu,
                  {
                    trigger: m(Anchor, key),
                  },
                  m(MenuItem, {
                    icon: Icons.Add,
                    label: 'Add column',
                    disabled: !context.hasColumn(argColumn),
                    onclick: () => {
                      context.addColumn(argColumn);
                    },
                  }),
                  m(MenuItem, {
                    icon: Icons.Copy,
                    label: 'Copy full key',
                    onclick: () => {
                      copyToClipboard(fullKey);
                    },
                  }),
                )
              : key;
          content.push(keyElement, ': ');
          renderJsonValue(val, fullKey);
        });
        content.push('}');
      } else if (Array.isArray(value)) {
        content.push('[');
        value.forEach((item, idx) => {
          if (idx > 0) {
            content.push(', ');
          }
          renderJsonValue(item, `${prefix}[${idx}]`);
        });
        content.push(']');
      } else if (typeof value === 'boolean') {
        content.push(value ? 'true' : 'false');
      } else if (typeof value === 'string') {
        content.push(`"${value.replace(/"/g, '\\"')}"`);
      } else {
        content.push(sqlValueToReadableString(value));
      }
    };

    renderJsonValue(data);
    return {
      content,
    };
  }

  initialColumns() {
    return [];
  }
}
