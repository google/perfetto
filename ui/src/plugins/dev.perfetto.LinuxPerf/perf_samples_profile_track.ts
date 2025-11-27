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
import {LONG, NUM} from '../../trace_processor/query_result';
import {getColorForSample} from '../../components/colorizer';
import {
  metricsFromTableOrSubquery,
  QueryFlamegraph,
  QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time, time} from '../../base/time';
import {
  Flamegraph,
  FlamegraphState,
  FLAMEGRAPH_STATE_SCHEMA,
} from '../../widgets/flamegraph';
import {Trace} from '../../public/trace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {Stack} from '../../widgets/stack';
import {TrackEventDetailsPanelSerializeArgs} from '../../public/details_panel';

// TODO(stevegolton): Dedupe this file with instruments_samples_profile_track.ts

export function createPerfCallsitesTrack(
  trace: Trace,
  uri: string,
  upid: number | undefined,
  utid: number | undefined,
  sessionId: number | undefined,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  const constraints = [];
  if (upid !== undefined) {
    constraints.push(`(upid = ${upid})`);
  }
  if (utid !== undefined) {
    constraints.push(`(utid = ${utid})`);
  }
  if (sessionId !== undefined) {
    constraints.push(`(perf_session_id = ${sessionId})`);
  }
  const trackConstraints = constraints.join(' AND ');

  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        callsiteId: NUM,
      },
      src: `
       SELECT
          p.id,
          ts,
          callsite_id AS callsiteId,
          upid
        FROM perf_sample AS p
        JOIN thread USING (utid)
        WHERE callsiteId IS NOT NULL
          AND ${trackConstraints}
        ORDER BY ts
      `,
    }),
    sliceName: () => 'Perf sample',
    colorizer: (row) => getColorForSample(row.callsiteId),
    detailsPanel: (row) => {
      // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
      // We moved serialization from being attached to selections to instead being
      // attached to the plugin that loaded the panel.
      const serialization: TrackEventDetailsPanelSerializeArgs<
        FlamegraphState | undefined
      > = {
        schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
        state: undefined,
      };
      const flamegraph = new QueryFlamegraph(trace);
      const metrics: ReadonlyArray<QueryFlamegraphMetric> =
        metricsFromTableOrSubquery(
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
                select ps.callsite_id
                from perf_sample ps
                join thread t using (utid)
                where ps.ts = ${row.ts}
                  and ${trackConstraints}
              ))
            )
          `,
          [
            {
              name: 'count',
              unit: '',
              columnName: 'self_count',
            },
          ],
          'include perfetto module linux.perf.samples',
          [{name: 'mapping_name', displayName: 'Mapping'}],
          [
            {
              name: 'source_location',
              displayName: 'Source location',
              mergeAggregation: 'ONE_OR_SUMMARY',
            },
          ],
        );
      let state = detailsPanelState ?? Flamegraph.createDefaultState(metrics);

      return {
        load: async () => {
          // If the state in the serialization is not undefined, we should read from
          // it.
          // TODO(lalitm): remove this in 26Q2 - see comment on `serialization`.
          if (serialization.state !== undefined) {
            state = Flamegraph.updateState(serialization.state, metrics);
            onDetailsPanelStateChange(state);
            serialization.state = undefined;
          }
        },
        render: () =>
          renderDetailsPanel(
            trace,
            flamegraph,
            metrics,
            Time.fromRaw(row.ts),
            state,
            (newState) => {
              state = newState;
              onDetailsPanelStateChange(newState);
            },
          ),
        serialization,
      };
    },
  });
}

function renderDetailsPanel(
  trace: Trace,
  flamegraph: QueryFlamegraph,
  metrics: ReadonlyArray<QueryFlamegraphMetric>,
  ts: time,
  state: FlamegraphState | undefined,
  onStateChange: (state: FlamegraphState) => void,
) {
  return m(
    '.pf-flamegraph-profile',
    m(
      DetailsShell,
      {
        fillHeight: true,
        title: 'Perf sample',
        buttons: m(Stack, {orientation: 'horizontal', spacing: 'large'}, [
          m('span', [
            `Timestamp: `,
            m(Timestamp, {
              trace,
              ts,
            }),
          ]),
        ]),
      },
      flamegraph.render({metrics, state, onStateChange}),
    ),
  );
}
