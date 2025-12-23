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
import {time} from '../../base/time';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {Timestamp} from '../../components/widgets/timestamp';
import {
  TrackEventDetailsPanel,
  TrackEventDetailsPanelSerializeArgs,
} from '../../public/details_panel';
import {DetailsShell} from '../../widgets/details_shell';
import {Trace} from '../../public/trace';
import {
  Flamegraph,
  FlamegraphState,
  FLAMEGRAPH_STATE_SCHEMA,
} from '../../widgets/flamegraph';

export class HeapProfileSampleFlamegraphDetailsPanel
  implements TrackEventDetailsPanel
{
  private flamegraph: QueryFlamegraph;

  readonly serialization: TrackEventDetailsPanelSerializeArgs<
    FlamegraphState | undefined
  > = {
    schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
    state: undefined,
  };

  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;

  constructor(
    private readonly trace: Trace,
    private readonly ts: time,
    private readonly utid: number,
    private state: FlamegraphState | undefined,
    private readonly onStateChange: (state: FlamegraphState) => void,
  ) {
    this.flamegraph = new QueryFlamegraph(trace);
    this.metrics = metricsFromTableOrSubquery(
      `
        (
          WITH profile_samples AS MATERIALIZED (
            SELECT callsite_id, size as sample_size
            FROM heap_profile_sample
            WHERE ts = ${this.ts} AND utid = ${this.utid}
          )
          SELECT
            c.id,
            c.parent_id as parentId,
            c.name,
            c.mapping_name,
            c.source_file || ':' || c.line_number as source_location,
            CASE WHEN c.is_leaf_function_in_callsite_frame
              THEN coalesce(m.sample_size, 0)
              ELSE 0
            END AS self_size
          FROM _callstacks_for_stack_profile_samples!(profile_samples) AS c
          LEFT JOIN profile_samples AS m USING (callsite_id)
        )
      `,
      [
        {
          name: 'Heap Allocation Size',
          unit: 'B',
          columnName: 'self_size',
        },
      ],
      'include perfetto module callstacks.stack_profile',
      [{name: 'mapping_name', displayName: 'Mapping'}],
      [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
    );
    if (!this.state) {
      this.state = Flamegraph.createDefaultState(this.metrics);
      onStateChange(this.state);
    }
  }

  async load() {
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
      '.pf-flamegraph-profile',
      m(
        DetailsShell,
        {
          fillHeight: true,
          title: 'Heap Profile Sample',
          buttons: m(
            'span',
            'Timestamp: ',
            m(Timestamp, {trace: this.trace, ts: this.ts}),
          ),
        },
        this.flamegraph.render({
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
