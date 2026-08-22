// Copyright (C) 2021 The Android Open Source Project
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

import m from 'mithril';
import {Time} from '../../base/time';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
  createIITable,
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {formatDurationValue} from '../../components/aggregation_panel';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import type {SQLTableSchema} from '../../components/widgets/datagrid/sql_schema';
import {Timestamp} from '../../components/widgets/timestamp';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

export class FrameSelectionAggregator implements Aggregator {
  readonly id = 'frame_aggregation';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(
      area.tracks,
      {
        id: NUM,
        ts: LONG,
        dur: LONG,
        jank_type: STR,
        track_id: NUM,
      },
      ACTUAL_FRAMES_SLICE_TRACK_KIND,
    );

    if (!dataset) return undefined;

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        await using iiTable = await createIITable(
          engine,
          dataset,
          area.start,
          area.end,
        );
        const table = await createPerfettoTable({
          engine,
          as: `
            select
              f.ts,
              f.jank_type,
              f.dur,
              f.track_id,
              process_track.upid,
              process.name as process_name
            from ${iiTable.name} f
            left join process_track on (f.track_id = process_track.id)
            left join process on (process_track.upid = process.upid)
          `,
        });

        return createAggregationData(table);
      },
    };
  }

  getTabName() {
    return 'Frames';
  }

  private getGridConfig(): AggregatorGridConfig {
    const schema: ColumnSchema = {
      jank_type: {title: 'Jank Type', columnType: 'text'},
      ts: {
        title: 'Timestamp',
        columnType: 'quantitative',
        cellRenderer: (value: unknown) => {
          if (typeof value === 'bigint') {
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
          }
          return String(value ?? '');
        },
      },
      dur: {
        title: 'Duration',
        columnType: 'quantitative',
        cellRenderer: formatDurationValue,
      },
      process: {
        title: 'Process',
        schema: {
          id: {
            title: 'UPID',
            columnType: 'identifier',
          },
          name: {
            title: 'Process Name',
            columnType: 'text',
          },
          pid: {
            title: 'PID',
            columnType: 'identifier',
          },
          cmdline: {
            title: 'Cmdline',
            columnType: 'text',
          },
        },
      },
    };

    return {
      schema,
      sqlConfig: ({sqlTable}): SQLTableSchema => ({
        tableOrSubquery: sqlTable.get().name,
        columns: {
          process: {
            foreignKey: 'upid',
            schema: {
              tableOrSubquery: 'process',
              columns: {
                id: {},
                name: {},
                pid: {},
                cmdline: {},
              },
            },
          },
        },
      }),
      initialColumns: [
        {id: 'jank_type', field: 'jank_type'},
        {id: 'ts', field: 'ts'},
        {id: 'dur', field: 'dur'},
        {id: 'process_name', field: 'process_name'},
      ],
      initialPivot: {
        groupBy: [{id: 'jank_type', field: 'jank_type'}],
        aggregates: [
          {id: 'count', function: 'COUNT', sort: 'DESC'},
          {id: 'dur_min', field: 'dur', function: 'MIN'},
          {id: 'dur_max', field: 'dur', function: 'MAX'},
          {id: 'dur_avg', field: 'dur', function: 'AVG'},
        ],
      },
    };
  }
}
