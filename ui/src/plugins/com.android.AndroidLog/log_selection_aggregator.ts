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
import {
  type Aggregation,
  type Aggregator,
  type AggregatorGridConfig,
  createAggregationData,
} from '../../components/aggregation_adapter';
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

    return {
      prepareData: async (engine: Engine) => {
        let whereClause = `al.ts >= ${area.start} AND al.ts <= ${area.end}`;
        if (utids.length > 0) {
          whereClause += ` AND al.utid IN (${utids.join(', ')})`;
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

  getGridConfig(): AggregatorGridConfig {
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
        ts: {title: 'Timestamp', columnType: 'quantitative'},
        prio: {title: 'Priority', columnType: 'quantitative'},
        tag: {title: 'Tag', columnType: 'text'},
        msg: {title: 'Message', columnType: 'text'},
        tid: {title: 'TID', columnType: 'quantitative'},
        thread_name: {title: 'Thread', columnType: 'text'},
        pid: {title: 'PID', columnType: 'quantitative'},
        process_name: {title: 'Process', columnType: 'text'},
      },
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
