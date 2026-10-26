// Copyright (C) 2020 The Android Open Source Project
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
import {Icons} from '../../base/semantic_icons';
import {Duration, Time} from '../../base/time';
import type {BarChartData} from '../../components/aggregation';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridPreset,
  createAggregationData,
  createIITable,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {
  type Dataset,
  type DatasetSchema,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  type SqlValue,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {colorForThreadState} from './common';
import {
  formatDurationValue,
  formatPercentValue,
} from '../../components/aggregation_panel';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Timestamp} from '../../components/widgets/timestamp';

const THREAD_STATE_SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  ucpu: NUM_NULL,
  state: STR,
  utid: NUM,
};

export class ThreadStateSelectionAggregator implements Aggregator {
  readonly id = 'thread_state_aggregation';

  private readonly trace: Trace;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  probe(area: AreaSelection): Aggregation | undefined {
    // Collect thread state tracks
    const threadStateTracks: Track[] = [];

    for (const track of area.tracks) {
      if (!track.tags?.kinds?.includes(THREAD_STATE_TRACK_KIND)) continue;
      const dataset = track.renderer.getDataset?.();
      if (!dataset || !(dataset instanceof SourceDataset)) continue;
      if (!dataset.implements(THREAD_STATE_SPEC)) continue;
      threadStateTracks.push(track);
    }

    if (threadStateTracks.length === 0) return undefined;

    // Build track-to-dataset mapping synchronously
    const trackDatasetMap = new Map<Dataset, Track>();
    const datasets: Dataset[] = [];
    for (const track of threadStateTracks) {
      const dataset = track.renderer.getDataset?.();
      if (dataset) {
        datasets.push(dataset);
        trackDatasetMap.set(dataset, track);
      }
    }

    // Create union dataset with lineage tracking
    const unionDataset = UnionDatasetWithLineage.create(datasets);

    return {
      getGridConfig: () =>
        this.getGridConfig((groupId, partition) =>
          this.resolveTrack(groupId, partition, trackDatasetMap, unionDataset),
        ),
      prepareData: async (engine: Engine) => {
        // Query with needed columns for II table
        const iiQuerySchema = {
          ...THREAD_STATE_SPEC,
          __groupid: NUM,
          __partition: UNKNOWN,
        };
        const sql = unionDataset.query(iiQuerySchema);

        // Create interval-intersect table for time filtering
        await using iiTable = await createIITable(
          engine,
          new SourceDataset({src: `(${sql})`, schema: iiQuerySchema}),
          area.start,
          area.end,
        );

        await engine.query('include perfetto module android.cpu.cluster_type');

        const table = await createPerfettoTable({
          engine,
          as: `
            select
              json_object('id', tstate.id, 'groupid', __groupid, 'partition', __partition) as id_with_lineage,
              process.name as process_name,
              process.pid as pid,
              thread.name as thread_name,
              thread.tid as tid,
              tstate.state as state,
              utid,
              ucpu,
              tstate.ts,
              dur,
              dur * 1.0 / sum(dur) OVER () as fraction_of_total,
              android_cpu_cluster_mapping.cluster_type as cluster_type
            from ${iiTable.name} tstate
            join thread using (utid)
            left join process using (upid)
            left join android_cpu_cluster_mapping using(ucpu)
          `,
        });

        const query = `
          select
            tstate.state as state,
            sum(dur) as totalDur
          from ${iiTable.name} tstate
          join thread using (utid)
          group by tstate.state
        `;
        const result = await engine.query(query);

        const it = result.iter({
          state: STR_NULL,
          totalDur: LONG,
        });

        const states: BarChartData[] = [];
        for (; it.valid(); it.next()) {
          const name = it.state ?? 'Unknown';
          states.push({
            title: `${name}: ${Duration.humanise(it.totalDur)}`,
            value: Number(it.totalDur),
            color: colorForThreadState(name),
          });
        }

        return createAggregationData(table, states);
      },
    };
  }

  private getGridConfig(
    resolveTrack: (groupId: number, partition: SqlValue) => Track | undefined,
  ): ReadonlyArray<AggregatorGridPreset> {
    const schema = {
      id_with_lineage: {
        title: 'ID',
        columnType: 'identifier' as const,
        cellRenderer: (value: unknown) => {
          // Value is a JSON object {id, groupid, partition}
          if (typeof value !== 'string') {
            return String(value);
          }

          const parsed = JSON.parse(value) as {
            id: number;
            groupid: number;
            partition: SqlValue;
          };
          const {id, groupid, partition} = parsed;

          // Resolve track from lineage
          const track = resolveTrack(groupid, partition);
          if (!track) {
            return String(id);
          }

          return m(
            Anchor,
            {
              title: 'Go to thread state',
              icon: Icons.UpdateSelection,
              onclick: () => {
                this.trace.selection.selectTrackEvent(track.uri, id, {
                  scrollToSelection: true,
                  switchToCurrentSelectionTab: false,
                });
              },
            },
            String(id),
          );
        },
      },
      cluster_type: {title: 'Cluster Type', columnType: 'text' as const},
      process_name: {title: 'Process', columnType: 'text' as const},
      pid: {title: 'PID', columnType: 'identifier' as const},
      thread_name: {title: 'Thread', columnType: 'text' as const},
      tid: {title: 'TID', columnType: 'identifier' as const},
      ucpu: {title: 'CPU', columnType: 'quantitative' as const},
      utid: {title: 'UTID', columnType: 'identifier' as const},
      state: {title: 'State', columnType: 'text' as const},
      ts: {
        title: 'Timestamp',
        columnType: 'quantitative' as const,
        cellRenderer: (value: unknown) => {
          if (typeof value === 'bigint') {
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
          }
          return String(value ?? '');
        },
      },
      dur: {
        title: 'Wall duration',
        columnType: 'quantitative' as const,
        cellRenderer: formatDurationValue,
      },
      fraction_of_total: {
        title: 'Wall duration %',
        columnType: 'quantitative' as const,
        cellRenderer: formatPercentValue,
      },
    };

    const aggregates = [
      {id: 'count', function: 'COUNT' as const},
      {id: 'process_name_any', field: 'process_name', function: 'ANY' as const},
      {id: 'pid_any', field: 'pid', function: 'ANY' as const},
      {id: 'thread_name_any', field: 'thread_name', function: 'ANY' as const},
      {id: 'tid_any', field: 'tid', function: 'ANY' as const},
      {
        id: 'dur_sum',
        field: 'dur',
        function: 'SUM' as const,
        sort: 'DESC' as const,
      },
      {
        id: 'fraction_of_total_sum',
        field: 'fraction_of_total',
        function: 'SUM' as const,
      },
      {id: 'dur_avg', field: 'dur', function: 'AVG' as const},
    ];

    const initialColumns = [
      {id: 'id_with_lineage', field: 'id_with_lineage'},
      {id: 'process_name', field: 'process_name'},
      {id: 'pid', field: 'pid'},
      {id: 'thread_name', field: 'thread_name'},
      {id: 'tid', field: 'tid'},
      {id: 'state', field: 'state'},
      {id: 'ts', field: 'ts'},
      {id: 'dur', field: 'dur'},
      {id: 'ucpu', field: 'ucpu'},
    ];

    return [
      {
        displayName: 'By Thread',
        config: {
          schema,
          initialColumns,
          initialPivot: {
            groupBy: [
              {id: 'thread_name', field: 'thread_name'},
              {id: 'state', field: 'state'},
            ],
            aggregates,
          },
        },
      },
      {
        displayName: 'By CPU',
        config: {
          schema,
          initialColumns,
          initialPivot: {
            groupBy: [
              {id: 'thread_name', field: 'thread_name'},
              {id: 'state', field: 'state'},
              {id: 'ucpu', field: 'ucpu'},
            ],
            aggregates,
          },
        },
      },
    ];
  }

  getTabName() {
    return 'Thread States';
  }

  /**
   * Resolve a track from lineage information.
   */
  private resolveTrack(
    groupId: number,
    partition: SqlValue,
    trackDatasetMap: Map<Dataset, Track>,
    unionDataset?: UnionDatasetWithLineage<DatasetSchema>,
  ): Track | undefined {
    if (!unionDataset) return undefined;

    // Ensure partition is a valid SqlValue
    const partitionValue =
      partition === null ||
      typeof partition === 'number' ||
      typeof partition === 'bigint' ||
      typeof partition === 'string' ||
      partition instanceof Uint8Array
        ? partition
        : null;

    const datasets = unionDataset.resolveLineage({
      __groupid: groupId,
      __partition: partitionValue,
    });

    for (const dataset of datasets) {
      const track = trackDatasetMap.get(dataset);
      if (track) return track;
    }

    return undefined;
  }
}
