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
  AggregatePivotModel,
  Aggregation,
  Aggregator,
} from '../../components/aggregation_adapter';
import {AreaSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {ANDROID_LOGS_TRACK_KIND} from '../../public/track_kinds';
import {Engine} from '../../trace_processor/engine';
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

        await engine.query(`
          CREATE OR REPLACE PERFETTO TABLE ${this.id} AS
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
        `);

        return {tableName: this.id};
      },
    };
  }

  getTabName() {
    return 'Android Logs';
  }

  getColumnDefinitions(): AggregatePivotModel {
    return {
      groupBy: [
        {id: 'tag', field: 'tag'},
        {id: 'prio', field: 'prio'},
      ],
      aggregates: [{id: 'count', function: 'COUNT'}],
      columns: [
        {
          title: 'ID',
          columnId: 'id',
          formatHint: 'NUMERIC',
          cellRenderer: (value) => {
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
        {title: 'Timestamp', columnId: 'ts', formatHint: 'TIMESTAMP_NS'},
        {title: 'Priority', columnId: 'prio', formatHint: 'NUMERIC'},
        {title: 'Tag', columnId: 'tag', formatHint: 'STRING'},
        {title: 'Message', columnId: 'msg', formatHint: 'STRING'},
        {title: 'TID', columnId: 'tid', formatHint: 'NUMERIC'},
        {title: 'Thread', columnId: 'thread_name', formatHint: 'STRING'},
        {title: 'PID', columnId: 'pid', formatHint: 'NUMERIC'},
        {title: 'Process', columnId: 'process_name', formatHint: 'STRING'},
      ],
    };
  }
}
