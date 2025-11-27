// Copyright (C) 2025 The Android Open Source Project
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

// TODO(stevegolton): Dedupe this file with perf_samples_profile_track.ts

export function createProcessInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  upid: number,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
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
          callsite_id as callsiteId,
          upid
        FROM instruments_sample p
        JOIN thread using (utid)
        WHERE callsite_id IS NOT NULL
        ORDER BY ts
      `,
      filter: {
        col: 'upid',
        eq: upid,
      },
    }),
    sliceName: () => 'Instruments Sample',
    colorizer: (row) => getColorForSample(row.callsiteId),
    detailsPanel: (row) => {
      // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
      // We moved serialization from being attached to selections to instead being
      // attached to the plugin that loaded the panel.
      const serialization = {
        schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
        state: undefined as FlamegraphState | undefined,
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
                select p.callsite_id
                from instruments_sample p
                join thread t using (utid)
                where p.ts >= ${row.ts}
                  and p.ts <= ${row.ts}
                  and t.upid = ${upid}
              ))
            )
          `,
          [
            {
              name: 'Instruments Samples',
              unit: '',
              columnName: 'self_count',
            },
          ],
          'include perfetto module appleos.instruments.samples',
          [{name: 'mapping_name', displayName: 'Mapping'}],
          [
            {
              name: 'source_location',
              displayName: 'Source Location',
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

export function createThreadInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  utid: number,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
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
          callsite_id as callsiteId,
          utid
        FROM instruments_sample p
        WHERE callsite_id IS NOT NULL
        ORDER BY ts
      `,
      filter: {
        col: 'utid',
        eq: utid,
      },
    }),
    sliceName: () => 'Instruments Sample',
    colorizer: (row) => getColorForSample(row.callsiteId),
    detailsPanel: (row) => {
      // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
      // We moved serialization from being attached to selections to instead being
      // attached to the plugin that loaded the panel.
      const serialization = {
        schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
        state: undefined as FlamegraphState | undefined,
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
                select p.callsite_id
                from instruments_sample p
                where p.ts >= ${row.ts}
                  and p.ts <= ${row.ts}
                  and p.utid = ${utid}
              ))
            )
          `,
          [
            {
              name: 'Instruments Samples',
              unit: '',
              columnName: 'self_count',
            },
          ],
          'include perfetto module appleos.instruments.samples',
          [{name: 'mapping_name', displayName: 'Mapping'}],
          [
            {
              name: 'source_location',
              displayName: 'Source Location',
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
        title: 'Instruments Samples',
        buttons: m(Stack, {orientation: 'horizontal', spacing: 'large'}, [
          m('span', [
            `First timestamp: `,
            m(Timestamp, {
              trace,
              ts,
            }),
          ]),
          m('span', [
            `Last timestamp: `,
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
