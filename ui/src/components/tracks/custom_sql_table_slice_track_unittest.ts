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

import {Trace} from '../../public/trace';
import {
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from './custom_sql_table_slice_track';
import {SourceDataset} from '../../trace_processor/dataset';

describe('CustomSqlTableSliceTrack.getDataset()', () => {
  test('simple track', () => {
    class Track extends CustomSqlTableSliceTrack {
      getSqlDataSource(): CustomSqlTableDefConfig {
        return {
          sqlTableName: 'footable',
        };
      }
    }

    const foo = new Track(undefined as unknown as Trace, 'foo');
    const dataset = foo.getDataset() as SourceDataset;

    expect(dataset.src).toBe('SELECT * FROM footable');
  });

  test('track with cols', () => {
    class Track extends CustomSqlTableSliceTrack {
      getSqlDataSource(): CustomSqlTableDefConfig {
        return {
          columns: ['foo', 'bar', 'baz'],
          sqlTableName: 'footable',
        };
      }
    }

    const foo = new Track(undefined as unknown as Trace, 'foo');
    const dataset = foo.getDataset() as SourceDataset;

    expect(dataset.src).toBe('SELECT foo,bar,baz FROM footable');
  });

  test('track with where clause', () => {
    class Track extends CustomSqlTableSliceTrack {
      getSqlDataSource(): CustomSqlTableDefConfig {
        return {
          sqlTableName: 'footable',
          columns: ['foo', 'bar', 'baz'],
          whereClause: 'bar = 123',
        };
      }
    }

    const foo = new Track(undefined as unknown as Trace, 'foo');
    const dataset = foo.getDataset() as SourceDataset;

    expect(dataset.src).toBe(
      'SELECT foo,bar,baz FROM footable WHERE bar = 123',
    );
  });
});
