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
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {
  type AreaSelection,
  type AreaSelectionTab,
  areaSelectionsEqual,
} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {Flamegraph, type FlamegraphState} from '../../widgets/flamegraph';

export class TrackEventCallstackFlamegraphTab implements AreaSelectionTab {
  readonly id = 'track_event_callstack_flamegraph';
  readonly name = 'Track Event Callstacks';

  private previousSelection?: AreaSelection;
  private metrics?: ReadonlyArray<QueryFlamegraphMetric>;

  constructor(
    private readonly trace: Trace,
    private readonly getState: () => FlamegraphState | undefined,
    private readonly setState: (state: FlamegraphState) => void,
  ) {}

  render(selection: AreaSelection) {
    if (
      this.previousSelection === undefined ||
      !areaSelectionsEqual(this.previousSelection, selection)
    ) {
      this.metrics = this.computeMetrics(selection);
      this.previousSelection = selection;
    }
    if (this.metrics === undefined) return undefined;

    return {
      isLoading: false,
      content: m(FlamegraphPanel, {
        trace: this.trace,
        metrics: this.metrics,
        state: this.getState(),
        onStateChange: this.setState,
      }),
    };
  }

  private computeMetrics(
    selection: AreaSelection,
  ): ReadonlyArray<QueryFlamegraphMetric> | undefined {
    const trackIds = selection.tracks
      .filter((track) => track.tags?.hasCallstacks === true)
      .flatMap((track) => track.tags?.trackIds ?? []);
    if (trackIds.length === 0) return undefined;

    const metrics = metricsFromTableOrSubquery({
      tableOrSubquery: `
        (
          with relevant_slices as (
            select id
            from _interval_intersect_single!(
              ${selection.start},
              ${selection.end},
              (
                select
                  id,
                  ts,
                  max(dur, 0) as dur
                from slice
                where track_id in (${trackIds.join()})
              )
            )
          )
          select
            id,
            parent_id as parentId,
            name,
            mapping_name,
            source_file || ':' || line_number as source_location,
            self_count
          from _callstacks_for_callsites!((
            select callsite_id
            from relevant_slices
            join slice using (id)
            join __intrinsic_track_event_callstacks using (slice_id)
            where ts >= ${selection.start}
              and ts <= ${selection.end}
              and callsite_id is not null
            union all
            select end_callsite_id as callsite_id
            from relevant_slices
            join slice using (id)
            join __intrinsic_track_event_callstacks using (slice_id)
            where ts + dur >= ${selection.start}
              and ts + dur <= ${selection.end}
              and dur > 0
              and end_callsite_id is not null
          ))
        )
      `,
      tableMetrics: [
        {
          name: 'Samples',
          unit: '',
          columnName: 'self_count',
        },
      ],
      dependencySql: `
        include perfetto module callstacks.stack_profile;
        include perfetto module intervals.intersect;
      `,
      unaggregatableProperties: [
        {name: 'mapping_name', displayName: 'Mapping'},
      ],
      aggregatableProperties: [
        {
          name: 'source_location',
          displayName: 'Source Location',
          mergeAggregation: 'ONE_OR_SUMMARY',
        },
      ],
      nameColumnLabel: 'Symbol',
    });
    this.setState(Flamegraph.updateState(this.getState(), metrics));
    return metrics;
  }
}
