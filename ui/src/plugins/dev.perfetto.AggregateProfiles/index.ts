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

import './styles.scss';
import m from 'mithril';

import type {TreeExplorerQueryMetric} from '../../components/tree_explorer_fetcher';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import {AggregateProfilesPage} from './aggregate_profiles_page';
import {
  type AggregateProfile,
  type AggregateProfilesPageState,
  AGGREGATE_PROFILES_PAGE_STATE_SCHEMA,
} from './types';
import type {Store} from '../../base/store';
import {ensureExists} from '../../base/assert';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AggregateProfiles';
  private store?: Store<AggregateProfilesPageState>;

  private migratePageState(init: unknown): AggregateProfilesPageState {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse(init);
    return result.data ?? {};
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.store = trace.mountStore('dev.perfetto.AggregateProfiles', (init) =>
      this.migratePageState(init),
    );
    const {profiles, mergedMetrics} = await getProfiles(trace);
    if (profiles.length === 0) {
      return;
    }
    const store = ensureExists(this.store);
    trace.pages.registerPage({
      route: '/aggregateprofiles',
      render: () =>
        m(AggregateProfilesPage, {
          trace,
          state: store.state,
          onStateChange: (state: AggregateProfilesPageState) => {
            store.edit((draft) => {
              draft.flamegraphState = state.flamegraphState;
            });
          },
          profiles,
          mergedMetrics,
        }),
    });
    trace.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 11,
      text: 'Aggregate Profiles',
      href: '#!/aggregateprofiles',
      icon: 'analytics',
    });
    trace.onTraceReady.addListener(async () => {
      const hasAnyTracks = trace.workspaces.all[0].flatTracks.length > 0;
      // TODO(lalitm): it's really bad that we're unconditionally navigating
      // to the profiles page: really we should check if the user has not already
      // set a page and then only navigate if no page is set. However:
      //  a) no API exists for checking the current page
      //  b) there is already some code in UI load time which navigates
      //     to the viewer page so we would always fail this check.
      // So for now just leave this as-is.
      if (!hasAnyTracks && profiles.length > 0) {
        trace.navigate('#!/aggregateprofiles');
      }
    });
  }
}

// A sample type of one or more profiles, and the rows of
// __intrinsic_aggregate_profile holding its samples in them.
interface SampleType {
  readonly type: string;
  readonly unit: string;
  readonly aggregateProfileIds: number[];
}

// The profiles of the trace (one per scope, e.g. per file of a pprof
// archive) in scope order, and the metrics of their merge: one per sample
// type, summing the samples of all the profiles which have it.
async function getProfiles(trace: Trace): Promise<{
  readonly profiles: ReadonlyArray<AggregateProfile>;
  readonly mergedMetrics: ReadonlyArray<TreeExplorerQueryMetric>;
}> {
  const result = await trace.engine.query(`
    SELECT
      id,
      scope,
      sample_type_type AS type,
      sample_type_unit AS unit
    FROM __intrinsic_aggregate_profile
    ORDER BY scope, type, unit, id
  `);
  // Sample types by metric name, per scope and across all scopes.
  const sampleTypesByScope = new Map<string, Map<string, SampleType>>();
  const mergedSampleTypes = new Map<string, SampleType>();
  const addTo = (
    sampleTypes: Map<string, SampleType>,
    type: string,
    unit: string,
    id: number,
  ) => {
    const name = metricName(type, unit);
    let sampleType = sampleTypes.get(name);
    if (sampleType === undefined) {
      sampleType = {type, unit, aggregateProfileIds: []};
      sampleTypes.set(name, sampleType);
    }
    sampleType.aggregateProfileIds.push(id);
  };
  for (
    const it = result.iter({id: NUM, scope: STR, type: STR, unit: STR});
    it.valid();
    it.next()
  ) {
    let sampleTypes = sampleTypesByScope.get(it.scope);
    if (sampleTypes === undefined) {
      sampleTypes = new Map();
      sampleTypesByScope.set(it.scope, sampleTypes);
    }
    addTo(sampleTypes, it.type, it.unit, it.id);
    addTo(mergedSampleTypes, it.type, it.unit, it.id);
  }
  const profiles = Array.from(sampleTypesByScope, ([scope, sampleTypes]) => ({
    key: scope,
    label: scope,
    metrics: Array.from(sampleTypes.values(), aggregateProfileMetric),
  }));
  // Ordered like the metrics of a single profile, by type and unit.
  const mergedMetrics = Array.from(mergedSampleTypes.values())
    .sort((a, b) =>
      a.type === b.type
        ? compareStrings(a.unit, b.unit)
        : compareStrings(a.type, b.type),
    )
    .map(aggregateProfileMetric);
  return {profiles, mergedMetrics};
}

function metricName(type: string, unit: string): string {
  return `${type} (${unit})`;
}

function compareStrings(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

// The metric of a sample type, summing its samples in the given aggregate
// profiles. Given those of several profiles, the flamegraph merges them, as
// it merges the frames with the same name under the same parent.
function aggregateProfileMetric({
  type,
  unit,
  aggregateProfileIds,
}: SampleType): TreeExplorerQueryMetric {
  return {
    name: metricName(type, unit),
    unit,
    nameColumnLabel: 'Symbol',
    dependencySql: 'include perfetto module callstacks.stack_profile',
    statement: `
      WITH profile_samples AS MATERIALIZED (
        SELECT callsite_id, sum(sample.value) AS sample_value
        FROM __intrinsic_aggregate_sample sample
        WHERE sample.aggregate_profile_id IN (${aggregateProfileIds.join(', ')})
        GROUP BY callsite_id
      )
      SELECT
        c.id,
        c.parent_id as parentId,
        c.name,
        c.mapping_name,
        c.source_file || ':' || c.line_number as source_location,
        cast_string!(c.inlined) AS inlined,
        CASE WHEN c.is_leaf_function_in_callsite_frame
          THEN coalesce(m.sample_value, 0)
          ELSE 0
        END AS value
      FROM _callstacks_for_stack_profile_samples!(profile_samples) AS c
      LEFT JOIN profile_samples AS m USING (callsite_id)
    `,
    unaggregatableProperties: [
      {name: 'mapping_name', displayName: 'Mapping'},
      {
        name: 'inlined',
        displayName: 'Inlined',
        isVisible: () => false,
      },
    ],
    aggregatableProperties: [
      {
        name: 'source_location',
        displayName: 'Source Location',
        mergeAggregation: 'ONE_OR_SUMMARY',
      },
    ],
    optionalMarker: {
      name: 'Inlined Function',
      isVisible: (properties: ReadonlyMap<string, string>) =>
        properties.get('inlined') === '1',
    },
  };
}
