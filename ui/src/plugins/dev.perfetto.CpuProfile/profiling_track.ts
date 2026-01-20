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

/**
 * Configuration for creating a profiling track (CPU profile, perf samples, etc)
 */
export interface ProfilingTrackConfig {
  /**
   * The SourceDataset that provides the profiling samples.
   * Must have schema: {id: NUM, ts: LONG, callsiteId: NUM}
   */
  readonly dataset: SourceDataset<{
    id: number;
    ts: bigint;
    callsiteId: number;
  }>;

  /**
   * SQL query to get callsite_id values for the details panel.
   * This will be passed to _callstacks_for_callsites!()
   * Should return rows with a callsite_id column.
   *
   * Example: `select callsite_id from perf_sample where ts = ${ts}`
   *
   * Available template variables:
   * - ${ts}: The timestamp of the clicked sample
   */
  readonly callsiteQuery: (ts: bigint) => string;

  /**
   * Perfetto SQL module to include for _callstacks_for_callsites function.
   *
   * Examples:
   * - 'callstacks.stack_profile' for CPU profiles
   * - 'linux.perf.samples' for perf samples
   * - 'appleos.instruments.samples' for Instruments samples
   */
  readonly sqlModule: string;

  /**
   * Name to display for the metric in the flamegraph.
   * Examples: 'CPU Profile Samples', 'Perf Samples', 'Instruments Samples'
   */
  readonly metricName: string;

  /**
   * Title to display in the details panel.
   * Examples: 'CPU Profile Samples', 'Perf sample', 'Instruments Samples'
   */
  readonly panelTitle: string;

  /**
   * Name to display for each sample slice on the track.
   * Examples: 'CPU Sample', 'Perf sample', 'Instruments Sample'
   */
  readonly sliceName: string;
}

/**
 * Creates a profiling track (CPU profile, perf samples, Instruments samples, etc).
 *
 * This is a unified factory function that handles all types of profiling tracks.
 * The differences between track types are abstracted through the ProfilingTrackConfig.
 *
 * @param trace - The trace object
 * @param uri - Unique URI for this track
 * @param config - Configuration specific to the profiling track type
 * @param detailsPanelState - Current flamegraph state (for persistence)
 * @param onDetailsPanelStateChange - Callback when flamegraph state changes
 * @returns A configured SliceTrack instance
 */
export function createProfilingTrack(
  trace: Trace,
  uri: string,
  config: ProfilingTrackConfig,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: config.dataset,
    sliceName: () => config.sliceName,
    colorizer: (row) => getColorForSample(row.callsiteId),
    detailsPanel: (row) => {
      // Create flamegraph, metrics, and initial state once per panel, not on every render
      const flamegraph = new QueryFlamegraph(trace);
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
                ${config.callsiteQuery(ts)}
              ))
            )
          `,
          tableMetrics: [
            {
              name: config.metricName,
              unit: '',
              columnName: 'self_count',
            },
          ],
          dependencySql: `include perfetto module ${config.sqlModule}`,
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
      // Use provided state or create initial state once
      let state = detailsPanelState ?? Flamegraph.createDefaultState(metrics);
      return {
        load: async () => {},
        render: () =>
          renderProfilingDetailsPanel(
            trace,
            ts,
            config,
            state,
            (newState) => {
              state = newState;
              onDetailsPanelStateChange(newState);
            },
            flamegraph,
            metrics,
          ),
        // TODO(lalitm): we should be able remove this around the 26Q2 timeframe
        // We moved serialization from being attached to selections to instead being
        // attached to the plugin that loaded the panel.
        serialization: {
          schema: FLAMEGRAPH_STATE_SCHEMA.optional(),
          state: undefined as FlamegraphState | undefined,
        },
      };
    },
  });
}

/**
 * Internal function to render the details panel for a profiling sample.
 */
function renderProfilingDetailsPanel(
  trace: Trace,
  ts: time,
  config: ProfilingTrackConfig,
  state: FlamegraphState,
  onStateChange: (state: FlamegraphState) => void,
  flamegraph: QueryFlamegraph,
  metrics: ReadonlyArray<QueryFlamegraphMetric>,
): m.Children {
  return m(
    '.pf-flamegraph-profile',
    m(
      DetailsShell,
      {
        fillHeight: true,
        title: config.panelTitle,
        buttons: m('span', 'Timestamp: ', m(Timestamp, {trace, ts})),
      },
      flamegraph.render({
        metrics,
        state,
        onStateChange,
      }),
    ),
  );
}
