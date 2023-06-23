// Copyright (C) 2023 The Android Open Source Project
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

import {SqlTableDescription} from './table';

const sliceTable: SqlTableDescription = {
  imports: ['experimental.slices'],
  name: 'experimental_slice_with_thread_and_process_info',
  displayName: 'slice',
  columns: [
    {
      name: 'id',
      title: 'ID',
      display: {
        type: 'slice_id',
        ts: 'ts',
        dur: 'dur',
        trackId: 'track_id',
      },
    },
    {
      name: 'ts',
      title: 'Timestamp',
      display: {
        type: 'timestamp',
      },
    },
    {
      name: 'dur',
      title: 'Duration',
      display: {
        type: 'duration',
      },
    },
    {
      name: 'thread_dur',
      title: 'Thread duration',
      display: {
        type: 'thread_duration',
      },
    },
    {
      name: 'category',
      title: 'Category',
    },
    {
      name: 'name',
      title: 'Name',
    },
    {
      name: 'track_id',
      title: 'Track ID',
      startsHidden: true,
    },
    {
      name: 'track_name',
      title: 'Track name',
      startsHidden: true,
    },
    {
      name: 'thread_name',
      title: 'Thread name',
    },
    {
      name: 'utid',
      startsHidden: true,
    },
    {
      name: 'tid',
    },
    {
      name: 'process_name',
      title: 'Process name',
    },
    {
      name: 'upid',
      startsHidden: true,
    },
    {
      name: 'pid',
    },
    {
      name: 'depth',
      title: 'Depth',
      startsHidden: true,
    },
    {
      name: 'parent_id',
      title: 'Parent slice ID',
      startsHidden: true,
    },
    {
      name: 'arg_set_id',
      title: 'Arg',
      type: 'arg_set_id',
    },
  ],
};

export class SqlTables {
  static readonly slice = sliceTable;
}
