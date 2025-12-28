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
import {
  AggregatePivotModel,
  Aggregation,
  Aggregator,
  createIITable,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Track} from '../../public/track';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {
  Dataset,
  DatasetSchema,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, Row, UNKNOWN} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';

const CPU_SLICE_SPEC = {
  id: NUM,
  dur: LONG,
  ts: LONG,
  utid: NUM,
};

export class CpuSliceSelectionAggregator implements Aggregator {
  readonly id = 'cpu_aggregation';

  private readonly trace: Trace;
  private trackDatasetMap?: Map<Dataset, Track>;
  private unionDataset?: UnionDatasetWithLineage<DatasetSchema>;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  probe(area: AreaSelection): Aggregation | undefined {
    // Collect CPU slice tracks
    const cpuTracks: Track[] = [];

    for (const track of area.tracks) {
      if (!track.tags?.kinds?.includes(CPU_SLICE_TRACK_KIND)) continue;
      const dataset = track.renderer.getDataset?.();
      if (!dataset || !(dataset instanceof SourceDataset)) continue;
      if (!dataset.implements(CPU_SLICE_SPEC)) continue;
      cpuTracks.push(track);
    }

    if (cpuTracks.length === 0) return undefined;

    return {
      prepareData: async (engine: Engine) => {
        // Build track-to-dataset mapping
        this.trackDatasetMap = new Map();
        const datasets: Dataset[] = [];
        for (const track of cpuTracks) {
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
          ...CPU_SLICE_SPEC,
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
          create or replace perfetto table ${this.id} as
          select
            sched.id,
            utid,
            process.name as process_name,
            pid,
            thread.name as thread_name,
            tid,
            sched.dur,
            sched.dur * 1.0 / sum(sched.dur) OVER () as fraction_of_total,
            sched.dur * 1.0 / ${area.end - area.start} as fraction_of_selection,
            __groupid,
            __partition
          from ${iiTable.name} as sched
          join thread using (utid)
          left join process using (upid)
        `);

        return {
          tableName: this.id,
        };
      },
    };
  }

  getTabName() {
    return 'CPU by thread';
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [
        {field: 'pid'},
        {field: 'process_name'},
        {field: 'tid'},
        {field: 'thread_name'},
      ],
      aggregates: [
        {function: 'COUNT'},
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
                title: 'Go to sched slice',
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
        {
          title: 'PID',
          columnId: 'pid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Process Name',
          columnId: 'process_name',
          formatHint: 'STRING',
        },
        {
          title: 'TID',
          columnId: 'tid',
          formatHint: 'NUMERIC',
        },
        {
          title: 'Thread Name',
          columnId: 'thread_name',
          formatHint: 'STRING',
        },
        {
          title: 'Wall Duration',
          formatHint: 'DURATION_NS',
          columnId: 'dur',
        },
        {
          title: 'Wall Duration as % of Total',
          columnId: 'fraction_of_total',
          formatHint: 'PERCENT',
        },
        {
          title: 'Wall Duration % of Selection',
          columnId: 'fraction_of_selection',
          formatHint: 'PERCENT',
        },
        {
          title: 'Partition',
          columnId: '__partition',
          formatHint: 'ID',
        },
        {
          title: 'GroupID',
          columnId: '__groupid',
          formatHint: 'ID',
        },
      ],
    };
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
