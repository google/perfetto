// Copyright (C) 2024 The Android Open Source Project
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

import type { time } from '../../base/time';
import {
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import { FlamegraphPanel } from '../../components/flamegraph_panel';
import { FlamegraphProfile } from '../../components/flamegraph_profile';
import { Timestamp } from '../../components/widgets/timestamp';
import type {
  TrackEventDetailsPanel,
  TrackEventDetailsPanelSerializeArgs,
} from '../../public/details_panel';
import type { Trace } from '../../public/trace';
import { STR } from '../../trace_processor/query_result';
import { DetailsShell } from '../../widgets/details_shell';
import {
  Flamegraph,
  type FlamegraphState,
  FLAMEGRAPH_STATE_SCHEMA,
} from '../../widgets/flamegraph';
import { Stack } from '../../widgets/stack';

export class OomCallstackDetailsPanel implements TrackEventDetailsPanel {
  private oomErrorMsg?: string;

  readonly serialization: TrackEventDetailsPanelSerializeArgs<
    FlamegraphState | undefined
  > = {
      schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
      state: undefined,
    };

  // Defines the queries used to fetch data for the flamegraph.
  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;

  constructor(
    private readonly trace: Trace,
    private readonly upid: number,
    private readonly ts: time,
    private state: FlamegraphState | undefined,
    private readonly onStateChange: (state: FlamegraphState) => void,
  ) {
    this.metrics = [
      {
        name: 'OOM Callstack',
        unit: '',

        statement: `
          WITH RECURSIVE callstack AS (
            SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
            FROM stack_profile_callsite c
            WHERE c.id = (
              SELECT callsite_id
              FROM heap_graph_thread_callsite hc
              JOIN heap_graph g ON hc.heap_graph_id = g.id
              WHERE g.ts = ${ts}
              LIMIT 1
            )

            UNION ALL

            SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
            FROM stack_profile_callsite c
            JOIN callstack ON c.id = callstack.parentId
          )
          SELECT
            cs.id,
            cs.parentId,
            ifnull(f.name, '[Unknown]') as name,
            -- Assign 1 to the deepest node to draw a single-width stack.
            iif(cs.depth = (select max(depth) from callstack), 1, 0) as value,
            s.source_file || ':' || cast(s.line_number as text) as source_location
          FROM callstack cs
          JOIN stack_profile_frame f ON cs.frame_id = f.id
          LEFT JOIN stack_profile_symbol s ON f.symbol_set_id = s.symbol_set_id
        `,
        unaggregatableProperties: [],
        // Defines the properties that can be aggregated and displayed when clicking a node.
        aggregatableProperties: [
          {
            name: 'source_location',
            displayName: 'Source Location',
            mergeAggregation: 'ONE_OR_SUMMARY',
          },
        ],
      },
    ];

    if (this.state === undefined) {
      this.state = Flamegraph.createDefaultState(this.metrics);
      onStateChange(this.state);
    }
  }

  async load() {
    const res = await this.trace.engine.query(`
      SELECT error_msg 
      FROM heap_graph g
      JOIN android_heap_graph_java_oome_details o ON o.heap_graph_id = g.id
      WHERE g.ts = ${this.ts} AND g.upid = ${this.upid}
    `);

    if (res.numRows() > 0) {
      this.oomErrorMsg = res.firstRow({ error_msg: STR }).error_msg;
    }

    if (this.serialization.state !== undefined) {
      this.state = Flamegraph.updateState(
        this.serialization.state,
        this.metrics,
      );
      this.onStateChange(this.state);
      this.serialization.state = undefined;
    }
  }

  render() {
    return m(
      FlamegraphProfile,
      undefined,
      m(
        DetailsShell,
        {
          fillHeight: true,
          title: m(
            Stack,
            { orientation: 'vertical' },
            m('span', 'OOM Callstack'),
            this.oomErrorMsg &&
            m(
              'span',
              { style: { fontSize: '12px', color: '#ff4081' } },
              this.oomErrorMsg,
            ),
          ),
          buttons: m(Stack, { orientation: 'horizontal', spacing: 'large' }, [
            m('span', `Snapshot time: `, m(Timestamp, { trace: this.trace, ts: this.ts })),
          ]),
        },
        m(FlamegraphPanel, {
          trace: this.trace,
          metrics: this.metrics,
          state: this.state,
          onStateChange: (state) => {
            this.state = state;
            this.onStateChange(state);
          },
        }),
      ),
    );
  }
}
