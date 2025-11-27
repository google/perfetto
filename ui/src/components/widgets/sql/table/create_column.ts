// Copyright (C) 2025 The Android Open Source Project
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

import {Trace} from '../../../../public/trace';
import {PerfettoSqlType} from '../../../../trace_processor/perfetto_sql_type';
import {
  ArgSetIdColumn,
  DurationColumn,
  ProcessIdColumn,
  SchedIdColumn,
  SliceIdColumn,
  StandardColumn,
  ThreadIdColumn,
  ThreadStateIdColumn,
  TimestampColumn,
} from './columns';
import {SqlColumn} from './sql_column';
import {TableColumn} from './table_column';

export function createTableColumn(args: {
  trace: Trace;
  column: SqlColumn;
  type?: PerfettoSqlType;
}): TableColumn {
  if (args.type?.kind === 'timestamp') {
    return new TimestampColumn(args.trace, args.column);
  }
  if (args.type?.kind === 'duration') {
    return new DurationColumn(args.trace, args.column);
  }
  if (args.type?.kind === 'arg_set_id') {
    return new ArgSetIdColumn(args.column);
  }
  if (args.type?.kind === 'id' || args.type?.kind === 'joinid') {
    if (args.type.source.column === 'id') {
      switch (args.type.source?.table.toLowerCase()) {
        case 'slice':
          return new SliceIdColumn(args.trace, args.column, {
            type: 'id',
          });
        case 'thread':
          return new ThreadIdColumn(args.trace, args.column, {
            type: 'id',
          });
        case 'process':
          return new ProcessIdColumn(args.trace, args.column, {
            type: 'id',
          });
        case 'thread_state':
          return new ThreadStateIdColumn(args.trace, args.column);
        case 'sched':
          return new SchedIdColumn(args.trace, args.column);
      }
    }
  }
  return new StandardColumn(args.column, args.type);
}
