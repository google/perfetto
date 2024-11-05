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
} from '../../public/lib/query_flamegraph';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {
  TrackEventDetailsPanel,
  TrackEventDetailsPanelSerializeArgs,
} from '../../public/details_panel';
import {DetailsShell} from '../../widgets/details_shell';
import {Trace} from '../../public/trace';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphState,
} from '../../widgets/flamegraph';

export class CpuProfileSampleFlamegraphDetailsPanel
  implements TrackEventDetailsPanel
{
  private readonly flamegraph: QueryFlamegraph;
  readonly serialization: TrackEventDetailsPanelSerializeArgs<FlamegraphState>;

  constructor(
    trace: Trace,
    private ts: time,
    utid: number,
  ) {
    const metrics = metricsFromTableOrSubquery(
      `
        (
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file,
            cast(line_number AS text) as line_number,
            self_count
          from _callstacks_for_callsites!((
            select p.callsite_id
            from cpu_profile_stack_sample p
            where p.ts = ${ts} and p.utid = ${utid}
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
          name: 'source_file',
          displayName: 'Source File',
          mergeAggregation: 'ONE_OR_NULL',
        },
        {
          name: 'line_number',
          displayName: 'Line Number',
          mergeAggregation: 'ONE_OR_NULL',
        },
      ],
    );
    this.serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(metrics),
    };
    this.flamegraph = new QueryFlamegraph(trace, metrics, this.serialization);
  }

  render() {
    return m(
      '.flamegraph-profile',
      m(
        DetailsShell,
        {
          fillParent: true,
          title: m('.title', 'CPU Profile Samples'),
          description: [],
          buttons: [m('div.time', `Timestamp: `, m(Timestamp, {ts: this.ts}))],
        },
        this.flamegraph.render(),
      ),
    );
  }
}
