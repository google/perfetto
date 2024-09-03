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

import {SqlTableDescription} from '../../frontend/widgets/sql/table/table_description';
import {
  DurationColumn,
  ProcessColumnSet,
  StandardColumn,
  ThreadColumn,
  ThreadColumnSet,
  ThreadStateIdColumn,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';

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
