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

import {DatasetSliceTrack} from './dataset_slice_track';
import {
  ARG_PREFIX,
  DebugSliceTrackDetailsPanel,
} from './debug_slice_track_details_panel';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Trace} from '../../public/trace';
import {sqlNameSafe} from '../../base/string_utils';
import {Engine} from '../../trace_processor/engine';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

export interface QuerySliceTrackArgs {
  // The trace object used to run queries.
  readonly trace: Trace;

  // A unique, reproducible ID for this track.
  readonly uri: string;

  // The query and optional column remapping.
  readonly data: SqlDataSource;

  // Optional: Which columns should be used for ts, dur, and name. If omitted,
  // the defaults 'ts', 'dur', and 'name' will be used.
  readonly columns?: Partial<SliceColumnMapping>;

  // Optional: A list of column names which are displayed in the details panel
  // when a slice is selected.
  readonly argColumns?: string[];
}

export interface SqlDataSource {
  // SQL source selecting the necessary data.
  readonly sqlSource: string;

  // Optional: Rename columns from the query result.
  // If omitted, original column names from the query are used instead.
  // The caller is responsible for ensuring that the number of items in this
  // list matches the number of columns returned by sqlSource.
  readonly columns?: string[];
}

export interface SliceColumnMapping {
  readonly ts: string;
  readonly dur: string;
  readonly name: string;
}

/**
 * Creates a slice track based on a query with automatic slice layout.
 *
 * The query must provide the following columns:
 * - ts: INTEGER - The timestamp of the start of each slice.
 * - dur: INTEGER - The length of each slice.
 * - name: TEXT - A name to show on each slice, which is also used to derive the
 *   color.
 *
 * The column names don't have to be 'ts', 'dur', and 'name' and can be remapped
 * if convenient using the config.columns parameter.
 *
 * An optional set of columns can be provided which will be displayed in the
 * details panel when a slice is selected.
 *
 * The layout (vertical depth) of each slice will be determined automatically to
 * avoid overlapping slices.
 */
export async function createQuerySliceTrack(args: QuerySliceTrackArgs) {
  const tableName = `__query_slice_track_${sqlNameSafe(args.uri)}`;
  await createPerfettoTableForTrack(
    args.trace.engine,
    tableName,
    args.data,
    args.columns,
    args.argColumns,
  );
  return new DatasetSliceTrack({
    trace: args.trace,
    uri: args.uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
      },
      src: tableName,
    }),
    detailsPanel: (row) => {
      return new DebugSliceTrackDetailsPanel(args.trace, tableName, row.id);
    },
  });
}

async function createPerfettoTableForTrack(
  engine: Engine,
  tableName: string,
  data: SqlDataSource,
  columns: Partial<SliceColumnMapping> = {},
  argColumns: string[] = [],
) {
  const {ts = 'ts', dur = 'dur', name = 'name'} = columns;

  // If the view has clashing names (e.g. "name" coming from joining two
  // different tables, we will see names like "name_1", "name_2", but they
  // won't be addressable from the SQL. So we explicitly name them through a
  // list of columns passed to CTE.
  const dataColumns =
    data.columns !== undefined ? `(${data.columns.join(', ')})` : '';

  const query = `
    with data${dataColumns} as (
      ${data.sqlSource}
    ),
    prepared_data as (
      select
        ${ts} as ts,
        ifnull(cast(${dur} as int), -1) as dur,
        printf('%s', ${name}) as name
        ${argColumns.length > 0 ? ',' : ''}
        ${argColumns.map((c) => `${c} as ${ARG_PREFIX}${c}`).join(',\n')}
      from data
    )
    select
      row_number() over (order by ts) as id,
      *
    from prepared_data
    order by ts
  `;

  return await createPerfettoTable(engine, tableName, query);
}
