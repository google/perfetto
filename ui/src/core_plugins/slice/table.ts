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
  ProcessColumnSet,
  SliceIdColumn,
  StandardColumn,
  ThreadColumnSet,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';

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
