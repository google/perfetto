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

import {sqliteString} from '../../../../base/string_utils';
import {SourceTable} from './column';
import {buildSqlQuery} from './query_builder';

function normalise(str: string): string {
  return str.replace(/\s+/g, ' ').trim();
}

test('query_builder.basic_select', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
        },
      }),
    ),
  ).toBe('SELECT slice_0.id AS id FROM slice AS slice_0');
});

test('query_builder.basic_filter', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
        },
        filters: [
          {
            op: (cols) => `${cols[0]} != -1`,
            columns: ['ts'],
          },
        ],
      }),
    ),
  ).toBe(
    'SELECT slice_0.id AS id FROM slice AS slice_0 WHERE slice_0.ts != -1',
  );
});

test('query_builder.basic_order_by', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
        },
        orderBy: [
          {
            column: 'ts',
            direction: 'ASC',
          },
        ],
      }),
    ),
  ).toBe(
    'SELECT slice_0.id AS id FROM slice AS slice_0 ORDER BY slice_0.ts ASC',
  );
});

test('query_builder.simple_join', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
          name: 'name',
          parent_name: {
            column: 'name',
            source: {
              table: 'slice',
              joinOn: {
                id: 'parent_id',
              },
            },
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      slice_0.id AS id,
      slice_0.name AS name,
      slice_1.name AS parent_name
    FROM slice AS slice_0
    LEFT JOIN slice AS slice_1 ON slice_1.id = slice_0.parent_id
  `),
  );
});

// Check a query with INNER JOIN instead of LEFT JOIN.
test('query_builder.left_join', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'foo',
        columns: {
          foo_id: 'id',
          slice_name: {
            column: 'name',
            source: {
              table: 'slice',
              innerJoin: true,
              joinOn: {
                id: 'slice_id',
              },
            },
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      foo_0.id AS foo_id,
      slice_1.name AS slice_name
    FROM foo AS foo_0
    JOIN slice AS slice_1 ON slice_1.id = foo_0.slice_id
  `),
  );
});

// Check a query which has both INNER JOIN and LEFT JOIN on the same table.
// The correct behaviour here is debatable (probably we can upgrade INNER JOIN to LEFT JOIN),
// but for now we just generate the query with two separate joins.
test('query_builder.left_join_and_inner_join', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'foo',
        columns: {
          foo_id: 'id',
          slice_name: {
            column: 'name',
            source: {
              table: 'slice',
              innerJoin: true,
              joinOn: {
                id: 'slice_id',
              },
            },
          },
          slice_depth: {
            column: 'depth',
            source: {
              table: 'slice',
              joinOn: {
                id: 'slice_id',
              },
            },
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      foo_0.id AS foo_id,
      slice_1.name AS slice_name,
      slice_2.depth AS slice_depth
    FROM foo AS foo_0
    JOIN slice AS slice_1 ON slice_1.id = foo_0.slice_id
    LEFT JOIN slice AS slice_2 ON slice_2.id = foo_0.slice_id
  `),
  );
});

test('query_builder.join_with_multiple_columns', () => {
  // This test checks that the query builder can correctly deduplicate joins when we request multiple columns from the joined table.
  const parent: SourceTable = {
    table: 'slice',
    joinOn: {
      id: 'parent_id',
    },
  };
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
          name: 'name',
          parent_name: {
            column: 'name',
            source: parent,
          },
          parent_dur: {
            column: 'dur',
            source: parent,
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      slice_0.id AS id,
      slice_0.name AS name,
      slice_1.name AS parent_name,
      slice_1.dur AS parent_dur
    FROM slice AS slice_0
    LEFT JOIN slice AS slice_1 ON slice_1.id = slice_0.parent_id
  `),
  );
});

test('query_builder.filter_on_joined_column', () => {
  // This test checks that the query builder can correctly deduplicate joins when we request multiple columns from the joined table.
  const parent: SourceTable = {
    table: 'slice',
    joinOn: {
      id: 'parent_id',
    },
  };
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
          name: 'name',
          parent_name: {
            column: 'name',
            source: parent,
          },
        },
        filters: [
          {
            op: (cols) => `${cols[0]} != -1`,
            columns: [
              {
                column: 'dur',
                source: parent,
              },
            ],
          },
        ],
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      slice_0.id AS id,
      slice_0.name AS name,
      slice_1.name AS parent_name
    FROM slice AS slice_0
    LEFT JOIN slice AS slice_1 ON slice_1.id = slice_0.parent_id
    WHERE slice_1.dur != -1
  `),
  );
});

test('query_builder.complex_join', () => {
  const threadTrack: SourceTable = {
    table: 'thread_track',
    joinOn: {
      id: 'track_id',
    },
  };

  const thread: SourceTable = {
    table: 'thread',
    joinOn: {
      utid: {
        column: 'utid',
        source: threadTrack,
      },
    },
  };

  const process: SourceTable = {
    table: 'process',
    joinOn: {
      upid: {
        column: 'upid',
        source: thread,
      },
    },
  };

  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          id: 'id',
          name: 'name',
          tid: {
            column: 'tid',
            source: thread,
          },
          thread_name: {
            column: 'name',
            source: thread,
          },
          pid: {
            column: 'pid',
            source: process,
          },
          process_name: {
            column: 'name',
            source: process,
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      slice_0.id AS id,
      slice_0.name AS name,
      thread_2.tid AS tid,
      thread_2.name AS thread_name,
      process_3.pid AS pid,
      process_3.name AS process_name
    FROM slice AS slice_0
    LEFT JOIN thread_track AS thread_track_1 ON thread_track_1.id = slice_0.track_id
    LEFT JOIN thread AS thread_2 ON thread_2.utid = thread_track_1.utid
    LEFT JOIN process AS process_3 ON process_3.upid = thread_2.upid
  `),
  );
});

test('query_builder.multiple_args', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          count: 'count()',
          arg1: {
            column: 'display_value',
            source: {
              table: 'args',
              joinOn: {
                arg_set_id: 'arg_set_id',
                key: sqliteString('arg1'),
              },
            },
          },
          arg2: {
            column: 'display_value',
            source: {
              table: 'args',
              joinOn: {
                arg_set_id: 'arg_set_id',
                key: sqliteString('arg2'),
              },
            },
          },
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      count() AS count,
      args_1.display_value AS arg1,
      args_2.display_value AS arg2
    FROM slice AS slice_0
    LEFT JOIN args AS args_1 ON args_1.arg_set_id = slice_0.arg_set_id AND args_1.key = 'arg1'
    LEFT JOIN args AS args_2 ON args_2.arg_set_id = slice_0.arg_set_id AND args_2.key = 'arg2'
  `),
  );
});

test('query_builder.expression', () => {
  expect(
    normalise(
      buildSqlQuery({
        table: 'slice',
        columns: {
          count: 'count()',
        },
      }),
    ),
  ).toBe(
    normalise(`
    SELECT
      count() AS count
    FROM slice AS slice_0
  `),
  );
});
