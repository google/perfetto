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

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {Time} from '../../base/time';
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
import {Timestamp} from '../../components/widgets/timestamp';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {ANDROID_LOGS_TRACK_KIND} from '../../public/track_kinds';
import type {Engine} from '../../trace_processor/engine';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';

export class AndroidLogSelectionAggregator implements Aggregator {
  readonly id = 'android_log_aggregation';

  constructor(private readonly trace: Trace) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const logTracks = area.tracks.filter((t) =>
      t.tags?.kinds?.includes(ANDROID_LOGS_TRACK_KIND),
    );

    if (logTracks.length === 0) return undefined;

    const utids = logTracks
      .map((t) => t.tags?.utid as number | undefined)
      .filter((u): u is number => u !== undefined);
    const machineIds = logTracks
      .map((t) => t.tags?.machineId as number | undefined)
      .filter((id): id is number => id !== undefined);

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        let whereClause = `al.ts >= ${area.start} AND al.ts <= ${area.end}`;
        if (utids.length > 0) {
          whereClause += ` AND al.utid IN (${utids.join(', ')})`;
        } else if (machineIds.length > 0) {
          whereClause += ` AND t.machine_id IN (${machineIds.join(', ')})`;
        }

        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT
              al.id,
              al.ts,
              al.prio,
              al.tag,
              al.msg,
              t.tid,
              t.name AS thread_name,
              p.pid,
              p.name AS process_name
            FROM android_logs al
            LEFT JOIN thread t ON al.utid = t.utid
            LEFT JOIN process p ON t.upid = p.upid
            WHERE ${whereClause}
          `,
        });

        return createAggregationData(table);
      },
    };
  }

  getTabName() {
    return 'Android Logs';
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        id: {
          title: 'ID',
          columnType: 'identifier',
          cellRenderer: (value: unknown) => {
            const id = typeof value === 'bigint' ? Number(value) : value;
            if (typeof id !== 'number') return String(value);
            return m(
              Anchor,
              {
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectSqlEvent('android_logs', id, {
                    scrollToSelection: true,
                  });
                },
              },
              String(id),
            );
          },
        },
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
        prio: {title: 'Priority', columnType: 'quantitative'},
        tag: {title: 'Tag', columnType: 'text'},
        msg: {title: 'Message', columnType: 'text'},
        tid: {title: 'TID', columnType: 'quantitative'},
        thread_name: {title: 'Thread', columnType: 'text'},
        pid: {title: 'PID', columnType: 'quantitative'},
        process_name: {title: 'Process', columnType: 'text'},
      },
      initialColumns: [
        {id: 'id', field: 'id'},
        {id: 'ts', field: 'ts'},
        {id: 'prio', field: 'prio'},
        {id: 'tag', field: 'tag'},
        {id: 'msg', field: 'msg'},
        {id: 'process_name', field: 'process_name'},
        {id: 'thread_name', field: 'thread_name'},
      ],
      initialPivot: {
        groupBy: [
          {id: 'tag', field: 'tag'},
          {id: 'prio', field: 'prio'},
        ],
        aggregates: [{id: 'count', function: 'COUNT'}],
      },
    };
  }
}
