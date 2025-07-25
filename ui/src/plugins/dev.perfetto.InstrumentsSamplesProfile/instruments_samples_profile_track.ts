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
} from '../../components/query_flamegraph';
import {DetailsShell} from '../../widgets/details_shell';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time, time} from '../../base/time';
import {Flamegraph, FLAMEGRAPH_STATE_SCHEMA} from '../../widgets/flamegraph';
import {Trace} from '../../public/trace';
import {DatasetSliceTrack} from '../../components/tracks/dataset_slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {Stack} from '../../widgets/stack';

// TODO(stevegolton): Dedupe this file with perf_samples_profile_track.ts

export function createProcessInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  upid: number,
) {
  return new DatasetSliceTrack({
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
      const serialization = {
        schema: FLAMEGRAPH_STATE_SCHEMA,
        state: Flamegraph.createDefaultState(metrics),
      };
      const flamegraph = new QueryFlamegraph(trace, metrics, serialization);
      return {
        render: () => renderDetailsPanel(flamegraph, Time.fromRaw(row.ts)),
        serialization,
      };
    },
  });
}

export function createThreadInstrumentsSamplesProfileTrack(
  trace: Trace,
  uri: string,
  utid: number,
) {
  return new DatasetSliceTrack({
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
      const serialization = {
        schema: FLAMEGRAPH_STATE_SCHEMA,
        state: Flamegraph.createDefaultState(metrics),
      };
      const flamegraph = new QueryFlamegraph(trace, metrics, serialization);
      return {
        render: () => renderDetailsPanel(flamegraph, Time.fromRaw(row.ts)),
        serialization,
      };
    },
  });
}

function renderDetailsPanel(flamegraph: QueryFlamegraph, ts: time) {
  return m(
    '.flamegraph-profile',
    m(
      DetailsShell,
      {
        fillParent: true,
        title: 'Instruments Samples',
        buttons: m(Stack, {orientation: 'horizontal', spacing: 'large'}, [
          m('span', [
            `First timestamp: `,
            m(Timestamp, {
              ts,
            }),
          ]),
          m('span', [
            `Last timestamp: `,
            m(Timestamp, {
              ts,
            }),
          ]),
        ]),
      },
      flamegraph.render(),
    ),
  );
}
