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

// This panel appears at the bottom of the screen when an OOM callstack event is selected.
// It fetches the OOM error message from the database and renders a flamegraph.
export class OomCallstackDetailsPanel implements TrackEventDetailsPanel {
  // Holds the text of the OutOfMemoryError if available in the trace.
  private oomErrorMsg?: string;

  // Defines how this panel's state is serialized for permanent permalinks.
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
    // Define the metrics array. A metric specifies the SQL used to compute the 
    // flamegraph nodes and their aggregations.
    this.metrics = [
      {
        name: 'OOM Callstack',
        unit: '', // No specific unit since we are mostly displaying the call stack structure.

        // This SQL statement reconstructs the Java call stack that caused the OOME.
        statement: `
          -- First, recursively fetch all frames starting from the leaf frame (where the OOM happened).
          WITH RECURSIVE callstack AS (
            SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
            FROM stack_profile_callsite c
            WHERE c.id = (
              -- Find the specific thread callsite that triggered the OOM event.
              SELECT callsite_id
              FROM heap_graph_thread_callsite hc
              JOIN heap_graph g ON hc.heap_graph_id = g.id
              WHERE g.ts = ${ts}
              LIMIT 1
            )

            UNION ALL

            -- Recursively walk up the parent references to build the full stack.
            SELECT c.id, c.parent_id as parentId, c.depth, c.frame_id
            FROM stack_profile_callsite c
            JOIN callstack ON c.id = callstack.parentId
          )
          -- Next, join the resolved callsites with their actual symbol names and source code locations.
          SELECT
            cs.id,
            cs.parentId,
            ifnull(f.name, '[Unknown]') as name,
            -- For a flamegraph, we usually assign a value (size/count) to the leaf nodes.
            -- Here we assign 1 to the deepest node to draw a single-width stack.
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

    // If there's no pre-existing state (e.g. from a permalink), create a default one.
    if (this.state === undefined) {
      this.state = Flamegraph.createDefaultState(this.metrics);
      onStateChange(this.state);
    }
  }

  // Called asynchronously when the panel is about to be shown.
  async load() {
    // Attempt to read the exact error message associated with this OOM.
    const res = await this.trace.engine.query(`
      SELECT error_msg 
      FROM heap_graph g
      JOIN android_heap_graph_java_oome_details o ON o.heap_graph_id = g.id
      WHERE g.ts = ${this.ts} AND g.upid = ${this.upid}
    `);

    // If found, store it so the render method can display it.
    if (res.numRows() > 0) {
      this.oomErrorMsg = res.firstRow({ error_msg: STR }).error_msg;
    }

    // Apply any serialized state if one was passed down.
    if (this.serialization.state !== undefined) {
      this.state = Flamegraph.updateState(
        this.serialization.state,
        this.metrics,
      );
      this.onStateChange(this.state);
      this.serialization.state = undefined;
    }
  }

  // The mithril view method that describes the UI structure of the panel.
  render() {
    return m(
      FlamegraphProfile,
      undefined, // We don't have any modal configuration here.
      m(
        // DetailsShell provides the standard Perfetto bottom panel layout.
        DetailsShell,
        {
          fillHeight: true,
          // The title area contains the "OOM Callstack" text and the OOM error message.
          title: m(
            Stack,
            { orientation: 'vertical' },
            m('span', 'OOM Callstack'),
            this.oomErrorMsg &&
            m(
              'span',
              // Highlight the error message in pink/red.
              { style: { fontSize: '12px', color: '#ff4081' } },
              this.oomErrorMsg,
            ),
          ),
          // Action buttons or extra info displayed in the top right of the panel.
          buttons: m(Stack, { orientation: 'horizontal', spacing: 'large' }, [
            m('span', `Snapshot time: `, m(Timestamp, { trace: this.trace, ts: this.ts })),
          ]),
        },
        // Render the actual interactive Flamegraph.
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
