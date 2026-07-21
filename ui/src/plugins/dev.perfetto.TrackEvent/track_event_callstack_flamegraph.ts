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
import {QuerySlot} from '../../base/query_slot';
import {sqliteString} from '../../base/string_utils';
import {FlamegraphPanel} from '../../components/flamegraph_panel';
import {
  metricsFromTableOrSubquery,
  type QueryFlamegraphMetric,
} from '../../components/query_flamegraph';
import type {AreaSelection, AreaSelectionTab} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {NUM, STR_NULL} from '../../trace_processor/query_result';
import {Flamegraph, type FlamegraphState} from '../../widgets/flamegraph';

const ARG_METRIC_PREFIX = 'arg:';

interface Metadata {
  readonly hasWeight: boolean;
  readonly availableArgs: ReadonlyArray<string>;
}

interface MetricsCache {
  readonly key: string;
  readonly metrics: ReadonlyArray<QueryFlamegraphMetric>;
}

export class TrackEventCallstackFlamegraphTab implements AreaSelectionTab {
  readonly id = 'track_event_callstack_flamegraph';
  readonly name = 'Track Event Callstacks';

  private readonly metadataSlot = new QuerySlot<Metadata>();
  private metricsCache?: MetricsCache;

  constructor(
    private readonly trace: Trace,
    private readonly getState: () => FlamegraphState | undefined,
    private readonly setState: (state: FlamegraphState) => void,
  ) {}

  render(selection: AreaSelection) {
    const trackIds = selection.tracks
      .filter((track) => track.tags?.hasCallstacks === true)
      .flatMap((track) => track.tags?.trackIds ?? []);
    if (trackIds.length === 0) return undefined;

    const samplesSql = buildSamplesSql(selection, trackIds);
    const metadata = this.metadataSlot.use({
      key: {start: selection.start, end: selection.end, trackIds},
      queryFn: () => this.queryMetadata(samplesSql),
    });
    if (metadata.data === undefined) {
      return {isLoading: metadata.isPending, content: undefined};
    }

    const state = this.getState();
    const addedMetricIds = state?.addedMetricIds ?? [];
    const metrics = this.getMetrics(
      samplesSql,
      metadata.data.hasWeight,
      addedMetricIds,
    );
    const currentState = Flamegraph.updateState(state, metrics);
    const added = new Set(addedMetricIds);
    const addableMetrics = metadata.data.availableArgs
      .map((name) => ({id: argMetricId(name), name}))
      .filter((metric) => !added.has(metric.id));

    return {
      isLoading: metadata.isPending,
      content: m(FlamegraphPanel, {
        trace: this.trace,
        metrics,
        addableMetrics,
        state: currentState,
        onAddMetric: (metric) => {
          this.setState({
            ...currentState,
            selectedMetricId: metric.id,
            addedMetricIds: [...currentState.addedMetricIds, metric.id],
          });
        },
        onStateChange: this.setState,
      }),
    };
  }

  private async queryMetadata(samplesSql: string): Promise<Metadata> {
    await this.trace.engine.query(
      'include perfetto module intervals.intersect;',
    );
    const result = await this.trace.engine.query(`
      WITH
        samples AS (${samplesSql}),
        summary AS (
          SELECT count(weight) > 0 AS has_weight
          FROM samples
        ),
        arg_keys AS (
          SELECT DISTINCT args.key
          FROM samples
          JOIN args USING (arg_set_id)
          WHERE args.value_type IN ('int', 'uint', 'real')
        )
      SELECT summary.has_weight, arg_keys.key
      FROM summary
      LEFT JOIN arg_keys ON true
      ORDER BY arg_keys.key
    `);
    let hasWeight = false;
    const availableArgs: string[] = [];
    const it = result.iter({has_weight: NUM, key: STR_NULL});
    for (; it.valid(); it.next()) {
      hasWeight = it.has_weight !== 0;
      if (it.key !== null) availableArgs.push(it.key);
    }
    return {hasWeight, availableArgs};
  }

  private getMetrics(
    samplesSql: string,
    hasWeight: boolean,
    addedMetricIds: ReadonlyArray<string>,
  ): ReadonlyArray<QueryFlamegraphMetric> {
    const key = `${samplesSql}\0${hasWeight}\0${addedMetricIds.join('\0')}`;
    if (this.metricsCache?.key === key) return this.metricsCache.metrics;

    const metrics = buildMetrics(samplesSql, hasWeight, addedMetricIds);
    this.metricsCache = {key, metrics};
    return metrics;
  }
}

function buildMetrics(
  samplesSql: string,
  hasWeight: boolean,
  addedMetricIds: ReadonlyArray<string>,
): ReadonlyArray<QueryFlamegraphMetric> {
  const dependencySql = `
    include perfetto module callstacks.stack_profile;
    include perfetto module intervals.intersect;
  `;
  const common = {
    dependencySql,
    unaggregatableProperties: [{name: 'mapping_name', displayName: 'Mapping'}],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY' as const,
      },
    ],
    nameColumnLabel: 'Symbol',
  };
  const callstackColumns = `
    id,
    parent_id AS parentId,
    name,
    mapping_name,
    source_file || ':' || line_number AS source_location
  `;
  const metrics: QueryFlamegraphMetric[] = [];

  if (hasWeight) {
    metrics.push(
      ...metricsFromTableOrSubquery({
        tableOrSubquery: `(
          SELECT ${callstackColumns}, self_value
          FROM _callstacks_for_callsites_weighted!((
            SELECT callsite_id, weight AS value
            FROM (${samplesSql})
            WHERE weight IS NOT NULL
          ))
        )`,
        tableMetrics: [
          {
            name: 'Weight',
            unit: '',
            columnName: 'self_value',
            provenance: 'DEFAULT',
          },
        ],
        ...common,
      }),
    );
  }

  metrics.push(
    ...metricsFromTableOrSubquery({
      tableOrSubquery: `(
        SELECT ${callstackColumns}, self_count
        FROM _callstacks_for_callsites!((
          SELECT callsite_id FROM (${samplesSql})
        ))
      )`,
      tableMetrics: [
        {
          name: 'Samples',
          unit: '',
          columnName: 'self_count',
          provenance: 'DEFAULT',
        },
      ],
      ...common,
    }),
  );

  for (const metricId of addedMetricIds) {
    const arg = argFromMetricId(metricId);
    if (arg === undefined) continue;
    metrics.push(
      ...metricsFromTableOrSubquery({
        tableOrSubquery: `(
          SELECT ${callstackColumns}, self_value
          FROM _callstacks_for_callsites_weighted!((
            SELECT
              callsite_id,
              coalesce(args.real_value, args.int_value) AS value
            FROM (${samplesSql}) samples
            JOIN args USING (arg_set_id)
            WHERE args.key = ${sqliteString(arg)}
              AND args.value_type IN ('int', 'uint', 'real')
          ))
        )`,
        tableMetrics: [
          {
            id: metricId,
            name: arg,
            unit: '',
            columnName: 'self_value',
          },
        ],
        ...common,
      }),
    );
  }
  return metrics;
}

function buildSamplesSql(
  selection: AreaSelection,
  trackIds: ReadonlyArray<number>,
): string {
  return `
    WITH relevant_slices AS (
      SELECT id
      FROM _interval_intersect_single!(
        ${selection.start},
        ${selection.end},
        (
          SELECT id, ts, max(dur, 0) AS dur
          FROM slice
          WHERE track_id IN (${trackIds.join()})
        )
      )
    )
    SELECT callsite_id, arg_set_id, weight
    FROM relevant_slices
    JOIN slice USING (id)
    JOIN __intrinsic_track_event_callstacks USING (slice_id)
    WHERE ts >= ${selection.start}
      AND ts <= ${selection.end}
      AND callsite_id IS NOT NULL
    UNION ALL
    SELECT end_callsite_id AS callsite_id, arg_set_id, weight
    FROM relevant_slices
    JOIN slice USING (id)
    JOIN __intrinsic_track_event_callstacks USING (slice_id)
    WHERE ts + dur >= ${selection.start}
      AND ts + dur <= ${selection.end}
      AND dur > 0
      AND end_callsite_id IS NOT NULL
  `;
}

function argMetricId(arg: string): string {
  return `${ARG_METRIC_PREFIX}${arg}`;
}

function argFromMetricId(id: string): string | undefined {
  return id.startsWith(ARG_METRIC_PREFIX)
    ? id.slice(ARG_METRIC_PREFIX.length)
    : undefined;
}
