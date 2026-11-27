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
  selectTracksAndGetDataset,
} from '../../components/aggregation_adapter';
import {Timestamp} from '../../components/widgets/timestamp';
import type {AreaSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';

export const LMK_TRACK_KIND = 'LmkTrack';

const LMK_SPEC = {
  id: NUM,
  ts: LONG,
  name: STR,
  pid: NUM_NULL,
  oom_bucket: STR,
  oom_score_adj: NUM,
  kill_reason: STR_NULL,
};

export class LmkSelectionAggregator implements Aggregator {
  readonly id = 'android_lmk_aggregation';

  constructor(
    private readonly trace: Trace,
    private readonly tableName: string,
  ) {}

  probe(area: AreaSelection): Aggregation | undefined {
    const dataset = selectTracksAndGetDataset(
      area.tracks,
      LMK_SPEC,
      LMK_TRACK_KIND,
    );
    if (dataset === undefined) return undefined;

    return {
      getGridConfig: () => this.getGridConfig(),
      prepareData: async (engine: Engine) => {
        const table = await createPerfettoTable({
          engine,
          as: `
            SELECT ts, id, name, pid, oom_bucket, oom_score_adj, kill_reason
            FROM (${dataset.query()})
            WHERE ts BETWEEN ${area.start} AND ${area.end}
          `,
        });
        return createAggregationData(table);
      },
    };
  }

  getTabName() {
    return 'LMK Events';
  }

  private getGridConfig(): AggregatorGridConfig {
    return {
      schema: {
        ts: {
          title: 'Timestamp',
          columnType: 'quantitative',
          cellRenderer: (value) => {
            if (typeof value !== 'bigint') return String(value);
            return m(Timestamp, {trace: this.trace, ts: Time.fromRaw(value)});
          },
        },
        id: {
          title: 'ID',
          columnType: 'identifier',
          cellRenderer: (value) => {
            if (typeof value !== 'bigint') return String(value);
            return m(
              Anchor,
              {
                title: 'Go to LMK event',
                icon: Icons.UpdateSelection,
                onclick: () => {
                  this.trace.selection.selectSqlEvent(
                    this.tableName,
                    Number(value),
                    {scrollToSelection: true},
                  );
                },
              },
              String(value),
            );
          },
        },
        name: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'identifier'},
        oom_bucket: {title: 'OOM Bucket', columnType: 'text'},
        oom_score_adj: {title: 'OOM Score Adj', columnType: 'quantitative'},
        kill_reason: {title: 'Kill Reason', columnType: 'text'},
      },
      initialColumns: [
        {id: 'ts', field: 'ts', sort: 'ASC'},
        {id: 'id', field: 'id'},
        {id: 'name', field: 'name'},
        {id: 'pid', field: 'pid'},
        {id: 'oom_bucket', field: 'oom_bucket'},
        {id: 'oom_score_adj', field: 'oom_score_adj'},
        {id: 'kill_reason', field: 'kill_reason'},
      ],
    };
  }
}
