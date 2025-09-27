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
import {Trace} from '../../public/trace';
import {Monitor} from '../../base/monitor';
import {NUM, STR} from '../../trace_processor/query_result';
import {Select} from '../../widgets/select';
import {Section} from '../../widgets/section';
import {FormLabel} from '../../widgets/form';
import {
  QueryFlamegraph,
  metricsFromTableOrSubquery,
} from '../../components/query_flamegraph';
import {
  Flamegraph,
  FLAMEGRAPH_STATE_SCHEMA,
  FlamegraphState,
} from '../../widgets/flamegraph';

export interface PprofPageState {
  selectedScope: string;
  selectedMetric: string;
  availableScopes: string[];
  availableMetrics: Array<{type: string; unit: string}>;
}

export interface PprofPageAttrs {
  readonly trace: Trace;
  readonly state: PprofPageState;
  readonly onStateUpdate: (
    update: PprofPageState | ((currentState: PprofPageState) => PprofPageState),
  ) => void;
}

interface SerializationState {
  schema: typeof FLAMEGRAPH_STATE_SCHEMA;
  state: FlamegraphState;
}

export class PprofPage implements m.ClassComponent<PprofPageAttrs> {
  private currentTrace?: Trace;
  private readonly monitor = new Monitor([() => this.currentTrace]);
  private flamegraph?: QueryFlamegraph;
  private serialization?: SerializationState;
  private loading = false;

  async loadAvailableData(
    trace: Trace,
    state: PprofPageState,
    onStateUpdate: (
      update:
        | PprofPageState
        | ((currentState: PprofPageState) => PprofPageState),
    ) => void,
  ): Promise<void> {
    // Load available scopes
    const scopesResult = await trace.engine.query(`
      SELECT DISTINCT scope FROM __intrinsic_aggregate_profile ORDER BY scope
    `);

    const scopes: string[] = [];
    const it = scopesResult.iter({scope: STR});
    for (; it.valid(); it.next()) {
      scopes.push(it.scope);
    }

    // If we have scopes but no selected scope, select the first one
    let selectedScope = state.selectedScope;
    if (
      scopes.length > 0 &&
      (selectedScope === undefined || selectedScope === '')
    ) {
      selectedScope = scopes[0];
    }

    // Load available metrics for the selected scope
    const metrics: Array<{type: string; unit: string}> = [];
    let selectedMetric = state.selectedMetric;

    if (selectedScope) {
      const metricsResult = await trace.engine.query(`
        SELECT sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        WHERE scope = '${selectedScope}'
        ORDER BY sample_type_type
      `);

      const it = metricsResult.iter({
        sample_type_type: STR,
        sample_type_unit: STR,
      });
      for (; it.valid(); it.next()) {
        metrics.push({
          type: it.sample_type_type,
          unit: it.sample_type_unit,
        });
      }

      // If we have metrics but no selected metric, select the first one
      if (
        metrics.length > 0 &&
        (selectedMetric === undefined || selectedMetric === '')
      ) {
        selectedMetric = metrics[0].type;
      }
    }

    onStateUpdate({
      selectedScope,
      selectedMetric,
      availableScopes: scopes,
      availableMetrics: metrics,
    });

    // Update flamegraph when scope/metric changes
    await this.updateFlamegraph(trace, selectedScope, selectedMetric);
  }

  private async updateFlamegraph(trace: Trace, scope: string, metric: string) {
    if (!scope || !metric) {
      this.flamegraph = undefined;
      this.serialization = undefined;
      return;
    }

    // Check if there are any samples for this scope/metric
    const sampleCheckResult = await trace.engine.query(`
      SELECT COUNT(*) as count
      FROM __intrinsic_aggregate_sample sample
      JOIN __intrinsic_aggregate_profile profile ON sample.aggregate_profile_id = profile.id
      WHERE profile.scope = '${scope}'
        AND profile.sample_type_type = '${metric}'
    `);

    const sampleCount = sampleCheckResult.firstRow({count: NUM}).count;
    if (sampleCount === 0) {
      this.flamegraph = undefined;
      this.serialization = undefined;
      return;
    }
    const flamegraphMetrics = metricsFromTableOrSubquery(
      `
        (
          WITH
            metrics AS MATERIALIZED (
              SELECT
                callsite_id,
                sum(sample.value) AS self_value
              FROM __intrinsic_aggregate_sample sample
              JOIN __intrinsic_aggregate_profile profile
                ON sample.aggregate_profile_id = profile.id
              WHERE profile.scope = '${scope}'
                AND profile.sample_type_type = '${metric}'
              GROUP BY callsite_id
            )
          select
            c.id,
            c.parent_id as parentId,
            c.name,
            c.mapping_name,
            c.source_file || ':' || c.line_number as source_location,
            iif(c.is_leaf_function_in_callsite_frame, coalesce(m.self_value, 0), 0) AS self_value
          FROM _callstacks_for_stack_profile_samples!(metrics) AS c
          LEFT JOIN metrics AS m
            USING (callsite_id)
        )
      `,
      [
        {
          name: 'Pprof Samples',
          unit: metric === 'cpu' ? 'ns' : 'count',
          columnName: 'self_value',
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

    this.serialization = {
      schema: FLAMEGRAPH_STATE_SCHEMA,
      state: Flamegraph.createDefaultState(flamegraphMetrics),
    };

    this.flamegraph = new QueryFlamegraph(
      trace,
      flamegraphMetrics,
      this.serialization,
    );
  }

  private handleScopeChange(attrs: PprofPageAttrs, newScope: string) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedScope: newScope,
      selectedMetric: '', // Reset metric when scope changes
    }));
  }

  private handleMetricChange(attrs: PprofPageAttrs, newMetric: string) {
    attrs.onStateUpdate((currentState) => ({
      ...currentState,
      selectedMetric: newMetric,
    }));
  }

  view({attrs}: m.CVnode<PprofPageAttrs>) {
    const {state} = attrs;

    this.currentTrace = attrs.trace;
    if (
      this.monitor.ifStateChanged() &&
      !this.loading &&
      state.availableScopes.length === 0
    ) {
      this.loading = true;
      this.loadAvailableData(
        attrs.trace,
        attrs.state,
        attrs.onStateUpdate,
      ).finally(() => {
        this.loading = false;
      });
    }

    const scopeOptions = state.availableScopes.map((scope) => ({
      value: scope,
      label: scope,
    }));

    const metricOptions = state.availableMetrics.map((metric) => ({
      value: metric.type,
      label: `${metric.type} (${metric.unit})`,
    }));

    return m(
      '.pf-pprof-page',
      m(
        Section,
        {
          title: 'Pprof Profiles',
        },
        [
          m(
            '.pf-pprof-page__controls',
            m(
              '.pf-pprof-page__control-group',
              m(FormLabel, {}, [
                'Scope:',
                m(
                  Select,
                  {
                    oninput: (e: Event) => {
                      const target = e.target as HTMLSelectElement;
                      this.handleScopeChange(attrs, target.value);
                    },
                  },
                  scopeOptions.map((option) =>
                    m(
                      'option',
                      {
                        value: option.value,
                        selected: state.selectedScope === option.value,
                      },
                      option.label,
                    ),
                  ),
                ),
              ]),
            ),
            state.availableMetrics.length > 0 &&
              m(
                '.pf-pprof-page__control-group',
                m(FormLabel, {}, [
                  'Metric:',
                  m(
                    Select,
                    {
                      oninput: (e: Event) => {
                        const target = e.target as HTMLSelectElement;
                        this.handleMetricChange(attrs, target.value);
                      },
                    },
                    metricOptions.map((option) =>
                      m(
                        'option',
                        {
                          value: option.value,
                          selected: state.selectedMetric === option.value,
                        },
                        option.label,
                      ),
                    ),
                  ),
                ]),
              ),
          ),
          this.flamegraph &&
            m('.pf-pprof-page__flamegraph', this.flamegraph.render()),
          !this.flamegraph &&
            state.selectedScope &&
            state.selectedMetric &&
            m(
              '.pf-pprof-page__empty-state',
              'No sample data available for the selected scope and metric. This may be a trace with profiling metadata but no actual samples.',
            ),
          !state.selectedScope &&
            state.availableScopes.length === 0 &&
            m(
              '.pf-pprof-page__empty-state',
              'No pprof data available in this trace.',
            ),
        ],
      ),
    );
  }
}
