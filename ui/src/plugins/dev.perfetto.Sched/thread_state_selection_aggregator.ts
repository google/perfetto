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
import {Duration} from '../../base/time';
import {BarChartData} from '../../components/aggregation';
import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Track} from '../../public/track';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  Row,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {colorForThreadState} from './common';

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
  private trackDatasetMap?: Map<Dataset, Track>;
  private unionDataset?: UnionDatasetWithLineage<DatasetSchema>;

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

    return {
      prepareData: async (engine: Engine) => {
        // Build track-to-dataset mapping
        this.trackDatasetMap = new Map();
        const datasets: Dataset[] = [];
        for (const track of threadStateTracks) {
          const dataset = track.renderer.getDataset?.();
          if (dataset) {
            datasets.push(dataset);
            this.trackDatasetMap.set(dataset, track);
          }
        }

        // Create union dataset with lineage tracking
        this.unionDataset = UnionDatasetWithLineage.create(datasets);

        // Query with needed columns for II table
        const iiQuerySchema = {
          ...THREAD_STATE_SPEC,
          __groupid: NUM,
          __partition: UNKNOWN,
        };
        const sql = this.unionDataset.query(iiQuerySchema);

        // Create interval-intersect table for time filtering
        await using iiTable = await createIITable(
          engine,
          new SourceDataset({src: `(${sql})`, schema: iiQuerySchema}),
          area.start,
          area.end,
        );

        await engine.query(`
          include perfetto module android.cpu.cluster_type;

          create or replace perfetto table ${this.id} as
          select
            tstate.id as id,
            process.name as process_name,
            process.pid as pid,
            thread.name as thread_name,
            thread.tid as tid,
            tstate.state as state,
            utid,
            ucpu,
            dur,
            dur * 1.0 / sum(dur) OVER () as fraction_of_total,
            android_cpu_cluster_mapping.cluster_type as cluster_type,
            __groupid,
            __partition
          from ${iiTable.name} tstate
          join thread using (utid)
          left join process using (upid)
          left join android_cpu_cluster_mapping using(ucpu)
        `);

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
        for (let i = 0; it.valid(); ++i, it.next()) {
          const name = it.state ?? 'Unknown';
          states.push({
            title: `${name}: ${Duration.humanise(it.totalDur)}`,
            value: Number(it.totalDur),
            color: colorForThreadState(name),
          });
        }

        return {
          tableName: this.id,
          barChartData: states,
        };
      },
    };
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [{field: 'utid'}, {field: 'state'}],
      aggregates: [
        {function: 'COUNT'},
        {field: 'process_name', function: 'ANY'},
        {field: 'pid', function: 'ANY'},
        {field: 'thread_name', function: 'ANY'},
        {field: 'tid', function: 'ANY'},
        {field: 'dur', function: 'SUM', sort: 'DESC'},
        {field: 'fraction_of_total', function: 'SUM'},
        {field: 'dur', function: 'AVG'},
      ],
      columns: [
        {
          title: 'ID',
          columnId: 'id',
          formatHint: 'ID',
          dependsOn: ['__groupid', '__partition'],
          cellRenderer: (value: unknown, row: Row) => {
            if (typeof value !== 'bigint') {
              return String(value);
            }

            const groupId = row['__groupid'];
            const partition = row['__partition'];

            if (typeof groupId !== 'bigint') {
              return String(value);
            }

            // Resolve track from lineage
            const track = this.resolveTrack(Number(groupId), partition);
            if (!track) {
              return String(value);
            }

            return m(
              Anchor,
              {
                title: 'Go to thread state',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(
                    track.uri,
                    Number(value),
                    {
                      scrollToSelection: true,
                    },
                  );
                },
              },
              String(value),
            );
          },
        },
        {title: 'Cluster Type', columnId: 'cluster_type', formatHint: 'STRING'},
        {
          title: 'Process',
          columnId: 'process_name',
          formatHint: 'STRING',
        },
        {
          title: 'PID',
          columnId: 'pid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Thread',
          columnId: 'thread_name',
          formatHint: 'STRING',
        },
        {
          title: 'TID',
          columnId: 'tid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'CPU',
          columnId: 'ucpu',
          formatHint: 'NUMERIC',
        },
        {
          title: 'UTID',
          columnId: 'utid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'State',
          columnId: 'state',
        },
        {
          title: 'Wall duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
        {
          title: 'Wall duration %',
          formatHint: 'PERCENT',
          columnId: 'fraction_of_total',
        },
      ],
    };
  }

  getTabName() {
    return 'Thread States';
  }

  /**
   * Resolve a track from lineage information.
   */
  private resolveTrack(groupId: number, partition: unknown): Track | undefined {
    if (!this.trackDatasetMap || !this.unionDataset) return undefined;

    // Ensure partition is a valid SqlValue
    const partitionValue =
      partition === null ||
      typeof partition === 'number' ||
      typeof partition === 'bigint' ||
      typeof partition === 'string' ||
      partition instanceof Uint8Array
        ? partition
        : null;

    const datasets = this.unionDataset.resolveLineage({
      __groupid: groupId,
      __partition: partitionValue,
    });

    for (const dataset of datasets) {
      const track = this.trackDatasetMap.get(dataset);
      if (track) return track;
    }

    return undefined;
  }
}
