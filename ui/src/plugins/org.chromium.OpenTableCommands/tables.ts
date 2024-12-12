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

import {SqlTableDescription} from '../../components/widgets/sql/legacy_table/table_description';
import {
  ArgSetColumnSet,
  DurationColumn,
  ProcessColumn,
  ProcessColumnSet,
  ProcessIdColumn,
  SchedIdColumn,
  SliceIdColumn,
  StandardColumn,
  ThreadColumn,
  ThreadColumnSet,
  ThreadIdColumn,
  ThreadStateIdColumn,
  TimestampColumn,
} from './well_known_columns';

export function getThreadTable(): SqlTableDescription {
  return {
    name: 'thread',
    columns: [
      new ThreadIdColumn('utid'),
      new StandardColumn('tid', {aggregationType: 'nominal'}),
      new StandardColumn('name'),
      new TimestampColumn('start_ts'),
      new TimestampColumn('end_ts'),
      new ProcessColumnSet('upid', {title: 'upid', notNull: true}),
      new StandardColumn('is_main_thread', {
        aggregationType: 'nominal',
      }),
    ],
  };
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

export function getSliceTable(): SqlTableDescription {
  return {
    imports: ['slices.slices'],
    name: '_slice_with_thread_and_process_info',
    displayName: 'slice',
    columns: [
      new SliceIdColumn('id', {notNull: true}),
      new TimestampColumn('ts', {title: 'Timestamp'}),
      new DurationColumn('dur', {title: 'Duration'}),
      new DurationColumn('thread_dur', {title: 'Thread duration'}),
      new StandardColumn('category', {title: 'Category'}),
      new StandardColumn('name', {title: 'Name'}),
      new StandardColumn('track_id', {
        title: 'Track ID',
        aggregationType: 'nominal',
        startsHidden: true,
      }),
      new ThreadColumnSet('utid', {title: 'utid'}),
      new ProcessColumnSet('upid', {title: 'upid'}),
      new StandardColumn('depth', {title: 'Depth', startsHidden: true}),
      new SliceIdColumn('parent_id', {
        startsHidden: true,
      }),
      new ArgSetColumnSet('arg_set_id'),
    ],
  };
}

export function getAndroidLogsTable(): SqlTableDescription {
  return {
    name: 'android_logs',
    columns: [
      new StandardColumn('id', {aggregationType: 'nominal'}),
      new TimestampColumn('ts'),
      new StandardColumn('tag'),
      new StandardColumn('prio', {aggregationType: 'nominal'}),
      new ThreadColumnSet('utid', {title: 'utid', notNull: true}),
      new ProcessColumnSet(
        {
          column: 'upid',
          source: {
            table: 'thread',
            joinOn: {utid: 'utid'},
          },
        },
        {title: 'upid', notNull: true},
      ),
      new StandardColumn('msg'),
    ],
  };
}

export function getSchedTable(): SqlTableDescription {
  return {
    name: 'sched',
    columns: [
      new SchedIdColumn('id'),
      new TimestampColumn('ts'),
      new DurationColumn('dur'),
      new StandardColumn('cpu', {aggregationType: 'nominal'}),
      new StandardColumn('priority', {aggregationType: 'nominal'}),
      new ThreadColumnSet('utid', {title: 'utid', notNull: true}),
      new ProcessColumnSet(
        {
          column: 'upid',
          source: {
            table: 'thread',
            joinOn: {
              utid: 'utid',
            },
            innerJoin: true,
          },
        },
        {title: 'upid', notNull: true},
      ),
      new StandardColumn('end_state'),
      new StandardColumn('ucpu', {
        aggregationType: 'nominal',
        startsHidden: true,
      }),
    ],
  };
}

export function getThreadStateTable(): SqlTableDescription {
  return {
    name: 'thread_state',
    columns: [
      new ThreadStateIdColumn('id', {notNull: true}),
      new TimestampColumn('ts'),
      new DurationColumn('dur'),
      new StandardColumn('state'),
      new StandardColumn('cpu', {aggregationType: 'nominal'}),
      new ThreadColumnSet('utid', {title: 'utid', notNull: true}),
      new ProcessColumnSet(
        {
          column: 'upid',
          source: {
            table: 'thread',
            joinOn: {
              utid: 'utid',
            },
            innerJoin: true,
          },
        },
        {title: 'upid (process)', notNull: true},
      ),
      new StandardColumn('io_wait', {aggregationType: 'nominal'}),
      new StandardColumn('blocked_function'),
      new ThreadColumn('waker_utid', {title: 'Waker thread'}),
      new ThreadStateIdColumn('waker_id'),
      new StandardColumn('irq_context', {aggregationType: 'nominal'}),
      new StandardColumn('ucpu', {
        aggregationType: 'nominal',
        startsHidden: true,
      }),
    ],
  };
}
