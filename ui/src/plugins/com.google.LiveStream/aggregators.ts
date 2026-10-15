// Copyright (C) 2026 The Android Open Source Project
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

// Area selection tabs over the live tracks: the core's aggregators only know
// trace processor tracks. They read the tables, not the tracks' datasets,
// which only hold the rows near the visible window.

import m from 'mithril';
import {Time, type time} from '../../base/time';
import {exists} from '../../base/utils';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
  createIITable,
} from '../../components/aggregation_adapter';
import {formatDurationValue} from '../../components/aggregation_panel';
import {Timestamp} from '../../components/widgets/timestamp';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {
  ACTIVE_DUR,
  COUNTER_TABLE,
  overlappingSlices,
  TRACK_TABLE,
} from './store';

/** Kind of the live slice tracks. */
export const LIVE_SLICE_TRACK_KIND = 'LiveStreamSliceTrack';

/** Kind of the live counter tracks. */
export const LIVE_COUNTER_TRACK_KIND = 'LiveStreamCounterTrack';

// The columns of the slices that the aggregation needs.
const SLICE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  depth: NUM,
  name: STR,
  track_id: NUM,
};

/**
 * Maps the URI of a live track to the track_id of its rows in the tables, if
 * it is a live track.
 */
export type TrackIdOf = (uri: string) => number | undefined;

// The track_ids of the area's live tracks of `kind`.
function trackIds(area: AreaSelection, kind: string, trackId: TrackIdOf) {
  return area.tracks
    .filter((t) => t.tags?.kinds?.includes(kind))
    .map((t) => trackId(t.uri))
    .filter(exists);
}

/**
 * Aggregates the live slices in an area by name. Running slices count up to
 * the playhead at the time of the selection.
 */
export class LiveSliceAggregator implements Aggregator {
  readonly id = 'live_stream_slices';

  constructor(
    private readonly trace: Trace,
    private readonly trackId: TrackIdOf,
    private readonly playhead: () => time,
  ) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const ids = trackIds(area, LIVE_SLICE_TRACK_KIND, this.trackId);
    if (ids.length === 0) return undefined;

    // The interval intersection drops slices without an end.
    const playhead = this.playhead();
    const slices = new SourceDataset({
      src: `
        SELECT
          id,
          ts,
          IIF(dur = ${ACTIVE_DUR}, MAX(0, ${playhead} - ts), dur) AS dur,
          depth,
          name,
          track_id
        FROM (${overlappingSlices(
          Object.keys(SLICE_SCHEMA).join(', '),
          `track_id IN (${ids.join(', ')})`,
          area.start,
          area.end,
        )})
      `,
      schema: SLICE_SCHEMA,
    });

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        await using clipped = await createIITable(
          engine,
          slices,
          area.start,
          area.end,
        );
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT s.id, s.ts, s.dur, s.name, t.name AS lane, s.depth
            FROM ${clipped.name} s
            LEFT JOIN ${TRACK_TABLE} t ON t.id = s.track_id
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  getTabName() {
    return 'Slices';
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        name: {title: 'Name', columnType: 'text'},
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
        lane: {title: 'Lane', columnType: 'text'},
        depth: {title: 'Depth', columnType: 'quantitative'},
      },
      initialColumns: [
        {id: 'name', field: 'name'},
        {id: 'ts', field: 'ts'},
        {id: 'dur', field: 'dur'},
        {id: 'lane', field: 'lane'},
        {id: 'depth', field: 'depth'},
      ],
      initialPivot: {
        groupBy: [{id: 'name', field: 'name'}],
        aggregates: [
          {id: 'count', function: 'COUNT', sort: 'DESC'},
          {id: 'dur_sum', field: 'dur', function: 'SUM'},
          {id: 'dur_avg', field: 'dur', function: 'AVG'},
          {id: 'dur_min', field: 'dur', function: 'MIN'},
          {id: 'dur_max', field: 'dur', function: 'MAX'},
        ],
      },
    };
  }
}

/** Summarizes the samples of the live counter tracks in an area. */
export class LiveCounterAggregator implements Aggregator {
  readonly id = 'live_stream_counters';

  constructor(private readonly trackId: TrackIdOf) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const ids = trackIds(area, LIVE_COUNTER_TRACK_KIND, this.trackId);
    if (ids.length === 0) return undefined;

    return {
      getGridConfig: () => COUNTER_GRID_CONFIG,
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              t.name AS name,
              COUNT(*) AS count,
              MIN(c.value) AS min_value,
              AVG(c.value) AS avg_value,
              MAX(c.value) AS max_value
            FROM ${COUNTER_TABLE} c
            JOIN ${TRACK_TABLE} t ON t.id = c.track_id
            WHERE c.track_id IN (${ids.join(', ')})
              AND c.ts BETWEEN ${area.start} AND ${area.end}
            GROUP BY c.track_id
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  getTabName() {
    return 'Counters';
  }
}

const COUNTER_GRID_CONFIG: AggregatorGridConfig = {
  schema: {
    name: {title: 'Name', columnType: 'text'},
    count: {title: 'Samples', columnType: 'quantitative'},
    min_value: {title: 'Min value', columnType: 'quantitative'},
    avg_value: {title: 'Avg value', columnType: 'quantitative'},
    max_value: {title: 'Max value', columnType: 'quantitative'},
  },
  initialColumns: [
    {id: 'name', field: 'name'},
    {id: 'count', field: 'count'},
    {id: 'min_value', field: 'min_value'},
    {id: 'avg_value', field: 'avg_value'},
    {id: 'max_value', field: 'max_value'},
  ],
};
