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
  ProcessColumnSet,
  StandardColumn,
  ThreadColumnSet,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';

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
