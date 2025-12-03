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

import {createFakeTraceImpl} from '../../../../core/fake_trace_impl';
import {tableColumnId} from './table_column';
import {SqlTableState} from './state';
import {SqlTableDescription} from './table_description';
import {StandardColumn, TimestampColumn} from './columns';

const trace = createFakeTraceImpl({allowQueries: true});
const idColumn = new StandardColumn('id', undefined);
const nameColumn = new StandardColumn('name', undefined);
const tsColumn = new TimestampColumn(trace, 'ts');

const table: SqlTableDescription = {
  name: 'table',
  displayName: 'Table',
  columns: [idColumn, nameColumn, tsColumn],
};

test('sqlTableState: columnManupulation', () => {
  const state = new SqlTableState(trace, table);

  state.addColumn(tsColumn, 0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'id',
    'ts',
    'name',
    'ts',
  ]);

  state.hideColumnAtIndex(0);

  expect(state.getSelectedColumns().map((c) => tableColumnId(c))).toEqual([
    'ts',
    'name',
    'ts',
  ]);
});
