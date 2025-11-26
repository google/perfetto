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

export class CpuProfileSampleFlamegraphDetailsPanel
  implements TrackEventDetailsPanel
{
  private flamegraph: QueryFlamegraph;

  // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
  // We moved serialization from being attached to selections to instead being
  // attached to the plugin that loaded the panel.
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
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file || ':' || line_number as source_location,
            self_count
          from _callstacks_for_callsites!((
            select p.callsite_id
            from cpu_profile_stack_sample p
            where p.ts = ${this.ts} and p.utid = ${this.utid}
          ))
        )
      `,
      [
        {
          name: 'CPU Profile Samples',
          unit: '',
          columnName: 'self_count',
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
    // If the state in the serialization is not undefined, we should read from
    // it.
    // TODO(lalitm): remove this in 26Q2 - see comment on `serialization`.
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
          title: 'CPU Profile Samples',
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
