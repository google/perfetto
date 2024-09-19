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
  ArgSetColumnSet,
  DurationColumn,
  SliceIdColumn,
  StandardColumn,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';

export const chromeTasksTable: SqlTableDescription = {
  imports: ['chrome.tasks'],
  name: 'chrome_tasks',
  columns: [
    new SliceIdColumn('id', {title: 'ID'}),
    new TimestampColumn('ts', {title: 'Timestamp'}),
    new DurationColumn('dur', {title: 'Duration'}),
    new DurationColumn('thread_dur', {title: 'Thread duration'}),
    new StandardColumn('name', {title: 'Name'}),
    new StandardColumn('track_id', {title: 'Track ID', startsHidden: true}),
    new StandardColumn('thread_name', {title: 'Thread name'}),
    new StandardColumn('utid', {startsHidden: true}),
    new StandardColumn('tid'),
    new StandardColumn('process_name', {title: 'Process name'}),
    new StandardColumn('upid', {startsHidden: true}),
    new ArgSetColumnSet('arg_set_id'),
  ],
};
