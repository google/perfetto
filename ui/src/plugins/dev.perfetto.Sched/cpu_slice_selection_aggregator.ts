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
import {SourceDataset} from '../../trace_processor/dataset';
import {
  buildQueryWithLineage,
  LineageResolver,
} from '../../trace_processor/dataset_query_utils';
import {Engine} from '../../trace_processor/engine';
import {LONG, NUM, Row} from '../../trace_processor/query_result';
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
  private lineageResolver?: LineageResolver<Track>;

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
        // Build query with lineage tracking
        const {sql, lineageResolver} = buildQueryWithLineage({
          inputs: cpuTracks,
          datasetFetcher: (t) => t.renderer.getDataset?.() as SourceDataset,
          columns: CPU_SLICE_SPEC,
        });

        this.lineageResolver = lineageResolver;

        // Schema including lineage columns
        const schemaWithLineage = {
          ...CPU_SLICE_SPEC,
          z__groupid: NUM,
          z__partition: NUM,
        };

        // Create interval-intersect table for time filtering
        await using iiTable = await createIITable(
          engine,
          new SourceDataset({src: `(${sql})`, schema: schemaWithLineage}),
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
            z__groupid,
            z__partition
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
          cellRenderer: (value: unknown, row: Row) => {
            if (typeof value !== 'bigint') {
              return String(value);
            }

            const groupId = row['z__groupid'];
            const partition = row['z__partition'];

            if (
              typeof groupId !== 'bigint' ||
              (typeof partition !== 'bigint' && partition !== null)
            ) {
              return String(value);
            }

            const track = this.lineageResolver?.resolve(
              Number(groupId),
              Number(partition),
            );
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
          columnId: 'z__partition',
          formatHint: 'ID',
        },
        {
          title: 'GroupID',
          columnId: 'z__groupid',
          formatHint: 'ID',
        },
      ],
    };
  }
}
