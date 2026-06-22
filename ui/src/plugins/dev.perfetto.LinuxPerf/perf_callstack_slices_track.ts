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

import {LONG, NUM, STR} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {SliceTrack} from '../../components/tracks/slice_track';
import {getColorForSample} from '../../components/colorizer';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {FlamegraphProfile} from '../../components/flamegraph_profile';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time} from '../../base/time';
import {
  Flamegraph,
  type FlamegraphState,
  FLAMEGRAPH_STATE_SCHEMA,
} from '../../widgets/flamegraph';
import m from 'mithril';

export function createPerfCallstackSlicesTrack(
  trace: Trace,
  uri: string,
  tableName: string,
  trackConstraints: string,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  return SliceTrack.createMaterialized({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        depth: NUM,
        callsiteId: NUM,
      },
      src: tableName,
    }),
    sliceName: (row) => row.name,
    colorizer: (row) => getColorForSample(row.callsiteId),
    detailsPanel: (row) => {
      const ts = Time.fromRaw(row.ts);
      const metrics: ReadonlyArray<QueryFlamegraphMetric> =
        metricsFromTableOrSubquery({
          tableOrSubquery: `
            (
              select
                id,
                parent_id as parentId,
                name,
                mapping_name,
                source_file || ':' || line_number as source_location,
                self_count
              from _callstacks_for_callsites!((
                SELECT ps.callsite_id
                FROM perf_sample ps
                JOIN thread t USING (utid)
                WHERE ps.ts = ${ts}
                  AND ${trackConstraints}
              ))
            )
          `,
          tableMetrics: [
            {
              name: 'Perf Samples',
              unit: '',
              columnName: 'self_count',
            },
          ],
          dependencySql: `include perfetto module linux.perf.samples`,
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
      let state = detailsPanelState ?? Flamegraph.createDefaultState(metrics);
      if (detailsPanelState === undefined) {
        onDetailsPanelStateChange(state);
      }
      return {
        load: async () => {},
        render: () =>
          m(
            FlamegraphProfile,
            m(
              DetailsShell,
              {
                fillHeight: true,
                title: 'Perf sample',
                buttons: m('span', 'Timestamp: ', m(Timestamp, {trace, ts})),
              },
              m(FlamegraphPanel, {
                trace,
                metrics,
                state,
                onStateChange: (newState) => {
                  state = newState;
                  onDetailsPanelStateChange(newState);
                },
              }),
            ),
          ),
        serialization: {
          schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
          state: undefined as FlamegraphState | undefined,
        },
      };
    },
  });
}
