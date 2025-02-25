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

import {sqlColumnId} from '../../components/widgets/sql/table/sql_column';
import {argTableColumn} from '../../components/widgets/sql/table/columns';

test('arg_set_column.id', () => {
  expect(sqlColumnId(argTableColumn('arg_set_id', 'arg1').column)).toBe(
    'arg_set_id[arg1]',
  );
});

test('arg_set_column.id_with_join', () => {
  expect(
    sqlColumnId(
      argTableColumn(
        {
          column: 'arg_set_id',
          source: {
            table: 'foo',
            joinOn: {
              x: 'y',
            },
          },
        },
        'arg1',
      ).column,
    ),
  ).toBe('foo[x=y].arg_set_id[arg1]');
});
