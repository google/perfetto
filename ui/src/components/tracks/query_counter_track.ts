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

import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Trace} from '../../public/trace';
import {sqlNameSafe} from '../../base/string_utils';
import {BaseCounterTrack, CounterOptions} from './base_counter_track';
import {Engine} from '../../trace_processor/engine';

export interface QueryCounterTrackArgs {
  // The trace object used to run queries.
  readonly trace: Trace;

  // A unique, reproducible ID for this track.
  readonly uri: string;

  // The query and optional column remapping.
  readonly data: SqlDataSource;

  // Optional: Which columns should be used for ts, and value. If omitted,
  // the defaults 'ts', and 'value' will be used.
  readonly columns?: Partial<CounterColumnMapping>;

  // Optional: Display options for the counter track.
  readonly options?: Partial<CounterOptions>;

  // Optional: Whether to materialize the query results. If omitted, we
  // will materialize.
  readonly materialize?: boolean;
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

export interface CounterColumnMapping {
  readonly ts: string;
  readonly value: string;
}

/**
 * Creates a counter track based on a query.
 *
 * The query must provide the following columns:
 * - ts: INTEGER - The timestamp of each sample.
 * - value: REAL | INTEGER - The value of each sample.
 *
 * The column names don't have to be 'ts' and 'value', and can be remapped if
 * convenient using the config.columns parameter.
 */
export async function createQueryCounterTrack(args: QueryCounterTrackArgs) {
  if (args.materialize === false) {
    return new SqlTableCounterTrack(
      args.trace,
      args.uri,
      wrapQueryForCounterTrack(args.data, args.columns),
      args.options,
    );
  } else {
    const tableName = `__query_counter_track_${sqlNameSafe(args.uri)}`;
    await createPerfettoTableForTrack(
      args.trace.engine,
      tableName,
      args.data,
      args.columns,
    );
    return new SqlTableCounterTrack(
      args.trace,
      args.uri,
      tableName,
      args.options,
    );
  }
}

function wrapQueryForCounterTrack(
  data: SqlDataSource,
  columnMapping: Partial<CounterColumnMapping> = {},
) {
  const {ts = 'ts', value = 'value'} = columnMapping;
  return `
    with data as (
      ${data.sqlSource}
    )
    select
      ${ts} as ts,
      ${value} as value
    from data
    order by ts
  `;
}

async function createPerfettoTableForTrack(
  engine: Engine,
  tableName: string,
  data: SqlDataSource,
  columnMapping: Partial<CounterColumnMapping> = {},
) {
  return await createPerfettoTable({
    engine,
    name: tableName,
    as: wrapQueryForCounterTrack(data, columnMapping),
  });
}

export class SqlTableCounterTrack extends BaseCounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    private readonly sqlSource: string,
    options?: Partial<CounterOptions>,
  ) {
    super(trace, uri, options);
  }

  getSqlSource(): string {
    return `select * from (${this.sqlSource})`;
  }
}
