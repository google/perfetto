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
  ProcessColumn,
  ProcessIdColumn,
  StandardColumn,
  TimestampColumn,
} from '../../frontend/widgets/sql/table/well_known_columns';

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
