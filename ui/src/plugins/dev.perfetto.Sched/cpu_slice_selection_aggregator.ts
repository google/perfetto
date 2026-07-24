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
  type Aggregation,
  type AggregationData,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
  createIITable,
} from '../../components/aggregation_adapter';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Track} from '../../public/track';
import {CPU_SLICE_TRACK_KIND} from '../../public/track_kinds';
import {SourceDataset} from '../../trace_processor/dataset';
import type {Engine} from '../../trace_processor/engine';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {
  LONG,
  NUM,
  type SqlValue,
  UNKNOWN,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {
  formatDurationValue,
  formatPercentValue,
} from '../../components/aggregation_panel';
import {
  createTrackLineage,
  resolveTrackFromLineage,
  type TrackLineageAggregationData,
} from './selection_aggregation_utils';

const CPU_SLICE_SPEC = {
  id: NUM,
  dur: LONG,
  ts: LONG,
  utid: NUM,
  ucpu: NUM,
};

export class CpuSliceSelectionAggregator implements Aggregator {
  readonly id = 'cpu_aggregation';

  private readonly trace: Trace;

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
        const lineage = createTrackLineage(cpuTracks);

        // Query with needed columns for II table
        const iiQuerySchema = {
          ...CPU_SLICE_SPEC,
          __groupid: NUM,
          __partition: UNKNOWN,
        };
        const sql = lineage.unionDataset.query(iiQuerySchema);

        // Create interval-intersect table for time filtering
        await using iiTable = await createIITable(
          engine,
          new SourceDataset({src: `(${sql})`, schema: iiQuerySchema}),
          area.start,
          area.end,
        );

        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              json_object('id', sched.id, 'groupid', __groupid, 'partition', __partition) as id_with_lineage,
              utid,
              process.name as process_name,
              pid,
              thread.name as thread_name,
              tid,
              sched.dur,
              sched.dur * 1.0 / sum(sched.dur) OVER () as fraction_of_total,
              sched.dur * 1.0 / ${area.end - area.start} as fraction_of_selection,
              ucpu
            FROM ${iiTable.name} AS sched
            JOIN thread USING (utid)
            LEFT JOIN process USING (upid)
          `,
        });

        return {
          ...createAggregationData(table),
          ...lineage,
        };
      },
    };
  }

  getTabName() {
    return `CPU by thread`;
  }

  getGridConfig(data?: AggregationData): AggregatorGridConfig {
    const lineage = data as TrackLineageAggregationData | undefined;
    return {
      schema: {
        id_with_lineage: {
          title: 'ID',
          columnType: 'identifier',
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
            const track =
              lineage && resolveTrackFromLineage(lineage, groupid, partition);
            if (!track) {
              return String(id);
            }

            return m(
              Anchor,
              {
                title: 'Go to sched slice',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectTrackEvent(track.uri, id, {
                    scrollToSelection: true,
                  });
                },
              },
              String(id),
            );
          },
        },
        pid: {title: 'PID', columnType: 'identifier'},
        process_name: {title: 'Process Name', columnType: 'text'},
        tid: {title: 'TID', columnType: 'identifier'},
        thread_name: {title: 'Thread Name', columnType: 'text'},
        dur: {
          title: 'CPU Time',
          columnType: 'quantitative',
          cellRenderer: formatDurationValue,
        },
        fraction_of_total: {
          title: 'CPU Time %',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
        fraction_of_selection: {
          title: 'CPU Time / Wall Time',
          columnType: 'quantitative',
          cellRenderer: formatPercentValue,
        },
        ucpu: {title: 'CPU', columnType: 'quantitative'},
      },
      initialPivot: {
        groupBy: [
          {id: 'process_name', field: 'process_name'},
          {id: 'thread_name', field: 'thread_name'},
        ],
        aggregates: [
          {id: 'count', function: 'COUNT'},
          {id: 'dur_sum', field: 'dur', function: 'SUM', sort: 'DESC'},
          {
            id: 'fraction_of_total_sum',
            field: 'fraction_of_total',
            function: 'SUM',
          },
          {id: 'dur_avg', field: 'dur', function: 'AVG'},
        ],
      },
    };
  }
}
